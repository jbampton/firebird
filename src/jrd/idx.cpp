/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		idx.cpp
 *	DESCRIPTION:	Index manager
 *
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
 *
 * 2003.03.04 Dmitry Yemanov: Added support for NULLs in unique indices.
 *							  Done in two stages:
 *								1. Restored old behaviour of having only _one_
 *								   NULL key allowed (i.e. two NULLs are considered
 *								   duplicates). idx_e_nullunique error was removed.
 *								2. Changed algorithms in IDX_create_index() and
 *								   check_duplicates() to ignore NULL key duplicates.
 */

#include "firebird.h"
#include <string.h>
#include "../jrd/jrd.h"
#include "../jrd/val.h"
#include "../jrd/intl.h"
#include "../jrd/req.h"
#include "../jrd/ods.h"
#include "../jrd/btr.h"
#include "../jrd/sort.h"
#include "../jrd/lls.h"
#include "../jrd/tra.h"
#include "iberror.h"
#include "../jrd/sbm.h"
#include "../jrd/exe.h"
#include "../jrd/scl.h"
#include "../jrd/lck.h"
#include "../jrd/cch.h"
#include "../common/gdsassert.h"
#include "../jrd/btr_proto.h"
#include "../jrd/cch_proto.h"
#include "../jrd/cmp_proto.h"
#include "../jrd/dpm_proto.h"
#include "../jrd/err_proto.h"
#include "../jrd/evl_proto.h"
#include "../yvalve/gds_proto.h"
#include "../jrd/idx_proto.h"
#include "../jrd/intl_proto.h"
#include "../jrd/jrd_proto.h"
#include "../jrd/lck_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/vio_proto.h"
#include "../jrd/tra_proto.h"
#include "../jrd/Collation.h"
#include "../common/Task.h"
#include "../jrd/WorkerAttachment.h"

using namespace Jrd;
using namespace Ods;
using namespace Firebird;

static idx_e check_duplicates(thread_db*, Record*, index_desc*, index_insertion*, jrd_rel*);
static idx_e check_foreign_key(thread_db*, Record*, jrd_rel*, jrd_tra*, index_desc*, IndexErrorContext&);
static idx_e check_partner_index(thread_db*, jrd_rel*, Record*, jrd_tra*, index_desc*, jrd_rel*, USHORT);
static bool cmpRecordKeys(thread_db*, Record*, jrd_rel*, index_desc*, Record*, jrd_rel*, index_desc*);
static bool duplicate_key(const UCHAR*, const UCHAR*, void*);
static PageNumber get_root_page(thread_db*, jrd_rel*);
static int index_block_flush(void*);
static idx_e insert_key(thread_db*, jrd_rel*, Record*, jrd_tra*, WIN *, index_insertion*, IndexErrorContext&);
static void release_index_block(thread_db*, IndexBlock*);
static void signal_index_deletion(thread_db*, jrd_rel*, USHORT);


void IDX_check_access(thread_db* tdbb, CompilerScratch* csb, jrd_rel* view, jrd_rel* relation)
{
/**************************************
 *
 *	I D X _ c h e c k _ a c c e s s
 *
 **************************************
 *
 * Functional description
 *	Check the various indices in a relation
 *	to see if we need REFERENCES access to fields
 *	in the primary key.   Don't call this routine for
 *	views or external relations, since the mechanism
 *	ain't there.
 *
 **************************************/
	SET_TDBB(tdbb);

	index_desc idx;
	idx.idx_id = idx_invalid;
	RelationPages* relPages = relation->getPages(tdbb);
	WIN window(relPages->rel_pg_space_id, -1);
	WIN referenced_window(relPages->rel_pg_space_id, -1);

	while (BTR_next_index(tdbb, relation, NULL, &idx, &window))
	{
		if (idx.idx_flags & idx_foreign)
		{
			// find the corresponding primary key index

			if (!MET_lookup_partner(tdbb, relation, &idx, {}))
				continue;

			jrd_rel* referenced_relation = MET_relation(tdbb, idx.idx_primary_relation);
			MET_scan_relation(tdbb, referenced_relation);
			const USHORT index_id = idx.idx_primary_index;

			// get the description of the primary key index

			referenced_window.win_page = get_root_page(tdbb, referenced_relation);
			referenced_window.win_flags = 0;
			index_root_page* referenced_root =
				(index_root_page*) CCH_FETCH(tdbb, &referenced_window, LCK_read, pag_root);
			index_desc referenced_idx;
			if (!BTR_description(tdbb, referenced_relation, referenced_root,
								 &referenced_idx, index_id))
			{
				CCH_RELEASE(tdbb, &referenced_window);
				BUGCHECK(173);	// msg 173 referenced index description not found
			}

			// post references access to each field in the index

			const index_desc::idx_repeat* idx_desc = referenced_idx.idx_rpt;
			for (USHORT i = 0; i < referenced_idx.idx_count; i++, idx_desc++)
			{
				const SLONG ssRelationId = view ? view->rel_id : 0;
				const jrd_fld* referenced_field = MET_get_field(referenced_relation, idx_desc->idx_field);

				CMP_post_access(tdbb, csb, relation->rel_security_name.schema, ssRelationId,
					SCL_usage, obj_schemas, QualifiedName(relation->rel_name.schema));

				CMP_post_access(tdbb, csb,
								referenced_relation->rel_security_name.object, ssRelationId,
								SCL_references, obj_relations,
								referenced_relation->rel_name);

				CMP_post_access(tdbb, csb,
								referenced_field->fld_security_name, 0,
								SCL_references, obj_column,
								referenced_relation->rel_name, referenced_field->fld_name);
			}

			CCH_RELEASE(tdbb, &referenced_window);
		}
	}
}


bool IDX_check_master_types(thread_db* tdbb, index_desc& idx, jrd_rel* partner_relation, int& bad_segment)
{
/**********************************************
 *
 *	I D X _ c h e c k _ m a s t e r _ t y p e s
 *
 **********************************************
 *
 * Functional description
 *	Check if both indices of foreign key constraint
 *	has compatible data types in appropriate segments.
 *	Called when detail index is created after idx_itype
 *	was assigned
 *
 **********************************************/

	SET_TDBB(tdbb);

	index_desc partner_idx;

	// get the index root page for the partner relation
	WIN window(get_root_page(tdbb, partner_relation));
	index_root_page* root = (index_root_page*) CCH_FETCH(tdbb, &window, LCK_read, pag_root);

	// get the description of the partner index
	const bool ok = BTR_description(tdbb, partner_relation, root, &partner_idx, idx.idx_primary_index);
	CCH_RELEASE(tdbb, &window);

	if (!ok)
		BUGCHECK(175);			// msg 175 partner index description not found

	// make sure partner index have the same segment count as our
	fb_assert(idx.idx_count == partner_idx.idx_count);

	for (int i = 0; i < idx.idx_count; i++)
	{
		if (idx.idx_rpt[i].idx_itype != partner_idx.idx_rpt[i].idx_itype)
		{
			bad_segment = i;
			return false;
		}
	}

	return true;
}


namespace Jrd {

class IndexCreateTask : public Task
{
public:
	const ULONG IS_GBAK			= 0x01;		// main attachment is gbak attachment
	const ULONG IS_LARGE_SCAN	= 0x02;		// relation not fits into page cache

	IndexCreateTask(thread_db* tdbb, MemoryPool* pool, IndexCreation* creation) : Task(),
		m_pool(pool),
		m_dbb(tdbb->getDatabase()),
		m_tdbb_flags(tdbb->tdbb_flags),
		m_flags(0),
		m_creation(creation),
		m_items(*m_pool),
		m_stop(false),
		m_countPP(0),
		m_nextPP(0)
	{
		Attachment* att = tdbb->getAttachment();

		if (att->isGbak())
			m_flags |= IS_GBAK;

		m_exprBlob.clear();
		m_condBlob.clear();

		int workers = 1;
		if (att->att_parallel_workers > 0)
			workers = att->att_parallel_workers;

		// Classic in single-user shutdown mode can't create additional worker attachments
		if ((m_dbb->dbb_ast_flags & DBB_shutdown_single) && !(m_dbb->dbb_flags & DBB_shared))
			workers = 1;

		for (int i = 0; i < workers; i++)
			m_items.add(FB_NEW_POOL(*m_pool) Item(this));

		m_items[0]->m_ownAttach = false;
		m_items[0]->m_attStable = att->getStable();
		m_items[0]->m_tra = m_creation->transaction;

		if (m_creation)
		{
			// Unless this is the only attachment or a database restore, worry about
			// preserving the page working sets of other attachments.
			if (att && (att != m_dbb->dbb_attachments || att->att_next))
			{
				if (att->isGbak() || DPM_data_pages(tdbb, m_creation->relation) > m_dbb->dbb_bcb->bcb_count)
					m_flags |= IS_LARGE_SCAN;
			}

			m_countPP = DPM_pointer_pages(tdbb, m_creation->relation);

			if ((m_creation->index->idx_flags & (idx_expression | idx_condition)) && (workers > 1))
				MET_lookup_index_expr_cond_blr(tdbb, m_creation->index_name, m_exprBlob, m_condBlob);
		}
	}

	virtual ~IndexCreateTask()
	{
		for (Item** p = m_items.begin(); p < m_items.end(); p++)
			delete *p;
	}

	bool handler(WorkItem& _item);
	bool getWorkItem(WorkItem** pItem);
	bool getResult(IStatus* status);
	int getMaxWorkers();

	bool isGbak() const
	{
		return (m_flags & IS_GBAK);
	}

	class Item : public Task::WorkItem
	{
	public:
		Item(IndexCreateTask* task) : Task::WorkItem(task),
			m_inuse(false),
			m_ownAttach(true),
			m_tra(NULL),
			m_sort(NULL),
			m_ppSequence(0)
		{}

		virtual ~Item()
		{
			if (m_sort)
			{
				delete m_sort;
				m_sort = NULL;
			}

			if (!m_ownAttach || !m_attStable)
				return;

			Attachment* att = NULL;
			{
				AttSyncLockGuard guard(*m_attStable->getSync(), FB_FUNCTION);

				att = m_attStable->getHandle();
				if (!att)
					return;
				fb_assert(att->att_use_count > 0);
			}

			FbLocalStatus status;
			if (m_tra)
			{
				BackgroundContextHolder tdbb(att->att_database, att, &status, FB_FUNCTION);
				TRA_commit(tdbb, m_tra, false);
			}
			WorkerAttachment::releaseAttachment(&status, m_attStable);
		}

		bool init(thread_db* tdbb)
		{
			FbStatusVector* status = tdbb->tdbb_status_vector;
			Attachment* att = NULL;

			if (m_ownAttach && !m_attStable.hasData())
				m_attStable = WorkerAttachment::getAttachment(status, getTask()->m_dbb);

			if (m_attStable)
				att = m_attStable->getHandle();

			if (!att)
			{
				if (!status->hasData())
					Arg::Gds(isc_bad_db_handle).copyTo(status);

				return false;
			}

			if (getTask()->isGbak())
				att->att_utility = Attachment::UTIL_GBAK;

			IndexCreation* creation = getTask()->m_creation;
			tdbb->setDatabase(att->att_database);
			tdbb->setAttachment(att);

			if (m_ownAttach && !m_tra)
			{
				try
				{
					WorkerContextHolder holder(tdbb, FB_FUNCTION);
					m_tra = TRA_start(tdbb, creation->transaction->tra_flags,
											creation->transaction->tra_lock_timeout);
				}
				catch (const Exception& ex)
				{
					ex.stuffException(tdbb->tdbb_status_vector);
					return false;
				}
			}

			tdbb->setTransaction(m_tra);

			if (!m_sort)
			{
				m_idx = *creation->index;	// copy
				if (m_ownAttach)
				{
					m_idx.idx_expression = NULL;
					m_idx.idx_expression_statement = NULL;
					m_idx.idx_condition = NULL;
					m_idx.idx_condition_statement = NULL;
					m_idx.idx_foreign_indexes = NULL;
					m_idx.idx_foreign_primaries = NULL;
					m_idx.idx_foreign_relations = NULL;
				}

				FPTR_REJECT_DUP_CALLBACK callback = NULL;
				void* callback_arg = NULL;

				if (m_idx.idx_flags & idx_unique)
				{
					callback = duplicate_key;
					callback_arg = creation;
				}

				MutexLockGuard guard(getTask()->m_mutex, FB_FUNCTION);

				m_sort = FB_NEW_POOL(m_tra->tra_sorts.getPool())
							Sort(att->att_database, &m_tra->tra_sorts,
								 creation->key_length + sizeof(index_sort_record),
								 2, 1, creation->key_desc, callback, callback_arg);

				creation->sort->addPartition(m_sort);
			}

			return true;
		}

		IndexCreateTask* getTask() const
		{
			return reinterpret_cast<IndexCreateTask*> (m_task);
		}

		bool m_inuse;
		bool m_ownAttach;
		RefPtr<StableAttachmentPart> m_attStable;
		jrd_tra* m_tra;
		index_desc m_idx;
		Sort* m_sort;
		ULONG m_ppSequence;
	};

private:
	void setError(IStatus* status, bool stopTask)
	{
		const bool copyStatus = (m_status.isSuccess() && status && status->getState() == IStatus::STATE_ERRORS);
		if (!copyStatus && (!stopTask || m_stop))
			return;

		MutexLockGuard guard(m_mutex, FB_FUNCTION);
		if (m_status.isSuccess() && copyStatus)
			m_status.save(status);
		if (stopTask)
			m_stop = true;
	}

	MemoryPool* m_pool;
	Database* m_dbb;
	const ULONG m_tdbb_flags;
	ULONG m_flags;
	IndexCreation* m_creation;
	bid m_exprBlob;
	bid m_condBlob;

	Mutex m_mutex;
	HalfStaticArray<Item*, 8> m_items;
	StatusHolder m_status;

	volatile bool m_stop;
	ULONG m_countPP;
	ULONG m_nextPP;
};

bool IndexCreateTask::handler(WorkItem& _item)
{
	Item* item = reinterpret_cast<Item*>(&_item);

	ThreadContextHolder tdbb(NULL);
	tdbb->tdbb_flags = m_tdbb_flags;

	if (!item->init(tdbb))
	{
		setError(tdbb->tdbb_status_vector, true);
		return false;
	}

	try {

	WorkerContextHolder holder(tdbb, FB_FUNCTION);

	Database* dbb = tdbb->getDatabase();
	Attachment* attachment = tdbb->getAttachment();
	jrd_rel* relation = MET_relation(tdbb, m_creation->relation->rel_id);
	if (!(relation->rel_flags & REL_scanned))
		MET_scan_relation(tdbb, relation);

	index_desc* idx = &item->m_idx;
	jrd_tra* transaction = item->m_tra ? item->m_tra : m_creation->transaction;
	Sort* scb = item->m_sort;

	RecordStack stack;
	record_param primary, secondary;
	secondary.rpb_relation = relation;
	primary.rpb_relation   = relation;
	primary.rpb_number.setValue(BOF_NUMBER);
	//primary.getWindow(tdbb).win_flags = secondary.getWindow(tdbb).win_flags = 0; redundant

	IndexErrorContext context(relation, idx, m_creation->index_name);

	// If scan is finished, do final sort pass over own sort
	if (item->m_ppSequence == m_countPP)
	{
		//fb_assert((scb->scb_flags & scb_sorted) == 0);

		if (item->m_ownAttach && idx->idx_expression_statement)
		{
			idx->idx_expression_statement->release(tdbb);
			idx->idx_expression_statement = NULL;
		}

		if (!m_stop && m_creation->duplicates.value() == 0)
			scb->sort(tdbb);

		if (!m_stop && m_creation->duplicates.value() > 0)
		{
			AutoPtr<Record> error_record;
			primary.rpb_record = NULL;
			fb_assert(m_creation->dup_recno >= 0);
			primary.rpb_number.setValue(m_creation->dup_recno);

			if (DPM_get(tdbb, &primary, LCK_read))
			{
				if (primary.rpb_flags & rpb_deleted)
					CCH_RELEASE(tdbb, &primary.getWindow(tdbb));
				else
				{
					VIO_data(tdbb, &primary, dbb->dbb_permanent);
					error_record = primary.rpb_record;
				}
			}

			context.raise(tdbb, idx_e_duplicate, error_record);
		}

		return true;
	}

	jrd_rel* partner_relation = 0;
	USHORT partner_index_id = 0;
	if (idx->idx_flags & idx_foreign)
	{
//		if (!MET_lookup_partner(tdbb, relation, idx, m_creation->index_name)) {
//			BUGCHECK(173);		// msg 173 referenced index description not found
//		}
		partner_relation = MET_relation(tdbb, idx->idx_primary_relation);
		partner_index_id = idx->idx_primary_index;
	}

	if ((idx->idx_flags & idx_expression) && (idx->idx_expression == NULL))
	{
		fb_assert(!m_exprBlob.isEmpty());

		CompilerScratch* csb = NULL;
		Jrd::ContextPoolHolder context(tdbb, attachment->createPool());

		idx->idx_expression = static_cast<ValueExprNode*> (MET_parse_blob(tdbb, &relation->rel_name.schema,
			relation, &m_exprBlob, &csb, &idx->idx_expression_statement, false, false));

		delete csb;
	}

	if ((idx->idx_flags & idx_condition) && (idx->idx_condition == NULL))
	{
		fb_assert(!m_condBlob.isEmpty());

		CompilerScratch* csb = NULL;
		Jrd::ContextPoolHolder context(tdbb, attachment->createPool());

		idx->idx_condition = static_cast<BoolExprNode*> (MET_parse_blob(tdbb, &relation->rel_name.schema, relation,
			&m_condBlob, &csb, &idx->idx_condition_statement, false, false));

		delete csb;
	}

	// Checkout a garbage collect record block for fetching data.

	AutoTempRecord gc_record(VIO_gc_record(tdbb, relation));

	if (m_flags & IS_LARGE_SCAN)
	{
		primary.getWindow(tdbb).win_flags = secondary.getWindow(tdbb).win_flags = WIN_large_scan;
		primary.rpb_org_scans = secondary.rpb_org_scans = relation->rel_scan_count++;
	}

	const bool isDescending = (idx->idx_flags & idx_descending);
	const bool isPrimary = (idx->idx_flags & idx_primary);
	const bool isForeign = (idx->idx_flags & idx_foreign);
	const UCHAR pad = isDescending ? -1 : 0;
	bool key_is_null = false;

	primary.rpb_number.compose(dbb->dbb_max_records, dbb->dbb_dp_per_pp, 0, 0, item->m_ppSequence);
	primary.rpb_number.decrement();

	RecordNumber lastRecNo;
	lastRecNo.compose(dbb->dbb_max_records, dbb->dbb_dp_per_pp, 0, 0, item->m_ppSequence + 1);
	lastRecNo.decrement();

	IndexKey key(tdbb, relation, idx);
	IndexCondition condition(tdbb, idx);

	// Loop thru the relation computing index keys.  If there are old versions, find them, too.
	while (DPM_next(tdbb, &primary, LCK_read, DPM_next_pointer_page))
	{
		if (primary.rpb_number >= lastRecNo)
		{
			CCH_RELEASE(tdbb, &primary.getWindow(tdbb));
			break;
		}

		if (!VIO_garbage_collect(tdbb, &primary, transaction))
			continue;

		// If there are any back-versions left make an attempt at intermediate GC.
		if (primary.rpb_b_page)
		{
			VIO_intermediate_gc(tdbb, &primary, transaction);

			if (!DPM_get(tdbb, &primary, LCK_read))
				continue;
		}

		const bool deleted = primary.rpb_flags & rpb_deleted;
		if (deleted)
			CCH_RELEASE(tdbb, &primary.getWindow(tdbb));
		else
		{
			primary.rpb_record = gc_record;
			VIO_data(tdbb, &primary, relation->rel_pool);
			stack.push(primary.rpb_record);
		}

		secondary.rpb_page = primary.rpb_b_page;
		secondary.rpb_line = primary.rpb_b_line;
		secondary.rpb_prior = primary.rpb_prior;

		while (!m_stop && secondary.rpb_page)
		{
			if (!DPM_fetch(tdbb, &secondary, LCK_read))
				break;			// must be garbage collected

			secondary.rpb_record = NULL;
			VIO_data(tdbb, &secondary, relation->rel_pool);
			stack.push(secondary.rpb_record);
			secondary.rpb_page = secondary.rpb_b_page;
			secondary.rpb_line = secondary.rpb_b_line;
		}

		while (!m_stop && stack.hasData())
		{
			Record* record = stack.pop();
			idx_e result = idx_e_ok;

			const auto checkResult = condition.check(record, &result);

			if (result == idx_e_ok)
			{
				fb_assert(checkResult.isAssigned());
				if (!checkResult.asBool())
					continue;

				result = key.compose(record);

				if (result == idx_e_ok)
				{
					if (isPrimary && key->key_nulls != 0)
					{
						const auto key_null_segment = key.getNullSegment();
						fb_assert(key_null_segment < idx->idx_count);
						const auto bad_id = idx->idx_rpt[key_null_segment].idx_field;
						const jrd_fld *bad_fld = MET_get_field(relation, bad_id);

						ERR_post(Arg::Gds(isc_not_valid) << Arg::Str(bad_fld->fld_name) <<
															Arg::Str(NULL_STRING_MARK));
					}

					// If foreign key index is being defined, make sure foreign
					// key definition will not be violated

					if (isForeign && key->key_nulls == 0)
					{
						result = check_partner_index(tdbb, relation, record, transaction, idx,
													 partner_relation, partner_index_id);
					}
				}
			}

			if (result != idx_e_ok)
			{
				do {
					if (record != gc_record)
						delete record;
				} while (stack.hasData() && (record = stack.pop()));

				if (primary.getWindow(tdbb).win_flags & WIN_large_scan)
					--relation->rel_scan_count;

				context.raise(tdbb, result, record);
			}

			if (key->key_length > m_creation->key_length)
			{
				do {
					if (record != gc_record)
						delete record;
				} while (stack.hasData() && (record = stack.pop()));

				if (primary.getWindow(tdbb).win_flags & WIN_large_scan)
					--relation->rel_scan_count;

				context.raise(tdbb, idx_e_keytoobig, record);
			}

			UCHAR* p;
			scb->put(tdbb, reinterpret_cast<ULONG**>(&p));

			// try to catch duplicates early

			if (m_creation->duplicates.value() > 0)
			{
				do {
					if (record != gc_record)
						delete record;
				} while (stack.hasData() && (record = stack.pop()));

				break;
			}

			if (m_creation->nullIndLen)
				*p++ = (key->key_length == 0) ? 0 : 1;

			if (key->key_length > 0)
			{
				memcpy(p, key->key_data, key->key_length);
				p += key->key_length;
			}

			int l = int(m_creation->key_length) - m_creation->nullIndLen - key->key_length;	// must be signed

			if (l > 0)
			{
				memset(p, pad, l);
				p += l;
			}

			const bool key_is_null = (key->key_nulls == (1 << idx->idx_count) - 1);

			index_sort_record* isr = (index_sort_record*) p;
			isr->isr_record_number = primary.rpb_number.getValue();
			isr->isr_key_length = key->key_length;
			isr->isr_flags = ((stack.hasData() || deleted) ? ISR_secondary : 0) | (key_is_null ? ISR_null : 0);
			if (record != gc_record)
				delete record;
		}

		if (m_stop)
			break;

		if (m_creation->duplicates.value() > 0)
			break;

		JRD_reschedule(tdbb);
	}

	gc_record.release();

	if (primary.getWindow(tdbb).win_flags & WIN_large_scan)
		--relation->rel_scan_count;
	}
	catch (const Exception& ex)
	{
		ex.stuffException(tdbb->tdbb_status_vector);
		setError(tdbb->tdbb_status_vector, true);
		return false;
	}

	return true;
}

bool IndexCreateTask::getWorkItem(WorkItem** pItem)
{
	Item* item = reinterpret_cast<Item*> (*pItem);

	MutexLockGuard guard(m_mutex, FB_FUNCTION);

	if (m_stop)
		return false;

	if (item == NULL)
	{
		for (Item** p = m_items.begin(); p < m_items.end(); p++)
			if (!(*p)->m_inuse)
			{
				(*p)->m_inuse = true;
				*pItem = item = *p;
				break;
			}
	}

	if (!item)
		return false;

	item->m_inuse = (m_nextPP < m_countPP) ||
		(item->m_sort && item->m_sort->isSorted()) == 0;

	if (item->m_inuse)
	{
		item->m_ppSequence = m_nextPP;
		if (m_nextPP < m_countPP)
			m_nextPP += 1;
	}

	return item->m_inuse;
}

bool IndexCreateTask::getResult(IStatus* status)
{
	if (status)
	{
		status->init();
		status->setErrors(m_status.getErrors());
	}

	return m_status.isSuccess();
}

int IndexCreateTask::getMaxWorkers()
{
	const int parWorkers = m_items.getCount();
	if (parWorkers == 1 || m_countPP == 0)
		return 1;

	fb_assert(m_creation != NULL);

	if (!m_creation || m_creation->relation->isTemporary())
		return 1;

	return MIN(parWorkers, m_countPP);
}

}; // namespace Jrd


void IDX_create_index(thread_db* tdbb,
					  jrd_rel* relation,
					  index_desc* idx,
					  const QualifiedName& index_name,
					  USHORT* index_id,
					  jrd_tra* transaction,
					  SelectivityList& selectivity)
{
/**************************************
 *
 *	I D X _ c r e a t e _ i n d e x
 *
 **************************************
 *
 * Functional description
 *	Create and populate index.
 *
 **************************************/
	SET_TDBB(tdbb);
	Database* dbb = tdbb->getDatabase();
	Jrd::Attachment* attachment = tdbb->getAttachment();

	if (relation->rel_file)
	{
		ERR_post(Arg::Gds(isc_no_meta_update) <<
				 Arg::Gds(isc_extfile_uns_op) << relation->rel_name.toQuotedString());
	}
	else if (relation->isVirtual())
	{
		ERR_post(Arg::Gds(isc_no_meta_update) <<
				 Arg::Gds(isc_wish_list));
	}

	get_root_page(tdbb, relation);

	fb_assert(transaction);

	const bool isDescending = (idx->idx_flags & idx_descending);
	const bool isPrimary = (idx->idx_flags & idx_primary);
	const bool isForeign = (idx->idx_flags & idx_foreign);

	// hvlad: in ODS11 empty string and NULL values can have the same binary
	// representation in index keys. BTR can distinguish it by the key_length
	// but SORT module currently don't take it into account. Therefore add to
	// the index key one byte prefix with 0 for NULL value and 1 for not-NULL
	// value to produce right sorting.
	// BTR\fast_load will remove this one byte prefix from the index key.
	// Note that this is necessary only for single-segment ascending indexes
	// and only for ODS11 and higher.

	const int nullIndLen = !isDescending && (idx->idx_count == 1) ? 1 : 0;
	const USHORT key_length = ROUNDUP(BTR_key_length(tdbb, relation, idx) + nullIndLen, sizeof(SINT64));

	if (key_length >= dbb->getMaxIndexKeyLength())
	{
		ERR_post(Arg::Gds(isc_no_meta_update) <<
				 Arg::Gds(isc_keytoobig) << index_name.toQuotedString());
	}

	if (isForeign)
	{
		if (!MET_lookup_partner(tdbb, relation, idx, index_name)) {
			BUGCHECK(173);		// msg 173 referenced index description not found
		}
	}

	IndexCreation creation;
	creation.index = idx;
	creation.index_name = index_name;
	creation.relation = relation;
	creation.transaction = transaction;
	creation.sort = NULL;
	creation.key_length = key_length;
	creation.nullIndLen = nullIndLen;
	creation.dup_recno = -1;
	creation.duplicates.setValue(0);

	BTR_reserve_slot(tdbb, creation);

	if (index_id)
		*index_id = idx->idx_id;

	sort_key_def key_desc[2];
	// Key sort description
	key_desc[0].setSkdLength(SKD_bytes, key_length);
	key_desc[0].skd_flags = SKD_ascending;
	key_desc[0].setSkdOffset();
	key_desc[0].skd_vary_offset = 0;
	// RecordNumber sort description
	key_desc[1].setSkdLength(SKD_int64, sizeof(RecordNumber));
	key_desc[1].skd_flags = SKD_ascending;
	key_desc[1].setSkdOffset(key_desc);
	key_desc[1].skd_vary_offset = 0;

	creation.key_desc = key_desc;

	PartitionedSort sort(dbb, &transaction->tra_sorts);
	creation.sort = &sort;

	Coordinator coord(dbb->dbb_permanent);
	IndexCreateTask task(tdbb, dbb->dbb_permanent, &creation);

	{
		EngineCheckout cout(tdbb, FB_FUNCTION);

		FbLocalStatus local_status;
		fb_utils::init_status(&local_status);

		coord.runSync(&task);

		if (!task.getResult(&local_status))
			local_status.raise();
	}

	sort.buildMergeTree();

	if (creation.duplicates.value() == 0)
		BTR_create(tdbb, creation, selectivity);

	if (creation.duplicates.value() > 0)
	{
		AutoPtr<Record> error_record;
		record_param primary;
		primary.rpb_relation = relation;
		primary.rpb_record = NULL;
		fb_assert(creation.dup_recno >= 0);
		primary.rpb_number.setValue(creation.dup_recno);

		if (DPM_get(tdbb, &primary, LCK_read))
		{
			if (primary.rpb_flags & rpb_deleted)
				CCH_RELEASE(tdbb, &primary.getWindow(tdbb));
			else
			{
				VIO_data(tdbb, &primary, relation->rel_pool);
				error_record = primary.rpb_record;
			}

		}

		IndexErrorContext context(relation, idx, index_name);
		context.raise(tdbb, idx_e_duplicate, error_record);
	}

	if ((relation->rel_flags & REL_temp_conn) && (relation->getPages(tdbb)->rel_instance_id != 0))
	{
		IndexLock* idx_lock = CMP_get_index_lock(tdbb, relation, idx->idx_id);
		if (idx_lock)
		{
			++idx_lock->idl_count;
			if (idx_lock->idl_count == 1)
				LCK_lock(tdbb, idx_lock->idl_lock, LCK_SR, LCK_WAIT);
		}
	}
}


IndexBlock* IDX_create_index_block(thread_db* tdbb, jrd_rel* relation, USHORT id)
{
/**************************************
 *
 *	I D X _ c r e a t e _ i n d e x _ b l o c k
 *
 **************************************
 *
 * Functional description
 *	Create an index block and an associated
 *	lock block for the specified index.
 *
 **************************************/
	SET_TDBB(tdbb);
	Database* dbb = tdbb->getDatabase();
	CHECK_DBB(dbb);

	IndexBlock* index_block = FB_NEW_POOL(*relation->rel_pool) IndexBlock();
	index_block->idb_id = id;

	// link the block in with the relation linked list

	index_block->idb_next = relation->rel_index_blocks;
	relation->rel_index_blocks = index_block;

	// create a shared lock for the index, to coordinate
	// any modification to the index so that the cached information
	// about the index will be discarded

	Lock* lock = FB_NEW_RPT(*relation->rel_pool, 0)
		Lock(tdbb, sizeof(SLONG), LCK_expression, index_block, index_block_flush);
	index_block->idb_lock = lock;
	lock->setKey((relation->rel_id << 16) | index_block->idb_id);

	return index_block;
}


void IDX_delete_index(thread_db* tdbb, jrd_rel* relation, USHORT id)
{
/**************************************
 *
 *	I D X _ d e l e t e _ i n d e x
 *
 **************************************
 *
 * Functional description
 *	Delete a single index.
 *
 **************************************/
	SET_TDBB(tdbb);

	signal_index_deletion(tdbb, relation, id);

	WIN window(get_root_page(tdbb, relation));
	CCH_FETCH(tdbb, &window, LCK_write, pag_root);

	const bool tree_exists = BTR_delete_index(tdbb, &window, id);

	if ((relation->rel_flags & REL_temp_conn) && (relation->getPages(tdbb)->rel_instance_id != 0) &&
		tree_exists)
	{
		IndexLock* idx_lock = CMP_get_index_lock(tdbb, relation, id);
		if (idx_lock)
		{
			if (!--idx_lock->idl_count) {
				LCK_release(tdbb, idx_lock->idl_lock);
			}
		}
	}
}


void IDX_delete_indices(thread_db* tdbb, jrd_rel* relation, RelationPages* relPages)
{
/**************************************
 *
 *	I D X _ d e l e t e _ i n d i c e s
 *
 **************************************
 *
 * Functional description
 *	Delete all known indices in preparation for deleting a
 *	complete relation.
 *
 **************************************/
	SET_TDBB(tdbb);

	fb_assert(relPages->rel_index_root);

	WIN window(relPages->rel_pg_space_id, relPages->rel_index_root);
	index_root_page* root = (index_root_page*) CCH_FETCH(tdbb, &window, LCK_write, pag_root);

	const bool is_temp = (relation->rel_flags & REL_temp_conn) && (relPages->rel_instance_id != 0);

	for (USHORT i = 0; i < root->irt_count; i++)
	{
		const bool tree_exists = BTR_delete_index(tdbb, &window, i);
		root = (index_root_page*) CCH_FETCH(tdbb, &window, LCK_write, pag_root);

		if (is_temp && tree_exists)
		{
			IndexLock* idx_lock = CMP_get_index_lock(tdbb, relation, i);
			if (idx_lock)
			{
				if (!--idx_lock->idl_count) {
					LCK_release(tdbb, idx_lock->idl_lock);
				}
			}
		}
	}

	CCH_RELEASE(tdbb, &window);
}


void IDX_erase(thread_db* tdbb, record_param* rpb, jrd_tra* transaction)
{
/**************************************
 *
 *	I D X _ e r a s e
 *
 **************************************
 *
 * Functional description
 *	Check the various indices prior to an ERASE operation.
 *	If one is a primary key, check its partner for
 *	a duplicate record.
 *
 **************************************/
	SET_TDBB(tdbb);

	index_desc idx;
	idx.idx_id = idx_invalid;

	RelationPages* relPages = rpb->rpb_relation->getPages(tdbb);
	WIN window(relPages->rel_pg_space_id, -1);

	while (BTR_next_index(tdbb, rpb->rpb_relation, transaction, &idx, &window))
	{
		if (idx.idx_flags & (idx_primary | idx_unique))
		{
			IndexErrorContext context(rpb->rpb_relation, &idx);

			if (const auto error_code = check_foreign_key(tdbb, rpb->rpb_record, rpb->rpb_relation,
													   	  transaction, &idx, context))
			{
				CCH_RELEASE(tdbb, &window);
				context.raise(tdbb, error_code, rpb->rpb_record);
			}
		}
	}
}


void IDX_garbage_collect(thread_db* tdbb, record_param* rpb, RecordStack& going, RecordStack& staying)
{
/**************************************
 *
 *	I D X _ g a r b a g e _ c o l l e c t
 *
 **************************************
 *
 * Functional description
 *	Perform garbage collection for a bunch of records.  Scan
 *	through the indices defined for a relation.  Garbage collect
 *	each.
 *
 **************************************/
	SET_TDBB(tdbb);

	index_desc idx;

	index_insertion insertion;
	insertion.iib_descriptor = &idx;
	insertion.iib_number = rpb->rpb_number;
	insertion.iib_relation = rpb->rpb_relation;
	insertion.iib_btr_level = 0;

	WIN window(get_root_page(tdbb, rpb->rpb_relation));

	index_root_page* root = (index_root_page*) CCH_FETCH(tdbb, &window, LCK_read, pag_root);

	for (USHORT i = 0; i < root->irt_count; i++)
	{
		if (BTR_description(tdbb, rpb->rpb_relation, root, &idx, i))
		{
			IndexErrorContext context(rpb->rpb_relation, &idx);
			IndexCondition condition(tdbb, &idx);

			AutoIndexExpression expression;
			IndexKey key1(tdbb, rpb->rpb_relation, &idx, expression), key2(key1);

			for (RecordStack::iterator stack1(going); stack1.hasData(); ++stack1)
			{
				Record* const rec1 = stack1.object();

				if (!condition.check(rec1).asBool())
					continue;

				if (const auto result = key1.compose(rec1))
				{
					if (result == idx_e_conversion)
						continue;

					CCH_RELEASE(tdbb, &window);
					context.raise(tdbb, result, rec1);
				}

				// Cancel index if there are duplicates in the remaining records

				RecordStack::iterator stack2(stack1);
				for (++stack2; stack2.hasData(); ++stack2)
				{
					Record* const rec2 = stack2.object();

					if (const auto result = key2.compose(rec2))
					{
						if (result == idx_e_conversion)
							continue;

						CCH_RELEASE(tdbb, &window);
						context.raise(tdbb, result, rec2);
					}

					if (key1 == key2)
						break;
				}

				if (stack2.hasData())
					continue;

				// Make sure the index doesn't exist in any record remaining

				RecordStack::iterator stack3(staying);
				for (; stack3.hasData(); ++stack3)
				{
					Record* const rec3 = stack3.object();

					if (const auto result = key2.compose(rec3))
					{
						if (result == idx_e_conversion)
							continue;

						CCH_RELEASE(tdbb, &window);
						context.raise(tdbb, result, rec3);
					}

					if (key1 == key2)
						break;
				}

				if (stack3.hasData())
					continue;

				// Get rid of index node

				insertion.iib_key = key1;
				BTR_remove(tdbb, &window, &insertion);
				root = (index_root_page*) CCH_FETCH(tdbb, &window, LCK_read, pag_root);

				if (stack1.hasMore(1))
					BTR_description(tdbb, rpb->rpb_relation, root, &idx, i);
			}
		}
	}

	CCH_RELEASE(tdbb, &window);
}


void IDX_modify(thread_db* tdbb,
				record_param* org_rpb,
				record_param* new_rpb,
				jrd_tra* transaction)
{
/**************************************
 *
 *	I D X _ m o d i f y
 *
 **************************************
 *
 * Functional description
 *	Update the various indices after a MODIFY operation.  If a duplicate
 *	index is violated, return the index number.  If successful, return
 *	-1.
 *
 **************************************/
	SET_TDBB(tdbb);

	index_desc idx;
	idx.idx_id = idx_invalid;

	index_insertion insertion;
	insertion.iib_relation = org_rpb->rpb_relation;
	insertion.iib_number = org_rpb->rpb_number;
	insertion.iib_descriptor = &idx;
	insertion.iib_transaction = transaction;
	insertion.iib_btr_level = 0;

	RelationPages* relPages = org_rpb->rpb_relation->getPages(tdbb);
	WIN window(relPages->rel_pg_space_id, -1);

	while (BTR_next_index(tdbb, org_rpb->rpb_relation, transaction, &idx, &window))
	{
		IndexErrorContext context(new_rpb->rpb_relation, &idx);
		idx_e error_code = idx_e_ok;

		{
			IndexCondition condition(tdbb, &idx);
			const auto checkResult = condition.check(new_rpb->rpb_record, &error_code);

			if (error_code)
			{
				CCH_RELEASE(tdbb, &window);
				context.raise(tdbb, error_code, new_rpb->rpb_record);
			}

			fb_assert(checkResult.isAssigned());
			if (!checkResult.asBool())
				continue;
		}

		AutoIndexExpression expression;
		IndexKey newKey(tdbb, new_rpb->rpb_relation, &idx, expression), orgKey(newKey);

		if ( (error_code = newKey.compose(new_rpb->rpb_record)) )
		{
			CCH_RELEASE(tdbb, &window);
			context.raise(tdbb, error_code, new_rpb->rpb_record);
		}

		if ( (error_code = orgKey.compose(org_rpb->rpb_record)) )
		{
			CCH_RELEASE(tdbb, &window);
			context.raise(tdbb, error_code, org_rpb->rpb_record);
		}

		expression.reset();

		if (newKey == orgKey)
		{
			// The new record satisfies index condition, check old record too:
			// if it does not satisfies condition, key should be inserted into index.
			// Note, condition.check() is always true for non-conditional indeces.

			IndexCondition condition(tdbb, &idx);
			const auto checkResult = condition.check(org_rpb->rpb_record, &error_code);

			if (error_code)
			{
				CCH_RELEASE(tdbb, &window);
				context.raise(tdbb, error_code, org_rpb->rpb_record);
			}

			fb_assert(checkResult.isAssigned());
			if (checkResult.asBool())
				continue;
		}

		insertion.iib_key = newKey;
		if ( (error_code = insert_key(tdbb, new_rpb->rpb_relation, new_rpb->rpb_record,
										transaction, &window, &insertion, context)) )
		{
			context.raise(tdbb, error_code, new_rpb->rpb_record);
		}
	}
}


void IDX_modify_check_constraints(thread_db* tdbb,
								  record_param* org_rpb,
								  record_param* new_rpb,
								  jrd_tra* transaction)
{
/**************************************
 *
 *	I D X _ m o d i f y _ c h e c k _ c o n s t r a i n t
 *
 **************************************
 *
 * Functional description
 *	Check for foreign key constraint after a modify statement
 *
 **************************************/
	SET_TDBB(tdbb);

	// If relation's primary/unique keys have no dependencies by other
	// relations' foreign keys then don't bother cycling thru all index descriptions.

	if (!(org_rpb->rpb_relation->rel_flags & REL_check_partners) &&
		!(org_rpb->rpb_relation->rel_primary_dpnds.prim_reference_ids))
	{
		return;
	}

	index_desc idx;
	idx.idx_id = idx_invalid;

	RelationPages* relPages = org_rpb->rpb_relation->getPages(tdbb);
	WIN window(relPages->rel_pg_space_id, -1);

	// Now check all the foreign key constraints. Referential integrity relation
	// could be established by primary key/foreign key or unique key/foreign key

	while (BTR_next_index(tdbb, org_rpb->rpb_relation, transaction, &idx, &window))
	{
		if (!(idx.idx_flags & (idx_primary | idx_unique)) ||
			!MET_lookup_partner(tdbb, org_rpb->rpb_relation, &idx, {}))
		{
			continue;
		}

		fb_assert(!(idx.idx_flags & idx_condition));

		IndexErrorContext context(new_rpb->rpb_relation, &idx);
		idx_e error_code;

		AutoIndexExpression expression;
		IndexKey newKey(tdbb, new_rpb->rpb_relation, &idx, expression), orgKey(newKey);

		if ( (error_code = newKey.compose(new_rpb->rpb_record)) )
		{
			CCH_RELEASE(tdbb, &window);
			context.raise(tdbb, error_code, new_rpb->rpb_record);
		}

		if ( (error_code = orgKey.compose(org_rpb->rpb_record)) )
		{
			CCH_RELEASE(tdbb, &window);
			context.raise(tdbb, error_code, org_rpb->rpb_record);
		}

		expression.reset();

		if (newKey != orgKey)
		{
			if ( (error_code = check_foreign_key(tdbb, org_rpb->rpb_record, org_rpb->rpb_relation,
										   	     transaction, &idx, context)) )
			{
				CCH_RELEASE(tdbb, &window);
				context.raise(tdbb, error_code, org_rpb->rpb_record);
			}
		}
	}
}


void IDX_modify_flag_uk_modified(thread_db* tdbb,
								 record_param* org_rpb,
								 record_param* new_rpb,
								 jrd_tra* transaction)
{
/**************************************
 *
 *	I D X _ m o d i f y _ f l a g _ u k _ m o d i f i e d
 *
 **************************************
 *
 * Functional description
 *	Set record flag if key field value was changed by this update or
 *  if this is second update of this record in the same transaction and
 *  flag is already set by one of the previous update.
 *
 **************************************/

	SET_TDBB(tdbb);

	if ((org_rpb->rpb_flags & rpb_uk_modified) &&
		(org_rpb->rpb_transaction_nr == new_rpb->rpb_transaction_nr))
	{
		new_rpb->rpb_flags |= rpb_uk_modified;
		return;
	}

	jrd_rel* const relation = org_rpb->rpb_relation;
	fb_assert(new_rpb->rpb_relation == relation);

	RelationPages* const relPages = relation->getPages(tdbb);
	WIN window(relPages->rel_pg_space_id, -1);

	DSC desc1, desc2;
	index_desc idx;
	idx.idx_id = idx_invalid;

	while (BTR_next_index(tdbb, relation, transaction, &idx, &window))
	{
		if (!(idx.idx_flags & (idx_primary | idx_unique)) ||
			!MET_lookup_partner(tdbb, relation, &idx, {}))
		{
			continue;
		}

		for (USHORT i = 0; i < idx.idx_count; i++)
		{
			const USHORT field_id = idx.idx_rpt[i].idx_field;

			const bool flag_org = EVL_field(relation, org_rpb->rpb_record, field_id, &desc1);
			const bool flag_new = EVL_field(relation, new_rpb->rpb_record, field_id, &desc2);

			if (flag_org != flag_new || (flag_new && MOV_compare(tdbb, &desc1, &desc2)))
			{
				new_rpb->rpb_flags |= rpb_uk_modified;
				CCH_RELEASE(tdbb, &window);
				return;
			}
		}
	}
}


void IDX_statistics(thread_db* tdbb, jrd_rel* relation, USHORT id, SelectivityList& selectivity)
{
/**************************************
 *
 *	I D X _ s t a t i s t i c s
 *
 **************************************
 *
 * Functional description
 *	Scan index pages recomputing
 *	selectivity.
 *
 **************************************/

	SET_TDBB(tdbb);

	BTR_selectivity(tdbb, relation, id, selectivity);
}


void IDX_store(thread_db* tdbb, record_param* rpb, jrd_tra* transaction)
{
/**************************************
 *
 *	I D X _ s t o r e
 *
 **************************************
 *
 * Functional description
 *	Update the various indices after a STORE operation.  If a duplicate
 *	index is violated, return the index number.  If successful, return
 *	-1.
 *
 **************************************/
	SET_TDBB(tdbb);

	index_desc idx;
	idx.idx_id = idx_invalid;

	index_insertion insertion;
	insertion.iib_relation = rpb->rpb_relation;
	insertion.iib_number = rpb->rpb_number;
	insertion.iib_descriptor = &idx;
	insertion.iib_transaction = transaction;
	insertion.iib_btr_level = 0;

	RelationPages* relPages = rpb->rpb_relation->getPages(tdbb);
	WIN window(relPages->rel_pg_space_id, -1);

	while (BTR_next_index(tdbb, rpb->rpb_relation, transaction, &idx, &window))
	{
		IndexErrorContext context(rpb->rpb_relation, &idx);
		idx_e error_code = idx_e_ok;

		{
			IndexCondition condition(tdbb, &idx);
			const auto checkResult = condition.check(rpb->rpb_record, &error_code);

			if (error_code)
			{
				CCH_RELEASE(tdbb, &window);
				context.raise(tdbb, error_code, rpb->rpb_record);
			}

			fb_assert(checkResult.isAssigned());
			if (!checkResult.asBool())
				continue;
		}

		AutoIndexExpression expression;
		IndexKey key(tdbb, rpb->rpb_relation, &idx, expression);

		if ( (error_code = key.compose(rpb->rpb_record)) )
		{
			CCH_RELEASE(tdbb, &window);
			context.raise(tdbb, error_code, rpb->rpb_record);
		}

		expression.reset();

		insertion.iib_key = key;

		if ( (error_code = insert_key(tdbb, rpb->rpb_relation, rpb->rpb_record, transaction,
									  &window, &insertion, context)) )
		{
			context.raise(tdbb, error_code, rpb->rpb_record);
		}
	}
}

static bool cmpRecordKeys(thread_db* tdbb,
						  Record* rec1, jrd_rel* rel1, index_desc* idx1,
						  Record* rec2, jrd_rel* rel2, index_desc* idx2)
{
/**************************************
 *
 *	c m p R e c o r d K e y s
 *
 **************************************
 *
 * Functional description
 *	Compare indexed fields in two records. Records could belong to different
 *  relations but set of indexed fields to compare should be equal.
 *
 **************************************/
	if (idx2->idx_flags & idx_expression)
	{
		// Remove assertion below if\when expression index will participate in FK,
		// currently it is impossible.
		fb_assert(idx1->idx_flags & idx_expression);

		if (auto idxDesc = BTR_eval_expression(tdbb, idx2, rec2))
		{
			HalfStaticArray<UCHAR, 256> tmp;
			dsc tempDesc;

			if (idx1 == idx2)
			{
				// hvlad: BTR_eval_expression call EVL_expr which returns impure->vlu_desc.
				// If idx2 and idx1 are the same indexes then the second call to
				// BTR_eval_expression will overwrite value from the first call.
				// So we must save the first result into another dsc.

				tempDesc = *idxDesc;
				const auto idxDscLength = idx2->idx_expression_desc.dsc_length;
				tempDesc.dsc_address = tmp.getBuffer(idxDscLength + FB_DOUBLE_ALIGN);
				tempDesc.dsc_address = FB_ALIGN(tempDesc.dsc_address, FB_DOUBLE_ALIGN);
				fb_assert(idxDesc->dsc_length <= idxDscLength);
				memmove(tempDesc.dsc_address, idxDesc->dsc_address, idxDesc->dsc_length);
				idxDesc = &tempDesc;
			}

			if (const auto recDesc = BTR_eval_expression(tdbb, idx1, rec1))
			{
				if (!MOV_compare(tdbb, recDesc, idxDesc))
					return true;
			}
		}
	}
	else
	{
		fb_assert(idx1->idx_count == idx2->idx_count);

		dsc desc1, desc2;
		bool all_nulls = true;
		USHORT i;

		for (i = 0; i < idx1->idx_count; i++)
		{
			USHORT field_id = idx1->idx_rpt[i].idx_field;
			// In order to "map a null to a default" value (in EVL_field()),
			// the relation block is referenced.
			// Reference: Bug 10116, 10424
			const bool flag_rec = EVL_field(rel1, rec1, field_id, &desc1);

			field_id = idx2->idx_rpt[i].idx_field;
			const bool flag_idx = EVL_field(rel2, rec2, field_id, &desc2);

			if (flag_rec != flag_idx || (flag_rec && MOV_compare(tdbb, &desc1, &desc2)))
				break;

			all_nulls = all_nulls && !flag_rec && !flag_idx;
		}

		if (i >= idx1->idx_count && !all_nulls)
			return true;
	}

	return false;
}

static idx_e check_duplicates(thread_db* tdbb,
							  Record* record,
							  index_desc* record_idx,
							  index_insertion* insertion, jrd_rel* relation_2)
{
/**************************************
 *
 *	c h e c k _ d u p l i c a t e s
 *
 **************************************
 *
 * Functional description
 *	Make sure there aren't any active duplicates for
 *	a unique index or a foreign key.
 *
 **************************************/
	DSC desc1, desc2;

	SET_TDBB(tdbb);

	idx_e result = idx_e_ok;
	jrd_tra* const transaction = insertion->iib_transaction;
	index_desc* insertion_idx = insertion->iib_descriptor;
	record_param rpb;
	rpb.rpb_relation = insertion->iib_relation;

	AutoTempRecord gc_record(VIO_gc_record(tdbb, rpb.rpb_relation));
	rpb.rpb_record = gc_record;

	jrd_rel* const relation_1 = insertion->iib_relation;
	RecordBitmap::Accessor accessor(insertion->iib_duplicates);

	fb_assert(!(tdbb->tdbb_status_vector->getState() & IStatus::STATE_ERRORS));

	if (accessor.getFirst())
	do {
		bool rec_tx_active;
		const bool is_fk = (record_idx->idx_flags & idx_foreign) != 0;

		rpb.rpb_number.setValue(accessor.current());

		if (rpb.rpb_number != insertion->iib_number &&
			VIO_get_current(tdbb, &rpb, transaction, tdbb->getDefaultPool(),
							is_fk, rec_tx_active) )
		{
			// hvlad: if record's transaction is still active, we should consider
			// it as present and prevent duplicates

			if ((rpb.rpb_flags & rpb_deleted) || rec_tx_active)
			{
				result = idx_e_duplicate;
				break;
			}

			// check the values of the fields in the record being inserted with the
			// record retrieved -- for unique indexes the insertion index and the
			// record index are the same, but for foreign keys they are different

			if (cmpRecordKeys(tdbb, rpb.rpb_record, relation_1, insertion_idx,
							  record, relation_2, record_idx))
			{
				IndexCondition condition(tdbb, record_idx);
				auto checkResult = condition.check(rpb.rpb_record, &result);

				if (result)
					break;

				fb_assert(checkResult.isAssigned());
				if (!checkResult.asBool())
					continue;

				// When check foreign keys in snapshot or read consistency transaction,
				// ensure that master record is visible in transaction context and still
				// satisfy foreign key constraint.

				if (is_fk &&
					(!(transaction->tra_flags & TRA_read_committed) ||
					(transaction->tra_flags & TRA_read_consistency)))
				{
					const int state = TRA_snapshot_state(tdbb, transaction, rpb.rpb_transaction_nr);

					if (state != tra_committed && state != tra_us)
					{
						if (!VIO_get(tdbb, &rpb, transaction, tdbb->getDefaultPool()))
							continue;

						if (!cmpRecordKeys(tdbb, rpb.rpb_record, relation_1, insertion_idx,
										   record, relation_2, record_idx))
						{
							continue;
						}
					}
				}

				result = idx_e_duplicate;
				break;
			}
		}
	} while (accessor.getNext());

	if (rpb.rpb_record != gc_record)
		delete rpb.rpb_record;

	return result;
}


static idx_e check_foreign_key(thread_db* tdbb,
							   Record* record,
							   jrd_rel* relation,
							   jrd_tra* transaction,
							   index_desc* idx,
							   IndexErrorContext& context)
{
/**************************************
 *
 *	c h e c k _ f o r e i g n _ k e y
 *
 **************************************
 *
 * Functional description
 *	The passed index participates in a foreign key.
 *	Check the passed record to see if a corresponding
 *	record appears in the partner index.
 *
 **************************************/
	SET_TDBB(tdbb);

	idx_e result = idx_e_ok;

	if (!MET_lookup_partner(tdbb, relation, idx, {}))
		return result;

	jrd_rel* partner_relation = NULL;
	USHORT index_id = 0;

	if (idx->idx_flags & idx_foreign)
	{
		partner_relation = MET_relation(tdbb, idx->idx_primary_relation);
		index_id = idx->idx_primary_index;
		result = check_partner_index(tdbb, relation, record, transaction, idx,
									 partner_relation, index_id);
	}
	else if (idx->idx_flags & (idx_primary | idx_unique))
	{
		for (int index_number = 0;
			index_number < (int) idx->idx_foreign_primaries->count();
			index_number++)
		{
			if (idx->idx_id != (*idx->idx_foreign_primaries)[index_number])
				continue;

			partner_relation = MET_relation(tdbb, (*idx->idx_foreign_relations)[index_number]);
			index_id = (*idx->idx_foreign_indexes)[index_number];

			if ((relation->rel_flags & REL_temp_conn) && (partner_relation->rel_flags & REL_temp_tran))
			{
				jrd_rel::RelPagesSnapshot pagesSnapshot(tdbb, partner_relation);
				partner_relation->fillPagesSnapshot(pagesSnapshot, true);

				for (FB_SIZE_T i = 0; i < pagesSnapshot.getCount(); i++)
				{
					RelationPages* partnerPages = pagesSnapshot[i];
					tdbb->tdbb_temp_traid = partnerPages->rel_instance_id;
					if ( (result = check_partner_index(tdbb, relation, record,
								transaction, idx, partner_relation, index_id)) )
					{
						break;
					}
				}

				tdbb->tdbb_temp_traid = 0;
				if (result)
					break;
			}
			else
			{
				if ( (result = check_partner_index(tdbb, relation, record,
							transaction, idx, partner_relation, index_id)) )
				{
					break;
				}
			}
		}
	}

	if (result)
	{
		if (idx->idx_flags & idx_foreign)
			context.setErrorLocation(relation, idx->idx_id);
		else
			context.setErrorLocation(partner_relation, index_id);
	}

	return result;
}


static idx_e check_partner_index(thread_db* tdbb,
								 jrd_rel* relation,
								 Record* record,
								 jrd_tra* transaction,
								 index_desc* idx,
								 jrd_rel* partner_relation,
								 USHORT index_id)
{
/**************************************
 *
 *	c h e c k _ p a r t n e r _ i n d e x
 *
 **************************************
 *
 * Functional description
 *	The passed index participates in a foreign key.
 *	Check the passed record to see if a corresponding
 *	record appears in the partner index.
 *
 **************************************/
	SET_TDBB(tdbb);

	// get the index root page for the partner relation

	WIN window(get_root_page(tdbb, partner_relation));
	index_root_page* root = (index_root_page*) CCH_FETCH(tdbb, &window, LCK_read, pag_root);

	// get the description of the partner index

	index_desc partner_idx;
	if (!BTR_description(tdbb, partner_relation, root, &partner_idx, index_id))
	{
		CCH_RELEASE(tdbb, &window);
		BUGCHECK(175);			// msg 175 partner index description not found
	}

	fb_assert(!(idx->idx_flags & idx_condition));
	fb_assert(!(partner_idx.idx_flags & idx_condition));

	bool starting = false;
	USHORT segment;

	if (!(partner_idx.idx_flags & idx_unique))
	{
		const index_desc::idx_repeat* idx_desc = partner_idx.idx_rpt;
		for (segment = 0; segment < partner_idx.idx_count; ++segment, ++idx_desc)
		{
			if (idx_desc->idx_itype >= idx_first_intl_string)
			{
				TextType* textType = INTL_texttype_lookup(tdbb, INTL_INDEX_TO_TEXT(idx_desc->idx_itype));

				if (textType->getFlags() & TEXTTYPE_SEPARATE_UNIQUE)
				{
					starting = true;
					++segment;
					break;
				}
			}
		}
	}
	else
		segment = idx->idx_count;

	// get the key in the original index
	// AB: Fake the index to be an unique index, because the INTL makes
	// different keys depending on unique index or not.
	// The key build should be exactly the same as stored in the
	// unique index, because a comparison is done on both keys.
	index_desc tmpIndex = *idx;
	// ASF: Was incorrect to verify broken foreign keys.
	// Should not use an unique key to search a non-unique index.
	// tmpIndex.idx_flags |= idx_unique;
	tmpIndex.idx_flags = (tmpIndex.idx_flags & ~idx_unique) | (partner_idx.idx_flags & idx_unique);

	const auto keyType = starting ? INTL_KEY_PARTIAL :
		(tmpIndex.idx_flags & idx_unique) ? INTL_KEY_UNIQUE : INTL_KEY_SORT;

	IndexKey key(tdbb, relation, &tmpIndex, keyType, segment);
	auto result = key.compose(record);

	CCH_RELEASE(tdbb, &window);

	// now check for current duplicates

	if (result == idx_e_ok)
	{
		// fill out a retrieval block for the purpose of
		// generating a bitmap of duplicate records

		IndexRetrieval retrieval(partner_relation, &partner_idx, segment, key);
		retrieval.irb_generic = irb_equality | (starting ? irb_starting : 0);

		if (starting && segment < partner_idx.idx_count)
			retrieval.irb_generic |= irb_partial;

		if (partner_idx.idx_flags & idx_descending)
			retrieval.irb_generic |= irb_descending;

		if ((idx->idx_flags & idx_descending) != (partner_idx.idx_flags & idx_descending))
			BTR_complement_key(key);

		RecordBitmap bm(*tdbb->getDefaultPool());
		RecordBitmap* bitmap = &bm;
		BTR_evaluate(tdbb, &retrieval, &bitmap, NULL);

		// if there is a bitmap, it means duplicates were found

		if (bitmap->getFirst())
		{
			index_insertion insertion;
			insertion.iib_descriptor = &partner_idx;
			insertion.iib_relation = partner_relation;
			insertion.iib_number.setValue(BOF_NUMBER);
			insertion.iib_duplicates = bitmap;
			insertion.iib_transaction = transaction;
			insertion.iib_btr_level = 0;

			result = check_duplicates(tdbb, record, idx, &insertion, relation);
			if (idx->idx_flags & (idx_primary | idx_unique))
				result = result ? idx_e_foreign_references_present : idx_e_ok;
			if (idx->idx_flags & idx_foreign)
				result = result ? idx_e_ok : idx_e_foreign_target_doesnt_exist;
		}
		else if (idx->idx_flags & idx_foreign)
			result = idx_e_foreign_target_doesnt_exist;
	}

	return result;
}


static bool duplicate_key(const UCHAR* record1, const UCHAR* record2, void* ifl_void)
{
/**************************************
 *
 *	d u p l i c a t e _ k e y
 *
 **************************************
 *
 * Functional description
 *	Callback routine for duplicate keys during index creation.  Just
 *	bump a counter.
 *
 **************************************/
	IndexCreation* ifl_data = static_cast<IndexCreation*>(ifl_void);
	const index_sort_record* rec1 = (index_sort_record*) (record1 + ifl_data->key_length);
	const index_sort_record* rec2 = (index_sort_record*) (record2 + ifl_data->key_length);

	if (!(rec1->isr_flags & (ISR_secondary | ISR_null)) &&
		!(rec2->isr_flags & (ISR_secondary | ISR_null)))
	{
		if (ifl_data->duplicates.exchangeAdd(1) == 0)
			ifl_data->dup_recno = rec2->isr_record_number;
	}

	return false;
}


static PageNumber get_root_page(thread_db* tdbb, jrd_rel* relation)
{
/**************************************
 *
 *	g e t _ r o o t _ p a g e
 *
 **************************************
 *
 * Functional description
 *	Find the root page for a relation.
 *
 **************************************/
	SET_TDBB(tdbb);

	RelationPages* relPages = relation->getPages(tdbb);
	SLONG page = relPages->rel_index_root;
	if (!page)
	{
		DPM_scan_pages(tdbb);
		page = relPages->rel_index_root;
	}

	return PageNumber(relPages->rel_pg_space_id, page);
}


static int index_block_flush(void* ast_object)
{
/**************************************
 *
 *	i n d e x _ b l o c k _ f l u s h
 *
 **************************************
 *
 * Functional description
 *	An exclusive lock has been requested on the
 *	index block.  The information in the cached
 *	index block is no longer valid, so clear it
 *	out and release the lock.
 *
 **************************************/
	IndexBlock* const index_block = static_cast<IndexBlock*>(ast_object);

	try
	{
		Lock* const lock = index_block->idb_lock;
		Database* const dbb = lock->lck_dbb;

		AsyncContextHolder tdbb(dbb, FB_FUNCTION, lock);

		release_index_block(tdbb, index_block);
	}
	catch (const Firebird::Exception&)
	{} // no-op

	return 0;
}


static idx_e insert_key(thread_db* tdbb,
						jrd_rel* relation,
						Record* record,
						jrd_tra* transaction,
						WIN * window_ptr,
						index_insertion* insertion,
						IndexErrorContext& context)
{
/**************************************
 *
 *	i n s e r t _ k e y
 *
 **************************************
 *
 * Functional description
 *	Insert a key in the index.
 *	If this is a unique index, check for active duplicates.
 *	If this is a foreign key, check for duplicates in the
 *	primary key index.
 *
 **************************************/
	SET_TDBB(tdbb);

	idx_e result = idx_e_ok;
	index_desc* idx = insertion->iib_descriptor;

	// Insert the key into the index.  If the index is unique, btr will keep track of duplicates.

	insertion->iib_duplicates = NULL;
	BTR_insert(tdbb, window_ptr, insertion);

	if (insertion->iib_duplicates)
	{
		result = check_duplicates(tdbb, record, idx, insertion, NULL);
		delete insertion->iib_duplicates;
		insertion->iib_duplicates = 0;
	}

	if (result != idx_e_ok)
		return result;

	// if we are dealing with a foreign key index,
	// check for an insert into the corresponding primary key index
	if (idx->idx_flags & idx_foreign)
	{
		// Find out if there is a null segment. If there is one,
		// don't bother to check the primary key.
		if (result == idx_e_ok && insertion->iib_key->key_nulls == 0)
		{
			result = check_foreign_key(tdbb, record, insertion->iib_relation,
									   transaction, idx, context);
		}
	}

	return result;
}


static void release_index_block(thread_db* tdbb, IndexBlock* index_block)
{
/**************************************
 *
 *	r e l e a s e _ i n d e x _ b l o c k
 *
 **************************************
 *
 * Functional description
 *	Release index block structure.
 *
 **************************************/
	if (index_block->idb_expression_statement)
	{
		index_block->idb_expression_statement->release(tdbb);
		index_block->idb_expression_statement = nullptr;
	}
	index_block->idb_expression = nullptr;
	index_block->idb_expression_desc.clear();

	if (index_block->idb_condition_statement)
	{
		index_block->idb_condition_statement->release(tdbb);
		index_block->idb_condition_statement = nullptr;
	}
	index_block->idb_condition = nullptr;

	LCK_release(tdbb, index_block->idb_lock);
}


static void signal_index_deletion(thread_db* tdbb, jrd_rel* relation, USHORT id)
{
/**************************************
 *
 *	s i g n a l _ i n d e x _ d e l e t i o n
 *
 **************************************
 *
 * Functional description
 *	On delete of an index, force all
 *	processes to get rid of index info.
 *
 **************************************/
	IndexBlock* index_block;
	Lock* lock = NULL;

	SET_TDBB(tdbb);

	// get an exclusive lock on the associated index
	// block (if it exists) to make sure that all other
	// processes flush their cached information about this index

	for (index_block = relation->rel_index_blocks; index_block; index_block = index_block->idb_next)
	{
		if (index_block->idb_id == id)
		{
			lock = index_block->idb_lock;
			break;
		}
	}

	// if one didn't exist, create it

	if (!index_block)
	{
		index_block = IDX_create_index_block(tdbb, relation, id);
		lock = index_block->idb_lock;
	}

	// signal other processes to clear out the index block

	if (lock->lck_physical == LCK_SR) {
		LCK_convert(tdbb, lock, LCK_EX, LCK_WAIT);
	}
	else {
		LCK_lock(tdbb, lock, LCK_EX, LCK_WAIT);
	}

	release_index_block(tdbb, index_block);
}
