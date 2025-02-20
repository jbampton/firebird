/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Arno Brinkman
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2004 Arno Brinkman <firebird@abvisie.nl>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 *  Adriano dos Santos Fernandes
 *  Dmitry Yemanov
 *
 */

#include "firebird.h"

#include "../jrd/jrd.h"
#include "../jrd/exe.h"
#include "../jrd/btr.h"
#include "../jrd/intl.h"
#include "../jrd/Collation.h"
#include "../jrd/ods.h"
#include "../jrd/RecordSourceNodes.h"
#include "../jrd/recsrc/RecordSource.h"
#include "../dsql/BoolNodes.h"
#include "../dsql/ExprNodes.h"
#include "../dsql/StmtNodes.h"
#include "../jrd/btr_proto.h"
#include "../jrd/cch_proto.h"
#include "../jrd/cmp_proto.h"
#include "../jrd/dpm_proto.h"
#include "../jrd/exe_proto.h"
#include "../jrd/ext_proto.h"
#include "../jrd/intl_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/par_proto.h"

#include "../jrd/optimizer/Optimizer.h"

#include <cmath>

using namespace Firebird;
using namespace Jrd;


namespace
{
	ValueExprNode* invertBoolValue(CompilerScratch* csb, ValueExprNode* value)
	{
		// Having a condition (<field> != <boolean value>),
		// invert it by making (<field> == (<boolean value> == FALSE)),
		// so that an index lookup could be possible

		static const UCHAR falseValue = '\0';
		LiteralNode* const falseLiteral =
			FB_NEW_POOL(csb->csb_pool) LiteralNode(csb->csb_pool);
		falseLiteral->litDesc.makeBoolean(const_cast<UCHAR*>(&falseValue));

		ComparativeBoolNode* const cmpNode =
			FB_NEW_POOL(csb->csb_pool) ComparativeBoolNode(csb->csb_pool, blr_eql);
		cmpNode->arg1 = value;
		cmpNode->arg2 = falseLiteral;

		// Recreate the boolean expression as a value
		BoolAsValueNode* const newValue =
			FB_NEW_POOL(csb->csb_pool) BoolAsValueNode(csb->csb_pool);
		newValue->boolean = cmpNode;

		newValue->impureOffset = csb->allocImpure<impure_value>();

		return newValue;
	}

	bool matchSubset(const BoolExprNode* boolean, const BoolExprNode* sub)
	{
		if (boolean->sameAs(sub, true))
			return true;

		auto binaryNode = nodeAs<BinaryBoolNode>(boolean);
		if (binaryNode && binaryNode->blrOp == blr_or)
		{
			if (matchSubset(binaryNode->arg1, sub) ||
				matchSubset(binaryNode->arg2, sub))
			{
				return true;
			}

			binaryNode = nodeAs<BinaryBoolNode>(sub);
			if (binaryNode && binaryNode->blrOp == blr_or)
			{
				if (matchSubset(boolean, binaryNode->arg1) &&
					matchSubset(boolean, binaryNode->arg2))
				{
					return true;
				}
			}
		}

		return false;
	};

} // namespace


IndexScratch::IndexScratch(MemoryPool& p, index_desc* idx)
	: index(idx), segments(p), matches(p)
{
	segments.resize(index->idx_count);
}

IndexScratch::IndexScratch(MemoryPool& p, const IndexScratch& other)
	: index(other.index),
	  cardinality(other.cardinality),
	  selectivity(other.selectivity),
	  candidate(other.candidate),
	  scopeCandidate(other.scopeCandidate),
	  lowerCount(other.lowerCount),
	  upperCount(other.upperCount),
	  nonFullMatchedSegments(other.nonFullMatchedSegments),
	  usePartialKey(other.usePartialKey),
	  useMultiStartingKeys(other.useMultiStartingKeys),
	  useRootListScan(other.useRootListScan),
	  segments(p, other.segments),
	  matches(p, other.matches)
{}


Retrieval::Retrieval(thread_db* aTdbb, Optimizer* opt, StreamType streamNumber,
					 bool outer, bool inner, SortNode* sortNode, bool costOnly)
	: PermanentStorage(*aTdbb->getDefaultPool()),
	  tdbb(aTdbb),
	  optimizer(opt),
	  csb(opt->getCompilerScratch()),
	  stream(streamNumber),
	  innerFlag(inner),
	  outerFlag(outer),
	  sort(sortNode),
	  createIndexScanNodes(!costOnly),
	  setConjunctionsMatched(!costOnly),
	  alias(getPool()),
	  indexScratches(getPool()),
	  inversionCandidates(getPool())
{
	const auto dbb = tdbb->getDatabase();

	const auto tail = &csb->csb_rpt[stream];
	relation = tail->csb_relation;
	fb_assert(relation);

	if (!tail->csb_idx)
		return;

	MatchedBooleanList matches;

	for (auto& index : *tail->csb_idx)
	{
		matches.clear();

		index.idx_fraction = MAXIMUM_SELECTIVITY;

		if ((index.idx_flags & idx_condition) && !checkIndexCondition(index, matches))
			continue;

		const auto length = ROUNDUP(BTR_key_length(tdbb, relation, &index), sizeof(SLONG));

		// AB: Calculate the cardinality which should reflect the total number
		// of index pages for this index.
		// We assume that the average index-key can be compressed by a factor 0.5
		// In the future the average key-length should be stored and retrieved
		// from a system table (RDB$INDICES for example).
		// Multiplying the selectivity with this cardinality gives the estimated
		// number of index pages that are read for the index retrieval.
		// Compound indexes are generally less compressed.
		const double factor = (index.idx_count == 1) ? 0.5 : 0.7;

		double cardinality = tail->csb_cardinality * index.idx_fraction;
		cardinality *= (2 + length * factor);
		cardinality /= (dbb->dbb_page_size - BTR_SIZE);
		cardinality = MAX(cardinality, MINIMUM_CARDINALITY);

		IndexScratch scratch(getPool(), &index);
		scratch.cardinality = cardinality;
		scratch.matches.assign(matches);

		indexScratches.add(scratch);
	}
}


//
// Melt two inversions together by the type given in node_type
//

InversionNode* Retrieval::composeInversion(InversionNode* node1,
										   InversionNode* node2,
										   InversionNode::Type node_type) const
{
	if (!node2)
		return node1;

	if (!node1)
		return node2;

	if (node_type == InversionNode::TYPE_OR)
	{
		if (node1->type == InversionNode::TYPE_INDEX &&
			node2->type == InversionNode::TYPE_INDEX &&
			node1->retrieval->irb_index == node2->retrieval->irb_index)
		{
			node_type = InversionNode::TYPE_IN;
		}
		else if (node1->type == InversionNode::TYPE_IN &&
			node2->type == InversionNode::TYPE_INDEX &&
			node1->node2->retrieval->irb_index == node2->retrieval->irb_index)
		{
			node_type = InversionNode::TYPE_IN;
		}
	}

	return FB_NEW_POOL(getPool()) InversionNode(node_type, node1, node2);
}

const string& Retrieval::getAlias()
{
	if (alias.isEmpty())
		alias = optimizer->makeAlias(this->stream);

	return alias;
}

InversionCandidate* Retrieval::getInversion()
{
	if (finalCandidate)
		return finalCandidate;

	auto iter = optimizer->getConjuncts(outerFlag, innerFlag);

	InversionCandidate* invCandidate = nullptr;

	if (relation && !relation->rel_file && !relation->isVirtual())
	{
		InversionCandidateList inversions;

		// First, handle "AND" comparisons (all nodes except OR)
		for (iter.rewind(); iter.hasData(); ++iter)
		{
			const auto booleanNode = nodeAs<BinaryBoolNode>(*iter);

			if (!(iter & Optimizer::CONJUNCT_USED) &&
				(!booleanNode || booleanNode->blrOp != blr_or))
			{
				invCandidate = matchOnIndexes(indexScratches, iter, 1);

				if (invCandidate)
					inversions.add(invCandidate);
			}
		}

		getInversionCandidates(inversions, indexScratches, 1);

		// Second, handle "OR" comparisons
		for (iter.rewind(); iter.hasData(); ++iter)
		{
			const auto booleanNode = nodeAs<BinaryBoolNode>(*iter);

			if (!(iter & Optimizer::CONJUNCT_USED) &&
				(booleanNode && booleanNode->blrOp == blr_or))
			{
				invCandidate = matchOnIndexes(indexScratches, iter, 1);

				if (invCandidate)
				{
					invCandidate->boolean = iter;
					inversions.add(invCandidate);
				}
			}
		}

		if (sort)
			analyzeNavigation(inversions);

#ifdef OPT_DEBUG_RETRIEVAL
		// Debug
		printCandidates(inversions);
#endif

		invCandidate = makeInversion(inversions);

		// Clean up intermediate inversion candidates
		for (const auto candidate : inversions)
		{
			if (candidate != navigationCandidate)
				delete candidate;
		}
	}

	const auto cardinality = csb->csb_rpt[stream].csb_cardinality;

	if (!invCandidate)
	{
		// No index will be used, thus create a dummy inversion candidate
		// representing the natural table access. All the necessary properties
		// (selectivity: 1.0, cost: 0, unique: false) are set up by the constructor.
		invCandidate = FB_NEW_POOL(getPool()) InversionCandidate(getPool());
	}

	if (invCandidate->unique)
	{
		// Set up the unique retrieval cost to be fixed and not dependent on
		// possibly outdated statistics. It includes N index scans plus one data page fetch.
		invCandidate->cost = DEFAULT_INDEX_COST * invCandidate->indexes + 1;
	}
	else
	{
		// Add the records retrieval cost to the priorly calculated index scan cost
		invCandidate->cost += cardinality * invCandidate->selectivity;
	}

	// Adjust the effective selectivity by treating computable but unmatched conjunctions
	// as filters. But consider only those local to our stream.
	// While being here, also mark matched conjuncts, if requested.
	double selectivity = MAXIMUM_SELECTIVITY;
	for (iter.rewind(); iter.hasData(); ++iter)
	{
		if (!(iter & Optimizer::CONJUNCT_USED))
		{
			const auto matched = invCandidate->matches.exist(iter);

			if (setConjunctionsMatched && matched)
				iter |= Optimizer::CONJUNCT_MATCHED;
			else if (!setConjunctionsMatched && !matched &&
				iter->computable(csb, stream, true) &&
				iter->containsStream(stream))
			{
				selectivity *= Optimizer::getSelectivity(*iter);
			}
		}
	}

	Optimizer::adjustSelectivity(invCandidate->selectivity, selectivity, cardinality);

	// Add the streams where this stream is depending on
	for (auto match : invCandidate->matches)
	{
		match->findDependentFromStreams(csb, stream,
			&invCandidate->dependentFromStreams);
	}

#ifdef OPT_DEBUG_RETRIEVAL
	// Debug
	printFinalCandidate(invCandidate);
#endif

	finalCandidate = invCandidate;

	return invCandidate;
}

IndexTableScan* Retrieval::getNavigation(const InversionCandidate* candidate)
{
	if (!navigationCandidate)
		return nullptr;

	const auto scratch = navigationCandidate->scratch;

	const auto streamCardinality = csb->csb_rpt[stream].csb_cardinality;

	// If the table looks like empty during preparation time, we cannot be sure about
	// its real cardinality during execution. So, unless we have some index-based
	// filtering applied, let's better be pessimistic and avoid external sorting
	// due to likely cardinality under-estimation.
	const bool avoidSorting = (streamCardinality <= MINIMUM_CARDINALITY && !candidate->inversion);

	if (!(scratch->index->idx_runtime_flags & idx_plan_navigate) && !avoidSorting)
	{
		// Check whether the navigational index scan is cheaper than the external sort
		// and give up if it's not worth the efforts.
		//
		// We ignore candidate->cost in the calculations below as it belongs
		// to both parts being compared.

		fb_assert(candidate);

		// Restore the original selectivity of the inversion,
		// i.e. before the navigation candidate was accounted
		auto selectivity = candidate->selectivity / navigationCandidate->selectivity;

		// Non-indexed booleans are checked before sorting, so they improve the selectivity

		double factor = MAXIMUM_SELECTIVITY;
		for (auto iter = optimizer->getConjuncts(outerFlag, innerFlag); iter.hasData(); ++iter)
		{
			if (!(iter & Optimizer::CONJUNCT_USED) &&
				!candidate->matches.exist(iter) &&
				iter->computable(csb, stream, true) &&
				iter->containsStream(stream))
			{
				factor *= Optimizer::getSelectivity(*iter);
			}
		}

		Optimizer::adjustSelectivity(selectivity, factor, streamCardinality);

		// Don't consider external sorting if optimization for first rows is requested
		// and we have no local filtering applied

		if (!optimizer->favorFirstRows() || selectivity < MAXIMUM_SELECTIVITY)
		{
			// Estimate amount of records to be sorted
			const auto cardinality = streamCardinality * selectivity;

			// We optimistically assume that records will be cached during sorting
			const auto sortCost =
				// record copying (to the sort buffer and back)
				cardinality * COST_FACTOR_MEMCOPY * 2 +
				// quicksort algorithm is O(n*log(n)) in average
				cardinality * log2(cardinality) * COST_FACTOR_QUICKSORT;

			// During navigation we fetch an index leaf page per every record being returned,
			// thus add the estimated cardinality to the cost
			auto navigationCost = navigationCandidate->cost +
				streamCardinality * candidate->selectivity;

			if (optimizer->favorFirstRows())
			{
				// Reset the cost to represent a single record retrieval
				navigationCost = DEFAULT_INDEX_COST;

				// We know that some local filtering is applied, so we need
				// to adjust the cost as we need to walk the index
				// until the first matching record is found
				const auto fullIndexCost = navigationCandidate->scratch->cardinality;
				const auto ratio = MAXIMUM_SELECTIVITY / selectivity;
				const auto fraction = ratio / streamCardinality;
				const auto walkCost = fullIndexCost * fraction * navigationCandidate->selectivity;
				navigationCost += walkCost;
			}

			if (sortCost < navigationCost)
				return nullptr;
		}
	}

	// Looks like we can do a navigational walk.  Flag that
	// we have used this index for navigation, and allocate
	// a navigational rsb for it.
	scratch->index->idx_runtime_flags |= idx_navigate;

	const auto indexNode = makeIndexScanNode(scratch);

	const USHORT keyLength =
		ROUNDUP(BTR_key_length(tdbb, relation, scratch->index), sizeof(SLONG));

	return FB_NEW_POOL(getPool())
		IndexTableScan(csb, getAlias(), stream, relation, indexNode, keyLength,
					   navigationCandidate->selectivity);
}

void Retrieval::analyzeNavigation(const InversionCandidateList& inversions)
{
	fb_assert(sort);

	HalfStaticArray<InversionCandidate*, OPT_STATIC_ITEMS> tempCandidates;
	InversionCandidate* bestCandidate = nullptr;

	for (auto& indexScratch : indexScratches)
	{
		auto idx = indexScratch.index;

		// if the number of fields in the sort is greater than the number of
		// fields in the index, the index will not be used to optimize the
		// sort--note that in the case where the first field is unique, this
		// could be optimized, since the sort will be performed correctly by
		// navigating on a unique index on the first field--deej
		if (sort->expressions.getCount() > idx->idx_count)
			continue;

		// if the user-specified access plan for this request didn't
		// mention this index, forget it
		if ((idx->idx_runtime_flags & idx_plan_dont_use) &&
			!(idx->idx_runtime_flags & idx_plan_navigate))
		{
			continue;
		}

		// only a single-column ORDER BY clause can be mapped to
		// an expression index
		if (idx->idx_flags & idx_expression)
		{
			if (sort->expressions.getCount() != 1)
				continue;
		}

		// check to see if the fields in the sort match the fields in the index
		// in the exact same order

		unsigned equalSegments = 0;
		for (unsigned i = 0; i < MIN(indexScratch.lowerCount, indexScratch.upperCount); i++)
		{
			const auto& segment = indexScratch.segments[i];

			if (segment.scanType == segmentScanEqual ||
				segment.scanType == segmentScanEquivalent ||
				segment.scanType == segmentScanMissing)
			{
				equalSegments++;
			}
		}

		bool usableIndex = true;
		const index_desc::idx_repeat* idx_tail = idx->idx_rpt;
		const index_desc::idx_repeat* const idx_end = idx_tail + idx->idx_count;
		NestConst<ValueExprNode>* ptr = sort->expressions.begin();
		const SortDirection* direction = sort->direction.begin();
		const NullsPlacement* nullOrder = sort->nullOrder.begin();

		for (const NestConst<ValueExprNode>* const end = sort->expressions.end();
			 ptr != end;
			 ++ptr, ++direction, ++nullOrder, ++idx_tail)
		{
			ValueExprNode* const orgNode = *ptr;
			FieldNode* fieldNode;
			bool nodeMatched = false;

			// Collect nodes equivalent to the given sort node

			HalfStaticArray<ValueExprNode*, OPT_STATIC_ITEMS> nodes;
			nodes.add(orgNode);

			for (auto iter = optimizer->getConjuncts(outerFlag, innerFlag); iter.hasData(); ++iter)
			{
				const auto cmpNode = nodeAs<ComparativeBoolNode>(*iter);

				if (cmpNode && (cmpNode->blrOp == blr_eql || cmpNode->blrOp == blr_equiv))
				{
					ValueExprNode* const node1 = cmpNode->arg1;
					ValueExprNode* const node2 = cmpNode->arg2;

					if (node1->sameAs(orgNode, false))
						nodes.add(node2);

					if (node2->sameAs(orgNode, false))
						nodes.add(node1);
				}
			}

			// Check whether any of the equivalent nodes is suitable for index navigation

			for (const auto node : nodes)
			{
				if (idx->idx_flags & idx_expression)
				{
					if (!checkIndexExpression(idx, node))
						continue;
				}
				else if (!(fieldNode = nodeAs<FieldNode>(node)) || fieldNode->fieldStream != stream)
				{
					continue;
				}
				else
				{
					for (; idx_tail < idx_end && fieldNode->fieldId != idx_tail->idx_field; idx_tail++)
					{
						const unsigned segmentNumber = idx_tail - idx->idx_rpt;

						if (segmentNumber >= equalSegments)
							break;
					}

					if (idx_tail >= idx_end || fieldNode->fieldId != idx_tail->idx_field)
						continue;
				}

				if ((*direction == ORDER_DESC && !(idx->idx_flags & idx_descending)) ||
					(*direction == ORDER_ASC && (idx->idx_flags & idx_descending)) ||
					((*nullOrder == NULLS_FIRST && *direction == ORDER_DESC) ||
					 (*nullOrder == NULLS_LAST && *direction == ORDER_ASC)))
				{
					continue;
				}

				dsc desc;
				node->getDesc(tdbb, csb, &desc);

				// ASF: "desc.dsc_ttype() > ttype_last_internal" is to avoid recursion
				// when looking for charsets/collations

				if (DTYPE_IS_TEXT(desc.dsc_dtype) && desc.dsc_ttype() > ttype_last_internal)
				{
					const TextType* const tt = INTL_texttype_lookup(tdbb, desc.dsc_ttype());

					if (idx->idx_flags & idx_unique)
					{
						if (tt->getFlags() & TEXTTYPE_UNSORTED_UNIQUE)
							continue;
					}
					else
					{
						// ASF: We currently can't use non-unique index for GROUP BY and DISTINCT with
						// multi-level and insensitive collation. In NAV, keys are verified with memcmp
						// but there we don't know length of each level.
						if (sort->unique && (tt->getFlags() & TEXTTYPE_SEPARATE_UNIQUE))
							continue;
					}
				}

				nodeMatched = true;
				break;
			}

			if (!nodeMatched)
			{
				usableIndex = false;
				break;
			}
		}

		if (!usableIndex)
			continue;

		// Lookup the inversion candidate matching our navigational index

		InversionCandidate* candidate = nullptr;

		for (const auto inversion : inversions)
		{
			if (inversion->scratch == &indexScratch)
			{
				candidate = inversion;
				break;
			}
		}

		// Check whether the navigational index has any matches shared with other inversion
		// candidates. If so, compare inversions and decide whether navigation is acceptable.
		// However, if the user-specified access plan mentions this index,
		// then don't consider any (possibly better) alternatives.
		// Another exception is when the FIRST ROWS optimization strategy is applied.

		if (candidate && !optimizer->favorFirstRows() &&
			!(idx->idx_runtime_flags & idx_plan_navigate))
		{
			for (const auto otherCandidate : inversions)
			{
				if (otherCandidate != candidate)
				{
					for (const auto otherMatch : otherCandidate->matches)
					{
						if (candidate->matches.exist(otherMatch) &&
							betterInversion(otherCandidate, candidate, true))
						{
							usableIndex = false;
							break;
						}
					}
				}

				if (!usableIndex)
					break;
			}
		}

		if (!usableIndex)
			continue;

		// Looks like we can do a navigational walk. Remember this candidate
		// and compare it against other possible candidates.

		if (!candidate)
		{
			// If no inversion candidate is found, create a fake one representing full index scan

			candidate = FB_NEW_POOL(getPool()) InversionCandidate(getPool());
			candidate->cost = DEFAULT_INDEX_COST + indexScratch.cardinality;
			candidate->indexes = 1;
			candidate->scratch = &indexScratch;
			candidate->nonFullMatchedSegments = indexScratch.segments.getCount();
			tempCandidates.add(candidate);
		}

		if (!bestCandidate ||
			betterInversion(candidate, bestCandidate, false))
		{
			bestCandidate = candidate;
		}
	}

	// Clean up intermediate inversion candidates
	for (const auto candidate : tempCandidates)
	{
		if (candidate != bestCandidate)
			delete candidate;
	}

	navigationCandidate = bestCandidate;
}

bool Retrieval::betterInversion(const InversionCandidate* inv1,
								const InversionCandidate* inv2,
								bool navigation) const
{
	// Return true if inversion1 is *better* than inversion2.
	// It's mostly about the retrieval cost, but other aspects are also taken into account.

	if (inv1->unique && !inv2->unique)
	{
		// A unique full equal match is better than anything else
		return true;
	}

	if (inv1->unique == inv2->unique)
	{
		if (inv1->dependencies > inv2->dependencies)
		{
			// Index used for a relationship must be always prefered to
			// the filtering ones, otherwise the nested loop join has
			// no chances to be better than a sort merge.
			// An alternative (simplified) condition might be:
			//   currentInv->dependencies > 0
			//   && bestCandidate->dependencies == 0
			// but so far I tend to think that the current one is better.
			return true;
		}

		if (inv1->dependencies == inv2->dependencies)
		{
			const double cardinality = csb->csb_rpt[stream].csb_cardinality;

			const double cost1 = inv1->cost + (inv1->selectivity * cardinality);
			const double cost2 = inv2->cost + (inv2->selectivity * cardinality);

			// Do we have very similar costs?
			double diffCost = 0;
			if (!cost1 && !cost2)
			{
				// Two zero costs should be handled as being the same
				// (other comparison criteria should be applied, see below)
				diffCost = 1;
			}
			else if (cost1)
			{
				// Calculate the difference
				diffCost = cost2 / cost1;
			}

			if ((diffCost >= 0.98) && (diffCost <= 1.02))
			{
				// If the "same" costs then compare with the nr of unmatched segments,
				// how many indexes and matched segments. First compare number of indexes.

				int diff = (inv1->indexes - inv2->indexes);

				if (diff == 0)
				{
					// For the same number of indexes compare number of matched segments.
					// Note the inverted condition: the more matched segments the better.

					diff = (inv2->matchedSegments - inv1->matchedSegments);

					if (diff == 0 && !navigation)
					{
						// For the same number of matched segments compare ones that aren't full matched.
						//
						// However, unmatched segments and small cost difference do not matter
						// if we already know the first retrieval being usable for navigation.

						diff = (inv1->nonFullMatchedSegments - inv2->nonFullMatchedSegments);

						if (diff == 0)
						{
							// For inversions with nearly the same cost but without other preferences found,
							// return the actually cheaper inversion (based on cost only)

							if (cost1 < cost2)
								return true;
						}
					}
				}

				if (diff < 0)
					return true;
			}
			else if (cost1 < cost2)
				return true;
		}
	}

	return false;
}

bool Retrieval::checkIndexCondition(index_desc& idx, MatchedBooleanList& matches) const
{
	fb_assert(idx.idx_condition);

	if (!idx.idx_condition->containsStream(0, true))
		return false;

	fb_assert(matches.isEmpty());

	auto iter = optimizer->getConjuncts(outerFlag, innerFlag);

	BoolExprNodeStack idxConjuncts;
	const auto conjunctCount = optimizer->decomposeBoolean(idx.idx_condition, idxConjuncts);
	fb_assert(conjunctCount);

	idx.idx_fraction = MAXIMUM_SELECTIVITY;

	for (BoolExprNodeStack::const_iterator idxIter(idxConjuncts);
		idxIter.hasData(); ++idxIter)
	{
		const auto boolean = idxIter.object();

		// If the index condition is (A OR B) and any of the {A, B} is present
		// among the available booleans, then the index is possibly usable.
		// Note: this check also includes the exact match.

		for (iter.rewind(); iter.hasData(); ++iter)
		{
			if (!iter->containsStream(stream))
				continue;

			if (matchSubset(boolean, *iter))
			{
				matches.add(*iter);
				break;
			}
		}

		// If the index condition is (A IS NOT NULL) and the available booleans
		// includes any comparative predicate that explicitly mentions A,
		// then the index is possibly usable

		const auto notNode = nodeAs<NotBoolNode>(boolean);
		const auto missingNode = notNode ? nodeAs<MissingBoolNode>(notNode->arg) : nullptr;
		if (missingNode)
		{
			for (iter.rewind(); iter.hasData(); ++iter)
			{
				if (!iter->containsStream(stream))
					continue;

				const auto cmpNode = nodeAs<ComparativeBoolNode>(*iter);
				if (cmpNode && cmpNode->blrOp != blr_equiv)
				{
					if (cmpNode->arg1->sameAs(missingNode->arg, true) ||
						cmpNode->arg2->sameAs(missingNode->arg, true))
					{
						matches.add(*iter);
						break;
					}

					if (cmpNode->arg3 &&
						cmpNode->arg3->sameAs(missingNode->arg, true))
					{
						matches.add(*iter);
						break;
					}
				}
			}
		}

		idx.idx_fraction *= optimizer->getSelectivity(boolean);
	}

	return (matches.getCount() >= conjunctCount);
}

bool Retrieval::checkIndexExpression(const index_desc* idx, ValueExprNode* node) const
{
	fb_assert(idx && idx->idx_expression);

	// The desired expression can be hidden inside a derived expression node,
	// so try to recover it (see CORE-4118).
	while (!idx->idx_expression->sameAs(node, true))
	{
		const auto derivedExpr = nodeAs<DerivedExprNode>(node);
		const auto cast = nodeAs<CastNode>(node);

		if (derivedExpr)
			node = derivedExpr->arg;
		else if (cast && cast->artificial)
			node = cast->source;
		else
			return false;
	}

	// Check the index for matching both the given stream and the given expression tree

	return idx->idx_expression->containsStream(0, true) &&
		node->containsStream(stream, true);
}

void Retrieval::getInversionCandidates(InversionCandidateList& inversions,
									   IndexScratchList& fromIndexScratches,
									   unsigned scope) const
{
	const double cardinality = csb->csb_rpt[stream].csb_cardinality;
	fb_assert(cardinality);
	const double minSelectivity = MIN(MAXIMUM_SELECTIVITY / cardinality, DEFAULT_SELECTIVITY);

	// Walk through indexes to calculate selectivity / candidate
	MatchedBooleanList matches;

	for (auto& scratch : fromIndexScratches)
	{
		scratch.scopeCandidate = false;
		scratch.lowerCount = 0;
		scratch.upperCount = 0;
		scratch.nonFullMatchedSegments = MAX_INDEX_SEGMENTS + 1;
		scratch.usePartialKey = false;
		scratch.useMultiStartingKeys = false;
		scratch.useRootListScan = false;

		const auto idx = scratch.index;

		if (scratch.candidate)
		{
			matches.assign(scratch.matches);
			scratch.selectivity = MAXIMUM_SELECTIVITY;

			bool unique = false;
			unsigned listCount = 0;
			auto maxSelectivity = scratch.selectivity;

			for (unsigned j = 0; j < scratch.segments.getCount(); j++)
			{
				const auto& segment = scratch.segments[j];

				auto scanType = segment.scanType;

				if (segment.scope == scope)
					scratch.scopeCandidate = true;

				const USHORT iType = idx->idx_rpt[j].idx_itype;

				if (iType >= idx_first_intl_string)
				{
					auto textType = INTL_texttype_lookup(tdbb, INTL_INDEX_TO_TEXT(iType));

					if (scanType != segmentScanMissing && !(idx->idx_flags & idx_unique))
					{
						if (textType->getFlags() & TEXTTYPE_SEPARATE_UNIQUE)
						{
							// ASF: Order is more precise than equivalence class.
							// We can't use the next segments, and we'll need to use
							// INTL_KEY_PARTIAL to construct the last segment's key.
							scratch.usePartialKey = true;
						}
					}

					if (scanType == segmentScanStarting)
					{
						if (textType->getFlags() & TEXTTYPE_MULTI_STARTING_KEY)
							scratch.useMultiStartingKeys = true;	// use INTL_KEY_MULTI_STARTING

						scratch.usePartialKey = true;
					}
				}

				auto selectivity = idx->idx_rpt[j].idx_selectivity;
				const auto useDefaultSelectivity = (selectivity <= 0);

				// When the index selectivity is zero then the statement is prepared
				// on an empty table or the statistics aren't updated. So assume every
				// match to represent 1/10 of the maximum selectivity.
				if (useDefaultSelectivity)
					selectivity = MAX(scratch.selectivity * DEFAULT_SELECTIVITY, minSelectivity);

				if (scanType == segmentScanList)
				{
					if (listCount) // we cannot have more than one list matched to an index
						break;

					const auto list = segment.valueList;
					fb_assert(list);

					listCount = list->getCount();
					maxSelectivity = scratch.selectivity;
				}

				// Check if this is the last usable segment
				if (!scratch.usePartialKey &&
					(scanType == segmentScanEqual ||
					 scanType == segmentScanEquivalent ||
					 scanType == segmentScanMissing ||
					 scanType == segmentScanList))
				{
					// This is a perfect usable segment thus update root selectivity
					scratch.lowerCount++;
					scratch.upperCount++;
					scratch.nonFullMatchedSegments = idx->idx_count - (j + 1);
					// Add matches for this segment to the main matches list
					matches.join(segment.matches);
					scratch.selectivity = selectivity;

					// An equality scan for any unique index cannot retrieve more
					// than one row. The same is true for an equivalence scan for
					// any primary index. A missing scan for any primary index is
					// known to return no rows, but let's treat it the same way.
					const bool uniqueMatch =
						(scanType == segmentScanEqual && (idx->idx_flags & idx_unique)) ||
						(scanType == segmentScanEquivalent && (idx->idx_flags & idx_primary)) ||
						(scanType == segmentScanMissing && (idx->idx_flags & idx_primary));

					if (uniqueMatch && ((j + 1) == idx->idx_count))
					{
						// We have found a full equal matching index and it's unique,
						// so we can stop looking further, because this is the best
						// one we can get.
						unique = true;

						// If selectivity is assumed, a better guess for the unique match
						// would be 1 / cardinality
						if (useDefaultSelectivity)
							scratch.selectivity = minSelectivity;

						break;
					}

					// dimitr: number of nulls is not reflected by our selectivity,
					//		   so IS NOT DISTINCT and IS NULL scans may retrieve
					//		   much bigger bitmap than expected here. I think
					//		   appropriate reduce selectivity factors are required
					//		   to be applied here.
				}
				else
				{
					if (scanType != segmentScanNone)
					{
						// This is our last segment that we can use,
						// estimate the selectivity
						double factor = 1;

						switch (scanType)
						{
							case segmentScanBetween:
								scratch.lowerCount++;
								scratch.upperCount++;
								factor = REDUCE_SELECTIVITY_FACTOR_BETWEEN;
								break;

							case segmentScanLess:
								scratch.upperCount++;
								factor = REDUCE_SELECTIVITY_FACTOR_LESS;
								break;

							case segmentScanGreater:
								scratch.lowerCount++;
								factor = REDUCE_SELECTIVITY_FACTOR_GREATER;
								break;

							case segmentScanStarting:
							case segmentScanEqual:
							case segmentScanEquivalent:
							case segmentScanList:
								scratch.lowerCount++;
								scratch.upperCount++;
								factor = REDUCE_SELECTIVITY_FACTOR_STARTING;
								break;

							default:
								fb_assert(false);
								break;
						}

						// Adjust the compound selectivity using the reduce factor.
						// It should be better than the previous segment but worse
						// than a full match.
						const double diffSelectivity = scratch.selectivity - selectivity;
						selectivity += (diffSelectivity * factor);
						fb_assert(selectivity <= scratch.selectivity);
						scratch.selectivity = selectivity;

						scratch.nonFullMatchedSegments = idx->idx_count - j;
						matches.join(segment.matches);
					}

					break;
				}
			}

			if (scratch.scopeCandidate)
			{
				double selectivity = scratch.selectivity;
				fb_assert(selectivity);

				// Calculate the cost (only index pages) for this index
				auto cost = DEFAULT_INDEX_COST + selectivity * scratch.cardinality;

				if (listCount)
				{
					// Adjust selectivity based on the list items count
					selectivity *= listCount;
					selectivity = MIN(selectivity, maxSelectivity);

					const auto rootScanCost = cost * listCount;
					const auto siblingScanCost = DEFAULT_INDEX_COST +
						scratch.cardinality * maxSelectivity * (listCount - 1) / (listCount + 1);

					if (rootScanCost < siblingScanCost)
					{
						cost = rootScanCost;
						scratch.useRootListScan = true;
					}
					else
						cost = siblingScanCost;
				}

				const auto invCandidate = FB_NEW_POOL(getPool()) InversionCandidate(getPool());
				invCandidate->unique = unique;
				invCandidate->selectivity = idx->idx_fraction * selectivity;
				invCandidate->cost = cost;
				invCandidate->nonFullMatchedSegments = scratch.nonFullMatchedSegments;
				invCandidate->matchedSegments = MAX(scratch.lowerCount, scratch.upperCount);
				invCandidate->indexes = 1;
				invCandidate->scratch = &scratch;
				invCandidate->matches.join(matches);

				for (auto match : invCandidate->matches)
				{
					match->findDependentFromStreams(csb, stream,
						&invCandidate->dependentFromStreams);
				}

				invCandidate->dependencies = invCandidate->dependentFromStreams.getCount();
				inversions.add(invCandidate);
			}
		}
		else if (idx->idx_flags & idx_condition)
		{
			const auto invCandidate = FB_NEW_POOL(getPool()) InversionCandidate(getPool());
			invCandidate->selectivity = idx->idx_fraction;
			invCandidate->cost = DEFAULT_INDEX_COST + scratch.cardinality;
			invCandidate->indexes = 1;
			invCandidate->scratch = &scratch;
			invCandidate->nonFullMatchedSegments = scratch.segments.getCount();
			invCandidate->matches.assign(scratch.matches);

			for (auto match : invCandidate->matches)
			{
				match->findDependentFromStreams(csb, stream,
					&invCandidate->dependentFromStreams);
			}

			invCandidate->dependencies = invCandidate->dependentFromStreams.getCount();
			inversions.add(invCandidate);
		}
	}
}


//
// Search a dbkey (possibly a concatenated one) for a dbkey for specified stream
//

ValueExprNode* Retrieval::findDbKey(ValueExprNode* dbkey, SLONG* position) const
{
	const auto keyNode = nodeAs<RecordKeyNode>(dbkey);

	if (keyNode && keyNode->blrOp == blr_dbkey)
	{
		if (keyNode->recStream == stream)
			return dbkey;

		++*position;
		return nullptr;
	}

	const auto concatNode = nodeAs<ConcatenateNode>(dbkey);

	if (concatNode)
	{
		ValueExprNode* dbkey_temp = findDbKey(concatNode->arg1, position);

		if (dbkey_temp)
			return dbkey_temp;

		dbkey_temp = findDbKey(concatNode->arg2, position);

		if (dbkey_temp)
			return dbkey_temp;
	}

	return nullptr;
}


//
// Build node for index scan
//

InversionNode* Retrieval::makeIndexScanNode(IndexScratch* indexScratch) const
{
	if (!createIndexScanNodes)
		return nullptr;

	const auto idx = indexScratch->index;
	auto& segments = indexScratch->segments;

	// Check whether this is during a compile or during a SET INDEX operation
	if (csb)
		CMP_post_resource(&csb->csb_resources, relation, Resource::rsc_index, idx->idx_id);
	else
	{
		CMP_post_resource(&tdbb->getRequest()->getStatement()->resources, relation,
			Resource::rsc_index, idx->idx_id);
	}

	// For external requests, determine index name (to be reported in plans)
	QualifiedName indexName;
	if (!(csb->csb_g_flags & csb_internal))
		MET_lookup_index(tdbb, indexName, relation->rel_name, idx->idx_id + 1);

	const auto retrieval =
		FB_NEW_POOL(getPool()) IndexRetrieval(getPool(), relation, idx, indexName);

	// Pick up lower bound segment values
	ValueExprNode** lower = retrieval->irb_value;
	ValueExprNode** upper = retrieval->irb_value + idx->idx_count;
	retrieval->irb_lower_count = indexScratch->lowerCount;
	retrieval->irb_upper_count = indexScratch->upperCount;

	if (idx->idx_flags & idx_descending)
	{
		// switch upper/lower information
		upper = retrieval->irb_value;
		lower = retrieval->irb_value + idx->idx_count;
		retrieval->irb_lower_count = indexScratch->upperCount;
		retrieval->irb_upper_count = indexScratch->lowerCount;
		retrieval->irb_generic |= irb_descending;
	}

	if (const auto count = MAX(indexScratch->lowerCount, indexScratch->upperCount))
	{
		bool ignoreNullsOnScan = true;

		for (unsigned i = 0; i < count; i++)
		{
			if (segments[i].scanType == segmentScanMissing)
			{
				*lower++ = *upper++ = NullNode::instance();
				ignoreNullsOnScan = false;
			}
			else
			{
				if (i < indexScratch->lowerCount)
					*lower++ = segments[i].lowerValue;

				if (i < indexScratch->upperCount)
					*upper++ = segments[i].upperValue;

				if (segments[i].scanType == segmentScanEquivalent)
					ignoreNullsOnScan = false;

				if (segments[i].scanType == segmentScanList)
				{
					fb_assert(!retrieval->irb_list);
					retrieval->irb_list = segments[i].valueList;
				}

				if (segments[i].scale)
				{
					if (!retrieval->irb_scale)
					{
						retrieval->irb_scale = FB_NEW_POOL(getPool()) SSHORT[count];
						memset(retrieval->irb_scale, 0, sizeof(SSHORT) * count);
					}
					retrieval->irb_scale[i] = segments[i].scale;
				}
			}
		}

		// This index is never used for IS NULL, thus we can ignore NULLs
		// already at index scan. But this rule doesn't apply to nod_equiv
		// which requires NULLs to be found in the index.
		//
		// dimitr:	make sure the check below is never moved outside the IF scope,
		// 			as this flag must not be set for a full index scan,
		//			see also the assertion below
		if (ignoreNullsOnScan)
		{
			fb_assert(indexScratch->lowerCount || indexScratch->upperCount);
			retrieval->irb_generic |= irb_ignore_null_value_key;
		}

		const auto& lastSegment = segments[count - 1];

		if (lastSegment.scanType == segmentScanStarting)
			retrieval->irb_generic |= irb_starting;

		if (lastSegment.excludeLower)
			retrieval->irb_generic |= irb_exclude_lower;

		if (lastSegment.excludeUpper)
			retrieval->irb_generic |= irb_exclude_upper;
	}

	if (indexScratch->usePartialKey)
		retrieval->irb_generic |= irb_starting;	// Flag the need to use INTL_KEY_PARTIAL in btr.

	if (indexScratch->useMultiStartingKeys)
	{
		// Flag the need to use INTL_KEY_MULTI_STARTING in btr.
		retrieval->irb_generic |= irb_multi_starting | irb_starting;
	}

	if (indexScratch->useRootListScan)
	{
		fb_assert(retrieval->irb_list);
		retrieval->irb_generic |= irb_root_list_scan;
	}

	// Check to see if this is really an equality retrieval
	if (retrieval->irb_lower_count == retrieval->irb_upper_count)
	{
		const bool fullMatch = (retrieval->irb_lower_count == idx->idx_count);
		bool uniqueMatch = false;

		retrieval->irb_generic |= irb_equality;

		for (unsigned i = 0; i < retrieval->irb_lower_count; i++)
		{
			if (segments[i].lowerValue != segments[i].upperValue)
			{
				retrieval->irb_generic &= ~irb_equality;
				break;
			}

			if (segments[i].scanType == segmentScanMissing ||
				segments[i].scanType == segmentScanEquivalent)
			{
				if (fullMatch && (idx->idx_flags & idx_primary))
					uniqueMatch = true;
			}
			else if (segments[i].scanType == segmentScanEqual)
			{
				if (fullMatch && (idx->idx_flags & idx_unique))
					uniqueMatch = true;
			}
		}

		if ((retrieval->irb_generic & irb_equality) && uniqueMatch)
			retrieval->irb_generic |= irb_unique;
	}

	// If we are matching less than the full index, this is a partial match
	if (idx->idx_flags & idx_descending)
	{
		if (retrieval->irb_lower_count < idx->idx_count)
			retrieval->irb_generic |= irb_partial;
	}
	else
	{
		if (retrieval->irb_upper_count < idx->idx_count)
			retrieval->irb_generic |= irb_partial;
	}

	// mark the index as utilized for the purposes of this compile
	idx->idx_runtime_flags |= idx_used;

	const ULONG impure = csb ? csb->allocImpure<impure_inversion>() : 0;
	return FB_NEW_POOL(getPool()) InversionNode(retrieval, impure);
}

//
// Select best available inversion candidates and compose them to 1 inversion
//

InversionCandidate* Retrieval::makeInversion(InversionCandidateList& inversions) const
{
	if (inversions.isEmpty() && !navigationCandidate)
		return nullptr;

	const double streamCardinality = csb->csb_rpt[stream].csb_cardinality;

	// Prepared statements could be optimized against an almost empty table
	// and then cached (such as in the restore process), thus causing slowdown
	// when the table grows. In this case, let's consider all available indices.
	const bool smallTable = (streamCardinality <= THRESHOLD_CARDINALITY);

	// These flags work around our smart index selection algorithm. Any explicit
	// (i.e. user specified) plan requires all existing indices to be considered
	// for a retrieval. Internal (system) requests used by the engine itself are
	// often optimized using zero or non-actual statistics, so they are processed
	// using somewhat relaxed rules.
	const bool customPlan = csb->csb_rpt[stream].csb_plan;
	const bool sysRequest = (csb->csb_g_flags & csb_internal);

	double totalSelectivity = MAXIMUM_SELECTIVITY; // worst selectivity
	double totalIndexCost = 0;

	// The upper limit to use an index based retrieval is five indexes + almost all datapages
	const double maximumCost = (DEFAULT_INDEX_COST * 5) + (streamCardinality * 0.95);
	const double minimumSelectivity = 1 / streamCardinality;

	double previousTotalCost = maximumCost;

	// Force to always choose at least one index
	bool firstCandidate = true;

	InversionCandidate* invCandidate = nullptr;

	for (auto inversion : inversions)
	{
		if (const auto indexScratch = inversion->scratch)
		{
			const auto idx = indexScratch->index;

			// If the explicit plan doesn't mention this index, fake it as used
			// thus excluding it from the cost-based algorithm. Otherwise,
			// given this index is suitable for navigation, also mark it as used.

			if (((idx->idx_runtime_flags & idx_plan_dont_use)) ||
				(!customPlan && inversion == navigationCandidate))
			{
				inversion->used = true;
			}

			// If the index is conditional and its condition is also present in
			// some other inversion as a boolean (it represents the OR operation),
			// fake these other inversions as used, so that the full index scan would
			// be preferred to multiple range scans. The cost-based algorithm below
			// cannot handle it currently.

			if (idx->idx_flags & idx_condition)
			{
				for (auto otherInversion : inversions)
				{
					if (otherInversion->boolean &&
						idx->idx_condition->sameAs(otherInversion->boolean, true))
					{
						otherInversion->used = true;
					}
				}
			}
		}
	}

	MatchedBooleanList matches;

	if (navigationCandidate)
	{
		matches.join(navigationCandidate->matches);

		// Reset the selectivity/cost prerequisites to account the navigational candidate
		totalSelectivity = navigationCandidate->selectivity;
		totalIndexCost = navigationCandidate->cost;
		previousTotalCost = totalIndexCost + totalSelectivity * streamCardinality;

		if (navigationCandidate->matchedSegments)
			firstCandidate = false;
	}

	for (FB_SIZE_T i = 0; i < inversions.getCount(); i++)
	{
		// Initialize vars before walking through candidates
		InversionCandidate* bestCandidate = nullptr;
		bool restartLoop = false;

		for (const auto currentInv : inversions)
		{
			if (!currentInv->used)
			{
				// If this is a unique full equal matched inversion we're done, so
				// we can make the inversion and return it.
				if (currentInv->unique && currentInv->dependencies && !currentInv->condition)
				{
					if (!invCandidate)
						invCandidate = FB_NEW_POOL(getPool()) InversionCandidate(getPool());

					const auto inversionNode = (!currentInv->inversion && currentInv->scratch) ?
						makeIndexScanNode(currentInv->scratch) : currentInv->inversion;
					invCandidate->inversion = inversionNode;
					invCandidate->dbkeyRanges.assign(currentInv->dbkeyRanges);
					invCandidate->unique = currentInv->unique;
					invCandidate->selectivity = currentInv->selectivity;
					invCandidate->cost = currentInv->cost;
					invCandidate->indexes = currentInv->indexes;
					invCandidate->nonFullMatchedSegments = 0;
					invCandidate->matchedSegments = currentInv->matchedSegments;
					invCandidate->dependencies = currentInv->dependencies;

					for (const auto currentMatch : currentInv->matches)
					{
						if (!invCandidate->matches.exist(currentMatch))
							invCandidate->matches.add(currentMatch);
					}

					if (const auto currentMatch = currentInv->boolean)
					{
						if (!invCandidate->matches.exist(currentMatch))
							invCandidate->matches.add(currentMatch);
					}

					matches.assign(invCandidate->matches);

					if (customPlan)
						continue;

					return invCandidate;
				}

				if (!customPlan)
				{
					// Look if a match is already used by previous matches
					bool anyMatchAlreadyUsed = false, matchUsedByNavigation = false;

					for (const auto currentMatch : currentInv->matches)
					{
						if (matches.exist(currentMatch))
						{
							anyMatchAlreadyUsed = true;

							if (navigationCandidate &&
								navigationCandidate->matches.exist(currentMatch))
							{
								matchUsedByNavigation = true;
							}

							break;
						}
					}

					if (const auto currentMatch = currentInv->boolean)
					{
						if (matches.exist(currentMatch))
						{
							anyMatchAlreadyUsed = true;

							if (navigationCandidate &&
								navigationCandidate->matches.exist(currentMatch))
							{
								matchUsedByNavigation = true;
							}
						}
						else if (matchUsedByNavigation)
							anyMatchAlreadyUsed = false;
					}

					// If some match was already used by another index, skip this index

					if (anyMatchAlreadyUsed)
					{
						if (!matchUsedByNavigation)
						{
							// Add the other matches from this index

							for (const auto currentMatch : currentInv->matches)
							{
								if (!matches.exist(currentMatch))
									matches.add(currentMatch);
							}

							if (const auto currentMatch = currentInv->boolean)
							{
								if (!matches.exist(currentMatch))
									matches.add(currentMatch);
							}
						}

						// Restart loop, because other indexes could also be excluded now
						currentInv->used = true;
						restartLoop = true;
						break;
					}
				}

				if (!bestCandidate)
				{
					// The first candidate
					bestCandidate = currentInv;
				}
				else
				{
					// Prefer unconditional inversions
					if (currentInv->condition)
					{
						currentInv->used = true;
						restartLoop = true;
						break;
					}

					if (bestCandidate->condition)
					{
						bestCandidate = currentInv;
						restartLoop = true;
						break;
					}

					if (betterInversion(currentInv, bestCandidate, false))
						bestCandidate = currentInv;
				}
			}
		}

		if (restartLoop)
			continue;

		// If we have a candidate which is interesting build the inversion
		// else we're done.
		if (bestCandidate)
		{
			// AB: Here we test if our new candidate is interesting enough to be added for
			// index retrieval.

			// AB: For now i'll use the calculation that's often used for and-ing selectivities (S1 * S2).
			// I think this calculation is not right for many cases.
			// For example two "good" selectivities will result in a very good selectivity, but
			// mostly a filter is made by adding criteria's where every criteria is an extra filter
			// compared to the previous one. Thus with the second criteria in _most_ cases still
			// records are returned. (Think also on the segment-selectivity in compound indexes)
			// Assume a table with 100000 records and two selectivities of 0.001 (100 records) which
			// are both AND-ed (with S1 * S2 => 0.001 * 0.001 = 0.000001 => 0.1 record).
			//
			// A better formula could be where the result is between "Sbest" and "Sbest * factor"
			// The reducing factor should be between 0 and 1 (Sbest = best selectivity)
			//
			// Example:
			/*
			double newTotalSelectivity = 0;
			double bestSel = bestCandidate->selectivity;
			double worstSel = totalSelectivity;
			if (bestCandidate->selectivity > totalSelectivity)
			{
				worstSel = bestCandidate->selectivity;
				bestSel = totalSelectivity;
			}

			if (bestSel >= MAXIMUM_SELECTIVITY) {
				newTotalSelectivity = MAXIMUM_SELECTIVITY;
			}
			else if (bestSel == 0) {
				newTotalSelectivity = 0;
			}
			else {
				newTotalSelectivity = bestSel - ((1 - worstSel) * (bestSel - (bestSel * 0.01)));
			}
			*/

			const double newTotalSelectivity = bestCandidate->selectivity * totalSelectivity;
			const double newTotalDataCost = newTotalSelectivity * streamCardinality;
			const double newTotalIndexCost = totalIndexCost + bestCandidate->cost;
			const double totalCost = newTotalDataCost + newTotalIndexCost;

			// Test if the new totalCost will be higher than the previous totalCost
			// and if the current selectivity (without the bestCandidate) is already good enough.
			if (customPlan || sysRequest || smallTable || firstCandidate ||
				(totalCost < previousTotalCost && totalSelectivity > minimumSelectivity))
			{
				// Exclude index from next pass
				bestCandidate->used = true;

				firstCandidate = false;

				previousTotalCost = totalCost;
				totalIndexCost = newTotalIndexCost;
				totalSelectivity = newTotalSelectivity;

				if (!invCandidate)
				{
					invCandidate = FB_NEW_POOL(getPool()) InversionCandidate(getPool());
					const auto inversionNode = (!bestCandidate->inversion && bestCandidate->scratch) ?
						makeIndexScanNode(bestCandidate->scratch) : bestCandidate->inversion;
					invCandidate->inversion = inversionNode;
					invCandidate->dbkeyRanges.assign(bestCandidate->dbkeyRanges);
					invCandidate->unique = bestCandidate->unique;
					invCandidate->selectivity = bestCandidate->selectivity;
					invCandidate->cost = bestCandidate->cost;
					invCandidate->indexes = bestCandidate->indexes;
					invCandidate->nonFullMatchedSegments = 0;
					invCandidate->matchedSegments = bestCandidate->matchedSegments;
					invCandidate->dependencies = bestCandidate->dependencies;
					invCandidate->condition = bestCandidate->condition;

					for (const auto bestMatch : bestCandidate->matches)
					{
						if (!invCandidate->matches.exist(bestMatch))
							invCandidate->matches.add(bestMatch);
					}

					if (const auto bestMatch = bestCandidate->boolean)
					{
						if (!invCandidate->matches.exist(bestMatch))
							invCandidate->matches.add(bestMatch);
					}

					matches.join(invCandidate->matches);
				}
				else if (!bestCandidate->condition)
				{
					const auto inversionNode = (!bestCandidate->inversion && bestCandidate->scratch) ?
						makeIndexScanNode(bestCandidate->scratch) : bestCandidate->inversion;
					invCandidate->inversion = composeInversion(invCandidate->inversion,
						inversionNode, InversionNode::TYPE_AND);
					invCandidate->dbkeyRanges.join(bestCandidate->dbkeyRanges);
					invCandidate->unique = (invCandidate->unique || bestCandidate->unique);
					invCandidate->selectivity = totalSelectivity;
					invCandidate->cost += bestCandidate->cost;
					invCandidate->indexes += bestCandidate->indexes;
					invCandidate->nonFullMatchedSegments = 0;
					invCandidate->matchedSegments =
						MAX(bestCandidate->matchedSegments, invCandidate->matchedSegments);
					invCandidate->dependencies += bestCandidate->dependencies;

					for (const auto bestMatch : bestCandidate->matches)
					{
						if (!invCandidate->matches.exist(bestMatch))
							invCandidate->matches.add(bestMatch);
					}

					if (const auto bestMatch = bestCandidate->boolean)
					{
						if (!invCandidate->matches.exist(bestMatch))
							invCandidate->matches.add(bestMatch);
					}

					matches.join(invCandidate->matches);
				}

				if (invCandidate->unique)
				{
					// Single unique full equal match is enough
					if (!customPlan)
						break;
				}
			}
			else
			{
				// We're done
				break;
			}
		}
		else {
			break;
		}
	}

	// If we have no index used for filtering, but there's a navigational walk,
	// set up the inversion candidate appropriately.

	if (navigationCandidate)
	{
		if (!invCandidate)
			invCandidate = FB_NEW_POOL(getPool()) InversionCandidate(getPool());

		invCandidate->unique = navigationCandidate->unique;
		invCandidate->selectivity *= navigationCandidate->selectivity;
		invCandidate->cost += navigationCandidate->cost;
		++invCandidate->indexes;
		invCandidate->navigated = true;

		for (const auto navMatch : navigationCandidate->matches)
		{
			if (!invCandidate->matches.exist(navMatch))
				invCandidate->matches.add(navMatch);
		}
	}

	return invCandidate;
}

bool Retrieval::matchBoolean(IndexScratch* indexScratch,
							 BoolExprNode* boolean,
							 unsigned scope) const
{
	if (boolean->nodFlags & ExprNode::FLAG_DEOPTIMIZE)
		return false;

	const auto idx = indexScratch->index;

	if (idx->idx_flags & idx_condition)
	{
		// If index condition matches the boolean, this should not be
		// considered a match. Full index scan will be used instead.
		if (idx->idx_condition->sameAs(boolean, true))
			return false;
	}

	const auto cmpNode = nodeAs<ComparativeBoolNode>(boolean);
	const auto missingNode = nodeAs<MissingBoolNode>(boolean);
	const auto listNode = nodeAs<InListBoolNode>(boolean);
	const auto notNode = nodeAs<NotBoolNode>(boolean);
	const auto rseNode = nodeAs<RseBoolNode>(boolean);
	bool forward = true;
	ValueExprNode* value = nullptr;
	ValueExprNode* match = nullptr;
	ValueListNode* list = nullptr;

	if (cmpNode)
	{
		match = cmpNode->arg1;
		value = cmpNode->arg2;
	}
	else if (listNode)
	{
		match = listNode->arg;
		list = listNode->list;

		if (!list->computable(csb, stream, false))
			return false;
	}
	else if (missingNode)
		match = missingNode->arg;
	else
	{
		fb_assert(notNode || rseNode);
		return false;
	}

	ValueExprNode* value2 = (cmpNode && cmpNode->blrOp == blr_between) ?
		cmpNode->arg3 : nullptr;

	if (idx->idx_flags & idx_expression)
	{
		// see if one side or the other is matchable to the index expression

		if (!checkIndexExpression(idx, match) ||
			(value && !value->computable(csb, stream, false)))
		{
			if ((!cmpNode || cmpNode->blrOp != blr_starting) && value &&
				checkIndexExpression(idx, value) &&
				match->computable(csb, stream, false))
			{
				std::swap(match, value);
				forward = false;
			}
			else
				return false;
		}
	}
	else
	{
		// If left side is not a field, swap sides.
		// If left side is still not a field, give up

		FieldNode* fieldNode;

		if (!(fieldNode = nodeAs<FieldNode>(match)) ||
			fieldNode->fieldStream != stream ||
			(value && !value->computable(csb, stream, false)))
		{
			std::swap(match, value);

			if ((!match || !(fieldNode = nodeAs<FieldNode>(match))) ||
				fieldNode->fieldStream != stream ||
				!value->computable(csb, stream, false))
			{
				return false;
			}

			forward = false;
		}
	}

	// check datatypes to ensure that the index scan is guaranteed
	// to deliver correct results

	bool excludeBound = cmpNode && (cmpNode->blrOp == blr_gtr || cmpNode->blrOp == blr_lss);

	dsc matchDesc, valueDesc;

	if (value || list)
	{
		match->getDesc(tdbb, csb, &matchDesc);

		if (value)
			value->getDesc(tdbb, csb, &valueDesc);
		else
			list->getDesc(tdbb, csb, &valueDesc);

		if (!BTR_types_comparable(matchDesc, valueDesc))
			return false;

		if (matchDesc.dsc_dtype == dtype_sql_date &&
			valueDesc.dsc_dtype == dtype_timestamp)
		{
			// for "DATE <op> TIMESTAMP" we need <op> to include the boundary value
			excludeBound = false;
		}
	}

	// match the field to an index, if possible, and save the value to be matched
	// as either the lower or upper bound for retrieval, or both

	const auto fieldNode = nodeAs<FieldNode>(match);

	if (!(idx->idx_flags & idx_expression))
		fb_assert(fieldNode);

	const bool isDesc = (idx->idx_flags & idx_descending);

	fb_assert(indexScratch->segments.getCount() == idx->idx_count);

	for (unsigned i = 0; i < idx->idx_count; i++)
	{
		if (!(idx->idx_flags & idx_expression) &&
			fieldNode->fieldId != idx->idx_rpt[i].idx_field)
		{
			continue;
		}

		const auto segment = &indexScratch->segments[i];

		if (cmpNode)
		{
			switch (cmpNode->blrOp)
			{
				case blr_between:
					if (!forward || !value2->computable(csb, stream, false))
						return false;
					segment->matches.add(boolean);
					// AB: If we have already an exact match don't
					// override it with worser matches.
					if (!((segment->scanType == segmentScanEqual) ||
						(segment->scanType == segmentScanEquivalent) ||
						(segment->scanType == segmentScanList)))
					{
						segment->lowerValue = value;
						segment->upperValue = value2;
						segment->scanType = segmentScanBetween;
						segment->excludeLower = false;
						segment->excludeUpper = false;
					}
					break;

				case blr_equiv:
					segment->matches.add(boolean);
					// AB: If we have already an exact match don't
					// override it with worser matches.
					if (!(segment->scanType == segmentScanEqual))
					{
						segment->lowerValue = segment->upperValue = value;
						segment->scanType = segmentScanEquivalent;
						segment->excludeLower = false;
						segment->excludeUpper = false;
					}
					break;

				case blr_eql:
					segment->matches.add(boolean);
					segment->lowerValue = segment->upperValue = value;
					segment->scanType = segmentScanEqual;
					segment->excludeLower = false;
					segment->excludeUpper = false;
					break;

				// ASF: Make "NOT boolean" work with indices.
				case blr_neq:
				{
					if (valueDesc.dsc_dtype != dtype_boolean)
						return false;

					// Invert the value and use it with "segmentScanEqual" for the index lookup
					segment->matches.add(boolean);
					segment->lowerValue = segment->upperValue =
						invertBoolValue(csb, value);
					segment->scanType = segmentScanEqual;
					segment->excludeLower = false;
					segment->excludeUpper = false;
					break;
				}

				case blr_gtr:
				case blr_geq:
					segment->matches.add(boolean);
					if (!((segment->scanType == segmentScanEqual) ||
						(segment->scanType == segmentScanEquivalent) ||
						(segment->scanType == segmentScanBetween) ||
						(segment->scanType == segmentScanList)))
					{
						if (forward != isDesc) // (forward && !isDesc || !forward && isDesc)
							segment->excludeLower = excludeBound;
						else
							segment->excludeUpper = excludeBound;

						if (forward)
						{
							segment->lowerValue = value;
							if (segment->scanType == segmentScanLess)
								segment->scanType = segmentScanBetween;
							else
								segment->scanType = segmentScanGreater;
						}
						else
						{
							segment->upperValue = value;
							if (segment->scanType == segmentScanGreater)
								segment->scanType = segmentScanBetween;
							else
								segment->scanType = segmentScanLess;
						}
					}
					break;

				case blr_lss:
				case blr_leq:
					segment->matches.add(boolean);
					if (!((segment->scanType == segmentScanEqual) ||
						(segment->scanType == segmentScanEquivalent) ||
						(segment->scanType == segmentScanBetween) ||
						(segment->scanType == segmentScanList)))
					{
						if (forward != isDesc)
							segment->excludeUpper = excludeBound;
						else
							segment->excludeLower = excludeBound;

						if (forward)
						{
							segment->upperValue = value;
							if (segment->scanType == segmentScanGreater)
								segment->scanType = segmentScanBetween;
							else
								segment->scanType = segmentScanLess;
						}
						else
						{
							segment->lowerValue = value;
							if (segment->scanType == segmentScanLess)
								segment->scanType = segmentScanBetween;
							else
								segment->scanType = segmentScanGreater;
						}
					}
					break;

				case blr_starting:
					// Check if validate for using index
					if (!forward || !validateStarts(indexScratch, cmpNode, i))
						return false;
					segment->matches.add(boolean);
					if (!((segment->scanType == segmentScanEqual) ||
						(segment->scanType == segmentScanEquivalent)))
					{
						segment->lowerValue = segment->upperValue = value;
						segment->scanType = segmentScanStarting;
						segment->excludeLower = false;
						segment->excludeUpper = false;
					}
					break;

				default:
					return false;
			}
		}
		else if (listNode)
		{
			segment->matches.add(boolean);
			if (!((segment->scanType == segmentScanEqual) ||
				(segment->scanType == segmentScanEquivalent)))
			{
				fb_assert(listNode->lookup);

				segment->lowerValue = segment->upperValue = nullptr;
				segment->valueList = listNode->lookup;
				segment->scanType = segmentScanList;
				segment->excludeLower = false;
				segment->excludeUpper = false;
			}
		}
		else if (missingNode)
		{
			segment->matches.add(boolean);
			if (!((segment->scanType == segmentScanEqual) ||
				(segment->scanType == segmentScanEquivalent)))
			{
				segment->lowerValue = segment->upperValue = nullptr;
				segment->scanType = segmentScanMissing;
				segment->excludeLower = false;
				segment->excludeUpper = false;
			}
		}
		else
		{
			fb_assert(false);
			return false;
		}

		// Scale for big exact numerics
		if (!missingNode)
		{
			switch (matchDesc.dsc_dtype)
			{
			case dtype_int64:
			case dtype_int128:
				segment->scale = matchDesc.dsc_scale;
				break;
			}
		}

		// A match could be made
		if (segment->scope < scope)
			segment->scope = scope;

		if (i == 0)
		{
			// If this is the first segment, then this index is a candidate.
			indexScratch->candidate = true;
		}

		return true;
	}

	return false;
}

//
// Check whether a boolean is a DB_KEY based comparison
//

InversionCandidate* Retrieval::matchDbKey(BoolExprNode* boolean) const
{
	// If this isn't an equality, it isn't even interesting

	const auto cmpNode = nodeAs<ComparativeBoolNode>(boolean);
	const auto listNode = nodeAs<InListBoolNode>(boolean);

	if (cmpNode)
	{
		switch (cmpNode->blrOp)
		{
			case blr_equiv:
			case blr_eql:
			case blr_gtr:
			case blr_geq:
			case blr_lss:
			case blr_leq:
			case blr_between:
				break;

			default:
				return nullptr;
		}
	}
	else if (!listNode)
		return nullptr;

	// Find the side of the equality that is potentially a dbkey.
	// If neither, make the obvious deduction.

	SLONG n = 0;
	int dbkeyArg = 1;
	ValueExprNode* dbkey = nullptr;

	if (cmpNode)
	{
		dbkey = findDbKey(cmpNode->arg1, &n);

		if (!dbkey)
		{
			n = 0;
			dbkeyArg = 2;
			dbkey = findDbKey(cmpNode->arg2, &n);
		}

		if (!dbkey && (cmpNode->blrOp == blr_between))
		{
			n = 0;
			dbkeyArg = 3;
			dbkey = findDbKey(cmpNode->arg3, &n);
		}
	}
	else
	{
		fb_assert(listNode);
		dbkey = findDbKey(listNode->arg, &n);
	}

	if (!dbkey)
		return nullptr;

	// Make sure we have the correct stream

	const auto keyNode = nodeAs<RecordKeyNode>(dbkey);

	if (!keyNode || keyNode->blrOp != blr_dbkey || keyNode->recStream != stream)
		return nullptr;

	// If this is a dbkey for the appropriate stream, it's invertable

	const double cardinality = csb->csb_rpt[stream].csb_cardinality;

	bool unique = false;
	double selectivity = 0;

	ValueExprNode* lower = nullptr;
	ValueExprNode* upper = nullptr;
	ValueListNode* list = listNode ? listNode->list : nullptr;

	if (cmpNode)
	{
		switch (cmpNode->blrOp)
		{
		case blr_eql:
		case blr_equiv:
			unique = true;
			selectivity = 1 / cardinality;
			lower = upper = (dbkeyArg == 1) ? cmpNode->arg2 : cmpNode->arg1;
			break;

		case blr_gtr:
		case blr_geq:
			selectivity = REDUCE_SELECTIVITY_FACTOR_GREATER;
			if (dbkeyArg == 1)
				lower = cmpNode->arg2;	// dbkey > arg2
			else
				upper = cmpNode->arg1;	// arg1 < dbkey
			break;

		case blr_lss:
		case blr_leq:
			selectivity = REDUCE_SELECTIVITY_FACTOR_LESS;
			if (dbkeyArg == 1)
				upper = cmpNode->arg2;	// dbkey < arg2
			else
				lower = cmpNode->arg1;	// arg1 < dbkey
			break;

		case blr_between:
			if (dbkeyArg == 1)			// dbkey between arg2 and arg3
			{
				selectivity = REDUCE_SELECTIVITY_FACTOR_BETWEEN;
				lower = cmpNode->arg2;
				upper = cmpNode->arg3;
			}
			else if (dbkeyArg == 2)		// arg1 between dbkey and arg3, or dbkey <= arg1 and arg1 <= arg3
			{
				selectivity = REDUCE_SELECTIVITY_FACTOR_LESS;
				upper = cmpNode->arg1;
			}
			else if (dbkeyArg == 3)		// arg1 between arg2 and dbkey, or arg2 <= arg1 and arg1 <= dbkey
			{
				selectivity = REDUCE_SELECTIVITY_FACTOR_GREATER;
				lower = cmpNode->arg1;
			}
			break;

		default:
			fb_assert(false);
		}
	}
	else
	{
		selectivity = list->items.getCount() / cardinality;
	}

	if (lower && !lower->computable(csb, stream, false))
		return nullptr;

	if (upper && !upper->computable(csb, stream, false))
		return nullptr;

	if (list && !list->computable(csb, stream, false))
		return nullptr;

	const auto invCandidate = FB_NEW_POOL(getPool()) InversionCandidate(getPool());
	invCandidate->unique = unique;
	invCandidate->selectivity = selectivity;
	invCandidate->cost = 0;
	invCandidate->matches.add(boolean);
	boolean->findDependentFromStreams(csb, stream, &invCandidate->dependentFromStreams);
	invCandidate->dependencies = invCandidate->dependentFromStreams.getCount();

	if (createIndexScanNodes)
	{
		if (list)
		{
			InversionNode* listInversion = nullptr;

			for (auto value : list->items)
			{
				const auto inversion = FB_NEW_POOL(getPool()) InversionNode(value, n);
				inversion->impure = csb->allocImpure<impure_inversion>();
				listInversion = composeInversion(listInversion, inversion, InversionNode::TYPE_OR);
			}

			invCandidate->inversion = listInversion;
		}
		else if (unique)
		{
			fb_assert(lower == upper);

			const auto inversion = FB_NEW_POOL(getPool()) InversionNode(lower, n);
			inversion->impure = csb->allocImpure<impure_inversion>();
			invCandidate->inversion = inversion;
		}
		else
		{
			fb_assert(n == 0);
			fb_assert(lower || upper);

			const auto dbkeyRange = FB_NEW_POOL(getPool()) DbKeyRangeNode(lower, upper);
			invCandidate->dbkeyRanges.add(dbkeyRange);
		}
	}

	return invCandidate;
}

//
// Try to match boolean on every index.
// If the boolean is an "OR" node then a inversion candidate could be returned.
//

InversionCandidate* Retrieval::matchOnIndexes(IndexScratchList& inputIndexScratches,
											  BoolExprNode* boolean,
											  unsigned scope) const
{
	const auto binaryNode = nodeAs<BinaryBoolNode>(boolean);

	// Handle the "OR" case up front
	if (binaryNode && binaryNode->blrOp == blr_or)
	{
		InversionCandidateList inversions;

		// Make list for index matches

		// Copy information from caller
		IndexScratchList indexOrScratches(inputIndexScratches);

		// We use a scope variable to see on how
		// deep we are in a nested or conjunction.
		scope++;

		auto invCandidate1 = matchOnIndexes(indexOrScratches, binaryNode->arg1, scope);

		if (invCandidate1)
			inversions.add(invCandidate1);

		auto childBoolNode = nodeAs<BinaryBoolNode>(binaryNode->arg1);

		// Get usable inversions based on indexOrScratches and scope
		if (!childBoolNode || childBoolNode->blrOp != blr_or)
			getInversionCandidates(inversions, indexOrScratches, scope);

		invCandidate1 = makeInversion(inversions);

		// Copy information from caller
		indexOrScratches = inputIndexScratches;

		// Clear inversion list
		inversions.clear();

		auto invCandidate2 = matchOnIndexes(indexOrScratches, binaryNode->arg2, scope);

		if (invCandidate2)
			inversions.add(invCandidate2);

		childBoolNode = nodeAs<BinaryBoolNode>(binaryNode->arg2);

		// Make inversion based on indexOrScratches and scope
		if (!childBoolNode || childBoolNode->blrOp != blr_or)
			getInversionCandidates(inversions, indexOrScratches, scope);

		invCandidate2 = makeInversion(inversions);

		if (invCandidate1 && invCandidate2 &&
			(invCandidate1->indexes || invCandidate1->unique) &&
			(invCandidate2->indexes || invCandidate2->unique))
		{
			const auto invCandidate = FB_NEW_POOL(getPool()) InversionCandidate(getPool());
			invCandidate->inversion = composeInversion(invCandidate1->inversion,
				invCandidate2->inversion, InversionNode::TYPE_OR);
			invCandidate->selectivity = invCandidate1->selectivity + invCandidate2->selectivity -
				invCandidate1->selectivity * invCandidate2->selectivity;
			invCandidate->cost = invCandidate1->cost + invCandidate2->cost;
			invCandidate->indexes = invCandidate1->indexes + invCandidate2->indexes;
			invCandidate->nonFullMatchedSegments = 0;
			invCandidate->matchedSegments =
				MIN(invCandidate1->matchedSegments, invCandidate2->matchedSegments);
			invCandidate->dependencies = invCandidate1->dependencies + invCandidate2->dependencies;

			if (invCandidate1->condition && invCandidate2->condition)
			{
				const auto newNode = FB_NEW_POOL(getPool()) BinaryBoolNode(getPool(), blr_or);
				newNode->arg1 = invCandidate1->condition;
				newNode->arg2 = invCandidate2->condition;
				invCandidate->condition = newNode;
			}
			else if (invCandidate1->condition)
			{
				invCandidate->condition = invCandidate1->condition;
			}
			else if (invCandidate2->condition)
			{
				invCandidate->condition = invCandidate2->condition;
			}

			// Add matches conjunctions that exists in both left and right inversion
			if (invCandidate1->matches.hasData() && invCandidate2->matches.hasData())
			{
				SortedArray<BoolExprNode*> matches;

				for (const auto match : invCandidate1->matches)
					matches.add(match);

				for (const auto match : invCandidate2->matches)
				{
					if (matches.exist(match))
						invCandidate->matches.add(match);
				}
			}

			return invCandidate;
		}

		if (invCandidate1)
		{
			BoolExprNode* condition = binaryNode->arg2;

			if (condition->computable(csb, stream, false))
			{
				if (invCandidate1->condition)
				{
					const auto newNode = FB_NEW_POOL(getPool()) BinaryBoolNode(getPool(), blr_or);
					newNode->arg1 = invCandidate1->condition;
					newNode->arg2 = condition;
					condition = newNode;
				}

				invCandidate1->condition = condition;
				return invCandidate1;
			}
		}

		if (invCandidate2)
		{
			BoolExprNode* condition = binaryNode->arg1;

			if (condition->computable(csb, stream, false))
			{
				if (invCandidate2->condition)
				{
					const auto newNode = FB_NEW_POOL(getPool()) BinaryBoolNode(getPool(), blr_or);
					newNode->arg1 = invCandidate2->condition;
					newNode->arg2 = condition;
					condition = newNode;
				}

				invCandidate2->condition = condition;
				return invCandidate2;
			}
		}

		return nullptr;
	}

	if (binaryNode && binaryNode->blrOp == blr_and)
	{
		// Recursively call this procedure for every boolean
		// and finally get candidate inversions.
		// Normally we come here from within a OR conjunction.
		InversionCandidateList inversions;

		InversionCandidate* invCandidate = matchOnIndexes(
			inputIndexScratches, binaryNode->arg1, scope);

		if (invCandidate)
			inversions.add(invCandidate);

		invCandidate = matchOnIndexes(inputIndexScratches, binaryNode->arg2, scope);

		if (invCandidate)
			inversions.add(invCandidate);

		return makeInversion(inversions);
	}

	// Check for DB_KEY comparison
	InversionCandidate* const invCandidate = matchDbKey(boolean);

	// Walk through indexes
	for (auto& indexScratch : inputIndexScratches)
	{
		// Try to match the boolean against a index.
		if (!(indexScratch.index->idx_runtime_flags & idx_plan_dont_use) ||
			(indexScratch.index->idx_runtime_flags & idx_plan_navigate))
		{
			matchBoolean(&indexScratch, boolean, scope);
		}
	}

	return invCandidate;
}


//
// Check if the boolean is valid for using it against the given index segment
//

bool Retrieval::validateStarts(IndexScratch* indexScratch,
							   ComparativeBoolNode* cmpNode,
							   unsigned segment) const
{
	fb_assert(cmpNode && cmpNode->blrOp == blr_starting);
	if (!cmpNode || cmpNode->blrOp != blr_starting)
		return false;

	ValueExprNode* field = cmpNode->arg1;
	ValueExprNode* value = cmpNode->arg2;

	const auto idx = indexScratch->index;

	if (idx->idx_flags & idx_expression)
	{
		// AB: What if the expression contains a number/float etc.. and
		// we use starting with against it? Is that allowed?

		if (!(checkIndexExpression(idx, field) ||
			(value && !value->computable(csb, stream, false))))
		{
			// AB: Can we swap de left and right sides by a starting with?
			// X STARTING WITH 'a' that is never the same as 'a' STARTING WITH X
			if (value &&
				checkIndexExpression(idx, value) &&
				field->computable(csb, stream, false))
			{
				field = value;
				value = cmpNode->arg1;
			}
			else
				return false;
		}
	}
	else
	{
		const auto fieldNode = nodeAs<FieldNode>(field);

		if (!fieldNode)
		{
			// dimitr:	any idea how we can use an index in this case?
			//			The code below produced wrong results.
			// AB: I don't think that it would be effective, because
			// this must include many matches (think about empty string)
			return false;
			/*
			if (!nodeIs<FieldNode>(value))
				return nullptr;
			field = value;
			value = cmpNode->arg1;
			*/
		}

		// Every string starts with an empty string so don't bother using an index in that case.
		if (const auto literal = nodeAs<LiteralNode>(value))
		{
			if ((literal->litDesc.dsc_dtype == dtype_text && literal->litDesc.dsc_length == 0) ||
				(literal->litDesc.dsc_dtype == dtype_varying &&
					literal->litDesc.dsc_length == sizeof(USHORT)))
			{
				return false;
			}
		}

		// AB: Check if the index-segment is usable for using starts.
		// Thus it should be of type string, etc...
		if (fieldNode->fieldStream != stream ||
			fieldNode->fieldId != idx->idx_rpt[segment].idx_field ||
			!(idx->idx_rpt[segment].idx_itype == idx_string ||
				idx->idx_rpt[segment].idx_itype == idx_byte_array ||
				idx->idx_rpt[segment].idx_itype == idx_metadata ||
				idx->idx_rpt[segment].idx_itype >= idx_first_intl_string) ||
			!value->computable(csb, stream, false))
		{
			return false;
		}
	}

	return true;
}


#ifdef OPT_DEBUG_RETRIEVAL
void Retrieval::printCandidate(const InversionCandidate* candidate) const
{
	optimizer->printf("    cost (%1.2f), selectivity (%1.10f), indexes (%d), matched (%d, %d)",
		candidate->cost, candidate->selectivity, candidate->indexes, candidate->matchedSegments,
		candidate->nonFullMatchedSegments);

	if (candidate->unique)
		optimizer->printf(", unique");

	if (candidate->dependentFromStreams.hasData())
	{
		optimizer->printf(", dependent from streams:");

		const auto end = candidate->dependentFromStreams.end();
		for (auto iter = candidate->dependentFromStreams.begin(); iter != end; iter++)
		{
			const auto name = optimizer->getStreamName(*iter);

			if (name.hasData())
				optimizer->printf(" %u (%s)", *iter, name.c_str());
			else
				optimizer->printf(" %u", *iter);

			if (iter != end - 1)
				optimizer->printf(",");
		}
	}

	optimizer->printf("\n");
}

void Retrieval::printCandidates(const InversionCandidateList& inversions) const
{
	if (inversions.getCount() < 2)
		return;

	const auto name = optimizer->getStreamName(stream);
	optimizer->printf("  retrieval candidates for stream %u (%s):\n",
					  stream, name.c_str());

	for (const auto candidate : inversions)
		printCandidate(candidate);
}

void Retrieval::printFinalCandidate(const InversionCandidate* candidate) const
{
	if (!candidate)
		return;

	const auto name = optimizer->getStreamName(stream);
	optimizer->printf("  final candidate for stream %u (%s):\n",
					  stream, name.c_str());

	printCandidate(candidate);
}
#endif
