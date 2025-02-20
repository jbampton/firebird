/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		exe.cpp
 *	DESCRIPTION:	Statement execution
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
 * 2001.6.21 Claudio Valderrama: Allow inserting strings into blob fields.
 * 2001.6.28 Claudio Valderrama: Move code to cleanup_rpb() as directed
 * by Ann Harrison and cleanup of new record in store() routine.
 * 2001.10.11 Claudio Valderrama: Fix SF Bug #436462: From now, we only
 * count real store, modify and delete operations either in an external
 * file or in a table. Counting on a view caused up to three operations
 * being reported instead of one.
 * 2001.12.03 Claudio Valderrama: new visit to the same issue: views need
 * to count virtual operations, not real I/O on the underlying tables.
 * 2002.09.28 Dmitry Yemanov: Reworked internal_info stuff, enhanced
 *                            exception handling in SPs/triggers,
 *                            implemented ROWS_AFFECTED system variable
 *
 * 2002.10.21 Nickolay Samofatov: Added support for explicit pessimistic locks
 * 2002.10.28 Sean Leyne - Code cleanup, removed obsolete "MPEXL" port
 * 2002.10.28 Sean Leyne - Code cleanup, removed obsolete "DecOSF" port
 * 2002.10.29 Nickolay Samofatov: Added support for savepoints
 * 2002.10.30 Sean Leyne - Removed support for obsolete "PC_PLATFORM" define
 * 2003.10.05 Dmitry Yemanov: Added support for explicit cursors in PSQL
 * Adriano dos Santos Fernandes
 *
 */

#include "firebird.h"
#ifdef TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#include "../jrd/ibsetjmp.h"
#include "../common/classes/VaryStr.h"
#include <string.h>
#include "../jrd/jrd.h"
#include "../jrd/req.h"
#include "../jrd/val.h"
#include "../jrd/exe.h"
#include "../jrd/extds/ExtDS.h"
#include "../jrd/tra.h"
#include "iberror.h"
#include "../jrd/ods.h"
#include "../jrd/btr.h"
#include "../jrd/lck.h"
#include "../jrd/intl.h"
#include "../jrd/sbm.h"
#include "../jrd/blb.h"
#include "../jrd/SystemTriggers.h"
#include "firebird/impl/blr.h"
#include "../dsql/ExprNodes.h"
#include "../dsql/StmtNodes.h"
#include "../jrd/blb_proto.h"
#include "../jrd/btr_proto.h"
#include "../jrd/cmp_proto.h"
#include "../jrd/dfw_proto.h"
#include "../jrd/dpm_proto.h"
#include "../jrd/err_proto.h"
#include "../jrd/evl_proto.h"
#include "../jrd/exe_proto.h"
#include "../jrd/ext_proto.h"
#include "../yvalve/gds_proto.h"
#include "../jrd/idx_proto.h"
#include "../jrd/intl_proto.h"
#include "../jrd/jrd_proto.h"

#include "../jrd/lck_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/par_proto.h"
#include "../jrd/rlck_proto.h"

#include "../jrd/tra_proto.h"
#include "../jrd/vio_proto.h"
#include "../common/isc_s_proto.h"

#include "../dsql/dsql_proto.h"
#include "../jrd/rpb_chain.h"
#include "../jrd/RecordSourceNodes.h"
#include "../jrd/VirtualTable.h"
#include "../jrd/trace/TraceManager.h"
#include "../jrd/trace/TraceJrdHelpers.h"

#include "../dsql/Nodes.h"
#include "../jrd/recsrc/RecordSource.h"
#include "../jrd/recsrc/Cursor.h"
#include "../jrd/Function.h"
#include "../jrd/ProfilerManager.h"


using namespace Jrd;
using namespace Firebird;

// Item class implementation

string Item::getDescription(Request* request, const ItemInfo* itemInfo) const
{
	if (itemInfo && itemInfo->name.hasData())
		return itemInfo->name.toQuotedString();

	int oneBasedIndex = index + 1;
	string s;

	if (type == Item::TYPE_VARIABLE)
	{
		const auto* const procedure = request->getStatement()->procedure;

		if (procedure)
		{
			if (oneBasedIndex <= int(procedure->getOutputFields().getCount()))
				s.printf("[output parameter number %d]", oneBasedIndex);
			else
			{
				s.printf("[number %d]",
					oneBasedIndex - int(procedure->getOutputFields().getCount()));
			}
		}
		else
			s.printf("[number %d]", oneBasedIndex);
	}
	else if (type == Item::TYPE_PARAMETER && subType == 0)
		s.printf("[input parameter number %d]", (oneBasedIndex - 1) / 2 + 1);
	else if (type == Item::TYPE_PARAMETER && subType == 1)
		s.printf("[output parameter number %d]", oneBasedIndex);

	if (s.isEmpty())
		s = UNKNOWN_STRING_MARK;

	return s;
}

// AffectedRows class implementation

AffectedRows::AffectedRows()
{
	clear();
}

void AffectedRows::clear()
{
	writeFlag = false;
	fetchedRows = modifiedRows = 0;
}

void AffectedRows::bumpFetched()
{
	fetchedRows++;
}

void AffectedRows::bumpModified(bool increment)
{
	if (increment) {
		modifiedRows++;
	}
	else {
		writeFlag = true;
	}
}

int AffectedRows::getCount() const
{
	return writeFlag ? modifiedRows : fetchedRows;
}

// StatusXcp class implementation

StatusXcp::StatusXcp()
	: status(*getDefaultMemoryPool())
{
	clear();
}

void StatusXcp::clear()
{
	status->init();
}

void StatusXcp::init(const FbStatusVector* vector)
{
	fb_utils::copyStatus(&status, vector);
}

void StatusXcp::copyTo(FbStatusVector* vector) const
{
	fb_utils::copyStatus(vector, &status);
}

bool StatusXcp::success() const
{
	return !(status->getState() & IStatus::STATE_ERRORS);
}

SLONG StatusXcp::as_gdscode() const
{
	return status->getErrors()[1];
}

SLONG StatusXcp::as_sqlcode() const
{
	return gds__sqlcode(status->getErrors());
}

void StatusXcp::as_sqlstate(char* sqlstate) const
{
	fb_sqlstate(sqlstate, status->getErrors());
}

SLONG StatusXcp::as_xcpcode() const
{
	return (status->getErrors()[1] == isc_except) ? (SLONG) status->getErrors()[3] : 0;
}

string StatusXcp::as_text() const
{
	const ISC_STATUS* status_ptr = status->getErrors();

	string errorText;

	TEXT buffer[BUFFER_LARGE];
	while (fb_interpret(buffer, sizeof(buffer), &status_ptr))
	{
		if (errorText.hasData())
			errorText += "\n";

		errorText += buffer;
	}

	return errorText;
}


static void activate_request(thread_db* tdbb, Request* request, jrd_tra* transaction);
static void execute_looper(thread_db*, Request*, jrd_tra*, const StmtNode*, Request::req_s);
static void looper_seh(thread_db*, Request*, const StmtNode*);
static void release_blobs(thread_db*, Request*);
static void trigger_failure(thread_db*, Request*);
static void stuff_stack_trace(const Request*);

const size_t MAX_STACK_TRACE = 2048;


namespace
{
	void forgetSavepoint(thread_db* tdbb, Request* request, jrd_tra* transaction, SavNumber savNumber);
	SavNumber startSavepoint(Request* request, jrd_tra* transaction);

	void forgetSavepoint(thread_db* tdbb, Request* request, jrd_tra* transaction, SavNumber savNumber)
	{
		while (transaction->tra_save_point &&
			transaction->tra_save_point->getNumber() >= savNumber)
		{
			const auto savepoint = transaction->tra_save_point;
			// Forget about any undo for this verb
			fb_assert(!transaction->tra_save_point->isChanging());
			transaction->releaseSavepoint(tdbb);
			// Preserve savepoint for reuse
			fb_assert(savepoint == transaction->tra_save_free);
			transaction->tra_save_free = savepoint->moveToStack(request->req_savepoints);
			fb_assert(savepoint != transaction->tra_save_free);

			// Ensure that the priorly existing savepoints are preserved,
			// e.g. 10-11-12-(5-6-7) where savNumber == 5. This may happen
			// due to looper savepoints being reused in subsequent invokations.
			if (savepoint->getNumber() == savNumber)
				break;
		}
	}

	SavNumber startSavepoint(Request* request, jrd_tra* transaction)
	{
		if (!(request->req_flags & req_proc_fetch) && request->req_transaction)
		{
			if (transaction && !(transaction->tra_flags & TRA_system))
			{
				if (request->req_savepoints)
				{
					request->req_savepoints =
						request->req_savepoints->moveToStack(transaction->tra_save_point);
				}
				else
					transaction->startSavepoint();

				return transaction->tra_save_point->getNumber();
			}
		}

		return 0;
	}
}	// anonymous namespace


// Perform an assignment.
void EXE_assignment(thread_db* tdbb, const AssignmentNode* node)
{
	DEV_BLKCHK(node, type_nod);

	SET_TDBB(tdbb);
	Request* request = tdbb->getRequest();

	// Get descriptors of src field/parameter/variable, etc.
	request->req_flags &= ~req_null;
	dsc* from_desc = EVL_expr(tdbb, request, node->asgnFrom);

	EXE_assignment(tdbb, node->asgnTo, from_desc, (request->req_flags & req_null),
		node->missing, node->missing2);
}

// Perform an assignment.
void EXE_assignment(thread_db* tdbb, const ValueExprNode* source, const ValueExprNode* target)
{
	SET_TDBB(tdbb);
	Request* request = tdbb->getRequest();

	// Get descriptors of src field/parameter/variable, etc.
	request->req_flags &= ~req_null;
	dsc* from_desc = EVL_expr(tdbb, request, source);

	EXE_assignment(tdbb, target, from_desc, (request->req_flags & req_null), NULL, NULL);
}

// Perform an assignment.
void EXE_assignment(thread_db* tdbb, const ValueExprNode* to, dsc* from_desc, bool from_null,
	const ValueExprNode* missing_node, const ValueExprNode* missing2_node)
{
	SET_TDBB(tdbb);
	Request* request = tdbb->getRequest();

	const auto toVar = nodeAs<VariableNode>(to);

	if (toVar && toVar->outerDecl)
		request = toVar->getVarRequest(request);

	AutoSetRestore2<Request*, thread_db> autoSetRequest(
		tdbb, &thread_db::getRequest, &thread_db::setRequest, request);

	// Get descriptors of receiving and sending fields/parameters, variables, etc.

	dsc* missing = NULL;
	if (missing_node)
		missing = EVL_expr(tdbb, request, missing_node);

	// Get descriptor of target field/parameter/variable, etc.
	DSC* to_desc = EVL_assign_to(tdbb, to);

	request->req_flags &= ~req_null;

	// NS: If we are assigning to NULL, we finished.
	// This functionality is currently used to allow calling UDF routines
	// without assigning resulting value anywhere.
	if (!to_desc)
		return;

	SSHORT null = from_null ? -1 : 0;

	if (!null && missing && MOV_compare(tdbb, missing, from_desc) == 0)
		null = -1;

	USHORT* impure_flags = NULL;
	const auto toParam = nodeAs<ParameterNode>(to);

	if (toParam)
	{
		const MessageNode* message = toParam->message;
		const auto paramRequest = toParam->getParamRequest(request);

		if (toParam->argInfo)
		{
			AutoSetRestore2<Request*, thread_db> autoSetRequest(
				tdbb, &thread_db::getRequest, &thread_db::setRequest, paramRequest);

			EVL_validate(tdbb, Item(Item::TYPE_PARAMETER, message->messageNumber, toParam->argNumber),
				toParam->argInfo, from_desc, null == -1);
		}

		impure_flags = paramRequest->getImpure<USHORT>(
			message->impureFlags + (sizeof(USHORT) * toParam->argNumber));
	}
	else if (toVar)
	{
		const auto varRequest = toVar->getVarRequest(request);

		if (toVar->varInfo)
		{
			AutoSetRestore2<Request*, thread_db> autoSetRequest(
				tdbb, &thread_db::getRequest, &thread_db::setRequest, varRequest);

			EVL_validate(tdbb, Item(Item::TYPE_VARIABLE, toVar->varId),
				toVar->varInfo, from_desc, null == -1);
		}

		impure_flags = &varRequest->getImpure<impure_value>(
			toVar->varDecl->impureOffset)->vlu_flags;
	}

	if (impure_flags)
		*impure_flags |= VLU_initialized | VLU_checked;

	// If the value is non-missing, move/convert it.  Otherwise fill the
	// field with appropriate nulls.
	dsc temp;

	if (!null)
	{
		// Validate range for datetime values

		if (DTYPE_IS_DATE(from_desc->dsc_dtype))
		{
			switch (from_desc->dsc_dtype)
			{
				case dtype_sql_date:
					if (!TimeStamp::isValidDate(*(GDS_DATE*) from_desc->dsc_address))
					{
						ERR_post(Arg::Gds(isc_date_range_exceeded));
					}
					break;

				case dtype_sql_time:
				case dtype_sql_time_tz:
				case dtype_ex_time_tz:
					if (!TimeStamp::isValidTime(*(GDS_TIME*) from_desc->dsc_address))
					{
						ERR_post(Arg::Gds(isc_time_range_exceeded));
					}
					break;

				case dtype_timestamp:
				case dtype_timestamp_tz:
				case dtype_ex_timestamp_tz:
					if (!TimeStamp::isValidTimeStamp(*(GDS_TIMESTAMP*) from_desc->dsc_address))
					{
						ERR_post(Arg::Gds(isc_datetime_range_exceeded));
					}
					break;

				default:
					fb_assert(false);
			}
		}

		if (DTYPE_IS_BLOB_OR_QUAD(from_desc->dsc_dtype) || DTYPE_IS_BLOB_OR_QUAD(to_desc->dsc_dtype))
		{
			// ASF: Don't let MOV_move call blb::move because MOV
			// will not pass the destination field to blb::move.

			jrd_rel* relation = nullptr;
			Record* record = nullptr;
			USHORT fieldId = 0;
			bool bulk = false;

			if (to)
			{
				const FieldNode* toField = nodeAs<FieldNode>(to);
				if (toField)
				{
					const auto rpb = &request->req_rpb[toField->fieldStream];
					relation = rpb->rpb_relation;
					record = rpb->rpb_record;
					fieldId = toField->fieldId;
					bulk = rpb->rpb_stream_flags & RPB_s_bulk;
				}
				else if (!(nodeAs<ParameterNode>(to) || nodeAs<VariableNode>(to)))
					BUGCHECK(199);	// msg 199 expected field node
			}

			blb::move(tdbb, from_desc, to_desc, relation, record, fieldId, bulk);
		}
		else if (!DSC_EQUIV(from_desc, to_desc, false))
		{
			MOV_move(tdbb, from_desc, to_desc);
		}
		else if (from_desc->dsc_dtype == dtype_short)
		{
			*((SSHORT*) to_desc->dsc_address) = *((SSHORT*) from_desc->dsc_address);
		}
		else if (from_desc->dsc_dtype == dtype_long)
		{
			*((SLONG*) to_desc->dsc_address) = *((SLONG*) from_desc->dsc_address);
		}
		else if (from_desc->dsc_dtype == dtype_int64)
		{
			*((SINT64*) to_desc->dsc_address) = *((SINT64*) from_desc->dsc_address);
		}
		else
		{
			memcpy(to_desc->dsc_address, from_desc->dsc_address, from_desc->dsc_length);
		}

		to_desc->dsc_flags &= ~DSC_null;
	}
	else
	{
		if (missing2_node && (missing = EVL_expr(tdbb, request, missing2_node)))
			MOV_move(tdbb, missing, to_desc);
		else
			memset(to_desc->dsc_address, 0, to_desc->dsc_length);

		to_desc->dsc_flags |= DSC_null;
	}

	// Handle the null flag as appropriate for fields and message arguments.

	const FieldNode* toField = nodeAs<FieldNode>(to);
	if (toField)
	{
		Record* record = request->req_rpb[toField->fieldStream].rpb_record;

		if (null)
			record->setNull(toField->fieldId);
		else
			record->clearNull(toField->fieldId);
	}
	else if (toParam && toParam->argFlag)
	{
		to_desc = EVL_assign_to(tdbb, toParam->argFlag);

		// If the null flag is a string with an effective length of one,
		// then -1 will not fit.  Therefore, store 1 instead.

		if (null && to_desc->dsc_dtype <= dtype_varying)
		{
			USHORT minlen;

			switch (to_desc->dsc_dtype)
			{
			case dtype_text:
				minlen = 1;
				break;
			case dtype_cstring:
				minlen = 2;
				break;
			case dtype_varying:
				minlen = 3;
				break;
			}

			if (to_desc->dsc_length <= minlen)
				null = 1;
		}

		temp.dsc_dtype = dtype_short;
		temp.dsc_length = sizeof(SSHORT);
		temp.dsc_scale = 0;
		temp.dsc_sub_type = 0;
		temp.dsc_address = (UCHAR*) &null;
		MOV_move(tdbb, &temp, to_desc);
	}
}


void EXE_execute_db_triggers(thread_db* tdbb, jrd_tra* transaction, TriggerAction trigger_action)
{
/**************************************
 *
 *	E X E _ e x e c u t e _ d b _ t r i g g e r s
 *
 **************************************
 *
 * Functional description
 *	Execute database triggers
 *
 **************************************/
	const auto attachment = tdbb->getAttachment();

	// Do nothing if user doesn't want database triggers
	if (attachment->att_flags & ATT_no_db_triggers)
		return;

	int type = 0;

	switch (trigger_action)
	{
		case TRIGGER_CONNECT:
			type = DB_TRIGGER_CONNECT;
			break;

		case TRIGGER_DISCONNECT:
			type = DB_TRIGGER_DISCONNECT;
			break;

		case TRIGGER_TRANS_START:
			type = DB_TRIGGER_TRANS_START;
			break;

		case TRIGGER_TRANS_COMMIT:
			type = DB_TRIGGER_TRANS_COMMIT;
			break;

		case TRIGGER_TRANS_ROLLBACK:
			type = DB_TRIGGER_TRANS_ROLLBACK;
			break;

		default:
			fb_assert(false);
			return;
	}

	if (attachment->att_triggers[type])
	{
		AutoSetRestore2<jrd_tra*, thread_db> tempTrans(tdbb,
			&thread_db::getTransaction,
			&thread_db::setTransaction,
			transaction);

		EXE_execute_triggers(tdbb, &attachment->att_triggers[type],
			NULL, NULL, trigger_action, StmtNode::ALL_TRIGS);
	}
}


// Execute DDL triggers.
void EXE_execute_ddl_triggers(thread_db* tdbb, jrd_tra* transaction, bool preTriggers, int action)
{
	const auto attachment = tdbb->getAttachment();

	// Our caller verifies (ATT_no_db_triggers) if DDL triggers should not run

	if (attachment->att_ddl_triggers)
	{
		AutoSetRestore2<jrd_tra*, thread_db> tempTrans(tdbb,
			&thread_db::getTransaction,
			&thread_db::setTransaction,
			transaction);

		EXE_execute_triggers(tdbb, &attachment->att_ddl_triggers, NULL, NULL, TRIGGER_DDL,
			preTriggers ? StmtNode::PRE_TRIG : StmtNode::POST_TRIG, action);
	}
}


void EXE_receive(thread_db* tdbb,
				 Request* request,
				 USHORT msg,
				 ULONG length,
				 void* buffer,
				 bool top_level)
{
/**************************************
 *
 *	E X E _ r e c e i v e
 *
 **************************************
 *
 * Functional description
 *	Move a message from JRD to the host program.  This corresponds to
 *	a JRD BLR/Stmtode* send.
 *
 **************************************/
	SET_TDBB(tdbb);

	DEV_BLKCHK(request, type_req);

	JRD_reschedule(tdbb);

	jrd_tra* transaction = request->req_transaction;

	if (!(request->req_flags & req_active))
		ERR_post(Arg::Gds(isc_req_sync));

	SavNumber savNumber = 0;

	if (request->req_flags & req_proc_fetch)
	{
		/* request->req_proc_sav_point stores the request savepoints.
		   When going to continue execution put request save point list
		   into transaction->tra_save_point so that it is used in looper.
		   When we come back to EXE_receive() merge all work done under
		   stored procedure savepoints into the current transaction
		   savepoint, which is the savepoint for fetch and save them into the list. */

		if (request->req_proc_sav_point)
		{
			// We assume here that the saved savepoint stack starts with the
			// smallest number, the logic will be broken if this ever changes
			savNumber = request->req_proc_sav_point->getNumber();
			// Push all saved savepoints to the top of transaction savepoints stack
			Savepoint::mergeStacks(transaction->tra_save_point, request->req_proc_sav_point);
			fb_assert(!request->req_proc_sav_point);
		}
		else
		{
			const auto savepoint = transaction->startSavepoint();
			savNumber = savepoint->getNumber();
		}
	}

	try
	{
		if (nodeIs<StallNode>(request->req_message))
			execute_looper(tdbb, request, transaction, request->req_next, Request::req_sync);

		if (!(request->req_flags & req_active) || request->req_operation != Request::req_send)
			ERR_post(Arg::Gds(isc_req_sync));

		const MessageNode* message = nodeAs<MessageNode>(request->req_message);
		const Format* format = message->format;

		if (msg != message->messageNumber)
			ERR_post(Arg::Gds(isc_req_sync));

		if (length != format->fmt_length)
			ERR_post(Arg::Gds(isc_port_len) << Arg::Num(length) << Arg::Num(format->fmt_length));

		memcpy(buffer, request->getImpure<UCHAR>(message->impureOffset), length);

		// ASF: temporary blobs returned to the client should not be released
		// with the request, but in the transaction end.
		if (top_level || transaction->tra_temp_blobs_count)
		{
			for (int i = 0; i < format->fmt_count; ++i)
			{
				const DSC* desc = &format->fmt_desc[i];

				if (desc->isBlob())
				{
					const bid* id = (bid*) (static_cast<UCHAR*>(buffer) + (ULONG)(IPTR) desc->dsc_address);

					if (transaction->tra_blobs->locate(id->bid_temp_id()))
					{
						BlobIndex* current = &transaction->tra_blobs->current();

						if (top_level &&
							current->bli_request &&
							current->bli_request->req_blobs.locate(id->bid_temp_id()))
						{
							current->bli_request->req_blobs.fastRemove();
							current->bli_request = NULL;
						}

						if (!current->bli_materialized &&
							(current->bli_blob_object->blb_flags & (BLB_close_on_read | BLB_stream)) ==
								(BLB_close_on_read | BLB_stream))
						{
							current->bli_blob_object->BLB_close(tdbb);
						}
					}
					else if (top_level)
					{
						transaction->checkBlob(tdbb, id, NULL, false);
					}
				}
			}
		}

		execute_looper(tdbb, request, transaction, request->req_next, Request::req_proceed);
	}
	catch (const Exception&)
	{
		// In the case of error, undo changes performed under our savepoint

		if (savNumber)
			transaction->rollbackToSavepoint(tdbb, savNumber);

		throw;
	}

	if (savNumber)
	{
		// At this point request->req_proc_sav_point == NULL that is assured by code above
		fb_assert(!request->req_proc_sav_point);

		try
		{
			// Merge work into target savepoint and save request's savepoints (with numbers!!!)
			// till the next looper iteration
			while (transaction->tra_save_point &&
				transaction->tra_save_point->getNumber() >= savNumber)
			{
				const auto savepoint = transaction->tra_save_point;
				fb_assert(!transaction->tra_save_point->isChanging());
				transaction->releaseSavepoint(tdbb);
				fb_assert(transaction->tra_save_free == savepoint);
				transaction->tra_save_free = savepoint->moveToStack(request->req_proc_sav_point);
				fb_assert(request->req_proc_sav_point == savepoint);

				// Ensure that the priorly existing savepoints are preserved,
				// e.g. 10-11-12-(5-6-7) where savNumber == 5. This may happen
				// due to looper savepoints being reused in subsequent invokations.
				if (savepoint->getNumber() == savNumber)
					break;
			}
		}
		catch (...)
		{
			// If something went wrong, drop already stored savepoints to prevent memory leak
			Savepoint::destroy(request->req_proc_sav_point);
			fb_assert(!request->req_proc_sav_point);
			throw;
		}
	}
}


// Release a request instance.
void EXE_release(thread_db* tdbb, Request* request)
{
	DEV_BLKCHK(request, type_req);

	SET_TDBB(tdbb);

	EXE_unwind(tdbb, request);

	// system requests are released after all attachments gone and with
	// req_attachment not cleared

	const Jrd::Attachment* attachment = tdbb->getAttachment();

	if (request->req_attachment && request->req_attachment == attachment)
	{
		FB_SIZE_T pos;
		if (request->req_attachment->att_requests.find(request, pos))
			request->req_attachment->att_requests.remove(pos);

		request->req_attachment = nullptr;
	}

	request->req_flags &= ~req_in_use;
	if (request->req_timer)
	{
		request->req_timer->stop();
		request->req_timer = nullptr;
	}
}


void EXE_send(thread_db* tdbb, Request* request, USHORT msg, ULONG length, const void* buffer)
{
/**************************************
 *
 *	E X E _ s e n d
 *
 **************************************
 *
 * Functional description
 *	Send a message from the host program to the engine.
 *	This corresponds to a blr_receive or blr_select statement.
 *
 **************************************/
	SET_TDBB(tdbb);
	DEV_BLKCHK(request, type_req);

	JRD_reschedule(tdbb);

	if (!(request->req_flags & req_active))
		ERR_post(Arg::Gds(isc_req_sync));

	if (request->req_operation != Request::req_receive)
		ERR_post(Arg::Gds(isc_req_sync));

	const auto node = request->req_message;
	const StmtNode* message = nullptr;

	if (nodeIs<MessageNode>(node))
		message = node;
	else if (const auto* const selectMessageNode = nodeAs<SelectMessageNode>(node))
	{
		for (const auto statement : selectMessageNode->statements)
		{
			const auto receiveNode = nodeAs<ReceiveNode>(statement);
			message = receiveNode->message;

			if (nodeAs<MessageNode>(message)->messageNumber == msg)
			{
				request->req_next = statement;
				break;
			}
		}
	}
	else
		BUGCHECK(167);	// msg 167 invalid SEND request

	const auto format = nodeAs<MessageNode>(message)->format;

	if (msg != nodeAs<MessageNode>(message)->messageNumber)
		ERR_post(Arg::Gds(isc_req_sync));

	if (length != format->fmt_length)
		ERR_post(Arg::Gds(isc_port_len) << Arg::Num(length) << Arg::Num(format->fmt_length));

	memcpy(request->getImpure<UCHAR>(message->impureOffset), buffer, length);

	execute_looper(tdbb, request, request->req_transaction, request->req_next, Request::req_proceed);
}


// Mark a request as active.
static void activate_request(thread_db* tdbb, Request* request, jrd_tra* transaction)
{
	SET_TDBB(tdbb);

	BLKCHK(request, type_req);
	BLKCHK(transaction, type_tra);

	if (request->req_flags & req_active)
		ERR_post(Arg::Gds(isc_req_sync) << Arg::Gds(isc_reqinuse));

	if (transaction->tra_flags & TRA_prepared)
		ERR_post(Arg::Gds(isc_req_no_trans));

	const auto dbb = tdbb->getDatabase();
	const auto statement = request->getStatement();

	// Generate request id.
	request->setRequestId(
		request->isRequestIdUnassigned() && request->isRoot() ?
			statement->getStatementId() :
			dbb->generateStatementId());

	/* Post resources to transaction block.  In particular, the interest locks
	on relations/indices are copied to the transaction, which is very
	important for (short-lived) dynamically compiled requests.  This will
	provide transaction stability by preventing a relation from being
	dropped after it has been referenced from an active transaction. */

	TRA_post_resources(tdbb, transaction, statement->resources);

	TRA_attach_request(transaction, request);
	request->req_flags &= req_in_use | req_restart_ready;
	request->req_flags |= req_active;
	request->req_flags &= ~req_reserved;

	// set up to count records affected by request

	request->req_records_selected = 0;
	request->req_records_updated = 0;
	request->req_records_inserted = 0;
	request->req_records_deleted = 0;

	request->req_records_affected.clear();

	for (auto& rpb : request->req_rpb)
		rpb.rpb_runtime_flags = 0;

	request->req_profiler_ticks = 0;

	// Store request start time for timestamp work
	request->validateTimeStamp();

	// Set all invariants to not computed.
	const ULONG* const* ptr, * const* end;
	for (ptr = statement->invariants.begin(), end = statement->invariants.end();
		 ptr < end; ++ptr)
	{
		impure_value* impure = request->getImpure<impure_value>(**ptr);
		impure->vlu_flags = 0;
	}

	request->req_src_line = 0;
	request->req_src_column = 0;

	TRA_setup_request_snapshot(tdbb, request);
}


// Execute function. A shortcut for node-based function but required for external functions.
void EXE_execute_function(thread_db* tdbb, Request* request, jrd_tra* transaction,
	ULONG inMsgLength, UCHAR* inMsg, ULONG outMsgLength, UCHAR* outMsg)
{
	if (const auto function = request->getStatement()->function; function && function->fun_external)
	{
		activate_request(tdbb, request, transaction);

		const auto attachment = tdbb->getAttachment();

		// Ensure the cancellation lock can be triggered
		const auto lock = attachment->att_cancel_lock;
		if (lock && lock->lck_logical == LCK_none)
			LCK_lock(tdbb, lock, LCK_SR, LCK_WAIT);

		const SavNumber savNumber = startSavepoint(request, transaction);

		if (!request->req_transaction)
			ERR_post(Arg::Gds(isc_req_no_trans));

		try
		{
			// Save the old pool and request to restore on exit
			StmtNode::ExeState exeState(tdbb, request, request->req_transaction);
			Jrd::ContextPoolHolder context(tdbb, request->req_pool);

			fb_assert(!request->req_caller);
			request->req_caller = exeState.oldRequest;

			tdbb->tdbb_flags &= ~(TDBB_stack_trace_done | TDBB_sys_error);

			// Execute stuff until we drop

			const auto profilerManager = attachment->isProfilerActive() && !request->hasInternalStatement() ?
				attachment->getProfilerManager(tdbb) : nullptr;
			const SINT64 profilerInitialTicks = profilerManager ? profilerManager->queryTicks() : 0;
			const SINT64 profilerInitialAccumulatedOverhead = profilerManager ?
				profilerManager->getAccumulatedOverhead() : 0;

			try
			{
				function->fun_external->execute(tdbb, request, transaction, inMsgLength, inMsg, outMsgLength, outMsg);

				tdbb->checkCancelState();
			}
			catch (const Exception& ex)
			{
				ex.stuffException(tdbb->tdbb_status_vector);

				request->adjustCallerStats();

				// Ensure the transaction hasn't disappeared in the meantime
				fb_assert(request->req_transaction);

				// If the database is already bug-checked, then get out
				if (tdbb->getDatabase()->dbb_flags & DBB_bugcheck)
					status_exception::raise(tdbb->tdbb_status_vector);

				exeState.errorPending = true;

				if (!(tdbb->tdbb_flags & TDBB_stack_trace_done) && !(tdbb->tdbb_flags & TDBB_sys_error))
				{
					stuff_stack_trace(request);
					tdbb->tdbb_flags |= TDBB_stack_trace_done;
				}
			}

			if (profilerInitialTicks && attachment->isProfilerActive())
			{
				const SINT64 currentProfilerTicks = profilerManager->queryTicks();
				const SINT64 elapsedTicks = profilerManager->getElapsedTicksAndAdjustOverhead(
					currentProfilerTicks, profilerInitialTicks, profilerInitialAccumulatedOverhead);

				request->req_profiler_ticks += elapsedTicks;
			}

			request->adjustCallerStats();

			if (!exeState.errorPending)
				TRA_release_request_snapshot(tdbb, request);

			request->req_flags &= ~(req_active | req_reserved);
			request->invalidateTimeStamp();

			if (profilerInitialTicks && attachment->isProfilerActive())
			{
				ProfilerManager::Stats stats(request->req_profiler_ticks);
				profilerManager->onRequestFinish(request, stats);
			}

			fb_assert(request->req_caller == exeState.oldRequest);
			request->req_caller = nullptr;

			// Ensure the transaction hasn't disappeared in the meantime
			fb_assert(request->req_transaction);

			// In the case of a pending error condition (one which did not
			// result in a exception to the top of looper), we need to
			// release the request snapshot

			if (exeState.errorPending)
			{
				TRA_release_request_snapshot(tdbb, request);
				ERR_punt();
			}

			if (request->req_flags & req_abort)
				ERR_post(Arg::Gds(isc_req_sync));
		}
		catch (const Exception&)
		{
			// In the case of error, undo changes performed under our savepoint

			if (savNumber)
				transaction->rollbackToSavepoint(tdbb, savNumber);

			throw;
		}

		// If any requested modify/delete/insert ops have completed, forget them

		if (savNumber)
		{
			// There should be no other savepoint but the one started by ourselves.
			fb_assert(transaction->tra_save_point && transaction->tra_save_point->getNumber() == savNumber);

			forgetSavepoint(tdbb, request, transaction, savNumber);
		}
	}
	else
	{
		EXE_start(tdbb, request, transaction);

		if (inMsgLength != 0)
			EXE_send(tdbb, request, 0, inMsgLength, inMsg);

		EXE_receive(tdbb, request, 1, outMsgLength, outMsg);
	}
}


// Start and execute a request.
void EXE_start(thread_db* tdbb, Request* request, jrd_tra* transaction)
{
	activate_request(tdbb, request, transaction);

	execute_looper(tdbb, request, transaction, request->getStatement()->topNode, Request::req_evaluate);
}


void EXE_unwind(thread_db* tdbb, Request* request)
{
/**************************************
 *
 *	E X E _ u n w i n d
 *
 **************************************
 *
 * Functional description
 *	Unwind a request, maybe active, maybe not.
 *
 **************************************/
	DEV_BLKCHK(request, type_req);

	SET_TDBB(tdbb);

	if (request->req_flags & req_active)
	{
		const Statement* statement = request->getStatement();

		if (statement->fors.hasData() || request->req_ext_resultset || request->req_ext_stmt)
		{
			Jrd::ContextPoolHolder context(tdbb, request->req_pool);
			Request* old_request = tdbb->getRequest();
			jrd_tra* old_transaction = tdbb->getTransaction();

			try
			{
				tdbb->setRequest(request);
				tdbb->setTransaction(request->req_transaction);

				for (const auto select : statement->fors)
					select->close(tdbb);

				if (request->req_ext_resultset)
				{
					delete request->req_ext_resultset;
					request->req_ext_resultset = nullptr;
				}

				while (request->req_ext_stmt)
					request->req_ext_stmt->close(tdbb);
			}
			catch (const Exception&)
			{
				tdbb->setRequest(old_request);
				tdbb->setTransaction(old_transaction);
				throw;
			}

			tdbb->setRequest(old_request);
			tdbb->setTransaction(old_transaction);
		}

		for (auto localTable : statement->localTables)
		{
			if (!localTable)
				continue;

			auto impure = localTable->getImpure(tdbb, request, false);
			impure->recordBuffer->reset();
		}

		release_blobs(tdbb, request);

		const auto attachment = request->req_attachment;

		if (request->req_profiler_ticks && attachment->isProfilerActive() && !request->hasInternalStatement())
		{
			ProfilerManager::Stats stats(request->req_profiler_ticks);
			attachment->getProfilerManager(tdbb)->onRequestFinish(request, stats);
		}
	}

	request->req_sorts.unlinkAll();

	TRA_release_request_snapshot(tdbb, request);
	TRA_detach_request(request);

	request->req_flags &= ~(req_active | req_proc_fetch | req_reserved);
	request->req_flags |= req_abort | req_stall;
	request->invalidateTimeStamp();
	request->req_caller = NULL;
	request->req_proc_inputs = NULL;
	request->req_proc_caller = NULL;
}


static void execute_looper(thread_db* tdbb,
						   Request* request,
						   jrd_tra* transaction,
						   const StmtNode* node,
						   Request::req_s next_state)
{
/**************************************
 *
 *	e x e c u t e _ l o o p e r
 *
 **************************************
 *
 * Functional description
 *	Wrapper around looper. This will execute
 *	looper with the save point mechanism.
 *
 **************************************/
	DEV_BLKCHK(request, type_req);

	SET_TDBB(tdbb);
	Jrd::Attachment* const attachment = tdbb->getAttachment();

	// Ensure the cancellation lock can be triggered

	Lock* const lock = attachment->att_cancel_lock;
	if (lock && lock->lck_logical == LCK_none)
		LCK_lock(tdbb, lock, LCK_SR, LCK_WAIT);

	const SavNumber savNumber = startSavepoint(request, transaction);

	request->req_flags &= ~req_stall;
	request->req_operation = next_state;

	try
	{
		looper_seh(tdbb, request, node);
	}
	catch (const Exception&)
	{
		// In the case of error, undo changes performed under our savepoint

		if (savNumber)
			transaction->rollbackToSavepoint(tdbb, savNumber);

		throw;
	}

	// If any requested modify/delete/insert ops have completed, forget them

	if (savNumber)
	{
		// Unless the looper returns after SUSPEND (this preserves existing savepoints),
		// there should be no other savepoint but the one started by ourselves.
		fb_assert((request->req_flags & req_stall) ||
			(transaction->tra_save_point &&
				transaction->tra_save_point->getNumber() == savNumber));

		forgetSavepoint(tdbb, request, transaction, savNumber);
	}
}


void EXE_execute_triggers(thread_db* tdbb,
						  TrigVector** triggers,
						  record_param* old_rpb,
						  record_param* new_rpb,
						  TriggerAction trigger_action,
						  StmtNode::WhichTrigger which_trig,
						  int ddl_action)
{
/**************************************
 *
 *	e x e c u t e _ t r i g g e r s
 *
 **************************************
 *
 * Functional description
 *	Execute group of triggers.  Return pointer to failing trigger
 *	if any blow up.
 *
 **************************************/
	SET_TDBB(tdbb);

	const auto dbb = tdbb->getDatabase();
	const auto old_rec = old_rpb ? old_rpb->rpb_record : nullptr;
	const auto new_rec = new_rpb ? new_rpb->rpb_record : nullptr;

	if (!(dbb->dbb_flags & DBB_creating) && (old_rpb || new_rpb))
	{
		if (const auto relation = old_rpb ? old_rpb->rpb_relation : new_rpb->rpb_relation;
			relation->rel_flags & REL_system)
		{
			switch (which_trig)
			{
				case StmtNode::PRE_TRIG:
				{
					switch (trigger_action)
					{
						case TriggerAction::TRIGGER_DELETE:
							SystemTriggers::executeBeforeDeleteTriggers(tdbb, relation, old_rec);
							break;

						case TriggerAction::TRIGGER_UPDATE:
							SystemTriggers::executeBeforeUpdateTriggers(tdbb, relation, old_rec, new_rec);
							break;

						case TriggerAction::TRIGGER_INSERT:
							SystemTriggers::executeBeforeInsertTriggers(tdbb, relation, new_rec);
							break;
					}
					break;
				}

				case StmtNode::POST_TRIG:
					switch (trigger_action)
					{
						case TriggerAction::TRIGGER_DELETE:
							SystemTriggers::executeAfterDeleteTriggers(tdbb, relation, old_rec);
							break;
					}
					break;
			}
		}
	}

	if (!*triggers || (*triggers)->isEmpty())
		return;

	Request* const request = tdbb->getRequest();
	jrd_tra* const transaction = request ? request->req_transaction : tdbb->getTransaction();

	RefPtr<TrigVector> vector(*triggers);
	AutoPtr<Record> null_rec;

	const bool is_db_trigger = (!old_rec && !new_rec);

	if (!is_db_trigger && (!old_rec || !new_rec))
	{
		record_param* rpb = old_rpb ? old_rpb : new_rpb;
		fb_assert(rpb && rpb->rpb_relation);
		// copy the record
		MemoryPool& pool = *tdbb->getDefaultPool();
		null_rec = FB_NEW_POOL(pool) Record(pool, MET_current(tdbb, rpb->rpb_relation));
		// initialize all fields to missing
		null_rec->nullify();
	}

	TimeStamp timestamp;

	if (request)
		timestamp = request->getGmtTimeStamp();
	else
		TimeZoneUtil::validateGmtTimeStamp(timestamp);

	Request* trigger = NULL;

	try
	{
		for (TrigVector::iterator ptr = vector->begin(); ptr != vector->end(); ++ptr)
		{
			if (trigger_action == TRIGGER_DDL && ddl_action)
			{
				// Skip triggers not matching our action

				fb_assert(which_trig == StmtNode::PRE_TRIG || which_trig == StmtNode::POST_TRIG);
				const bool preTriggers = (which_trig == StmtNode::PRE_TRIG);

				const auto type = ptr->type & ~TRIGGER_TYPE_MASK;
				const bool preTrigger = ((type & 1) == 0);

				if (!(type & (1LL << ddl_action)) || preTriggers != preTrigger)
					continue;
			}

			ptr->compile(tdbb);

			trigger = ptr->statement->findRequest(tdbb);

			if (!is_db_trigger)
			{
				if (trigger->req_rpb.getCount() > 0)
				{
					trigger->req_rpb[0].rpb_record = old_rec ? old_rec : null_rec.get();

					if (old_rec)
					{
						trigger->req_rpb[0].rpb_number = old_rpb->rpb_number;
						trigger->req_rpb[0].rpb_number.setValid(true);
					}
					else
						trigger->req_rpb[0].rpb_number.setValid(false);
				}

				if (which_trig == StmtNode::PRE_TRIG && trigger_action == TRIGGER_UPDATE)
				{
					new_rpb->rpb_number = old_rpb->rpb_number;
				}

				if (trigger->req_rpb.getCount() > 1)
				{
					trigger->req_rpb[1].rpb_record = new_rec ? new_rec : null_rec.get();

					if (new_rec)
					{
						trigger->req_rpb[1].rpb_number = new_rpb->rpb_number;
						trigger->req_rpb[1].rpb_number.setValid(true);
					}
					else
						trigger->req_rpb[1].rpb_number.setValid(false);
				}
			}

			trigger->setGmtTimeStamp(timestamp.value());
			trigger->req_trigger_action = trigger_action;

			TraceTrigExecute trace(tdbb, trigger, which_trig);

			{	// Scope to replace att_ss_user
				const Statement* s = trigger->getStatement();
				UserId* invoker = s->triggerInvoker ? s->triggerInvoker : tdbb->getAttachment()->att_ss_user;
				AutoSetRestore<UserId*> userIdHolder(&tdbb->getAttachment()->att_ss_user, invoker);

				AutoSetRestore<USHORT> autoOriginalTimeZone(
					&tdbb->getAttachment()->att_original_timezone,
					tdbb->getAttachment()->att_current_timezone);

				if (trigger_action == TRIGGER_DISCONNECT)
				{
					if (!trigger->req_timer)
						trigger->req_timer = FB_NEW_POOL(*tdbb->getAttachment()->att_pool) TimeoutTimer();

					const unsigned int timeOut = tdbb->getDatabase()->dbb_config->getOnDisconnectTrigTimeout() * 1000;
					trigger->req_timer->setup(timeOut, isc_cfg_stmt_timeout);
					trigger->req_timer->start();
					thread_db::TimerGuard timerGuard(tdbb, trigger->req_timer, true);
					EXE_start(tdbb, trigger, transaction); // Under timerGuard scope
				}
				else
					EXE_start(tdbb, trigger, transaction);
			}

			const bool ok = (trigger->req_operation != Request::req_unwind);
			trace.finish(ok ? ITracePlugin::RESULT_SUCCESS : ITracePlugin::RESULT_FAILED);

			EXE_unwind(tdbb, trigger);
			trigger->req_attachment = NULL;
			trigger->req_flags &= ~req_in_use;

			if (!ok)
				trigger_failure(tdbb, trigger);

			trigger = NULL;
		}
	}
	catch (const Exception& ex)
	{
		if (trigger)
		{
			EXE_unwind(tdbb, trigger);
			trigger->req_attachment = NULL;
			trigger->req_flags &= ~req_in_use;

			ex.stuffException(tdbb->tdbb_status_vector);

			if (trigger_action == TRIGGER_DISCONNECT &&
				!(tdbb->tdbb_flags & TDBB_stack_trace_done) && (tdbb->tdbb_flags & TDBB_sys_error))
			{
				stuff_stack_trace(trigger);
				tdbb->tdbb_flags |= TDBB_stack_trace_done;
			}

			trigger_failure(tdbb, trigger);
		}

		throw;
	}
}


bool EXE_get_stack_trace(const Request* request, string& sTrace)
{
	sTrace = "";
	for (const Request* req = request; req; req = req->req_caller)
	{
		const Statement* const statement = req->getStatement();

		string context, name;

		if (statement->triggerName.object.length())
		{
			context = "At trigger";
			name = statement->triggerName.toQuotedString();
		}
		else if (statement->procedure)
		{
			context = statement->parentStatement ? "At sub procedure" : "At procedure";
			name = statement->procedure->getName().toQuotedString();
		}
		else if (statement->function)
		{
			context = statement->parentStatement ? "At sub function" : "At function";
			name = statement->function->getName().toQuotedString();
		}
		else if (req->req_src_line)
		{
			context = "At block";
		}

		if (context.hasData())
		{
			name.trim();

			if (name.hasData())
				context += string(" ") + name;

			if (sTrace.length() + context.length() > MAX_STACK_TRACE)
				break;

			if (sTrace.hasData())
				sTrace += "\n";

			sTrace += context;

			if (req->req_src_line)
			{
				string src_info;
				src_info.printf(" line: %" ULONGFORMAT", col: %" ULONGFORMAT,
								req->req_src_line, req->req_src_column);

				if (sTrace.length() + src_info.length() > MAX_STACK_TRACE)
					break;

				sTrace += src_info;
			}
		}
	}

	return sTrace.hasData();
}


static void stuff_stack_trace(const Request* request)
{
	string sTrace;

	if (EXE_get_stack_trace(request, sTrace))
		ERR_post_nothrow(Arg::Gds(isc_stack_trace) << Arg::Str(sTrace));
}


const StmtNode* EXE_looper(thread_db* tdbb, Request* request, const StmtNode* node)
{
/**************************************
 *
 *	E X E _ l o o p e r
 *
 **************************************
 *
 * Functional description
 *	Cycle thru request execution tree.  Return next node for
 *	execution on stall or request complete.
 *
 **************************************/
	if (!request->req_transaction)
		ERR_post(Arg::Gds(isc_req_no_trans));

	SET_TDBB(tdbb);
	const auto dbb = tdbb->getDatabase();
	const auto attachment = tdbb->getAttachment();

	if (!node)
		BUGCHECK(147);

	// Save the old pool and request to restore on exit
	StmtNode::ExeState exeState(tdbb, request, request->req_transaction);
	Jrd::ContextPoolHolder context(tdbb, request->req_pool);

	fb_assert(request->req_caller == NULL);
	request->req_caller = exeState.oldRequest;

	tdbb->tdbb_flags &= ~(TDBB_stack_trace_done | TDBB_sys_error);

	// Execute stuff until we drop

	ProfilerManager* profilerManager;
	SINT64 profilerInitialTicks, profilerInitialAccumulatedOverhead, profilerLastTicks, profilerLastAccumulatedOverhead;

	if (attachment->isProfilerActive() && !request->hasInternalStatement())
	{
		profilerManager = attachment->getProfilerManager(tdbb);
		profilerInitialTicks = profilerLastTicks = profilerManager->queryTicks();
		profilerInitialAccumulatedOverhead = profilerLastAccumulatedOverhead =
			profilerManager->getAccumulatedOverhead();
	}
	else
	{
		profilerManager = nullptr;
		profilerInitialTicks = 0;
		profilerLastTicks = 0;
		profilerInitialAccumulatedOverhead = 0;
		profilerLastAccumulatedOverhead = 0;
	}

	const StmtNode* profileNode = nullptr;

	const auto profilerCallAfterPsqlLineColumn = [&] {
		const SINT64 currentProfilerTicks = profilerManager->queryTicks();

		if (profileNode)
		{
			const SINT64 elapsedTicks = profilerManager->getElapsedTicksAndAdjustOverhead(
				currentProfilerTicks, profilerLastTicks, profilerLastAccumulatedOverhead);
			ProfilerManager::Stats stats(elapsedTicks);
			profilerManager->afterPsqlLineColumn(request, profileNode->line, profileNode->column, stats);
		}

		return currentProfilerTicks;
	};

	while (node && !(request->req_flags & req_stall))
	{
		try
		{
			if (request->req_operation == Request::req_evaluate)
			{
				JRD_reschedule(tdbb);

				if (node->hasLineColumn)
				{
					request->req_src_line = node->line;
					request->req_src_column = node->column;
				}

				if (attachment->isProfilerActive() && !request->hasInternalStatement())
				{
					if (!profilerInitialTicks)
					{
						profilerManager = attachment->getProfilerManager(tdbb);
						profilerInitialTicks = profilerLastTicks = profilerManager->queryTicks();
						profilerInitialAccumulatedOverhead = profilerLastAccumulatedOverhead =
							profilerManager->getAccumulatedOverhead();
					}

					if (node->hasLineColumn &&
						node->isProfileAware() &&
						(!profileNode ||
						 !(node->line == profileNode->line && node->column == profileNode->column)))
					{
						profilerLastTicks = profilerCallAfterPsqlLineColumn();
						profilerLastAccumulatedOverhead = profilerManager->getAccumulatedOverhead();
						profileNode = node;

						profilerManager->beforePsqlLineColumn(request, profileNode->line, profileNode->column);
					}
				}
			}

			node = node->execute(tdbb, request, &exeState);

			if (exeState.exit)
			{
				if (profilerInitialTicks && attachment->isProfilerActive() && !request->hasInternalStatement())
				{
					const SINT64 elapsedTicks = profilerManager->getElapsedTicksAndAdjustOverhead(
						profilerCallAfterPsqlLineColumn(), profilerInitialTicks, profilerInitialAccumulatedOverhead);

					request->req_profiler_ticks += elapsedTicks;
				}

				return node;
			}
		}	// try
		catch (const Exception& ex)
		{
			ex.stuffException(tdbb->tdbb_status_vector);

			request->adjustCallerStats();

			// Ensure the transaction hasn't disappeared in the meantime
			fb_assert(request->req_transaction);

			// Skip this handling for errors coming from the nested looper calls,
			// as they're already handled properly. The only need is to undo
			// our own savepoints.
			if (exeState.catchDisabled)
			{
				// Put cleanup off till the point where it has meaning to avoid
				// sequence 1->2->3->4 being undone as 4->3->2->1 instead of 4->1

				ERR_punt();
			}

			// If the database is already bug-checked, then get out
			if (dbb->dbb_flags & DBB_bugcheck)
				status_exception::raise(tdbb->tdbb_status_vector);

			exeState.errorPending = true;
			exeState.catchDisabled = true;
			request->req_operation = Request::req_unwind;
			request->req_label = 0;

			if (!(tdbb->tdbb_flags & TDBB_stack_trace_done) && !(tdbb->tdbb_flags & TDBB_sys_error))
			{
				stuff_stack_trace(request);
				tdbb->tdbb_flags |= TDBB_stack_trace_done;
			}
		}
	} // while()

	if (profilerInitialTicks && attachment->isProfilerActive() && !request->hasInternalStatement())
	{
		const SINT64 elapsedTicks = profilerManager->getElapsedTicksAndAdjustOverhead(
			profilerCallAfterPsqlLineColumn(), profilerInitialTicks, profilerInitialAccumulatedOverhead);

		request->req_profiler_ticks += elapsedTicks;
	}

	request->adjustCallerStats();

	fb_assert(request->req_auto_trans.getCount() == 0);

	// If there is no node, assume we have finished processing the
	// request unless we are in the middle of processing an
	// asynchronous message

	if (!node)
	{
		// Close active cursors
		for (const Cursor* const* ptr = request->req_cursors.begin();
			 ptr < request->req_cursors.end(); ++ptr)
		{
			if (*ptr)
				(*ptr)->close(tdbb);
		}

		if (!exeState.errorPending)
			TRA_release_request_snapshot(tdbb, request);

		request->req_flags &= ~(req_active | req_reserved);
		request->invalidateTimeStamp();
		release_blobs(tdbb, request);

		if (profilerInitialTicks && attachment->isProfilerActive() && !request->hasInternalStatement())
		{
			ProfilerManager::Stats stats(request->req_profiler_ticks);
			profilerManager->onRequestFinish(request, stats);
		}
	}

	request->req_next = node;

	fb_assert(request->req_caller == exeState.oldRequest);
	request->req_caller = NULL;

	// Ensure the transaction hasn't disappeared in the meantime
	fb_assert(request->req_transaction);

	// In the case of a pending error condition (one which did not
	// result in a exception to the top of looper), we need to
	// release the request snapshot

	if (exeState.errorPending)
	{
		TRA_release_request_snapshot(tdbb, request);
		ERR_punt();
	}

	if (request->req_flags & req_abort)
		ERR_post(Arg::Gds(isc_req_sync));

	return node;
}


// Start looper under Windows SEH (Structured Exception Handling) control
static void looper_seh(thread_db* tdbb, Request* request, const StmtNode* node)
{
#ifdef WIN_NT
	START_CHECK_FOR_EXCEPTIONS(NULL);
#endif
	// TODO:
	// 1. Try to fix the problem with MSVC C++ runtime library, making
	// even C++ exceptions that are implemented in terms of Win32 SEH
	// getting catched by the SEH handler below.
	// 2. Check if it really is correct that only Win32 catches CPU
	// exceptions (such as SEH) here. Shouldn't any platform capable
	// of handling signals use this stuff?
	// (see jrd/ibsetjmp.h for implementation of these macros)

	EXE_looper(tdbb, request, node);

#ifdef WIN_NT
	END_CHECK_FOR_EXCEPTIONS(NULL);
#endif
}


static void release_blobs(thread_db* tdbb, Request* request)
{
/**************************************
 *
 *	r e l e a s e _ b l o b s
 *
 **************************************
 *
 * Functional description
 *	Release temporary blobs assigned by this request.
 *
 **************************************/
	SET_TDBB(tdbb);
	DEV_BLKCHK(request, type_req);

	jrd_tra* transaction = request->req_transaction;
	if (transaction)
	{
		DEV_BLKCHK(transaction, type_tra);
		transaction = transaction->getOuter();

		// Release blobs bound to this request

		if (request->req_blobs.getFirst())
		{
			while (true)
			{
				const ULONG blob_temp_id = request->req_blobs.current();
				if (transaction->tra_blobs->locate(blob_temp_id))
				{
					BlobIndex *current = &transaction->tra_blobs->current();
					if (current->bli_materialized)
					{
						request->req_blobs.fastRemove();
						current->bli_request = NULL;
					}
					else
					{
						// Blob was created by request, is accounted for internal needs,
						// but is not materialized. Get rid of it.
						current->bli_blob_object->BLB_cancel(tdbb);
						// Since the routine above modifies req_blobs
						// we need to reestablish accessor position
					}

					if (request->req_blobs.locate(locGreat, blob_temp_id))
						continue;

					break;
				}

				// Blob accounting inconsistent, only detected in DEV_BUILD.
				fb_assert(false);

				if (!request->req_blobs.getNext())
					break;
			}
		}

		request->req_blobs.clear();

		// Release arrays assigned by this request

		for (ArrayField** array = &transaction->tra_arrays; *array;)
		{
			DEV_BLKCHK(*array, type_arr);
			if ((*array)->arr_request == request)
				blb::release_array(*array);
			else
				array = &(*array)->arr_next;
		}
	}
}


static void trigger_failure(thread_db* tdbb, Request* trigger)
{
/**************************************
 *
 *	t r i g g e r _ f a i l u r e
 *
 **************************************
 *
 * Functional description
 *	Trigger failed, report error.
 *
 **************************************/

	SET_TDBB(tdbb);

	if (trigger->req_flags & req_leave)
	{
		trigger->req_flags &= ~req_leave;
		string msg;
		MET_trigger_msg(tdbb, msg, trigger->getStatement()->triggerName, trigger->req_label);
		if (msg.hasData())
		{
			ERR_post(Arg::Gds(isc_integ_fail) << Arg::Num(trigger->req_label) <<
					 Arg::Gds(isc_random) << Arg::Str(msg));
		}
		else
		{
			ERR_post(Arg::Gds(isc_integ_fail) << Arg::Num(trigger->req_label));
		}
	}
	else
	{
		ERR_punt();
	}
}


void AutoCacheRequest::cacheRequest()
{
	thread_db* tdbb = JRD_get_thread_data();
	Attachment* att = tdbb->getAttachment();

	if (which == CACHED_REQUESTS && id >= att->att_internal_cached_statements.getCount())
		att->att_internal_cached_statements.grow(id + 1);

	Statement** stmt =
		which == IRQ_REQUESTS ? &att->att_internal[id] :
		which == DYN_REQUESTS ? &att->att_dyn_req[id] :
		which == CACHED_REQUESTS ? &att->att_internal_cached_statements[id] :
		nullptr;

	if (!stmt)
	{
		fb_assert(false);
		return;
	}

	if (*stmt)
	{
		// self resursive call already filled cache
		request->getStatement()->release(tdbb);
		request = att->findSystemRequest(tdbb, id, which);
		fb_assert(request);
	}
	else
		*stmt = request->getStatement();
}

