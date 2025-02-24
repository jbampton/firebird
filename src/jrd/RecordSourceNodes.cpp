/*
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 * Adriano dos Santos Fernandes
 */

#include "firebird.h"
#include "../jrd/align.h"
#include "../jrd/RecordSourceNodes.h"
#include "../jrd/DataTypeUtil.h"
#include "../jrd/recsrc/RecordSource.h"
#include "../dsql/BoolNodes.h"
#include "../dsql/ExprNodes.h"
#include "../dsql/StmtNodes.h"
#include "../dsql/dsql.h"
#include "../jrd/btr_proto.h"
#include "../jrd/cmp_proto.h"
#include "../common/dsc_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/par_proto.h"
#include "../dsql/ddl_proto.h"
#include "../dsql/gen_proto.h"
#include "../dsql/metd_proto.h"
#include "../dsql/pass1_proto.h"
#include "../jrd/optimizer/Optimizer.h"

using namespace Firebird;
using namespace Jrd;


static RecordSourceNode* dsqlPassRelProc(DsqlCompilerScratch* dsqlScratch, RecordSourceNode* source);
static MapNode* parseMap(thread_db* tdbb, CompilerScratch* csb, StreamType stream, bool parseHeader);
static void processSource(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	RecordSourceNode* source, BoolExprNode** boolean, RecordSourceNodeStack& stack);
static void processMap(thread_db* tdbb, CompilerScratch* csb, MapNode* map, Format** inputFormat);
static void genDeliverUnmapped(CompilerScratch* csb, const BoolExprNodeStack& parentStack,
	BoolExprNodeStack& deliverStack, MapNode* map, StreamType shellStream);
static ValueExprNode* resolveUsingField(DsqlCompilerScratch* dsqlScratch, const MetaName& name,
	ValueListNode* list, const FieldNode* flawedNode, const TEXT* side, dsql_ctx*& ctx);

namespace
{
	class AutoActivateResetStreams : public AutoStorage
	{
	public:
		AutoActivateResetStreams(CompilerScratch* csb, const RseNode* rse)
			: m_csb(csb), m_streams(getPool()), m_flags(getPool())
		{
			rse->computeRseStreams(m_streams);

			m_flags.resize(m_streams.getCount());

			FB_SIZE_T pos = 0;
			for (const auto stream : m_streams)
			{
				m_flags[pos++] = m_csb->csb_rpt[stream].csb_flags;
				m_csb->csb_rpt[stream].csb_flags |= (csb_active | csb_sub_stream);
			}
		}

		~AutoActivateResetStreams()
		{
			FB_SIZE_T pos = 0;
			for (const auto stream : m_streams)
				m_csb->csb_rpt[stream].csb_flags = m_flags[pos++];
		}

	private:
		CompilerScratch* m_csb;
		StreamList m_streams;
		HalfStaticArray<USHORT, OPT_STATIC_ITEMS> m_flags;
	};
}

//--------------------


string RecordSourceNode::internalPrint(NodePrinter& printer) const
{
	ExprNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlFlags);
	NODE_PRINT(printer, dsqlContext);
	NODE_PRINT(printer, stream);

	return "RecordSourceNode";
}


//--------------------


SortNode* SortNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	SortNode* newSort = FB_NEW_POOL(*tdbb->getDefaultPool()) SortNode(*tdbb->getDefaultPool());
	newSort->unique = unique;

	for (const NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
		newSort->expressions.add(copier.copy(tdbb, *i));

	newSort->direction = direction;
	newSort->nullOrder = nullOrder;

	return newSort;
}

SortNode* SortNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
		DmlNode::doPass1(tdbb, csb, i->getAddress());

	return this;
}

SortNode* SortNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
		ExprNode::doPass2(tdbb, csb, i->getAddress());

	return this;
}

bool SortNode::computable(CompilerScratch* csb, StreamType stream, bool allowOnlyCurrentStream)
{
	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
	{
		if (!(*i)->computable(csb, stream, allowOnlyCurrentStream))
			return false;
	}

	return true;
}

void SortNode::findDependentFromStreams(const CompilerScratch* csb,
	StreamType currentStream, SortedStreamList* streamList)
{
	for (NestConst<ValueExprNode>* i = expressions.begin(); i != expressions.end(); ++i)
		(*i)->findDependentFromStreams(csb, currentStream, streamList);
}


//--------------------


MapNode* MapNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	MapNode* newMap = FB_NEW_POOL(*tdbb->getDefaultPool()) MapNode(*tdbb->getDefaultPool());

	const NestConst<ValueExprNode>* target = targetList.begin();

	for (const NestConst<ValueExprNode>* source = sourceList.begin();
		 source != sourceList.end();
		 ++source, ++target)
	{
		newMap->sourceList.add(copier.copy(tdbb, *source));
		newMap->targetList.add(copier.copy(tdbb, *target));
	}

	return newMap;
}

MapNode* MapNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	NestConst<ValueExprNode>* target = targetList.begin();

	for (NestConst<ValueExprNode>* source = sourceList.begin();
		 source != sourceList.end();
		 ++source, ++target)
	{
		DmlNode::doPass1(tdbb, csb, source->getAddress());
		DmlNode::doPass1(tdbb, csb, target->getAddress());
	}

	return this;
}

MapNode* MapNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	NestConst<ValueExprNode>* target = targetList.begin();

	for (NestConst<ValueExprNode>* source = sourceList.begin();
		 source != sourceList.end();
		 ++source, ++target)
	{
		ExprNode::doPass2(tdbb, csb, source->getAddress());
		ExprNode::doPass2(tdbb, csb, target->getAddress());
	}

	return this;
}


//--------------------


PlanNode* PlanNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	MemoryPool& pool = dsqlScratch->getPool();

	PlanNode* node = FB_NEW_POOL(pool) PlanNode(pool, type);

	if (accessType)
	{
		node->accessType = FB_NEW_POOL(pool) AccessType(pool, accessType->type);
		node->accessType->items = accessType->items;
	}

	node->recordSourceNode = recordSourceNode;

	for (NestConst<PlanNode>* i = subNodes.begin(); i != subNodes.end(); ++i)
		node->subNodes.add((*i)->dsqlPass(dsqlScratch));

	if (dsqlNames)
	{
		node->dsqlNames = FB_NEW_POOL(pool) ObjectsArray<QualifiedName>(pool);
		*node->dsqlNames = *dsqlNames;

		dsql_ctx* context = dsqlPassAliasList(dsqlScratch);

		if (context->ctx_relation)
		{
			RelationSourceNode* relNode = FB_NEW_POOL(pool) RelationSourceNode(pool);
			relNode->dsqlContext = context;
			node->recordSourceNode = relNode;
		}
		else if (context->ctx_procedure)
		{
			ProcedureSourceNode* procNode = FB_NEW_POOL(pool) ProcedureSourceNode(pool);
			procNode->dsqlContext = context;
			node->recordSourceNode = procNode;
		}
		//// TODO: LocalTableSourceNode

		// ASF: I think it's a error to let node->recordSourceNode be NULL here, but it happens
		// at least since v2.5. See gen.cpp/gen_plan for more information.
		///fb_assert(node->recordSourceNode);
	}

	if (node->recordSourceNode)
		node->recordSourceNode->dsqlFlags |= RecordSourceNode::DFLAG_PLAN_ITEM;

	return node;
}

// The passed alias list fully specifies a relation. The first alias represents a relation
// specified in the from list at this scope levels. Subsequent contexts, if there are any,
// represent base relations in a view stack. They are used to fully specify a base relation of
// a view. The aliases used in the view stack are those used in the view definition.
dsql_ctx* PlanNode::dsqlPassAliasList(DsqlCompilerScratch* dsqlScratch)
{
	DEV_BLKCHK(dsqlScratch, dsql_type_req);

	auto arg = dsqlNames->begin();
	const auto end = dsqlNames->end();

	// Loop through every alias and find the context for that alias.
	// All aliases should have a corresponding context.
	int aliasCount = dsqlNames->getCount();
	USHORT savedScopeLevel = dsqlScratch->scopeLevel;
	dsql_ctx* context = NULL;
	while (aliasCount > 0)
	{
		if (context)
		{
			if (context->ctx_rse && !context->ctx_relation && !context->ctx_procedure)
			{
				// Derived table
				dsqlScratch->scopeLevel++;
				context = dsqlPassAlias(dsqlScratch, context->ctx_childs_derived_table, *arg);
			}
			else if (context->ctx_relation)
			{
				// This must be a VIEW
				const auto startArg = arg;
				dsql_rel* viewRelation = context->ctx_relation;

				dsql_rel* relation = nullptr;
				dsql_prc* procedure = nullptr;

				// find the base table using the specified alias list, skipping the first one
				// since we already matched it to the context.
				for (; arg != end; ++arg)
				{
					if (!METD_get_view_relation(dsqlScratch->getTransaction(),
							dsqlScratch, viewRelation->rel_name, *arg, relation, procedure))
					{
						break;
					};

					--aliasCount;

					if (!relation)
						break;
				}

				// Found base relation
				if (aliasCount == 0 && (relation || procedure))
				{
					// AB: Pretty ugly huh?
					// make up a dummy context to hold the resultant relation.
					dsql_ctx* newContext = FB_NEW_POOL(dsqlScratch->getPool()) dsql_ctx(dsqlScratch->getPool());
					newContext->ctx_context = context->ctx_context;
					newContext->ctx_relation = relation;
					newContext->ctx_procedure = procedure;

					// Concatenate all the contexts to form the alias name.
					auto aliasArg = startArg;

					for (aliasArg = startArg; aliasArg != end; ++aliasArg)
						newContext->ctx_alias.add(*aliasArg);

					context = newContext;
				}
				else
					context = NULL;
			}
			else
				context = NULL;
		}
		else
			context = dsqlPassAlias(dsqlScratch, *dsqlScratch->context, *arg);

		if (!context)
			break;

		++arg;
		--aliasCount;
	}

	dsqlScratch->scopeLevel = savedScopeLevel;

	if (!context)
	{
		// there is no alias or table named %s at this scope level.
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  Arg::Gds(isc_dsql_command_err) <<
				  Arg::Gds(isc_dsql_no_relation_alias) << arg->toQuotedString());
	}

	return context;
}

// The passed relation or alias represents a context which was previously specified in the from
// list. Find and return the proper context.
dsql_ctx* PlanNode::dsqlPassAlias(DsqlCompilerScratch* dsqlScratch, DsqlContextStack& stack,
	const QualifiedName& alias)
{
	dsql_ctx* result_context = nullptr;

	DEV_BLKCHK(dsqlScratch, dsql_type_req);

	// look through all contexts at this scope level
	// to find one that has a relation name or alias
	// name which matches the identifier passed.
	for (DsqlContextStack::iterator itr(stack); itr.hasData(); ++itr)
	{
		dsql_ctx* context = itr.object();
		if (context->ctx_scope_level != dsqlScratch->scopeLevel)
			continue;

		// check for matching alias.
		if (context->ctx_internal_alias.object.hasData())
		{
			if (context->ctx_internal_alias == alias)
				return context;

			continue;
		}

		// If an unnamed derived table and empty alias.
		if (context->ctx_rse && !context->ctx_relation && !context->ctx_procedure && alias.object.isEmpty())
			result_context = context;

		// Check for matching relation name; aliases take priority so
		// save the context in case there is an alias of the same name.
		// Also to check that there is no self-join in the query.
		if ((context->ctx_relation && PASS1_compare_alias(context->ctx_relation->rel_name, alias)) ||
			(context->ctx_procedure && PASS1_compare_alias(context->ctx_procedure->prc_name, alias)))
		{
			if (result_context)
			{
				// the table %s is referenced twice; use aliases to differentiate
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
						  Arg::Gds(isc_dsql_command_err) <<
						  Arg::Gds(isc_dsql_self_join) << alias.toQuotedString());
			}

			result_context = context;
		}
	}

	return result_context;
}


//--------------------


RecSourceListNode::RecSourceListNode(MemoryPool& pool, unsigned count)
	: TypedNode<ListExprNode, ExprNode::TYPE_REC_SOURCE_LIST>(pool),
	  items(pool)
{
	items.resize(count);

	for (unsigned i = 0; i < count; ++i)
		items[i] = NULL;
}

RecSourceListNode::RecSourceListNode(MemoryPool& pool, RecordSourceNode* arg1)
	: TypedNode<ListExprNode, ExprNode::TYPE_REC_SOURCE_LIST>(pool),
	  items(pool)
{
	items.push(arg1);
}

RecSourceListNode* RecSourceListNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	RecSourceListNode* node = FB_NEW_POOL(dsqlScratch->getPool()) RecSourceListNode(dsqlScratch->getPool(),
		items.getCount());

	NestConst<RecordSourceNode>* dst = node->items.begin();

	for (NestConst<RecordSourceNode>* src = items.begin(); src != items.end(); ++src, ++dst)
		*dst = doDsqlPass(dsqlScratch, *src);

	return node;
}


//--------------------


// Parse a local table reference.
LocalTableSourceNode* LocalTableSourceNode::parse(thread_db* tdbb, CompilerScratch* csb,
	const SSHORT blrOp, bool parseContext)
{
	const USHORT tableNumber = csb->csb_blr_reader.getWord();

	if (tableNumber >= csb->csb_localTables.getCount())
		PAR_error(csb, Arg::Gds(isc_bad_loctab_num) << Arg::Num(tableNumber));

	// Make a relation reference node

	const auto node = FB_NEW_POOL(*tdbb->getDefaultPool()) LocalTableSourceNode(
		*tdbb->getDefaultPool());

	node->tableNumber = tableNumber;

	AutoPtr<string> aliasString(FB_NEW_POOL(csb->csb_pool) string(csb->csb_pool));
	csb->csb_blr_reader.getString(*aliasString);

	if (aliasString->hasData())
		node->alias = *aliasString;
	else
		aliasString.reset();

	// generate a stream for the relation reference, assuming it is a real reference

	if (parseContext)
	{
		node->stream = PAR_context(csb, &node->context);

		if (tableNumber >= csb->csb_localTables.getCount() || !csb->csb_localTables[tableNumber])
			PAR_error(csb, Arg::Gds(isc_bad_loctab_num) << Arg::Num(tableNumber));

		csb->csb_rpt[node->stream].csb_format = csb->csb_localTables[tableNumber]->format;
		csb->csb_rpt[node->stream].csb_alias = aliasString.release();
	}

	return node;
}

string LocalTableSourceNode::internalPrint(NodePrinter& printer) const
{
	RecordSourceNode::internalPrint(printer);

	NODE_PRINT(printer, alias);
	NODE_PRINT(printer, tableNumber);
	NODE_PRINT(printer, context);

	return "LocalTableSourceNode";
}

RecordSourceNode* LocalTableSourceNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return dsqlPassRelProc(dsqlScratch, this);
}

bool LocalTableSourceNode::dsqlMatch(DsqlCompilerScratch* /*dsqlScratch*/, const ExprNode* other,
	bool /*ignoreMapCast*/) const
{
	const auto o = nodeAs<LocalTableSourceNode>(other);
	return o && dsqlContext == o->dsqlContext;
}

void LocalTableSourceNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar(blr_local_table_id);
	dsqlScratch->appendUShort(tableNumber);
	dsqlScratch->appendMetaString(alias.c_str());	// dsqlContext->ctx_alias?

	GEN_stuff_context(dsqlScratch, dsqlContext);
}

LocalTableSourceNode* LocalTableSourceNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	if (!copier.remap)
		BUGCHECK(221);	// msg 221 (CMP) copy: cannot remap

	const auto newSource = FB_NEW_POOL(*tdbb->getDefaultPool()) LocalTableSourceNode(
		*tdbb->getDefaultPool());

	newSource->stream = copier.csb->nextStream();
	copier.remap[stream] = newSource->stream;

	newSource->context = context;

	if (tableNumber >= copier.csb->csb_localTables.getCount() || !copier.csb->csb_localTables[tableNumber])
		ERR_post(Arg::Gds(isc_bad_loctab_num) << Arg::Num(tableNumber));

	const auto element = CMP_csb_element(copier.csb, newSource->stream);

	element->csb_format = copier.csb->csb_localTables[tableNumber]->format;
	element->csb_view_stream = copier.remap[0];

	if (alias.hasData())
	{
		element->csb_alias = FB_NEW_POOL(*tdbb->getDefaultPool())
			string(*tdbb->getDefaultPool(), alias);
	}

	return newSource;
}

void LocalTableSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* /*rse*/,
	BoolExprNode** /*boolean*/, RecordSourceNodeStack& stack)
{
	fb_assert(!csb->csb_view);	// local tables cannot be inside a view

	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	pass1(tdbb, csb);
}

void LocalTableSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	csb->csb_rpt[stream].activate();

	pass2(tdbb, csb);
}

RecordSource* LocalTableSourceNode::compile(thread_db* tdbb, Optimizer* opt, bool /*innerSubStream*/)
{
	const auto csb = opt->getCompilerScratch();

	if (tableNumber >= csb->csb_localTables.getCount() || !csb->csb_localTables[tableNumber])
		ERR_post(Arg::Gds(isc_bad_loctab_num) << Arg::Num(tableNumber));

	auto localTable = csb->csb_localTables[tableNumber];

	return FB_NEW_POOL(*tdbb->getDefaultPool()) LocalTableStream(csb, stream, localTable);
}


//--------------------


// Parse a relation reference.
RelationSourceNode* RelationSourceNode::parse(thread_db* tdbb, CompilerScratch* csb,
	const SSHORT blrOp, bool parseContext)
{
	SET_TDBB(tdbb);

	// Make a relation reference node

	RelationSourceNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) RelationSourceNode(
		*tdbb->getDefaultPool());

	// Find relation either by id or by name
	AutoPtr<string> aliasString;
	QualifiedName name;

	switch (blrOp)
	{
		case blr_rid:
		case blr_rid2:
		{
			const SSHORT id = csb->csb_blr_reader.getWord();

			if (blrOp == blr_rid2)
			{
				aliasString = FB_NEW_POOL(csb->csb_pool) string(csb->csb_pool);
				csb->csb_blr_reader.getString(*aliasString);
			}

			if (!(node->relation = MET_lookup_relation_id(tdbb, id, false)))
				name.object.printf("id %d", id);

			break;
		}

		case blr_relation3:
			csb->csb_blr_reader.getMetaName(name.schema);
			[[fallthrough]];

		case blr_relation:
		case blr_relation2:
		{
			csb->csb_blr_reader.getMetaName(name.object);
			csb->qualifyExistingName(tdbb, name, obj_relation);

			if (blrOp == blr_relation2 || blrOp == blr_relation3)
			{
				aliasString = FB_NEW_POOL(csb->csb_pool) string(csb->csb_pool);
				csb->csb_blr_reader.getString(*aliasString);
			}

			node->relation = MET_lookup_relation(tdbb, name);
			break;
		}

		default:
			fb_assert(false);
	}

	if (!node->relation)
		PAR_error(csb, Arg::Gds(isc_relnotdef) << name.toQuotedString(), false);

	// if an alias was passed, store with the relation

	if (aliasString)
		node->alias = *aliasString;

	// Scan the relation if it hasn't already been scanned for meta data

	if ((!(node->relation->rel_flags & REL_scanned) ||
		(node->relation->rel_flags & REL_being_scanned)) &&
		!(csb->csb_g_flags & csb_internal))
	{
		MET_scan_relation(tdbb, node->relation);
	}

	// generate a stream for the relation reference, assuming it is a real reference

	if (parseContext)
	{
		node->stream = PAR_context(csb, &node->context);

		csb->csb_rpt[node->stream].csb_relation = node->relation;
		csb->csb_rpt[node->stream].csb_alias = aliasString.release();

		if (csb->collectingDependencies())
			PAR_dependency(tdbb, csb, node->stream, (SSHORT) -1, "");
	}

	return node;
}

string RelationSourceNode::internalPrint(NodePrinter& printer) const
{
	RecordSourceNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlName);
	NODE_PRINT(printer, alias);
	NODE_PRINT(printer, context);
	if (relation)
		printer.print("rel_name", relation->rel_name);

	return "RelationSourceNode";
}

RecordSourceNode* RelationSourceNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return dsqlPassRelProc(dsqlScratch, this);
}

bool RelationSourceNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other,
	bool /*ignoreMapCast*/) const
{
	const RelationSourceNode* o = nodeAs<RelationSourceNode>(other);
	return o && dsqlContext == o->dsqlContext;
}

// Generate blr for a relation reference.
void RelationSourceNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	const dsql_rel* relation = dsqlContext->ctx_relation;

	// if this is a trigger or procedure, don't want relation id used

	if (DDL_ids(dsqlScratch))
	{
		dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_rid2 : blr_rid);
		dsqlScratch->appendUShort(relation->rel_id);
	}
	else
	{
		if (relation->rel_name.schema != dsqlScratch->ddlSchema)
		{
			dsqlScratch->appendUChar(blr_relation3);
			dsqlScratch->appendMetaString(relation->rel_name.schema.c_str());
			dsqlScratch->appendMetaString(relation->rel_name.object.c_str());
			if (dsqlContext->ctx_alias.isEmpty())
				dsqlScratch->appendMetaString("");
		}
		else
		{
			dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_relation2 : blr_relation);
			dsqlScratch->appendMetaString(relation->rel_name.object.c_str());
		}
	}

	if (dsqlContext->ctx_alias.hasData())
	{
		const auto contextAliases = dsqlContext->getConcatenatedAlias();
		dsqlScratch->appendMetaString(contextAliases.c_str());
	}

	GEN_stuff_context(dsqlScratch, dsqlContext);
}

RelationSourceNode* RelationSourceNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	if (!copier.remap)
		BUGCHECK(221);	// msg 221 (CMP) copy: cannot remap

	RelationSourceNode* newSource = FB_NEW_POOL(*tdbb->getDefaultPool()) RelationSourceNode(
		*tdbb->getDefaultPool());

	newSource->stream = copier.csb->nextStream();
	copier.remap[stream] = newSource->stream;

	newSource->context = context;
	newSource->relation = relation;
	newSource->view = view;

	CompilerScratch::csb_repeat* element = CMP_csb_element(copier.csb, newSource->stream);
	element->csb_relation = newSource->relation;
	element->csb_view = newSource->view;
	element->csb_view_stream = copier.remap[0];

	if (alias.hasData())
	{
		element->csb_alias = FB_NEW_POOL(*tdbb->getDefaultPool())
			string(*tdbb->getDefaultPool(), alias);
	}

	return newSource;
}

RecordSourceNode* RelationSourceNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	const auto tail = &csb->csb_rpt[stream];
	const auto relation = tail->csb_relation;

	if (relation && !csb->csb_implicit_cursor)
	{
		const SLONG ssRelationId = tail->csb_view ? tail->csb_view->rel_id :
			view ? view->rel_id : csb->csb_view ? csb->csb_view->rel_id : 0;

		CMP_post_access(tdbb, csb, relation->rel_security_name.schema, ssRelationId,
			SCL_usage, obj_schemas, QualifiedName(relation->rel_name.schema));

		CMP_post_access(tdbb, csb, relation->rel_security_name.object, ssRelationId,
			SCL_select, obj_relations, relation->rel_name);
	}

	return this;
}

void RelationSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	BoolExprNode** boolean, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	pass1(tdbb, csb);

	// We have a view or a base table;
	// prepare to check protection of relation when a field in the stream of the
	// relation is accessed.

	jrd_rel* const parentView = csb->csb_view;
	const StreamType viewStream = csb->csb_view_stream;

	jrd_rel* relationView = relation;
	CMP_post_resource(&csb->csb_resources, relationView, Resource::rsc_relation, relationView->rel_id);
	view = parentView;

	CompilerScratch::csb_repeat* const element = CMP_csb_element(csb, stream);
	element->csb_view = parentView;
	element->csb_view_stream = viewStream;

	// in the case where there is a parent view, find the context name

	if (parentView)
	{
		const ViewContexts& ctx = parentView->rel_view_contexts;
		const USHORT key = context;
		FB_SIZE_T pos;

		if (ctx.find(key, pos))
		{
			element->csb_alias = FB_NEW_POOL(csb->csb_pool)
				string(csb->csb_pool, ctx[pos]->vcx_context_name);
		}
	}

	// check for a view - if not, nothing more to do

	RseNode* viewRse = relationView->rel_view_rse;
	if (!viewRse)
		return;

	// we've got a view, expand it

	stack.pop();
	StreamType* map = CMP_alloc_map(tdbb, csb, stream);

	AutoSetRestore<USHORT> autoRemapVariable(&csb->csb_remap_variable,
		(csb->csb_variables ? csb->csb_variables->count() : 0) + 1);
	AutoSetRestore<jrd_rel*> autoView(&csb->csb_view, relationView);
	AutoSetRestore<StreamType> autoViewStream(&csb->csb_view_stream, stream);

	// We don't expand the view in two cases:
	// 1) If the view has a projection, sort, first/skip or explicit plan.
	// 2) If it's part of an outer join.

	if (rse->rse_jointype != blr_inner || // viewRse->rse_jointype != blr_inner || ???
		viewRse->rse_sorted || viewRse->rse_projection || viewRse->rse_first ||
		viewRse->rse_skip || viewRse->rse_plan)
	{
		NodeCopier copier(csb->csb_pool, csb, map);
		RseNode* copy = viewRse->copy(tdbb, copier);
		doPass1(tdbb, csb, &copy);
		stack.push(copy);
		return;
	}

	// ASF: Below we start to do things when viewRse->rse_projection is not NULL.
	// But we should never come here, as the code above returns in this case.

	// if we have a projection which we can bubble up to the parent rse, set the
	// parent rse to our projection temporarily to flag the fact that we have already
	// seen one so that lower-level views will not try to map their projection; the
	// projection will be copied and correctly mapped later, but we don't have all
	// the base streams yet

	if (viewRse->rse_projection)
		rse->rse_projection = viewRse->rse_projection;

	// disect view into component relations

	for (const auto sub : viewRse->rse_relations)
	{
		// this call not only copies the node, it adds any streams it finds to the map
		NodeCopier copier(csb->csb_pool, csb, map);
		RecordSourceNode* node = sub->copy(tdbb, copier);

		// Now go out and process the base table itself. This table might also be a view,
		// in which case we will continue the process by recursion.
		processSource(tdbb, csb, rse, node, boolean, stack);
	}

	// When there is a projection in the view, copy the projection up to the query RseNode.
	// In order to make this work properly, we must remap the stream numbers of the fields
	// in the view to the stream number of the base table. Note that the map at this point
	// contains the stream numbers of the referenced relations, since it was added during the call
	// to copy() above. After the copy() below, the fields in the projection will reference the
	// base table(s) instead of the view's context (see bug #8822), so we are ready to context-
	// recognize them in doPass1() - that is, replace the field nodes with actual field blocks.

	if (viewRse->rse_projection)
	{
		NodeCopier copier(csb->csb_pool, csb, map);
		rse->rse_projection = viewRse->rse_projection->copy(tdbb, copier);
		doPass1(tdbb, csb, rse->rse_projection.getAddress());
	}

	// if we encounter a boolean, copy it and retain it by ANDing it in with the
	// boolean on the parent view, if any

	if (viewRse->rse_boolean)
	{
		NodeCopier copier(csb->csb_pool, csb, map);
		BoolExprNode* node = copier.copy(tdbb, viewRse->rse_boolean);

		doPass1(tdbb, csb, &node);

		if (*boolean)
		{
			// The order of the nodes here is important! The
			// boolean from the view must appear first so that
			// it gets expanded first in pass1.

			BinaryBoolNode* andNode = FB_NEW_POOL(csb->csb_pool) BinaryBoolNode(csb->csb_pool, blr_and);
			andNode->arg1 = node;
			andNode->arg2 = *boolean;

			*boolean = andNode;
		}
		else
			*boolean = node;
	}
}

void RelationSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	csb->csb_rpt[stream].activate();

	pass2(tdbb, csb);
}

RecordSource* RelationSourceNode::compile(thread_db* tdbb, Optimizer* opt, bool /*innerSubStream*/)
{
	opt->compileRelation(stream);

	return NULL;
}


//--------------------


// Parse an procedural view reference.
ProcedureSourceNode* ProcedureSourceNode::parse(thread_db* tdbb, CompilerScratch* csb,
	const SSHORT blrOp, bool parseContext)
{
	const auto predateCheck = [&](bool condition, const char* preVerb, const char* postVerb)
	{
		if (!condition)
		{
			string str;
			str.printf("%s should predate %s", preVerb, postVerb);
			PAR_error(csb, Arg::Gds(isc_random) << str);
		}
	};

	auto& pool = *tdbb->getDefaultPool();
	auto& blrReader = csb->csb_blr_reader;
	const auto blrStartPos = blrReader.getPos();
	const UCHAR* inArgNamesPos = nullptr;
	ObjectsArray<MetaName>* inArgNames = nullptr;
	USHORT inArgCount = 0;
	QualifiedName name;

	const auto node = FB_NEW_POOL(pool) ProcedureSourceNode(pool);

	switch (blrOp)
	{
		case blr_select_procedure:
		{
			CompilerScratch::csb_repeat* csbTail = nullptr;
			UCHAR subCode;

			while ((subCode = blrReader.getByte()) != blr_end)
			{
				switch (subCode)
				{
					case blr_invsel_procedure_id:
					{
						bool isSub = false;
						UCHAR procedureIdCode;

						while ((procedureIdCode = blrReader.getByte()) != blr_end)
						{
							switch (procedureIdCode)
							{
								case blr_invsel_procedure_id_schema:
									blrReader.getMetaName(name.schema);
									break;

								case blr_invsel_procedure_id_package:
									blrReader.getMetaName(name.package);
									break;

								case blr_invsel_procedure_id_name:
									blrReader.getMetaName(name.object);
									break;

								case blr_invsel_procedure_id_sub:
									isSub = true;
									break;

								default:
									PAR_error(csb, Arg::Gds(isc_random) << "Invalid blr_invsel_procedure_id");
									break;
							}
						}

						if (isSub)
						{
							for (auto curCsb = csb; curCsb && !node->procedure; curCsb = curCsb->mainCsb)
							{
								if (const auto declareNode = curCsb->subProcedures.get(name.object))
									node->procedure = (*declareNode)->routine;
							}
						}
						else if (!node->procedure)
						{
							csb->qualifyExistingName(tdbb, name, obj_procedure);
							node->procedure = MET_lookup_procedure(tdbb, name, false);
						}

						break;
					}

					case blr_invsel_procedure_in_arg_names:
					{
						predateCheck(node->procedure, "blr_invsel_procedure_id", "blr_invsel_procedure_in_arg_names");
						predateCheck(!node->inputSources, "blr_invsel_procedure_in_arg_names", "blr_invsel_procedure_in_args");

						inArgNamesPos = blrReader.getPos();
						USHORT inArgNamesCount = blrReader.getWord();
						MetaName argName;

						inArgNames = FB_NEW_POOL(pool) ObjectsArray<MetaName>(pool);

						while (inArgNamesCount--)
						{
							blrReader.getMetaName(argName);
							inArgNames->add(argName);
						}

						break;
					}

					case blr_invsel_procedure_in_args:
						predateCheck(node->procedure, "blr_invsel_procedure_id", "blr_invsel_procedure_in_args");

						inArgCount = blrReader.getWord();
						node->inputSources = PAR_args(tdbb, csb, inArgCount,
							MAX(inArgCount, node->procedure->getInputFields().getCount()));
						break;

					case blr_invsel_procedure_context:
						if (!parseContext)
						{
							PAR_error(csb,
								Arg::Gds(isc_random) <<
								"blr_invsel_procedure_context not expected inside plan clauses");
						}

						predateCheck(node->procedure, "blr_invsel_procedure_id", "blr_invsel_procedure_context");
						node->stream = PAR_context2(csb, &node->context);
						csbTail = &csb->csb_rpt[node->stream];
						csbTail->csb_procedure = node->procedure;

						if (node->alias.hasData())
							csbTail->csb_alias = &node->alias;

						if (csb->collectingDependencies())
							PAR_dependency(tdbb, csb, node->stream, (SSHORT) -1, "");

						break;

					case blr_invsel_procedure_alias:
						blrReader.getString(node->alias);
						if (csbTail && node->alias.hasData())
							csbTail->csb_alias = &node->alias;
						break;

					default:
						PAR_error(csb, Arg::Gds(isc_random) << "Invalid blr_select_procedure sub code");
				}
			}

			break;
		}

		case blr_pid:
		case blr_pid2:
		{
			const SSHORT pid = blrReader.getWord();

			if (blrOp == blr_pid2)
				blrReader.getString(node->alias);

			if (!(node->procedure = MET_lookup_procedure_id(tdbb, pid, false, false, 0)))
				name.object.printf("id %d", pid);

			break;
		}

		case blr_procedure:
		case blr_procedure2:
		case blr_procedure3:
		case blr_procedure4:
		case blr_subproc:
			if (blrOp == blr_procedure3 || blrOp == blr_procedure4)
				blrReader.getMetaName(name.package);

			blrReader.getMetaName(name.object);

			if (blrOp == blr_procedure2 || blrOp == blr_procedure4 || blrOp == blr_subproc)
				blrReader.getString(node->alias);

			if (blrOp == blr_subproc)
			{
				for (auto curCsb = csb; curCsb && !node->procedure; curCsb = curCsb->mainCsb)
				{
					if (const auto declareNode = curCsb->subProcedures.get(name.object))
						node->procedure = (*declareNode)->routine;
				}
			}
			else
			{
				csb->qualifyExistingName(tdbb, name, obj_procedure);
				node->procedure = MET_lookup_procedure(tdbb, name, false);
			}

			break;

		default:
			fb_assert(false);
	}

	if (inArgNames && inArgNames->getCount() > node->inputSources->items.getCount())
	{
		blrReader.setPos(inArgNamesPos);
		PAR_error(csb,
			Arg::Gds(isc_random) <<
			"blr_invsel_procedure_in_arg_names count cannot be greater than blr_invsel_procedure_in_args");
	}

	if (!node->procedure)
	{
		blrReader.setPos(blrStartPos);
		PAR_error(csb, Arg::Gds(isc_prcnotdef) << name.toQuotedString());
	}

	if (node->procedure->prc_type == prc_executable)
	{
		if (tdbb->getAttachment()->isGbak())
			PAR_warning(Arg::Warning(isc_illegal_prc_type) << node->procedure->getName().toQuotedString());
		else
			PAR_error(csb, Arg::Gds(isc_illegal_prc_type) << node->procedure->getName().toQuotedString());
	}

	node->isSubRoutine = node->procedure->isSubRoutine();
	node->procedureId = node->isSubRoutine ? 0 : node->procedure->getId();

	if (node->procedure->isImplemented() && !node->procedure->isDefined())
	{
		if (tdbb->getAttachment()->isGbak() || (tdbb->tdbb_flags & TDBB_replicator))
		{
			PAR_warning(
				Arg::Warning(isc_prcnotdef) << name.toQuotedString() <<
				Arg::Warning(isc_modnotfound));
		}
		else
		{
			blrReader.setPos(blrStartPos);
			PAR_error(csb,
				Arg::Gds(isc_prcnotdef) << name.toQuotedString() <<
				Arg::Gds(isc_modnotfound));
		}
	}

	if (parseContext)
	{
		if (blrOp != blr_select_procedure)
		{
			node->stream = PAR_context(csb, &node->context);

			csb->csb_rpt[node->stream].csb_procedure = node->procedure;
			if (node->alias.hasData())
				csb->csb_rpt[node->stream].csb_alias = &node->alias;

			inArgCount = blrReader.getWord();
			node->inputSources = PAR_args(tdbb, csb, inArgCount, inArgCount);
		}

		if (!node->inputSources)
			node->inputSources = FB_NEW_POOL(pool) ValueListNode(pool);

		node->inputTargets = FB_NEW_POOL(pool) ValueListNode(pool, node->procedure->getInputFields().getCount());

		Arg::StatusVector mismatchStatus;

		if (!CMP_procedure_arguments(
			tdbb,
			csb,
			node->procedure,
			true,
			inArgCount,
			inArgNames,
			node->inputSources,
			node->inputTargets,
			node->inputMessage,
			mismatchStatus))
		{
			status_exception::raise(Arg::Gds(isc_prcmismat) <<
				node->procedure->getName().toQuotedString() << mismatchStatus);
		}

		if (csb->collectingDependencies() && !node->procedure->isSubRoutine())
		{
			{	// scope
				CompilerScratch::Dependency dependency(obj_procedure);
				dependency.procedure = node->procedure;
				csb->addDependency(dependency);
			}

			if (inArgNames)
			{
				for (const auto& argName : *inArgNames)
				{
					CompilerScratch::Dependency dependency(obj_procedure);
					dependency.procedure = node->procedure;
					dependency.subName = &argName;
					csb->addDependency(dependency);
				}
			}
		}
	}

	if (node->inputSources && node->inputSources->items.isEmpty())
	{
		delete node->inputSources.getObject();
		node->inputSources = nullptr;

		delete node->inputTargets.getObject();
		node->inputTargets = nullptr;
	}

	return node;
}

string ProcedureSourceNode::internalPrint(NodePrinter& printer) const
{
	RecordSourceNode::internalPrint(printer);

	NODE_PRINT(printer, inputMessage);
	NODE_PRINT(printer, context);

	return "ProcedureSourceNode";
}

RecordSourceNode* ProcedureSourceNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	return dsqlPassRelProc(dsqlScratch, this);
}

bool ProcedureSourceNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	// Check if relation is a procedure.
	if (dsqlContext->ctx_procedure)
	{
		// Check if an aggregate is buried inside the input parameters.
		return visitor.visit(inputSources);
	}

	return false;
}

bool ProcedureSourceNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	if (dsqlContext->ctx_procedure)
		return visitor.visit(inputSources);

	return false;
}

bool ProcedureSourceNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	// If relation is a procedure, check if the parameters are valid.
	if (dsqlContext->ctx_procedure)
		return visitor.visit(inputSources);

	return false;
}

bool ProcedureSourceNode::dsqlSubSelectFinder(SubSelectFinder& visitor)
{
	// If relation is a procedure, check if the parameters are valid.
	if (dsqlContext->ctx_procedure)
		return visitor.visit(inputSources);

	return false;
}

bool ProcedureSourceNode::dsqlFieldFinder(FieldFinder& visitor)
{
	// If relation is a procedure, check if the parameters are valid.
	if (dsqlContext->ctx_procedure)
		return visitor.visit(inputSources);

	return false;
}

RecordSourceNode* ProcedureSourceNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	// Check if relation is a procedure.
	if (dsqlContext->ctx_procedure)
		doDsqlFieldRemapper(visitor, inputSources);	// Remap the input parameters.

	return this;
}

bool ProcedureSourceNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other,
	bool /*ignoreMapCast*/) const
{
	const ProcedureSourceNode* o = nodeAs<ProcedureSourceNode>(other);
	return o && dsqlContext == o->dsqlContext;
}

// Generate blr for a procedure reference.
void ProcedureSourceNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	const dsql_prc* dsqlProcedure = dsqlContext->ctx_procedure;

	if (dsqlInputArgNames || dsqlName.schema != dsqlScratch->ddlSchema)
	{
		dsqlScratch->appendUChar(blr_select_procedure);

		dsqlScratch->appendUChar(blr_invsel_procedure_id);

		if (dsqlProcedure->prc_flags & PRC_subproc)
			dsqlScratch->appendUChar(blr_invsel_procedure_id_sub);
		else
		{
			if (dsqlName.schema != dsqlScratch->ddlSchema)
			{
				dsqlScratch->appendUChar(blr_invsel_procedure_id_schema);
				dsqlScratch->appendMetaString(dsqlName.schema.c_str());
			}

			if (dsqlName.package.hasData())
			{
				dsqlScratch->appendUChar(blr_invsel_procedure_id_package);
				dsqlScratch->appendMetaString(dsqlName.package.c_str());
			}
		}

		dsqlScratch->appendUChar(blr_invsel_procedure_id_name);
		dsqlScratch->appendMetaString(dsqlName.object.c_str());
		dsqlScratch->appendUChar(blr_end);

		// Input parameters.
		if (inputSources)
		{
			if (dsqlInputArgNames && dsqlInputArgNames->hasData())
			{
				dsqlScratch->appendUChar(blr_invsel_procedure_in_arg_names);
				dsqlScratch->appendUShort(dsqlInputArgNames->getCount());

				for (auto& argName : *dsqlInputArgNames)
					dsqlScratch->appendMetaString(argName.c_str());
			}

			dsqlScratch->appendUChar(blr_invsel_procedure_in_args);
			dsqlScratch->appendUShort(inputSources->items.getCount());

			for (auto& arg : inputSources->items)
				GEN_arg(dsqlScratch, arg);
		}

		if (dsqlContext->ctx_context > MAX_UCHAR)
			ERRD_post(Arg::Gds(isc_too_many_contexts));

		dsqlScratch->appendUChar(blr_invsel_procedure_context);
		dsqlScratch->appendUShort(dsqlContext->ctx_context);

		if (dsqlContext->ctx_alias.hasData())
		{
			dsqlScratch->appendUChar(blr_invsel_procedure_alias);

			const auto contextAliases = dsqlContext->getConcatenatedAlias();
			dsqlScratch->appendMetaString(contextAliases.c_str());
		}

		dsqlScratch->appendUChar(blr_end);

		return;
	}

	if (dsqlProcedure->prc_flags & PRC_subproc)
	{
		dsqlScratch->appendUChar(blr_subproc);
		dsqlScratch->appendMetaString(dsqlProcedure->prc_name.object.c_str());

		const auto contextAliases = dsqlContext->getConcatenatedAlias();
		dsqlScratch->appendMetaString(contextAliases.c_str());
	}
	else
	{
		// If this is a trigger or procedure, don't want procedure id used.

		if (DDL_ids(dsqlScratch))
		{
			dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_pid2 : blr_pid);
			dsqlScratch->appendUShort(dsqlProcedure->prc_id);
		}
		else
		{
			if (dsqlProcedure->prc_name.package.hasData())
			{
				dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_procedure4 : blr_procedure3);
				dsqlScratch->appendMetaString(dsqlProcedure->prc_name.package.c_str());
				dsqlScratch->appendMetaString(dsqlProcedure->prc_name.object.c_str());
			}
			else
			{
				dsqlScratch->appendUChar(dsqlContext->ctx_alias.hasData() ? blr_procedure2 : blr_procedure);
				dsqlScratch->appendMetaString(dsqlProcedure->prc_name.object.c_str());
			}
		}

		if (dsqlContext->ctx_alias.hasData())
		{
			const auto contextAliases = dsqlContext->getConcatenatedAlias();
			dsqlScratch->appendMetaString(contextAliases.c_str());
		}
	}

	GEN_stuff_context(dsqlScratch, dsqlContext);

	if (inputSources && !(dsqlFlags & DFLAG_PLAN_ITEM))
	{
		dsqlScratch->appendUShort(inputSources->items.getCount());

		for (auto& arg : inputSources->items)
			GEN_arg(dsqlScratch, arg);
	}
	else
		dsqlScratch->appendUShort(0);
}

ProcedureSourceNode* ProcedureSourceNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	if (!copier.remap)
		BUGCHECK(221);	// msg 221 (CMP) copy: cannot remap

	ProcedureSourceNode* newSource = FB_NEW_POOL(*tdbb->getDefaultPool()) ProcedureSourceNode(
		*tdbb->getDefaultPool());

	if (isSubRoutine)
		newSource->procedure = procedure;
	else
	{
		newSource->procedure = MET_lookup_procedure_id(tdbb, procedureId, false, false, 0);
		if (!newSource->procedure)
		{
			string name;
			name.printf("id %d", procedureId);
			delete newSource;
			ERR_post(Arg::Gds(isc_prcnotdef) << name);
		}
	}

	// dimitr: See the appropriate code and comment in NodeCopier (in nod_argument).
	// We must copy the message first and only then use the new pointer to
	// copy the inputs properly.
	newSource->inputMessage = copier.copy(tdbb, inputMessage);

	{	// scope
		AutoSetRestore<MessageNode*> autoMessage(&copier.message, newSource->inputMessage);
		newSource->inputSources = copier.copy(tdbb, inputSources);
		newSource->inputTargets = copier.copy(tdbb, inputTargets);
	}

	newSource->stream = copier.csb->nextStream();
	copier.remap[stream] = newSource->stream;
	newSource->context = context;
	newSource->isSubRoutine = isSubRoutine;
	newSource->procedureId = procedureId;
	newSource->view = view;

	CompilerScratch::csb_repeat* element = CMP_csb_element(copier.csb, newSource->stream);
	element->csb_procedure = newSource->procedure;
	element->csb_view = newSource->view;
	element->csb_view_stream = copier.remap[0];

	if (alias.hasData())
	{
		element->csb_alias = FB_NEW_POOL(*tdbb->getDefaultPool())
			string(*tdbb->getDefaultPool(), alias);
	}

	return newSource;
}

RecordSourceNode* ProcedureSourceNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, inputSources.getAddress());
	doPass1(tdbb, csb, inputTargets.getAddress());
	doPass1(tdbb, csb, inputMessage.getAddress());
	return this;
}

void ProcedureSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* /*rse*/,
	BoolExprNode** /*boolean*/, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	pass1(tdbb, csb);

	if (!isSubRoutine)
	{
		CMP_post_procedure_access(tdbb, csb, procedure);
		CMP_post_resource(&csb->csb_resources, procedure, Resource::rsc_procedure, procedureId);
	}

	jrd_rel* const parentView = csb->csb_view;
	const StreamType viewStream = csb->csb_view_stream;
	view = parentView;

	CompilerScratch::csb_repeat* const element = CMP_csb_element(csb, stream);
	element->csb_view = parentView;
	element->csb_view_stream = viewStream;

	// in the case where there is a parent view, find the context name

	if (parentView)
	{
		const ViewContexts& ctx = parentView->rel_view_contexts;
		const USHORT key = context;
		FB_SIZE_T pos;

		if (ctx.find(key, pos))
		{
			element->csb_alias = FB_NEW_POOL(csb->csb_pool) string(
				csb->csb_pool, ctx[pos]->vcx_context_name);
		}
	}
}

RecordSourceNode* ProcedureSourceNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	ExprNode::doPass2(tdbb, csb, inputSources.getAddress());
	ExprNode::doPass2(tdbb, csb, inputTargets.getAddress());
	ExprNode::doPass2(tdbb, csb, inputMessage.getAddress());
	return this;
}

void ProcedureSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	csb->csb_rpt[stream].activate();

	pass2(tdbb, csb);
}

RecordSource* ProcedureSourceNode::compile(thread_db* tdbb, Optimizer* opt, bool /*innerSubStream*/)
{
	const auto csb = opt->getCompilerScratch();
	const string alias = opt->makeAlias(stream);

	return FB_NEW_POOL(*tdbb->getDefaultPool()) ProcedureScan(csb, alias, stream, procedure,
		inputSources, inputTargets, inputMessage);
}

bool ProcedureSourceNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	if (inputSources && !inputSources->computable(csb, stream, allowOnlyCurrentStream))
		return false;

	if (inputTargets && !inputTargets->computable(csb, stream, allowOnlyCurrentStream))
		return false;

	return true;
}

void ProcedureSourceNode::findDependentFromStreams(const CompilerScratch* csb,
	StreamType currentStream, SortedStreamList* streamList)
{
	if (inputSources)
		inputSources->findDependentFromStreams(csb, currentStream, streamList);

	if (inputTargets)
		inputTargets->findDependentFromStreams(csb, currentStream, streamList);
}

void ProcedureSourceNode::collectStreams(SortedStreamList& streamList) const
{
	RecordSourceNode::collectStreams(streamList);

	if (inputSources)
		inputSources->collectStreams(streamList);

	if (inputTargets)
		inputTargets->collectStreams(streamList);
}


//--------------------


// Parse an aggregate reference.
AggregateSourceNode* AggregateSourceNode::parse(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	AggregateSourceNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) AggregateSourceNode(
		*tdbb->getDefaultPool());

	node->stream = PAR_context(csb, NULL);
	node->rse = PAR_rse(tdbb, csb);
	node->group = PAR_sort(tdbb, csb, blr_group_by, true);
	node->map = parseMap(tdbb, csb, node->stream, true);

	return node;
}

string AggregateSourceNode::internalPrint(NodePrinter& printer) const
{
	RecordSourceNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlGroup);
	NODE_PRINT(printer, dsqlRse);
	NODE_PRINT(printer, dsqlWindow);
	NODE_PRINT(printer, group);
	NODE_PRINT(printer, map);

	return "AggregateSourceNode";
}

bool AggregateSourceNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	return !visitor.ignoreSubSelects && visitor.visit(dsqlRse);
}

bool AggregateSourceNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	// Pass only dsqlGroup.
	return visitor.visit(dsqlGroup);
}

bool AggregateSourceNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	return visitor.visit(dsqlRse);
}

bool AggregateSourceNode::dsqlSubSelectFinder(SubSelectFinder& /*visitor*/)
{
	return false;
}

bool AggregateSourceNode::dsqlFieldFinder(FieldFinder& visitor)
{
	// Pass only dsqlGroup.
	return visitor.visit(dsqlGroup);
}

RecordSourceNode* AggregateSourceNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	doDsqlFieldRemapper(visitor, dsqlRse);
	return this;
}

bool AggregateSourceNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	const AggregateSourceNode* o = nodeAs<AggregateSourceNode>(other);

	return o && dsqlContext == o->dsqlContext &&
		PASS1_node_match(dsqlScratch, dsqlGroup, o->dsqlGroup, ignoreMapCast) &&
		PASS1_node_match(dsqlScratch, dsqlRse, o->dsqlRse, ignoreMapCast);
}

void AggregateSourceNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar((dsqlWindow ? blr_window : blr_aggregate));

	if (!dsqlWindow)
		GEN_stuff_context(dsqlScratch, dsqlContext);

	GEN_rse(dsqlScratch, dsqlRse);

	// Handle PARTITION BY and GROUP BY clause

	if (dsqlWindow)
	{
		// There may be unused named windows.
		//fb_assert(dsqlContext->ctx_win_maps.hasData());

		dsqlScratch->appendUChar(dsqlContext->ctx_win_maps.getCount());	// number of windows

		for (Array<WindowMap*>::iterator i = dsqlContext->ctx_win_maps.begin();
			 i != dsqlContext->ctx_win_maps.end();
			 ++i)
		{
			bool v3 = !((*i)->window &&
				((*i)->window->extent ||
				 (*i)->window->exclusion != WindowClause::Exclusion::NO_OTHERS));

			ValueListNode* partition = (*i)->window ? (*i)->window->partition : NULL;
			ValueListNode* partitionRemapped = (*i)->partitionRemapped;
			ValueListNode* order = (*i)->window ? (*i)->window->order : NULL;

			if ((*i)->context > MAX_UCHAR)
				ERRD_post(Arg::Gds(isc_too_many_contexts));

			dsqlScratch->appendUChar(v3 ? blr_partition_by : blr_window_win);
			dsqlScratch->appendUChar((*i)->context);

			if (partition)
			{
				if (!v3)
					dsqlScratch->appendUChar(blr_window_win_partition);

				dsqlScratch->appendUChar(partition->items.getCount());	// partition by expression count

				NestConst<ValueExprNode>* ptr = partition->items.begin();
				for (const NestConst<ValueExprNode>* end = partition->items.end(); ptr != end; ++ptr)
					GEN_expr(dsqlScratch, *ptr);

				ptr = partitionRemapped->items.begin();
				for (const NestConst<ValueExprNode>* end = partitionRemapped->items.end();
					 ptr != end;
					 ++ptr)
				{
					GEN_expr(dsqlScratch, *ptr);
				}
			}
			else if (v3)
				dsqlScratch->appendUChar(0);	// partition by expression count

			if (v3 || order)
				GEN_sort(dsqlScratch, (v3 ? blr_sort : blr_window_win_order), order);

			genMap(dsqlScratch, (v3 ? blr_map : blr_window_win_map), (*i)->map);

			if (!v3)
			{
				if ((*i)->window->extent)
				{
					dsqlScratch->appendUChar(blr_window_win_extent_unit);
					dsqlScratch->appendUChar((UCHAR) (*i)->window->extent->unit);

					WindowClause::Frame* frames[] = {
						(*i)->window->extent->frame1, (*i)->window->extent->frame2
					};

					for (int j = 0; j < 2; ++j)
					{
						if (frames[j])
						{
							dsqlScratch->appendUChar(blr_window_win_extent_frame_bound);
							dsqlScratch->appendUChar(j + 1);
							dsqlScratch->appendUChar((UCHAR) frames[j]->bound);

							if (frames[j]->value)
							{
								dsqlScratch->appendUChar(blr_window_win_extent_frame_value);
								dsqlScratch->appendUChar(j + 1);
								frames[j]->value->genBlr(dsqlScratch);
							}
						}
					}
				}

				if ((*i)->window->exclusion != WindowClause::Exclusion::NO_OTHERS)
				{
					dsqlScratch->appendUChar(blr_window_win_exclusion);
					dsqlScratch->appendUChar((UCHAR) (*i)->window->exclusion);
				}

				dsqlScratch->appendUChar(blr_end);
			}
		}
	}
	else
	{
		dsqlScratch->appendUChar(blr_group_by);

		ValueListNode* list = dsqlGroup;

		if (list)
		{
			dsqlScratch->appendUChar(list->items.getCount());
			NestConst<ValueExprNode>* ptr = list->items.begin();

			for (const NestConst<ValueExprNode>* end = list->items.end(); ptr != end; ++ptr)
				(*ptr)->genBlr(dsqlScratch);
		}
		else
			dsqlScratch->appendUChar(0);

		genMap(dsqlScratch, blr_map, dsqlContext->ctx_map);
	}
}

// Generate a value map for a record selection expression.
void AggregateSourceNode::genMap(DsqlCompilerScratch* dsqlScratch, UCHAR blrVerb, dsql_map* map)
{
	USHORT count = 0;

	for (dsql_map* temp = map; temp; temp = temp->map_next)
		++count;

	//if (count >= STREAM_MAP_LENGTH) // not sure if the same limit applies
	//	ERR_post(Arg::Gds(isc_too_many_contexts)); // maybe there's better msg.

	dsqlScratch->appendUChar(blrVerb);
	dsqlScratch->appendUShort(count);

	for (dsql_map* temp = map; temp; temp = temp->map_next)
	{
		dsqlScratch->appendUShort(temp->map_position);
		GEN_expr(dsqlScratch, temp->map_node);
	}
}

AggregateSourceNode* AggregateSourceNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	if (!copier.remap)
		BUGCHECK(221);	// msg 221 (CMP) copy: cannot remap

	AggregateSourceNode* newSource = FB_NEW_POOL(*tdbb->getDefaultPool()) AggregateSourceNode(
		*tdbb->getDefaultPool());

	newSource->stream = copier.csb->nextStream();
	copier.remap[stream] = newSource->stream;
	CMP_csb_element(copier.csb, newSource->stream);

	newSource->rse = rse->copy(tdbb, copier);
	if (group)
		newSource->group = group->copy(tdbb, copier);
	newSource->map = map->copy(tdbb, copier);

	return newSource;
}

RecordSourceNode* AggregateSourceNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, rse.getAddress());
	doPass1(tdbb, csb, map.getAddress());
	doPass1(tdbb, csb, group.getAddress());

	return this;
}

void AggregateSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* /*rse*/,
	BoolExprNode** /*boolean*/, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	pass1(tdbb, csb);

	jrd_rel* const parentView = csb->csb_view;
	const StreamType viewStream = csb->csb_view_stream;

	CompilerScratch::csb_repeat* const element = CMP_csb_element(csb, stream);
	element->csb_view = parentView;
	element->csb_view_stream = viewStream;

}

RecordSourceNode* AggregateSourceNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	rse->pass2Rse(tdbb, csb);
	ExprNode::doPass2(tdbb, csb, map.getAddress());
	ExprNode::doPass2(tdbb, csb, group.getAddress());

	processMap(tdbb, csb, map, &csb->csb_rpt[stream].csb_internal_format);
	csb->csb_rpt[stream].csb_format = csb->csb_rpt[stream].csb_internal_format;

	return this;
}

void AggregateSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	csb->csb_rpt[stream].activate();

	pass2(tdbb, csb);
}

bool AggregateSourceNode::containsStream(StreamType checkStream) const
{
	// for aggregates, check current RseNode, if not found then check
	// the sub-rse

	if (checkStream == stream)
		return true;		// do not mark as variant

	if (rse->containsStream(checkStream))
		return true;		// do not mark as variant

	return false;
}

RecordSource* AggregateSourceNode::compile(thread_db* tdbb, Optimizer* opt, bool /*innerSubStream*/)
{
	const auto csb = opt->getCompilerScratch();
	rse->rse_sorted = group;

	// AB: Try to distribute items from the HAVING CLAUSE to the WHERE CLAUSE.
	// Zip thru stack of booleans looking for fields that belong to shellStream.
	// Those fields are mappings. Mappings that hold a plain field may be used
	// to distribute. Handle the simple cases only.
	BoolExprNodeStack parentStack, deliverStack;
	for (auto iter = opt->getConjuncts(); iter.hasData(); ++iter)
		parentStack.push(*iter);
	genDeliverUnmapped(csb, parentStack, deliverStack, map, stream);

	// try to optimize MAX and MIN to use an index; for now, optimize
	// only the simplest case, although it is probably possible
	// to use an index in more complex situations
	NestConst<ValueExprNode>* ptr;
	AggNode* aggNode = NULL;

	if (map->sourceList.getCount() == 1 && (ptr = map->sourceList.begin()) &&
		(aggNode = nodeAs<AggNode>(*ptr)) &&
		(aggNode->aggInfo.blr == blr_agg_min || aggNode->aggInfo.blr == blr_agg_max))
	{
		// generate a sort block which the optimizer will try to map to an index

		SortNode* aggregate = rse->rse_aggregate =
			FB_NEW_POOL(*tdbb->getDefaultPool()) SortNode(*tdbb->getDefaultPool());

		aggregate->expressions.add(aggNode->arg);
		// For MAX(), mark the sort as descending. For MIN(), mark as ascending.
		const SortDirection direction =
			(aggNode->aggInfo.blr == blr_agg_max) ? ORDER_DESC : ORDER_ASC;
		aggregate->direction.add(direction);
		// 10-Aug-2004. Nickolay Samofatov - Unneeded nulls seem to be skipped somehow.
		aggregate->nullOrder.add(NULLS_DEFAULT);

		rse->firstRows = true;
	}

	RecordSource* const nextRsb = opt->compile(rse, &deliverStack);

	// allocate and optimize the record source block

	AggregatedStream* const rsb = FB_NEW_POOL(*tdbb->getDefaultPool()) AggregatedStream(tdbb, csb,
		stream, (group ? &group->expressions : NULL), map, nextRsb);

	if (rse->rse_aggregate)
	{
		// The rse_aggregate is still set. That means the optimizer
		// was able to match the field to an index, so flag that fact
		// so that it can be handled in EVL_group
		aggNode->indexed = true;
	}

	opt->generateAggregateDistincts(map);

	return rsb;
}

bool AggregateSourceNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	rse->rse_sorted = group;
	return rse->computable(csb, stream, allowOnlyCurrentStream, NULL);
}

void AggregateSourceNode::findDependentFromStreams(const CompilerScratch* csb,
	StreamType currentStream, SortedStreamList* streamList)
{
	rse->rse_sorted = group;
	rse->findDependentFromStreams(csb, currentStream, streamList);
}


//--------------------


// Parse a union reference.
UnionSourceNode* UnionSourceNode::parse(thread_db* tdbb, CompilerScratch* csb, const SSHORT blrOp)
{
	SET_TDBB(tdbb);

	// Make the node, parse the context number, get a stream assigned,
	// and get the number of sub-RseNode's.

	UnionSourceNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) UnionSourceNode(
		*tdbb->getDefaultPool());

	node->recursive = blrOp == blr_recurse;

	node->stream = PAR_context(csb, NULL);

	// assign separate context for mapped record if union is recursive
	StreamType stream2 = node->stream;

	if (node->recursive)
	{
		stream2 = PAR_context(csb, nullptr);
		node->mapStream = stream2;
	}

	// bottleneck
	int count = (unsigned int) csb->csb_blr_reader.getByte();

	// Pick up the sub-RseNode's and maps.

	while (--count >= 0)
	{
		node->clauses.push(PAR_rse(tdbb, csb));
		node->maps.push(parseMap(tdbb, csb, stream2, true));
	}

	return node;
}

string UnionSourceNode::internalPrint(NodePrinter& printer) const
{
	RecordSourceNode::internalPrint(printer);

	NODE_PRINT(printer, clauses);
	NODE_PRINT(printer, maps);
	NODE_PRINT(printer, mapStream);

	return "UnionSourceNode";
}

bool UnionSourceNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	return dsqlClauses->dsqlAggregateFinder(visitor);
}

bool UnionSourceNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	return dsqlClauses->dsqlAggregate2Finder(visitor);
}

bool UnionSourceNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	return dsqlClauses->dsqlInvalidReferenceFinder(visitor);
}

bool UnionSourceNode::dsqlSubSelectFinder(SubSelectFinder& visitor)
{
	return dsqlClauses->dsqlSubSelectFinder(visitor);
}

bool UnionSourceNode::dsqlFieldFinder(FieldFinder& visitor)
{
	return dsqlClauses->dsqlFieldFinder(visitor);
}

RecordSourceNode* UnionSourceNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	dsqlClauses->dsqlFieldRemapper(visitor);
	return this;
}

void UnionSourceNode::genBlr(DsqlCompilerScratch* dsqlScratch)
{
	dsqlScratch->appendUChar((recursive ? blr_recurse : blr_union));

	// Obtain the context for UNION from the first dsql_map* node.
	ValueExprNode* mapItem = dsqlParentRse->dsqlSelectList->items[0];

	// AB: First item could be a virtual field generated by derived table.
	DerivedFieldNode* derivedField = nodeAs<DerivedFieldNode>(mapItem);

	if (derivedField)
		mapItem = derivedField->value;

	if (nodeIs<CastNode>(mapItem))
		mapItem = nodeAs<CastNode>(mapItem)->source;

	DsqlMapNode* mapNode = nodeAs<DsqlMapNode>(mapItem);
	fb_assert(mapNode);

	if (!mapNode)
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
				  Arg::Gds(isc_dsql_internal_err) <<
				  Arg::Gds(isc_random) << Arg::Str("UnionSourceNode::genBlr: expected DsqlMapNode") );
	}

	dsql_ctx* dsqlContext = mapNode->context;

	GEN_stuff_context(dsqlScratch, dsqlContext);
	// secondary context number must be present once in generated blr
	dsqlContext->ctx_flags &= ~CTX_recursive;

	RecSourceListNode* streams = dsqlClauses;
	dsqlScratch->appendUChar(streams->items.getCount());	// number of substreams

	NestConst<RecordSourceNode>* ptr = streams->items.begin();
	for (const NestConst<RecordSourceNode>* const end = streams->items.end(); ptr != end; ++ptr)
	{
		RseNode* sub_rse = nodeAs<RseNode>(*ptr);
		GEN_rse(dsqlScratch, sub_rse);

		ValueListNode* items = sub_rse->dsqlSelectList;

		dsqlScratch->appendUChar(blr_map);
		dsqlScratch->appendUShort(items->items.getCount());

		USHORT count = 0;
		NestConst<ValueExprNode>* iptr = items->items.begin();

		for (const NestConst<ValueExprNode>* const iend = items->items.end(); iptr != iend; ++iptr)
		{
			dsqlScratch->appendUShort(count);
			GEN_expr(dsqlScratch, *iptr);
			++count;
		}
	}
}

UnionSourceNode* UnionSourceNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	if (!copier.remap)
		BUGCHECK(221);		// msg 221 (CMP) copy: cannot remap

	UnionSourceNode* newSource = FB_NEW_POOL(*tdbb->getDefaultPool()) UnionSourceNode(
		*tdbb->getDefaultPool());
	newSource->recursive = recursive;

	newSource->stream = copier.csb->nextStream();
	copier.remap[stream] = newSource->stream;
	CMP_csb_element(copier.csb, newSource->stream);

	if (newSource->recursive)
	{
		newSource->mapStream = copier.csb->nextStream();
		copier.remap[mapStream] = newSource->mapStream;
		CMP_csb_element(copier.csb, newSource->mapStream);
	}

	const NestConst<RseNode>* ptr = clauses.begin();
	const NestConst<MapNode>* ptr2 = maps.begin();

	for (const NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr, ++ptr2)
	{
		newSource->clauses.add((*ptr)->copy(tdbb, copier));
		newSource->maps.add((*ptr2)->copy(tdbb, copier));
	}

	return newSource;
}

void UnionSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* /*rse*/,
	BoolExprNode** /*boolean*/, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	NestConst<RseNode>* ptr = clauses.begin();
	NestConst<MapNode>* ptr2 = maps.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr, ++ptr2)
	{
		doPass1(tdbb, csb, ptr->getAddress());
		doPass1(tdbb, csb, ptr2->getAddress());
	}

	jrd_rel* const parentView = csb->csb_view;
	const StreamType viewStream = csb->csb_view_stream;

	CompilerScratch::csb_repeat* const element = CMP_csb_element(csb, stream);
	element->csb_view = parentView;
	element->csb_view_stream = viewStream;
}

// Process a union clause of a RseNode.
RecordSourceNode* UnionSourceNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	// make up a format block sufficiently large to hold instantiated record

	const StreamType id = getStream();
	Format** format = &csb->csb_rpt[id].csb_internal_format;

	// Process RseNodes and map blocks.

	NestConst<RseNode>* ptr = clauses.begin();
	NestConst<MapNode>* ptr2 = maps.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr, ++ptr2)
	{
		(*ptr)->pass2Rse(tdbb, csb);
		ExprNode::doPass2(tdbb, csb, ptr2->getAddress());
		processMap(tdbb, csb, *ptr2, format);
		csb->csb_rpt[id].csb_format = *format;
	}

	if (recursive)
		csb->csb_rpt[mapStream].csb_format = *format;

	return this;
}

void UnionSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	csb->csb_rpt[stream].activate();

	pass2(tdbb, csb);
}

bool UnionSourceNode::containsStream(StreamType checkStream) const
{
	// for unions, check current RseNode, if not found then check
	// all sub-rse's

	if (checkStream == stream)
		return true;		// do not mark as variant

	const NestConst<RseNode>* ptr = clauses.begin();

	for (const NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr)
	{
		if ((*ptr)->containsStream(checkStream))
			return true;
	}

	return false;
}

RecordSource* UnionSourceNode::compile(thread_db* tdbb, Optimizer* opt, bool /*innerSubStream*/)
{
	const auto csb = opt->getCompilerScratch();
	HalfStaticArray<RecordSource*, OPT_STATIC_ITEMS> rsbs;

	const ULONG baseImpure = csb->allocImpure(FB_ALIGNMENT, 0);

	BoolExprNodeStack parentStack;
	for (auto iter = opt->getConjuncts(); iter.hasData(); ++iter)
		parentStack.push(*iter);

	NestConst<RseNode>* ptr = clauses.begin();
	NestConst<MapNode>* ptr2 = maps.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr, ++ptr2)
	{
		RseNode* rse = *ptr;
		MapNode* map = *ptr2;

		// AB: Try to distribute booleans from the top rse for an UNION to
		// the WHERE clause of every single rse.
		// hvlad: don't do it for recursive unions else they will work wrong !
		BoolExprNodeStack deliverStack;
		if (!recursive)
			genDeliverUnmapped(csb, parentStack, deliverStack, map, stream);

		rsbs.add(opt->compile(rse, &deliverStack));

		// hvlad: activate recursive union itself after processing first (non-recursive)
		// member to allow recursive members be optimized
		if (recursive)
			csb->csb_rpt[stream].activate();
	}

	StreamList keyStreams;
	computeDbKeyStreams(keyStreams);

	if (recursive)
	{
		fb_assert(rsbs.getCount() == 2 && maps.getCount() == 2);
		// hvlad: save size of inner impure area and context of mapped record
		// for recursive processing later
		return FB_NEW_POOL(*tdbb->getDefaultPool()) RecursiveStream(csb, stream, mapStream,
			rsbs[0], rsbs[1], maps[0], maps[1], keyStreams, baseImpure);
	}

	return FB_NEW_POOL(*tdbb->getDefaultPool()) Union(csb, stream, clauses.getCount(), rsbs.begin(),
		maps.begin(), keyStreams);
}

// Identify all of the streams for which a dbkey may need to be carried through a sort.
void UnionSourceNode::computeDbKeyStreams(StreamList& streamList) const
{
	const NestConst<RseNode>* ptr = clauses.begin();

	for (const NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr)
		(*ptr)->computeDbKeyStreams(streamList);
}

bool UnionSourceNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	NestConst<RseNode>* ptr = clauses.begin();

	for (NestConst<RseNode>* const end = clauses.end(); ptr != end; ++ptr)
	{
		if (!(*ptr)->computable(csb, stream, allowOnlyCurrentStream, NULL))
			return false;
	}

	return true;
}

void UnionSourceNode::findDependentFromStreams(const CompilerScratch* csb,
	StreamType currentStream, SortedStreamList* streamList)
{
	for (auto clause : clauses)
		clause->findDependentFromStreams(csb, currentStream, streamList);
}


//--------------------


// Parse a window reference.
WindowSourceNode* WindowSourceNode::parse(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	WindowSourceNode* node = FB_NEW_POOL(*tdbb->getDefaultPool()) WindowSourceNode(
		*tdbb->getDefaultPool());

	node->rse = PAR_rse(tdbb, csb);

	unsigned count = csb->csb_blr_reader.getByte();

	for (unsigned i = 0; i < count; ++i)
	{
		switch (csb->csb_blr_reader.getByte())
		{
			case blr_partition_by:
				node->parseLegacyPartitionBy(tdbb, csb);
				break;

			case blr_window_win:
				node->parseWindow(tdbb, csb);
				break;

			default:
				PAR_syntax_error(csb, "blr_window");
				break;
		}
	}

	return node;
}

string WindowSourceNode::internalPrint(NodePrinter& printer) const
{
	RecordSourceNode::internalPrint(printer);

	NODE_PRINT(printer, rse);
	//// FIXME-PRINT: NODE_PRINT(printer, partitions);

	return "WindowSourceNode";
}

// Parse PARTITION BY subclauses of window functions.
void WindowSourceNode::parseLegacyPartitionBy(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	SSHORT context;
	Window& window = windows.add();
	window.stream = PAR_context(csb, &context);

	const UCHAR count = csb->csb_blr_reader.getByte();

	if (count != 0)
	{
		window.group = PAR_sort_internal(tdbb, csb, false, count);
		window.regroup = PAR_sort_internal(tdbb, csb, false, count);
	}

	window.order = PAR_sort(tdbb, csb, blr_sort, true);
	window.map = parseMap(tdbb, csb, window.stream, true);
	window.frameExtent = WindowClause::FrameExtent::createDefault(*tdbb->getDefaultPool());
}

// Parse frame subclauses of window functions.
void WindowSourceNode::parseWindow(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	SSHORT context;
	Window& window = windows.add();
	window.stream = PAR_context(csb, &context);
	window.frameExtent = WindowClause::FrameExtent::createDefault(*tdbb->getDefaultPool());

	UCHAR verb, count;

	while ((verb = csb->csb_blr_reader.getByte()) != blr_end)
	{
		switch (verb)
		{
			case blr_window_win_partition:
				count = csb->csb_blr_reader.getByte();

				if (count != 0)
				{
					window.group = PAR_sort_internal(tdbb, csb, false, count);
					window.regroup = PAR_sort_internal(tdbb, csb, false, count);
				}

				break;

			case blr_window_win_order:
				count = csb->csb_blr_reader.getByte();

				if (count != 0)
					window.order = PAR_sort_internal(tdbb, csb, true, count);

				break;

			case blr_window_win_map:
				window.map = parseMap(tdbb, csb, window.stream, false);
				break;

			case blr_window_win_extent_unit:
				window.frameExtent->unit = (WindowClause::FrameExtent::Unit)
					csb->csb_blr_reader.getByte();

				switch (window.frameExtent->unit)
				{
					case WindowClause::FrameExtent::Unit::RANGE:
					case WindowClause::FrameExtent::Unit::ROWS:
						break;

					default:
						PAR_syntax_error(csb, "blr_window_win_extent_unit");
				}

				break;

			case blr_window_win_exclusion:
				//// TODO: CORE-5338 - write code for execution.
				PAR_error(csb,
					Arg::Gds(isc_wish_list) <<
					Arg::Gds(isc_random) << "window EXCLUDE clause");

				window.exclusion = (WindowClause::Exclusion) csb->csb_blr_reader.getByte();

				switch (window.exclusion)
				{
					case WindowClause::Exclusion::NO_OTHERS:
					case WindowClause::Exclusion::CURRENT_ROW:
					case WindowClause::Exclusion::GROUP:
					case WindowClause::Exclusion::TIES:
						break;

					default:
						PAR_syntax_error(csb, "blr_window_win_exclusion");
				}

				break;

			case blr_window_win_extent_frame_bound:
			case blr_window_win_extent_frame_value:
			{
				UCHAR num = csb->csb_blr_reader.getByte();

				if (num != 1 && num != 2)
				{
					PAR_syntax_error(csb, (verb == blr_window_win_extent_frame_bound ?
						"blr_window_win_extent_frame_bound" : "blr_window_win_extent_frame_value"));
				}

				NestConst<WindowClause::Frame>& frame = num == 1 ?
					window.frameExtent->frame1 : window.frameExtent->frame2;

				switch (verb)
				{
					case blr_window_win_extent_frame_bound:
						frame->bound = (WindowClause::Frame::Bound) csb->csb_blr_reader.getByte();

						switch (frame->bound)
						{
							case WindowClause::Frame::Bound::PRECEDING:
							case WindowClause::Frame::Bound::FOLLOWING:
							case WindowClause::Frame::Bound::CURRENT_ROW:
								break;

							default:
								PAR_syntax_error(csb, "blr_window_win_extent_frame_bound");
						}

						break;

					case blr_window_win_extent_frame_value:
						frame->value = PAR_parse_value(tdbb, csb);
						break;
				}

				break;
			}

			default:
				PAR_syntax_error(csb, "blr_window_win");
				break;
		}
	}
}

WindowSourceNode* WindowSourceNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	if (!copier.remap)
		BUGCHECK(221);		// msg 221 (CMP) copy: cannot remap

	WindowSourceNode* newSource = FB_NEW_POOL(*tdbb->getDefaultPool()) WindowSourceNode(
		*tdbb->getDefaultPool());

	newSource->rse = rse->copy(tdbb, copier);

	for (ObjectsArray<Window>::const_iterator inputWindow = windows.begin();
		 inputWindow != windows.end();
		 ++inputWindow)
	{
		Window& copyWindow = newSource->windows.add();

		copyWindow.stream = copier.csb->nextStream();
		copier.remap[inputWindow->stream] = copyWindow.stream;
		CMP_csb_element(copier.csb, copyWindow.stream);

		if (inputWindow->group)
			copyWindow.group = inputWindow->group->copy(tdbb, copier);

		if (inputWindow->regroup)
			copyWindow.regroup = inputWindow->regroup->copy(tdbb, copier);

		if (inputWindow->order)
			copyWindow.order = inputWindow->order->copy(tdbb, copier);

		if (inputWindow->frameExtent)
			copyWindow.frameExtent = inputWindow->frameExtent->copy(tdbb, copier);

		copyWindow.map = inputWindow->map->copy(tdbb, copier);
		copyWindow.exclusion = inputWindow->exclusion;
	}

	return newSource;
}

RecordSourceNode* WindowSourceNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	doPass1(tdbb, csb, rse.getAddress());

	for (ObjectsArray<Window>::iterator window = windows.begin();
		 window != windows.end();
		 ++window)
	{
		doPass1(tdbb, csb, window->group.getAddress());
		doPass1(tdbb, csb, window->regroup.getAddress());
		doPass1(tdbb, csb, window->order.getAddress());
		doPass1(tdbb, csb, window->frameExtent.getAddress());
		doPass1(tdbb, csb, window->map.getAddress());
	}

	return this;
}

void WindowSourceNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* /*rse*/,
	BoolExprNode** /*boolean*/, RecordSourceNodeStack& stack)
{
	stack.push(this);	// Assume that the source will be used. Push it on the final stream stack.

	pass1(tdbb, csb);

	jrd_rel* const parentView = csb->csb_view;
	const StreamType viewStream = csb->csb_view_stream;

	for (ObjectsArray<Window>::iterator window = windows.begin();
		 window != windows.end();
		 ++window)
	{
		CompilerScratch::csb_repeat* const element = CMP_csb_element(csb, window->stream);
		element->csb_view = parentView;
		element->csb_view_stream = viewStream;
	}
}

RecordSourceNode* WindowSourceNode::pass2(thread_db* tdbb, CompilerScratch* csb)
{
	rse->pass2Rse(tdbb, csb);

	for (ObjectsArray<Window>::iterator window = windows.begin();
		 window != windows.end();
		 ++window)
	{
		ExprNode::doPass2(tdbb, csb, window->map.getAddress());
		ExprNode::doPass2(tdbb, csb, window->group.getAddress());
		ExprNode::doPass2(tdbb, csb, window->order.getAddress());
		ExprNode::doPass2(tdbb, csb, window->frameExtent.getAddress());

		processMap(tdbb, csb, window->map, &csb->csb_rpt[window->stream].csb_internal_format);
		csb->csb_rpt[window->stream].csb_format =
			csb->csb_rpt[window->stream].csb_internal_format;
	}

	for (ObjectsArray<Window>::iterator window = windows.begin();
		 window != windows.end();
		 ++window)
	{
		ExprNode::doPass2(tdbb, csb, window->regroup.getAddress());
	}

	return this;
}

void WindowSourceNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	pass2(tdbb, csb);

	for (ObjectsArray<Window>::iterator window = windows.begin();
		 window != windows.end();
		 ++window)
	{
		csb->csb_rpt[window->stream].activate();
	}
}

bool WindowSourceNode::containsStream(StreamType checkStream) const
{
	for (ObjectsArray<Window>::const_iterator window = windows.begin();
		 window != windows.end();
		 ++window)
	{
		if (checkStream == window->stream)
			return true;		// do not mark as variant
	}

	if (rse->containsStream(checkStream))
		return true;		// do not mark as variant

	return false;
}

void WindowSourceNode::collectStreams(SortedStreamList& streamList) const
{
	for (ObjectsArray<Window>::const_iterator window = windows.begin();
		 window != windows.end();
		 ++window)
	{
		if (!streamList.exist(window->stream))
			streamList.add(window->stream);
	}
}

RecordSource* WindowSourceNode::compile(thread_db* tdbb, Optimizer* opt, bool /*innerSubStream*/)
{
	const auto csb = opt->getCompilerScratch();

	return FB_NEW_POOL(*tdbb->getDefaultPool()) WindowedStream(tdbb, opt,
		windows, opt->compile(rse, NULL));
}

bool WindowSourceNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* /*value*/)
{
	return rse->computable(csb, stream, allowOnlyCurrentStream, NULL);
}

void WindowSourceNode::computeRseStreams(StreamList& streamList) const
{
	for (ObjectsArray<Window>::const_iterator window = windows.begin();
		 window != windows.end();
		 ++window)
	{
		streamList.add(window->stream);
	}
}

void WindowSourceNode::findDependentFromStreams(const CompilerScratch* csb,
	StreamType currentStream, SortedStreamList* streamList)
{
	rse->findDependentFromStreams(csb, currentStream, streamList);
}


//--------------------


string RseNode::internalPrint(NodePrinter& printer) const
{
	RecordSourceNode::internalPrint(printer);

	NODE_PRINT(printer, dsqlFirst);
	NODE_PRINT(printer, dsqlSkip);
	NODE_PRINT(printer, dsqlDistinct);
	NODE_PRINT(printer, dsqlSelectList);
	NODE_PRINT(printer, dsqlFrom);
	NODE_PRINT(printer, dsqlWhere);
	NODE_PRINT(printer, dsqlJoinUsing);
	NODE_PRINT(printer, dsqlGroup);
	NODE_PRINT(printer, dsqlHaving);
	NODE_PRINT(printer, dsqlOrder);
	NODE_PRINT(printer, dsqlStreams);
	NODE_PRINT(printer, dsqlExplicitJoin);
	NODE_PRINT(printer, rse_jointype);
	NODE_PRINT(printer, rse_first);
	NODE_PRINT(printer, rse_skip);
	NODE_PRINT(printer, rse_boolean);
	NODE_PRINT(printer, rse_sorted);
	NODE_PRINT(printer, rse_projection);
	NODE_PRINT(printer, rse_aggregate);
	NODE_PRINT(printer, rse_plan);
	NODE_PRINT(printer, rse_relations);
	NODE_PRINT(printer, flags);

	return "RseNode";
}

bool RseNode::dsqlAggregateFinder(AggregateFinder& visitor)
{
	AutoSetRestore<USHORT> autoValidateExpr(&visitor.currentLevel, visitor.currentLevel + 1);
	return visitor.visit(dsqlStreams) | visitor.visit(dsqlWhere) | visitor.visit(dsqlSelectList);
}

bool RseNode::dsqlAggregate2Finder(Aggregate2Finder& visitor)
{
	AutoSetRestore<bool> autoCurrentScopeLevelEqual(&visitor.currentScopeLevelEqual, false);
	// Pass dsqlWhere, dsqlSelectList and dsqlStreams.
	return visitor.visit(dsqlWhere) | visitor.visit(dsqlSelectList) | visitor.visit(dsqlStreams);
}

bool RseNode::dsqlInvalidReferenceFinder(InvalidReferenceFinder& visitor)
{
	return //// CORE-4807: (flags & FLAG_DSQL_COMPARATIVE) &&
		RecordSourceNode::dsqlInvalidReferenceFinder(visitor);
}

bool RseNode::dsqlSubSelectFinder(SubSelectFinder& visitor)
{
	return !(flags & FLAG_DSQL_COMPARATIVE) || RecordSourceNode::dsqlSubSelectFinder(visitor);
}

bool RseNode::dsqlFieldFinder(FieldFinder& visitor)
{
	// Pass dsqlWhere and dsqlSelectList and dsqlStreams.
	return visitor.visit(dsqlWhere) | visitor.visit(dsqlSelectList) | visitor.visit(dsqlStreams);
}

RseNode* RseNode::dsqlFieldRemapper(FieldRemapper& visitor)
{
	AutoSetRestore<USHORT> autoCurrentLevel(&visitor.currentLevel, visitor.currentLevel +
		(flags & RseNode::FLAG_DSQL_COMPARATIVE ? 0 : 1));

	doDsqlFieldRemapper(visitor, dsqlStreams);
	doDsqlFieldRemapper(visitor, dsqlWhere);
	doDsqlFieldRemapper(visitor, dsqlSelectList);
	doDsqlFieldRemapper(visitor, dsqlOrder);

	return this;
}

bool RseNode::dsqlMatch(DsqlCompilerScratch* dsqlScratch, const ExprNode* other, bool ignoreMapCast) const
{
	const RseNode* o = nodeAs<RseNode>(other);

	if (!o)
		return false;

	// ASF: Commented-out code "Fixed assertion when subquery is used in group by" to make
	// CORE-4084 work again.
	return /***dsqlContext &&***/ dsqlContext == o->dsqlContext &&
		RecordSourceNode::dsqlMatch(dsqlScratch, other, ignoreMapCast);
}

// Make up join node and mark relations as "possibly NULL" if they are in outer joins (inOuterJoin).
RseNode* RseNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	// Set up a new (empty) context to process the joins, but ensure
	// that it includes system (e.g. trigger) contexts (if present),
	// as well as any outer (from other levels) contexts.

	MemoryPool& pool = dsqlScratch->getPool();
	DsqlContextStack* const base_context = dsqlScratch->context;
	DsqlContextStack temp, outer;
	dsqlScratch->context = &temp;

	for (DsqlContextStack::iterator iter(*base_context); iter.hasData(); ++iter)
	{
		if ((iter.object()->ctx_flags & CTX_system) ||
			iter.object()->ctx_scope_level != dsqlScratch->scopeLevel ||
			iter.object() == dsqlScratch->recursiveCtx)	// CORE-4322, CORE-4354
		{
			temp.push(iter.object());
		}
	}

	const size_t visibleContexts = temp.getCount();

	RecSourceListNode* fromList = dsqlFrom;
	RecSourceListNode* streamList = FB_NEW_POOL(pool) RecSourceListNode(
		pool, fromList->items.getCount());

	RseNode* node = FB_NEW_POOL(pool) RseNode(pool);
	node->dsqlExplicitJoin = dsqlExplicitJoin;
	node->rse_jointype = rse_jointype;
	node->dsqlStreams = streamList;

	switch (rse_jointype)
	{
		case blr_inner:
			streamList->items[0] = doDsqlPass(dsqlScratch, fromList->items[0]);
			streamList->items[1] = doDsqlPass(dsqlScratch, fromList->items[1]);
			break;

		case blr_left:
			streamList->items[0] = doDsqlPass(dsqlScratch, fromList->items[0]);
			++dsqlScratch->inOuterJoin;
			streamList->items[1] = doDsqlPass(dsqlScratch, fromList->items[1]);
			--dsqlScratch->inOuterJoin;
			break;

		case blr_right:
			++dsqlScratch->inOuterJoin;
			streamList->items[0] = doDsqlPass(dsqlScratch, fromList->items[0]);
			--dsqlScratch->inOuterJoin;
			// Temporarily remove just created context(s) from the stack,
			// because outer LATERAL references are not allowed in RIGHT/FULL joins
			while (temp.getCount() > visibleContexts)
				outer.push(temp.pop());
			streamList->items[1] = doDsqlPass(dsqlScratch, fromList->items[1]);
			break;

		case blr_full:
			++dsqlScratch->inOuterJoin;
			streamList->items[0] = doDsqlPass(dsqlScratch, fromList->items[0]);
			// Temporarily remove just created context(s) from the stack,
			// because outer LATERAL references are not allowed in RIGHT/FULL joins
			while (temp.getCount() > visibleContexts)
				outer.push(temp.pop());
			streamList->items[1] = doDsqlPass(dsqlScratch, fromList->items[1]);
			--dsqlScratch->inOuterJoin;
			break;

		default:
			fb_assert(false);
			break;
	}

	// Add outer contexts back to the stack
	while (outer.hasData())
		temp.push(outer.pop());

	NestConst<BoolExprNode> boolean = dsqlWhere;
	ValueListNode* usingList = dsqlJoinUsing;

	if (usingList)
	{
		if (dsqlScratch->clientDialect < SQL_DIALECT_V6)
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-901) <<
					  Arg::Gds(isc_dsql_unsupp_feature_dialect) << Arg::Num(dsqlScratch->clientDialect));
		}

		ValueListNode leftStack(pool, 0u);
		ValueListNode rightStack(pool, 0u);

		if (usingList->items.isEmpty())	// NATURAL JOIN
		{
			StrArray leftNames(pool);
			ValueListNode* matched = FB_NEW_POOL(pool) ValueListNode(pool, 0u);

			PASS1_expand_select_node(dsqlScratch, streamList->items[0], &leftStack, true);
			PASS1_expand_select_node(dsqlScratch, streamList->items[1], &rightStack, true);

			// verify columns that exist in both sides
			for (const auto* currentStack : {&leftStack, &rightStack})
			{
				for (auto& item : currentStack->items)
				{
					const TEXT* name = NULL;

					if (auto* aliasNode = nodeAs<DsqlAliasNode>(item))
						name = aliasNode->name.c_str();
					else if (auto* fieldNode = nodeAs<FieldNode>(item))
						name = fieldNode->dsqlField->fld_name.c_str();
					else if (auto* derivedField = nodeAs<DerivedFieldNode>(item))
						name = derivedField->name.c_str();

					if (name)
					{
						if (currentStack == &leftStack)
							leftNames.add(name);
						else	// right
						{
							if (leftNames.exist(name))
								matched->add(MAKE_field_name(name));
						}
					}
				}
			}

			if (matched->items.isEmpty())
			{
				// There is no match. Transform to CROSS JOIN.
				node->rse_jointype = blr_inner;
				usingList = NULL;

				delete matched;
			}
			else
				usingList = matched;	// Transform to USING
		}

		if (usingList)	// JOIN ... USING
		{
			BoolExprNode* newBoolean = NULL;
			StrArray usedColumns(pool);

			for (FB_SIZE_T i = 0; i < usingList->items.getCount(); ++i)
			{
				const FieldNode* field = nodeAs<FieldNode>(usingList->items[i]);

				// verify if the column was already used
				FB_SIZE_T pos;
				if (usedColumns.find(field->dsqlName.c_str(), pos))
				{
					ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
							  Arg::Gds(isc_dsql_col_more_than_once_using) << field->dsqlName);
				}
				else
					usedColumns.insert(pos, field->dsqlName.c_str());

				dsql_ctx* leftCtx = NULL;
				dsql_ctx* rightCtx = NULL;

				// clear the stacks for the next pass
				leftStack.clear();
				rightStack.clear();

				// get the column names from both sides
				PASS1_expand_select_node(dsqlScratch, streamList->items[0], &leftStack, true);
				PASS1_expand_select_node(dsqlScratch, streamList->items[1], &rightStack, true);

				// create the boolean

				ValueExprNode* arg1 = resolveUsingField(dsqlScratch, field->dsqlName, &leftStack,
					field, "left", leftCtx);
				ValueExprNode* arg2 = resolveUsingField(dsqlScratch, field->dsqlName, &rightStack,
					field, "right", rightCtx);

				ComparativeBoolNode* eqlNode = FB_NEW_POOL(pool) ComparativeBoolNode(pool,
					blr_eql, arg1, arg2);

				fb_assert(leftCtx);
				fb_assert(rightCtx);

				// We should hide the (unqualified) column in one side
				ImplicitJoin* impJoinLeft;
				if (!leftCtx->ctx_imp_join.get(field->dsqlName, impJoinLeft))
				{
					impJoinLeft = FB_NEW_POOL(pool) ImplicitJoin();
					impJoinLeft->value = eqlNode->arg1;
					impJoinLeft->visibleInContext = leftCtx;
				}
				else
					fb_assert(impJoinLeft->visibleInContext == leftCtx);

				ImplicitJoin* impJoinRight;
				if (!rightCtx->ctx_imp_join.get(field->dsqlName, impJoinRight))
				{
					impJoinRight = FB_NEW_POOL(pool) ImplicitJoin();
					impJoinRight->value = arg2;
				}
				else
					fb_assert(impJoinRight->visibleInContext == rightCtx);

				// create the COALESCE
				ValueListNode* stack = FB_NEW_POOL(pool) ValueListNode(pool, 0u);

				NestConst<ValueExprNode> tempNode = impJoinLeft->value;
				NestConst<DsqlAliasNode> aliasNode = nodeAs<DsqlAliasNode>(tempNode);
				NestConst<CoalesceNode> coalesceNode;

				if (aliasNode)
					tempNode = aliasNode->value;

				{	// scope
					PsqlChanger changer(dsqlScratch, false);

					if ((coalesceNode = nodeAs<CoalesceNode>(tempNode)))
					{
						ValueListNode* list = coalesceNode->args;

						for (NestConst<ValueExprNode>* ptr = list->items.begin();
							 ptr != list->items.end();
							 ++ptr)
						{
							stack->add(doDsqlPass(dsqlScratch, *ptr));
						}
					}
					else
						stack->add(doDsqlPass(dsqlScratch, tempNode));

					tempNode = impJoinRight->value;

					if ((aliasNode = nodeAs<DsqlAliasNode>(tempNode)))
						tempNode = aliasNode->value;

					if ((coalesceNode = nodeAs<CoalesceNode>(tempNode)))
					{
						ValueListNode* list = coalesceNode->args;

						for (NestConst<ValueExprNode>* ptr = list->items.begin();
							 ptr != list->items.end();
							 ++ptr)
						{
							stack->add(doDsqlPass(dsqlScratch, *ptr));
						}
					}
					else
						stack->add(doDsqlPass(dsqlScratch, tempNode));
				}

				coalesceNode = FB_NEW_POOL(pool) CoalesceNode(pool, stack);

				aliasNode = FB_NEW_POOL(pool) DsqlAliasNode(pool, field->dsqlName, coalesceNode);
				aliasNode->implicitJoin = impJoinLeft;

				impJoinLeft->value = aliasNode;

				impJoinRight->visibleInContext = NULL;

				// both sides should refer to the same ImplicitJoin
				leftCtx->ctx_imp_join.put(field->dsqlName, impJoinLeft);
				rightCtx->ctx_imp_join.put(field->dsqlName, impJoinLeft);

				newBoolean = PASS1_compose(newBoolean, eqlNode, blr_and);
			}

			boolean = newBoolean;
		}
	}

	node->dsqlWhere = doDsqlPass(dsqlScratch, boolean);

	// Merge the newly created contexts with the original ones

	while (temp.getCount() > visibleContexts)
		base_context->push(temp.pop());

	dsqlScratch->context = base_context;

	return node;
}

RseNode* RseNode::copy(thread_db* tdbb, NodeCopier& copier) const
{
	RseNode* newSource = FB_NEW_POOL(*tdbb->getDefaultPool()) RseNode(*tdbb->getDefaultPool());
	newSource->line = line;
	newSource->column = column;

	for (const auto sub : rse_relations)
		newSource->rse_relations.add(sub->copy(tdbb, copier));

	newSource->flags = flags;
	newSource->rse_jointype = rse_jointype;
	newSource->rse_first = copier.copy(tdbb, rse_first);
	newSource->rse_skip = copier.copy(tdbb, rse_skip);

	if (rse_boolean)
		newSource->rse_boolean = copier.copy(tdbb, rse_boolean);

	if (rse_sorted)
		newSource->rse_sorted = rse_sorted->copy(tdbb, copier);

	if (rse_projection)
		newSource->rse_projection = rse_projection->copy(tdbb, copier);

	return newSource;
}

// Process a record select expression during pass 1 of compilation.
// Mostly this involves expanding views.
RseNode* RseNode::pass1(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	// for scoping purposes, maintain a stack of RseNode's which are
	// currently being parsed; if there are none on the stack as
	// yet, mark the RseNode as variant to make sure that statement-
	// level aggregates are not treated as invariants -- bug #6535

	bool topLevelRse = true;

	for (const auto node : csb->csb_current_nodes)
	{
		if (nodeAs<RseNode>(node))
		{
			topLevelRse = false;
			break;
		}
	}

	if (topLevelRse)
		flags |= FLAG_VARIANT;

	csb->csb_current_nodes.push(this);

	RecordSourceNodeStack stack;
	BoolExprNode* boolean = NULL;
	SortNode* sort = rse_sorted;
	SortNode* project = rse_projection;
	ValueExprNode* first = rse_first;
	ValueExprNode* skip = rse_skip;
	PlanNode* plan = rse_plan;

	if (rse_jointype == blr_inner)
		csb->csb_inner_booleans.push(rse_boolean);

	// zip thru RseNode expanding views and inner joins
	for (auto sub : rse_relations)
		processSource(tdbb, csb, this, sub, &boolean, stack);

	if (rse_jointype == blr_inner)
		csb->csb_inner_booleans.pop();

	// Now, rebuild the RseNode block.

	rse_relations.resize(stack.getCount());
	auto arg = rse_relations.end();

	while (stack.hasData())
		*--arg = stack.pop();

	AutoSetRestore<bool> autoValidateExpr(&csb->csb_validate_expr, false);

	// finish of by processing other clauses

	if (first)
	{
		doPass1(tdbb, csb, &first);
		rse_first = first;
	}

	if (skip)
	{
		doPass1(tdbb, csb, &skip);
		rse_skip = skip;
	}

	if (boolean)
	{
		if (rse_boolean)
		{
			BinaryBoolNode* andNode = FB_NEW_POOL(csb->csb_pool) BinaryBoolNode(csb->csb_pool, blr_and);
			andNode->arg1 = boolean;
			andNode->arg2 = rse_boolean;

			doPass1(tdbb, csb, andNode->arg2.getAddress());

			rse_boolean = andNode;
		}
		else
			rse_boolean = boolean;
	}
	else if (rse_boolean)
		doPass1(tdbb, csb, rse_boolean.getAddress());

	if (sort)
	{
		doPass1(tdbb, csb, &sort);
		rse_sorted = sort;
	}

	if (project)
	{
		doPass1(tdbb, csb, &project);
		rse_projection = project;
	}

	if (plan)
		rse_plan = plan;

	// we are no longer in the scope of this RseNode
	csb->csb_current_nodes.pop();

	return this;
}

void RseNode::pass1Source(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	BoolExprNode** boolean, RecordSourceNodeStack& stack)
{
	if (rse_jointype != blr_inner)
	{
		// Check whether any of the upper level booleans (those belonging to the WHERE clause)
		// is able to filter out rows from the "inner" streams. If this is the case,
		// transform the join type accordingly (LEFT -> INNER, FULL -> LEFT or INNER).

		fb_assert(rse_relations.getCount() == 2);

		const auto rse1 = rse_relations[0];
		const auto rse2 = rse_relations[1];
		fb_assert(rse1 && rse2);

		StreamList streams;

		// First check the left stream of the full outer join
		if (rse_jointype == blr_full)
		{
			rse1->computeRseStreams(streams);

			for (const auto boolean : csb->csb_inner_booleans)
			{
				if (boolean && boolean->ignoreNulls(streams))
				{
					rse_jointype = blr_left;
					break;
				}
			}
		}

		// Then check the right stream of both left and full outer joins
		streams.clear();
		rse2->computeRseStreams(streams);

		for (const auto boolean : csb->csb_inner_booleans)
		{
			if (boolean && boolean->ignoreNulls(streams))
			{
				if (rse_jointype == blr_full)
				{
					// We should transform FULL join to RIGHT join,
					// but as we don't allow them inside the engine
					// just swap the sides and insist it's LEFT join
					std::swap(rse_relations[0], rse_relations[1]);
					rse_jointype = blr_left;
				}
				else
					rse_jointype = blr_inner;

				break;
			}
		}
	}

	// in the case of an RseNode, it is possible that a new RseNode will be generated,
	// so wait to process the source before we push it on the stack (bug 8039)

	// The addition of the JOIN syntax for specifying inner joins causes an
	// RseNode tree to be generated, which is undesirable in the simplest case
	// where we are just trying to inner join more than 2 streams. If possible,
	// try to flatten the tree out before we go any further.

	if (!isLateral() &&
		rse->rse_jointype == blr_inner &&
		rse_jointype == blr_inner &&
		!rse_sorted && !rse_projection &&
		!rse_first && !rse_skip && !rse_plan)
	{
		for (auto sub : rse_relations)
			processSource(tdbb, csb, rse, sub, boolean, stack);

		// fold in the boolean for this inner join with the one for the parent

		if (rse_boolean)
		{
			BoolExprNode* node = rse_boolean;
			doPass1(tdbb, csb, &node);

			if (*boolean)
			{
				BinaryBoolNode* andNode = FB_NEW_POOL(csb->csb_pool) BinaryBoolNode(csb->csb_pool, blr_and);
				andNode->arg1 = node;
				andNode->arg2 = *boolean;

				*boolean = andNode;
			}
			else
				*boolean = node;
		}

		return;
	}

	pass1(tdbb, csb);
	stack.push(this);
}

// Perform the first half of record selection expression compilation.
// The actual optimization is done in "post_rse".
void RseNode::pass2Rse(thread_db* tdbb, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	// Maintain stack of RSEe for scoping purposes
	csb->csb_current_nodes.push(this);

	if (rse_first)
		ExprNode::doPass2(tdbb, csb, rse_first.getAddress());

	if (rse_skip)
	    ExprNode::doPass2(tdbb, csb, rse_skip.getAddress());

	for (auto sub : rse_relations)
		sub->pass2Rse(tdbb, csb);

	ExprNode::doPass2(tdbb, csb, rse_boolean.getAddress());
	ExprNode::doPass2(tdbb, csb, rse_sorted.getAddress());
	ExprNode::doPass2(tdbb, csb, rse_projection.getAddress());

	// If the user has submitted a plan for this RseNode, check it for correctness.

	if (rse_plan)
	{
		planSet(csb, rse_plan);
		planCheck(csb);
	}

	csb->csb_current_nodes.pop();
}

// Return true if stream is contained in the specified RseNode.
bool RseNode::containsStream(StreamType checkStream) const
{
	// Look through all relation nodes in this RseNode to see
	// if the field references this instance of the relation.

	for (const auto sub : rse_relations)
	{
		if (sub->containsStream(checkStream))
			return true;		// do not mark as variant
	}

	return false;
}

RecordSource* RseNode::compile(thread_db* tdbb, Optimizer* opt, bool innerSubStream)
{
	// For nodes which are not relations, generate an rsb to
	// represent that work has to be done to retrieve them;
	// find all the substreams involved and compile them as well

	const auto csb = opt->getCompilerScratch();

	BoolExprNodeStack conjunctStack;

	// pass RseNode boolean only to inner substreams because join condition
	// should never exclude records from outer substreams
	if (opt->isInnerJoin() || (opt->isLeftJoin() && innerSubStream))
	{
		// AB: For an (X LEFT JOIN Y) mark the outer-streams (X) as
		// active because the inner-streams (Y) are always "dependent"
		// on the outer-streams. So that index retrieval nodes could be made.
		//
		// dimitr: the same for lateral derived tables in inner joins

		StreamStateHolder stateHolder(csb, opt->getOuterStreams());

		if (opt->isLeftJoin() || isLateral())
		{
			stateHolder.activate();

			if (opt->isLeftJoin())
			{
				// Push all conjuncts except "missing" ones (e.g. IS NULL)
				for (auto iter = opt->getConjuncts(false, true); iter.hasData(); ++iter)
					conjunctStack.push(iter);
			}
		}
		else
		{
			for (auto iter = opt->getConjuncts(); iter.hasData(); ++iter)
				conjunctStack.push(iter);
		}

		return opt->compile(this, &conjunctStack);
	}

	// Push only parent conjuncts to the outer stream
	for (auto iter = opt->getConjuncts(true, false); iter.hasData(); ++iter)
		conjunctStack.push(iter);

	return opt->compile(this, &conjunctStack);
}

// Check that all streams in the RseNode have a plan specified for them.
// If they are not, there are streams in the RseNode which were not mentioned in the plan.
void RseNode::planCheck(const CompilerScratch* csb) const
{
	// if any streams are not marked with a plan, give an error

	for (const auto node : rse_relations)
	{
		if (nodeIs<RelationSourceNode>(node) || nodeIs<ProcedureSourceNode>(node))
		{
			const auto stream = node->getStream();

			const auto relation = csb->csb_rpt[stream].csb_relation;
			const auto procedure = csb->csb_rpt[stream].csb_procedure;
			fb_assert(relation || procedure);

			if (!csb->csb_rpt[stream].csb_plan)
			{
				const auto name = relation ? relation->rel_name.toQuotedString() :
					procedure ? procedure->getName().toQuotedString() : "";

				ERR_post(Arg::Gds(isc_no_stream_plan) << Arg::Str(name));
			}
		}
		else if (const auto rse = nodeAs<RseNode>(node))
			rse->planCheck(csb);
	}
}

// Go through the streams in the plan, find the corresponding streams in the RseNode and store the
// plan for that stream. Do it once and only once to make sure there is a one-to-one correspondence
// between streams in the query and streams in the plan.
void RseNode::planSet(CompilerScratch* csb, PlanNode* plan)
{
	if (plan->type == PlanNode::TYPE_JOIN)
	{
		for (auto planNode : plan->subNodes)
			planSet(csb, planNode);
	}

	if (plan->type != PlanNode::TYPE_RETRIEVE)
		return;

	// Find the tail for the relation/procedure specified in the plan

	const auto stream = plan->recordSourceNode->getStream();
	auto tail = &csb->csb_rpt[stream];

	string planAlias;

	jrd_rel* planRelation = nullptr;
	if (const auto relationNode = nodeAs<RelationSourceNode>(plan->recordSourceNode))
	{
		planRelation = relationNode->relation;
		planAlias = relationNode->alias;
	}

	jrd_prc* planProcedure = nullptr;
	if (const auto procedureNode = nodeAs<ProcedureSourceNode>(plan->recordSourceNode))
	{
		planProcedure = procedureNode->procedure;
		planAlias = procedureNode->alias;
	}

	fb_assert(planRelation || planProcedure);

	const auto name = planRelation ? planRelation->rel_name :
		planProcedure ? planProcedure->getName() :
		QualifiedName();

	// If the plan references a view, find the real base relation
	// we are interested in by searching the view map
	StreamType* map = nullptr;
	jrd_rel* viewRelation = nullptr;
	jrd_prc* viewProcedure = nullptr;

	if (tail->csb_map)
	{
		auto tailName = tail->csb_relation ? tail->csb_relation->rel_name :
			tail->csb_procedure ? tail->csb_procedure->getName() :
			QualifiedName();

		// If the user has specified an alias, skip past it to find the alias
		// for the base table (if multiple aliases are specified)

		auto tailAlias = tail->csb_alias ? *tail->csb_alias : "";

		if (planAlias.hasData())
		{
			const auto spacePos = planAlias.find_first_of(' ');
			const auto subAlias = planAlias.substr(0, spacePos);	// FIXME:

			if (tailName.object == subAlias || tailAlias == subAlias)	// FIXME:
			{
				planAlias = planAlias.substr(spacePos);
				planAlias.ltrim();
			}
		}

		// Loop through potentially a stack of views to find the appropriate base table
		StreamType* mapBase;
		while ( (mapBase = tail->csb_map) )
		{
			map = mapBase;
			tail = &csb->csb_rpt[*map];
			viewRelation = tail->csb_relation;
			viewProcedure = tail->csb_procedure;

			// If the plan references the view itself, make sure that
			// the view is on a single table. If it is, fix up the plan
			// to point to the base relation.

			if ((viewRelation && planRelation &&
				viewRelation->rel_id == planRelation->rel_id) ||
				(viewProcedure && planProcedure &&
				viewProcedure->getId() == planProcedure->getId()))
			{
				if (!mapBase[2])
				{
					map++;
					tail = &csb->csb_rpt[*map];
				}
				else
				{
					// view %s has more than one base relation; use aliases to distinguish
					ERR_post(Arg::Gds(isc_view_alias) << name.toQuotedString());
				}

				break;
			}

			viewRelation = nullptr;
			viewProcedure = nullptr;

			// If the user didn't specify an alias (or didn't specify one
			// for this level), check to make sure there is one and only one
			// base relation in the table which matches the plan relation

			if (planAlias.isEmpty())
			{
				auto duplicateMap = mapBase;
				QualifiedName duplicateName;

				map = nullptr;

				for (duplicateMap++; *duplicateMap; ++duplicateMap)
				{
					const auto duplicateTail = &csb->csb_rpt[*duplicateMap];
					const auto relation = duplicateTail->csb_relation;
					const auto procedure = duplicateTail->csb_procedure;

					if ((relation && planRelation &&
						relation->rel_id == planRelation->rel_id) ||
						(procedure && planProcedure &&
						procedure->getId() == planProcedure->getId()))
					{
						if (duplicateName.object.hasData())
						{
							// table %s is referenced twice in view; use an alias to distinguish
							ERR_post(Arg::Gds(isc_duplicate_base_table) <<
								duplicateName.toQuotedString());
						}
						else
						{
							duplicateName =
								relation ? relation->rel_name :
								procedure ? procedure->getName() :
								QualifiedName();

							map = duplicateMap;
							tail = duplicateTail;
						}
					}
				}

				break;
			}

			// Look through all the base relations for a match

			map = mapBase;
			for (map++; *map; map++)
			{
				tail = &csb->csb_rpt[*map];

				tailName = tail->csb_relation ? tail->csb_relation->rel_name :
					tail->csb_procedure ? tail->csb_procedure->getName() : QualifiedName();

				// Match the user-supplied alias with the alias supplied
				// with the view definition. Failing that, try the base
				// table name itself.

				tailAlias = tail->csb_alias ? *tail->csb_alias : "";

				const auto spacePos = planAlias.find_first_of(' ');
				const auto subAlias = planAlias.substr(0, spacePos);	// FIXME:

				if (tailName.object == subAlias || tailAlias == subAlias)	// FIXME:
				{
					// Skip past the alias
					planAlias = planAlias.substr(spacePos);
					planAlias.ltrim();
					break;
				}
			}

			if (!*map)
			{
				// table or procedure %s is referenced in the plan but not the from list
				ERR_post(Arg::Gds(isc_stream_not_found) << name.toQuotedString());
			}
		}

		// Fix up the relation node to point to the base relation's stream

		if (!map || !*map)
		{
			// table or procedure %s is referenced in the plan but not the from list
			ERR_post(Arg::Gds(isc_stream_not_found) << name.toQuotedString());
		}

		plan->recordSourceNode->setStream(*map);
	}

	// Make some validity checks

	if (!tail->csb_relation && !tail->csb_procedure)
	{
		// table or procedure %s is referenced in the plan but not the from list
		ERR_post(Arg::Gds(isc_stream_not_found) << name.toQuotedString());
	}

	if ((tail->csb_relation && planRelation &&
		tail->csb_relation->rel_id != planRelation->rel_id && !viewRelation) ||
		(tail->csb_procedure && planProcedure &&
		tail->csb_procedure->getId() != planProcedure->getId() && !viewProcedure))
	{
		// table or procedure %s is referenced in the plan but not the from list
		ERR_post(Arg::Gds(isc_stream_not_found) << name.toQuotedString());
	}

	// Check if we already have a plan for this stream

	if (tail->csb_plan)
	{
		// table or procedure %s is referenced more than once in plan; use aliases to distinguish
		ERR_post(Arg::Gds(isc_stream_twice) << name.toQuotedString());
	}

	tail->csb_plan = plan;
}

void RseNode::computeDbKeyStreams(StreamList& streamList) const
{
	for (const auto sub : rse_relations)
		sub->computeDbKeyStreams(streamList);
}

void RseNode::computeRseStreams(StreamList& streamList) const
{
	for (const auto sub : rse_relations)
		sub->computeRseStreams(streamList);
}

bool RseNode::computable(CompilerScratch* csb, StreamType stream,
	bool allowOnlyCurrentStream, ValueExprNode* value)
{
	if (rse_first && !rse_first->computable(csb, stream, allowOnlyCurrentStream))
		return false;

	if (rse_skip && !rse_skip->computable(csb, stream, allowOnlyCurrentStream))
		return false;

	// Set sub-streams of rse active
	AutoActivateResetStreams activator(csb, this);

	// Check sub-stream
	if ((rse_boolean && !rse_boolean->computable(csb, stream, allowOnlyCurrentStream)) ||
	    (rse_sorted && !rse_sorted->computable(csb, stream, allowOnlyCurrentStream)) ||
	    (rse_projection && !rse_projection->computable(csb, stream, allowOnlyCurrentStream)))
	{
		return false;
	}

	for (auto sub : rse_relations)
	{
		if (!sub->computable(csb, stream, allowOnlyCurrentStream, NULL))
			return false;
	}

	// Check value expression, if any
	if (value && !value->computable(csb, stream, allowOnlyCurrentStream))
		return false;

	return true;
}

void RseNode::findDependentFromStreams(const CompilerScratch* csb,
	StreamType currentStream, SortedStreamList* streamList)
{
	if (rse_first)
		rse_first->findDependentFromStreams(csb, currentStream, streamList);

	if (rse_skip)
		rse_skip->findDependentFromStreams(csb, currentStream, streamList);

	if (rse_boolean)
		rse_boolean->findDependentFromStreams(csb, currentStream, streamList);

	if (rse_sorted)
		rse_sorted->findDependentFromStreams(csb, currentStream, streamList);

	if (rse_projection)
		rse_projection->findDependentFromStreams(csb, currentStream, streamList);

	for (auto sub : rse_relations)
		sub->findDependentFromStreams(csb, currentStream, streamList);
}

void RseNode::collectStreams(SortedStreamList& streamList) const
{
	if (rse_first)
		rse_first->collectStreams(streamList);

	if (rse_skip)
		rse_skip->collectStreams(streamList);

	if (rse_boolean)
		rse_boolean->collectStreams(streamList);

	// ASF: The legacy code used to visit rse_sorted and rse_projection, but the nod_sort was never
	// handled.
	// rse_sorted->collectStreams(streamList);
	// rse_projection->collectStreams(streamList);

	for (const auto sub : rse_relations)
		sub->collectStreams(streamList);
}


//--------------------


string SelectExprNode::internalPrint(NodePrinter& printer) const
{
	RecordSourceNode::internalPrint(printer);

	NODE_PRINT(printer, querySpec);
	NODE_PRINT(printer, orderClause);
	NODE_PRINT(printer, rowsClause);
	NODE_PRINT(printer, withClause);
	NODE_PRINT(printer, alias);
	NODE_PRINT(printer, columns);

	return "SelectExprNode";
}

RseNode* SelectExprNode::dsqlPass(DsqlCompilerScratch* dsqlScratch)
{
	fb_assert(dsqlFlags & DFLAG_DERIVED);
	return PASS1_derived_table(dsqlScratch, this, NULL);
}


//--------------------


static RecordSourceNode* dsqlPassRelProc(DsqlCompilerScratch* dsqlScratch, RecordSourceNode* source)
{
	bool couldBeCte = true;
	QualifiedName relName;
	string relAlias;

	if (const auto procNode = nodeAs<ProcedureSourceNode>(source))
	{
		relName = procNode->dsqlName;
		relAlias = procNode->alias;
		couldBeCte = !procNode->inputSources && procNode->dsqlName.package.isEmpty();
	}
	else if (const auto relNode = nodeAs<RelationSourceNode>(source))
	{
		relName = relNode->dsqlName;
		relAlias = relNode->alias;
	}
	//// TODO: LocalTableSourceNode
	else
		fb_assert(false);

	couldBeCte = couldBeCte && relName.schema.isEmpty() && relName.package.isEmpty();

	if (relAlias.isEmpty())
		relAlias = relName.object.c_str();

	SelectExprNode* cte = couldBeCte ? dsqlScratch->findCTE(relName.object) : NULL;

	if (!cte)
		return PASS1_relation(dsqlScratch, source);

	cte->dsqlFlags |= RecordSourceNode::DFLAG_DT_CTE_USED;

	if ((dsqlScratch->flags & DsqlCompilerScratch::FLAG_RECURSIVE_CTE) &&
		 dsqlScratch->currCtes.hasData() &&
		 (dsqlScratch->currCtes.object() == cte))
	{
		ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
				  // Recursive CTE member (%s) can refer itself only in FROM clause
				  Arg::Gds(isc_dsql_cte_wrong_reference) << relName.toQuotedString());
	}

	for (Stack<SelectExprNode*>::const_iterator stack(dsqlScratch->currCtes); stack.hasData(); ++stack)
	{
		SelectExprNode* cte1 = stack.object();
		if (cte1 == cte)
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
					  // CTE %s has cyclic dependencies
					  Arg::Gds(isc_dsql_cte_cycle) << relName.toQuotedString());
		}
	}

	RecordSourceNode* const query = cte->querySpec;
	UnionSourceNode* unionQuery = nodeAs<UnionSourceNode>(query);
	const bool isRecursive = unionQuery && unionQuery->recursive;

	const string saveCteName = cte->alias;
	if (!isRecursive)
		cte->alias = relAlias;

	dsqlScratch->currCtes.push(cte);

	RseNode* derivedNode = PASS1_derived_table(dsqlScratch,
		cte, (isRecursive ? relAlias.c_str() : NULL));

	if (!isRecursive)
		cte->alias = saveCteName;

	dsqlScratch->currCtes.pop();

	return derivedNode;
}

// Parse a MAP clause for a union or global aggregate expression.
static MapNode* parseMap(thread_db* tdbb, CompilerScratch* csb, StreamType stream,
	bool parseHeader)
{
	SET_TDBB(tdbb);

	if (parseHeader)
	{
		if (csb->csb_blr_reader.getByte() != blr_map)
			PAR_syntax_error(csb, "blr_map");
	}

	unsigned int count = csb->csb_blr_reader.getWord();
	MapNode* node = FB_NEW_POOL(csb->csb_pool) MapNode(csb->csb_pool);

	while (count-- > 0)
	{
		node->targetList.add(PAR_gen_field(tdbb, stream, csb->csb_blr_reader.getWord()));
		node->sourceList.add(PAR_parse_value(tdbb, csb));
	}

	return node;
}

// Process a single record source stream from an RseNode.
// Obviously, if the source is a view, there is more work to do.
static void processSource(thread_db* tdbb, CompilerScratch* csb, RseNode* rse,
	RecordSourceNode* source, BoolExprNode** boolean, RecordSourceNodeStack& stack)
{
	SET_TDBB(tdbb);

	Database* dbb = tdbb->getDatabase();
	CHECK_DBB(dbb);

	AutoSetRestore<bool> autoValidateExpr(&csb->csb_validate_expr, false);

	source->pass1Source(tdbb, csb, rse, boolean, stack);
}

// Translate a map block into a format. If the format is missing or incomplete, extend it.
static void processMap(thread_db* tdbb, CompilerScratch* csb, MapNode* map, Format** inputFormat)
{
	SET_TDBB(tdbb);

	Format* format = *inputFormat;
	if (!format)
		format = *inputFormat = Format::newFormat(*tdbb->getDefaultPool(), map->sourceList.getCount());

	// process alternating rse and map blocks
	dsc desc2;
	NestConst<ValueExprNode>* source = map->sourceList.begin();
	NestConst<ValueExprNode>* target = map->targetList.begin();

	for (const NestConst<ValueExprNode>* const sourceEnd = map->sourceList.end();
		 source != sourceEnd;
		 ++source, ++target)
	{
		FieldNode* field = nodeAs<FieldNode>(*target);
		const USHORT id = field->fieldId;

		if (id >= format->fmt_count)
			format->fmt_desc.resize(id + 1);

		dsc* desc = &format->fmt_desc[id];
		(*source)->getDesc(tdbb, csb, &desc2);
		const USHORT min = MIN(desc->dsc_dtype, desc2.dsc_dtype);
		const USHORT max = MAX(desc->dsc_dtype, desc2.dsc_dtype);

		if (!min)	// eg: dtype_unknown
			*desc = desc2;
		else if (max == dtype_blob)
		{
			USHORT subtype = DataTypeUtil::getResultBlobSubType(desc, &desc2);
			USHORT ttype = DataTypeUtil::getResultTextType(desc, &desc2);
			desc->makeBlob(subtype, ttype);
		}
		else if (min <= dtype_any_text)
		{
			// either field a text field?
			const USHORT len1 = DSC_string_length(desc);
			const USHORT len2 = DSC_string_length(&desc2);
			desc->dsc_dtype = dtype_varying;
			desc->dsc_length = MAX(len1, len2) + sizeof(USHORT);

			// pick the max text type, so any transparent casts from ints are
			// not left in ASCII format, but converted to the richer text format

			desc->setTextType(MAX(INTL_TEXT_TYPE(*desc), INTL_TEXT_TYPE(desc2)));
			desc->dsc_scale = 0;
			desc->dsc_flags = 0;
		}
		else if (DTYPE_IS_DATE(max) && !DTYPE_IS_DATE(min))
		{
			desc->dsc_dtype = dtype_varying;
			desc->dsc_length = DSC_convert_to_text_length(max) + sizeof(USHORT);
			desc->dsc_ttype() = ttype_ascii;
			desc->dsc_scale = 0;
			desc->dsc_flags = 0;
		}
		else if (max != min)
		{
			// different numeric types: if one is inexact use double,
			// if both are exact use int64
			if ((!DTYPE_IS_EXACT(max)) || (!DTYPE_IS_EXACT(min)))
			{
				desc->dsc_dtype = DEFAULT_DOUBLE;
				desc->dsc_length = sizeof(double);
				desc->dsc_scale = 0;
				desc->dsc_sub_type = 0;
				desc->dsc_flags = 0;
			}
			else
			{
				desc->dsc_dtype = dtype_int64;
				desc->dsc_length = sizeof(SINT64);
				desc->dsc_scale = MIN(desc->dsc_scale, desc2.dsc_scale);
				desc->dsc_sub_type = MAX(desc->dsc_sub_type, desc2.dsc_sub_type);
				desc->dsc_flags = 0;
			}
		}
	}

	// flesh out the format of the record

	ULONG offset = FLAG_BYTES(format->fmt_count);

	Format::fmt_desc_iterator desc3 = format->fmt_desc.begin();
	for (const Format::fmt_desc_const_iterator end_desc = format->fmt_desc.end();
		 desc3 < end_desc; ++desc3)
	{
		const USHORT align = type_alignments[desc3->dsc_dtype];

		if (align)
			offset = FB_ALIGN(offset, align);

		desc3->dsc_address = (UCHAR*)(IPTR) offset;
		offset += desc3->dsc_length;
	}

	format->fmt_length = offset;
	format->fmt_count = format->fmt_desc.getCount();
}

// Make new boolean nodes from nodes that contain a field from the given shellStream.
// Those fields are references (mappings) to other nodes and are used by aggregates and unions.
static void genDeliverUnmapped(CompilerScratch* csb,
							   const BoolExprNodeStack& conjunctStack,
							   BoolExprNodeStack& deliverStack,
							   MapNode* map,
							   StreamType shellStream)
{
	MemoryPool& pool = csb->csb_pool;

	for (BoolExprNodeStack::const_iterator iter(conjunctStack); iter.hasData(); ++iter)
	{
		const auto boolean = iter.object();

		// Handle the "OR" case first

		const auto binaryNode = nodeAs<BinaryBoolNode>(boolean);
		if (binaryNode && binaryNode->blrOp == blr_or)
		{
			BoolExprNodeStack orgStack, newStack;

			orgStack.push(binaryNode->arg1);
			orgStack.push(binaryNode->arg2);

			genDeliverUnmapped(csb, orgStack, newStack, map, shellStream);

			if (newStack.getCount() == 2)
			{
				const auto newArg2 = newStack.pop();
				const auto newArg1 = newStack.pop();

				const auto newBinaryNode =
					FB_NEW_POOL(pool) BinaryBoolNode(pool, blr_or, newArg1, newArg2);

				deliverStack.push(newBinaryNode);
			}
			else
			{
				while (newStack.hasData())
					delete newStack.pop();
			}

			continue;
		}

		// Reduce to simple comparisons

		const auto cmpNode = nodeAs<ComparativeBoolNode>(boolean);
		const auto missingNode = nodeAs<MissingBoolNode>(boolean);
		const auto listNode = nodeAs<InListBoolNode>(boolean);
		HalfStaticArray<ValueExprNode*, 2> children;

		if (cmpNode &&
			(cmpNode->blrOp == blr_eql || cmpNode->blrOp == blr_equiv ||
			 cmpNode->blrOp == blr_gtr || cmpNode->blrOp == blr_geq ||
			 cmpNode->blrOp == blr_leq || cmpNode->blrOp == blr_lss ||
			 cmpNode->blrOp == blr_starting))
		{
			children.add(cmpNode->arg1);
			children.add(cmpNode->arg2);
		}
		else if (listNode)
		{
			children.add(listNode->arg);
			for (auto item : listNode->list->items)
				children.add(item);
		}
		else if (missingNode)
			children.add(missingNode->arg);
		else
			continue;

		// At least 1 mapping should be used in the arguments
		FB_SIZE_T indexArg;
		bool mappingFound = false;

		for (indexArg = 0; (indexArg < children.getCount()) && !mappingFound; ++indexArg)
		{
			const auto fieldNode = nodeAs<FieldNode>(children[indexArg]);

			if (fieldNode && fieldNode->fieldStream == shellStream)
				mappingFound = true;
		}

		if (!mappingFound)
			continue;

		// Create new node and assign the correct existing arguments

		AutoPtr<BoolExprNode> deliverNode;
		HalfStaticArray<ValueExprNode**, 2> newChildren;

		if (cmpNode)
		{
			const auto newCmpNode =
				FB_NEW_POOL(pool) ComparativeBoolNode(pool, cmpNode->blrOp);

			newChildren.add(newCmpNode->arg1.getAddress());
			newChildren.add(newCmpNode->arg2.getAddress());

			deliverNode = newCmpNode;
		}
		else if (listNode)
		{
			const auto newListNode = FB_NEW_POOL(pool) InListBoolNode(pool);
			const auto count = listNode->list->items.getCount();
			newListNode->list = FB_NEW_POOL(pool) ValueListNode(pool, count);

			newChildren.add(newListNode->arg.getAddress());
			for (auto& item : newListNode->list->items)
				newChildren.add(item.getAddress());

			deliverNode = newListNode;
		}
		else if (missingNode)
		{
			const auto newMissingNode = FB_NEW_POOL(pool) MissingBoolNode(pool);

			newChildren.add(newMissingNode->arg.getAddress());

			deliverNode = newMissingNode;
		}

		deliverNode->nodFlags = boolean->nodFlags;
		deliverNode->impureOffset = boolean->impureOffset;

		bool okNode = true;

		for (indexArg = 0; (indexArg < children.getCount()) && okNode; ++indexArg)
		{
			// Check if node is a mapping and if so unmap it, but only for root nodes (not contained
			// in another node). This can be expanded by checking complete expression (Then don't
			// forget to leave aggregate-functions alone in case of aggregate rse).
			// Because this is only to help using an index we keep it simple.

			const auto fieldNode = nodeAs<FieldNode>(children[indexArg]);

			if (fieldNode && fieldNode->fieldStream == shellStream)
			{
				const auto fieldId = fieldNode->fieldId;

				if (fieldId >= map->sourceList.getCount())
					okNode = false;
				else
				{
					// Check also the expression inside the map, because aggregate
					// functions aren't allowed to be delivered to the WHERE clause.
					const auto value = map->sourceList[fieldId];
					okNode = value->unmappable(map, shellStream);

					if (okNode)
						*newChildren[indexArg] = map->sourceList[fieldId];
				}
			}
			else
			{
				if ((okNode = children[indexArg]->unmappable(map, shellStream)))
					*newChildren[indexArg] = children[indexArg];
			}
		}

		if (okNode)
		{
			const auto node = deliverNode.release();

			if (const auto newListNode = nodeAs<InListBoolNode>(node))
			{
				newListNode->lookup = FB_NEW_POOL(pool)
					LookupValueList(pool, newListNode->list, newListNode->impureOffset);
			}

			deliverStack.push(node);
		}
	}
}

// Resolve a field for JOIN USING purposes.
static ValueExprNode* resolveUsingField(DsqlCompilerScratch* dsqlScratch, const MetaName& name,
	ValueListNode* list, const FieldNode* flawedNode, const TEXT* side, dsql_ctx*& ctx)
{
	ValueExprNode* node = PASS1_lookup_alias(dsqlScratch, name, list, false);

	if (!node)
	{
		string qualifier;
		qualifier.printf("<%s side of USING>", side);
		PASS1_field_unknown(qualifier.c_str(), name.toQuotedString().c_str(), flawedNode);
	}

	DsqlAliasNode* aliasNode;
	FieldNode* fieldNode;
	DerivedFieldNode* derivedField;

	if ((aliasNode = nodeAs<DsqlAliasNode>(node)))
		ctx = aliasNode->implicitJoin->visibleInContext;
	else if ((fieldNode = nodeAs<FieldNode>(node)))
		ctx = fieldNode->dsqlContext;
	else if ((derivedField = nodeAs<DerivedFieldNode>(node)))
		ctx = derivedField->context;
	else
		fb_assert(false);

	return node;
}
