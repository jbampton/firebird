/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		req.h
 *	DESCRIPTION:	Request block definitions
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
 * 2002.09.28 Dmitry Yemanov: Reworked internal_info stuff, enhanced
 *                            exception handling in SPs/triggers,
 *                            implemented ROWS_AFFECTED system variable
 */

#ifndef JRD_REQ_H
#define JRD_REQ_H

#include "../jrd/exe.h"
#include "../jrd/sort.h"
#include "../jrd/Attachment.h"
#include "../jrd/Record.h"
#include "../jrd/RecordNumber.h"
#include "../common/classes/timestamp.h"
#include "../common/TimeZoneUtil.h"

namespace EDS {
class Statement;
}

namespace Jrd {

class Lock;
class jrd_rel;
class jrd_prc;
class ValueListNode;
class jrd_tra;
class Savepoint;
class Cursor;
class thread_db;


// record parameter block

struct RecordParameterBase
{
	RecordParameterBase()
		: rpb_transaction_nr(0), rpb_record(NULL), rpb_prior(NULL),
		  rpb_undo(NULL), rpb_format_number(0),
		  rpb_page(0), rpb_line(0),
		  rpb_f_page(0), rpb_f_line(0),
		  rpb_b_page(0), rpb_b_line(0),
		  rpb_address(NULL), rpb_length(0),
		  rpb_flags(0), rpb_stream_flags(0), rpb_runtime_flags(0),
		  rpb_org_scans(0), rpb_window(DB_PAGE_SPACE, -1)
	{
	}

	RecordNumber rpb_number;		// record number in relation
	TraNumber	rpb_transaction_nr;	// transaction number
	Record*		rpb_record;			// final record block
	Record*		rpb_prior;			// prior record block if this is a delta record
	Record*		rpb_undo;			// our first version of data if this is a second modification
	USHORT		rpb_format_number;	// format number in relation

	ULONG rpb_page;					// page number
	USHORT rpb_line;				// line number on page

	ULONG rpb_f_page;				// fragment page number
	USHORT rpb_f_line;				// fragment line number on page

	ULONG rpb_b_page;				// back page
	USHORT rpb_b_line;				// back line

	UCHAR* rpb_address;				// address of record sans header
	ULONG rpb_length;				// length of record
	USHORT rpb_flags;				// record ODS flags replica
	USHORT rpb_stream_flags;		// stream flags
	USHORT rpb_runtime_flags;		// runtime flags
	SSHORT rpb_org_scans;			// relation scan count at stream open

	RecordParameterBase& operator=(const RecordParameterBase&) = default;
	void assign(const RecordParameterBase& from)
	{
		*this = from;
	}

protected:
	struct win rpb_window;
};

struct RecordParameter : public RecordParameterBase
{
	RecordParameter()
		: RecordParameterBase(), rpb_relation()
	{ }

	WIN& getWindow(thread_db* tdbb);	// in Statement.cpp

	Rsc::Rel	rpb_relation;		// relation of record
};

struct record_param : public RecordParameterBase
{
	record_param()
		: RecordParameterBase(), rpb_relation(nullptr)
	{ }

	inline WIN& getWindow(thread_db* tdbb)
	{
		if (rpb_relation) {
			rpb_window.win_page.setPageSpaceID(getPermanent(rpb_relation)->getPages(tdbb)->rel_pg_space_id);
		}

		return rpb_window;
	}

	jrd_rel*	rpb_relation;		// relation of record

	// rpb_relation is not assigned here!!!
	record_param& operator=(const RecordParameter& from)
	{
		assign(from);
		return *this;
	}
};

// Record flags must be an exact replica of ODS record header flags

const USHORT rpb_deleted		= 1;
const USHORT rpb_chained		= 2;
const USHORT rpb_fragment		= 4;
const USHORT rpb_incomplete		= 8;
const USHORT rpb_blob			= 16;
const USHORT rpb_delta			= 32;		// prior version is a differences record
const USHORT rpb_large			= 64;		// object is large
const USHORT rpb_damaged		= 128;		// record is busted
const USHORT rpb_gc_active		= 256;		// garbage collecting dead record version
const USHORT rpb_uk_modified	= 512;		// record key field values are changed
const USHORT rpb_long_tranum	= 1024;		// transaction number is 64-bit
const USHORT rpb_not_packed		= 2048;		// record (or delta) is stored "as is"

// Stream flags

const USHORT RPB_s_update	= 0x01;	// input stream fetched for update
const USHORT RPB_s_no_data	= 0x02;	// nobody is going to access the data
const USHORT RPB_s_sweeper	= 0x04;	// garbage collector - skip swept pages
const USHORT RPB_s_unstable = 0x08;	// don't use undo log, used with unstable explicit cursors
const USHORT RPB_s_bulk		= 0x10;	// bulk operation (currently insert only)
const USHORT RPB_s_skipLocked = 0x20;	// skip locked record

// Runtime flags

const USHORT RPB_refetch		= 0x01;	// re-fetch is required
const USHORT RPB_undo_data		= 0x02;	// data got from undo log
const USHORT RPB_undo_read		= 0x04;	// read was performed using the undo log
const USHORT RPB_undo_deleted	= 0x08;	// read was performed using the undo log, primary version is deleted
const USHORT RPB_just_deleted	= 0x10;	// record was just deleted by us

const USHORT RPB_UNDO_FLAGS		= (RPB_undo_data | RPB_undo_read | RPB_undo_deleted);
const USHORT RPB_CLEAR_FLAGS	= (RPB_UNDO_FLAGS | RPB_just_deleted);

// List of active blobs controlled by request

typedef Firebird::BePlusTree<ULONG, ULONG, MemoryPool> TempBlobIdTree;

// Affected rows counter class

class AffectedRows
{
public:
	AffectedRows();

	void clear();
	void bumpFetched();
	void bumpModified(bool);

	int getCount() const;

private:
	bool writeFlag;
	int fetchedRows;
	int modifiedRows;
};


// request block

class Request : public pool_alloc<type_req>
{
private:
	class TimeStampCache
	{
	public:
		void invalidate()
		{
			gmtTimeStamp.invalidate();
			localTimeStampValid = localTimeValid = false;
		}

		ISC_TIMESTAMP getLocalTimeStamp(USHORT currentTimeZone) const
		{
			fb_assert(!gmtTimeStamp.isEmpty());

			if (!localTimeStampValid || timeZone != currentTimeZone)
				update(currentTimeZone, true);

			return localTimeStamp;
		}

		ISC_TIMESTAMP getGmtTimeStamp() const
		{
			fb_assert(!gmtTimeStamp.isEmpty());
			return gmtTimeStamp.value();
		}

		void setGmtTimeStamp(USHORT currentTimeZone, ISC_TIMESTAMP ts)
		{
			gmtTimeStamp = ts;
			update(currentTimeZone, false);
		}

		ISC_TIMESTAMP_TZ getTimeStampTz(USHORT currentTimeZone) const
		{
			fb_assert(!gmtTimeStamp.isEmpty());

			ISC_TIMESTAMP_TZ timeStampTz;
			timeStampTz.utc_timestamp = gmtTimeStamp.value();
			timeStampTz.time_zone = currentTimeZone;
			return timeStampTz;
		}

		ISC_TIME_TZ getTimeTz(USHORT currentTimeZone) const
		{
			fb_assert(!gmtTimeStamp.isEmpty());

			ISC_TIME_TZ timeTz;

			if (timeZone != currentTimeZone)
				update(currentTimeZone, false);

			if (localTimeValid)
			{
				timeTz.utc_time = localTime;
				timeTz.time_zone = timeZone;
			}
			else
			{
				ISC_TIMESTAMP_TZ timeStamp;
				timeStamp.utc_timestamp = gmtTimeStamp.value();
				timeStamp.time_zone = timeZone;

				timeTz = Firebird::TimeZoneUtil::timeStampTzToTimeTz(timeStamp);

				localTime = timeTz.utc_time;
				localTimeValid = true;
			}

			return timeTz;
		}

		void validate(USHORT currentTimeZone)
		{
			if (gmtTimeStamp.isEmpty())
			{
				Firebird::TimeZoneUtil::validateGmtTimeStamp(gmtTimeStamp);
				update(currentTimeZone, false);
			}
		}

	private:
		void update(USHORT currentTimeZone, bool updateLocalTimeStamp) const
		{
			if (updateLocalTimeStamp)
			{
				localTimeStamp = Firebird::TimeZoneUtil::timeStampTzToTimeStamp(
					getTimeStampTz(currentTimeZone), currentTimeZone);
			}

			localTimeStampValid = updateLocalTimeStamp;
			timeZone = currentTimeZone;
			localTimeValid = false;
		}

	private:
		Firebird::TimeStamp gmtTimeStamp;		// Start time of request in GMT time zone

		mutable bool localTimeStampValid;		// localTimeStamp calculation is expensive. So is it valid (calculated)?
		mutable bool localTimeValid;			// localTime calculation is expensive. So is it valid (calculated)?
		// These are valid only when !gmtTimeStamp.isEmpty(), so no initialization is necessary.
		mutable ISC_TIMESTAMP localTimeStamp;	// Timestamp in timeZone's zone
		mutable ISC_USHORT timeZone;			// Timezone borrowed from the attachment when updated
		mutable ISC_TIME localTime;				// gmtTimeStamp converted to local time (WITH TZ)
	};

	// Fields to support read consistency in READ COMMITTED transactions

	struct SnapshotData
	{
		Request*		m_owner;
		SnapshotHandle	m_handle;
		CommitNumber	m_number;

		void init()
		{
			m_owner = nullptr;
			m_handle = 0;
			m_number = 0;
		}
	};

	// Context data saved/restored with every new autonomous transaction

	struct AutoTranCtx
	{
		AutoTranCtx()
		{
			m_snapshot.init();
		};

		AutoTranCtx(const Request* request) :
			m_transaction(request->req_transaction),
			m_savepoints(request->req_savepoints),
			m_proc_savepoints(request->req_proc_sav_point),
			m_snapshot(request->req_snapshot)
		{}

		jrd_tra*		m_transaction = nullptr;
		Savepoint*		m_savepoints = nullptr;
		Savepoint*		m_proc_savepoints = nullptr;
		SnapshotData	m_snapshot;
	};

public:
	Request(Firebird::AutoMemoryPool& pool, Database* dbb, /*const*/ Statement* aStatement);

	Statement* getStatement()
	{
		return statement;
	}

	const Statement* getStatement() const
	{
		return statement;
	}

	bool hasInternalStatement() const;

	bool hasPowerfulStatement() const;

	void setAttachment(Attachment* newAttachment)
	{
		req_attachment = newAttachment;
	}

	bool isRoot() const;

	bool isRequestIdUnassigned() const
	{
		return req_id == 0;
	}

	StmtNumber getRequestId() const;

	void setRequestId(StmtNumber id)
	{
		req_id = id;
	}

	bool setUsed();
	void setUnused();
	bool isUsed() const;

private:
	Statement* const statement;
	mutable StmtNumber	req_id;			// request identifier
	TimeStampCache req_timeStampCache;	// time stamp cache
	std::atomic<bool> req_inUse;

public:
	MemoryPool* req_pool;
	Firebird::MemoryStats req_memory_stats;
	Attachment*	req_attachment;			// database attachment
	USHORT		req_incarnation;		// incarnation number

	// Transaction pointer and doubly linked list pointers for requests in this
	// transaction. Maintained by TRA_attach_request/TRA_detach_request.
	jrd_tra*	req_transaction;
	Request*	req_tra_next;
	Request*	req_tra_prev;

	Request*	req_caller;				// Caller of this request
										// This field may be used to reconstruct the whole call stack
	TempBlobIdTree req_blobs;			// Temporary BLOBs owned by this request
	const StmtNode*	req_message;		// Current message for send/receive

	ULONG		req_records_selected;	// count of records selected by request (meeting selection criteria)
	ULONG		req_records_inserted;	// count of records inserted by request
	ULONG		req_records_updated;	// count of records updated by request
	ULONG		req_records_deleted;	// count of records deleted by request
	RuntimeStatistics	req_stats;
	RuntimeStatistics	req_base_stats;
	AffectedRows req_records_affected;	// records affected by the last statement
	FB_UINT64 req_profiler_ticks;		// profiler ticks

	const StmtNode*	req_next;			// next node for execution
	EDS::Statement*	req_ext_stmt;		// head of list of active dynamic statements
	Firebird::Array<const Cursor*>	req_cursors;	// named cursors
	ExtEngineManager::ResultSet*	req_ext_resultset;	// external result set
	USHORT		req_label;				// label for leave
	ULONG		req_flags;				// misc request flags
	Savepoint*	req_savepoints;			// Looper savepoint list
	Savepoint*	req_proc_sav_point;		// procedure savepoint list
	unsigned int req_timeout;					// query timeout in milliseconds, set by the DsqlRequest::setupTimer
	Firebird::RefPtr<TimeoutTimer> req_timer;	// timeout timer, shared with DsqlRequest

	Firebird::AutoPtr<Jrd::RuntimeStatistics> req_fetch_baseline; // State of request performance counters when we reported it last time
	SINT64 req_fetch_elapsed;	// Number of clock ticks spent while fetching rows for this request since we reported it last time
	SINT64 req_fetch_rowcount;	// Total number of rows returned by this request
	Request* req_proc_caller;	// Procedure's caller request
	const ValueListNode* req_proc_inputs;	// and its node with input parameters
	TraNumber req_conflict_txn;	// Transaction number for update conflict in read consistency mode

	ULONG req_src_line;
	ULONG req_src_column;

	dsc*			req_domain_validation;	// Current VALUE for constraint validation
	Firebird::Stack<AutoTranCtx> req_auto_trans;	// Autonomous transactions
	SortOwner req_sorts;
	Firebird::Array<record_param> req_rpb;	// record parameter blocks
	Firebird::Array<UCHAR> impureArea;		// impure area
	TriggerAction req_trigger_action;		// action that caused trigger to fire
	SnapshotData req_snapshot;
	StatusXcp req_last_xcp;			// last known exception
	bool req_batch_mode;

private:
	Firebird::RefPtr<VersionedObjects> req_resources;

public:
	const Firebird::RefPtr<VersionedObjects>& getResources()
	{
		return req_resources;
	}

	void setResources(Firebird::RefPtr<VersionedObjects>& r)
	{
		req_resources = r;
	}

	enum req_s {
		req_evaluate,
		req_return,
		req_receive,
		req_send,
		req_proceed,
		req_sync,
		req_unwind
	} req_operation;				// operation for next node


	template <typename T> T* getImpure(unsigned offset)
	{
		return reinterpret_cast<T*>(&impureArea[offset]);
	}

	void adjustCallerStats()
	{
		if (req_caller) {
			req_caller->req_stats.adjust(req_base_stats, req_stats);
		}
		req_base_stats.assign(req_stats);
	}

	// Save context when switching to the autonomous transaction
	void pushTransaction()
	{
		fb_assert(req_transaction); // must be attached

		req_auto_trans.push(this);
		req_savepoints = nullptr;
		req_proc_sav_point = nullptr;
		req_snapshot.init();
	}

	// Restore context
	jrd_tra* popTransaction()
	{
		fb_assert(!req_transaction); // must be detached

		const auto tmp = req_auto_trans.pop();
		req_savepoints = tmp.m_savepoints;
		req_proc_sav_point = tmp.m_proc_savepoints;
		req_snapshot = tmp.m_snapshot;

		return tmp.m_transaction;
	}

	void invalidateTimeStamp()
	{
		req_timeStampCache.invalidate();
	}

	ISC_TIMESTAMP getLocalTimeStamp() const
	{
		return req_timeStampCache.getLocalTimeStamp(req_attachment->att_current_timezone);
	}

	ISC_TIMESTAMP getGmtTimeStamp() const
	{
		return req_timeStampCache.getGmtTimeStamp();
	}

	void setGmtTimeStamp(ISC_TIMESTAMP ts)
	{
		req_timeStampCache.setGmtTimeStamp(req_attachment->att_current_timezone, ts);
	}

	ISC_TIMESTAMP_TZ getTimeStampTz() const
	{
		return req_timeStampCache.getTimeStampTz(req_attachment->att_current_timezone);
	}

	ISC_TIME_TZ getTimeTz() const
	{
		return req_timeStampCache.getTimeTz(req_attachment->att_current_timezone);
	}

	void validateTimeStamp()
	{
		req_timeStampCache.validate(req_attachment->att_current_timezone);
	}
};

// Flags for req_flags
const ULONG req_active			= 0x1L;
const ULONG req_stall			= 0x2L;
const ULONG req_leave			= 0x4L;
const ULONG req_null			= 0x8L;
const ULONG req_abort			= 0x10L;
const ULONG req_error_handler	= 0x20L;		// looper is called to handle error
const ULONG req_warning			= 0x40L;
//const ULONG req_in_use			= 0x80L;
const ULONG req_continue_loop	= 0x100L;		// PSQL continue statement
const ULONG req_proc_fetch		= 0x200L;		// Fetch from procedure in progress
const ULONG req_proc_select		= 0x400L;		// Select from procedure in progress
const ULONG req_same_tx_upd		= 0x800L;		// record was updated by same transaction
const ULONG req_reserved		= 0x1000L;		// Request reserved for client
const ULONG req_update_conflict	= 0x2000L;		// We need to restart request due to update conflict
const ULONG req_restart_ready	= 0x4000L;		// Request is ready to restart in case of update conflict

} //namespace Jrd

namespace Firebird
{
template <>
inline void SimpleDelete<Jrd::Request>::clear(Jrd::Request* req)
{
	req->setUnused();
}

}

#endif // JRD_REQ_H
