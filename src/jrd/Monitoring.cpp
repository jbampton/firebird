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
 *  The Original Code was created by Dmitry Yemanov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2006 Dmitry Yemanov <dimitr@users.sf.net>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "../common/classes/auto.h"
#include "../common/classes/locks.h"
#include "../common/classes/fb_string.h"
#include "../jrd/jrd.h"
#include "../jrd/cch.h"
#include "../jrd/ids.h"
#include "../jrd/ini.h"
#include "../jrd/nbak.h"
#include "../jrd/req.h"
#include "../jrd/tra.h"
#include "../jrd/blb_proto.h"
#include "../common/isc_proto.h"
#include "../common/isc_f_proto.h"
#include "../common/isc_s_proto.h"
#include "../common/db_alias.h"
#include "../jrd/lck_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/pag_proto.h"
#include "../jrd/cvt_proto.h"
#include "../jrd/CryptoManager.h"
#include "../jrd/Relation.h"
#include "../jrd/RecordBuffer.h"
#include "../jrd/Monitoring.h"
#include "../jrd/Function.h"
#include "../jrd/optimizer/Optimizer.h"
#include <numeric>

#ifdef WIN_NT
#include <process.h>
#define getpid _getpid
#endif

const char* const SCRATCH = "fb_monitor_";

using namespace Firebird;
using namespace Jrd;


namespace
{
	class DumpWriter : public SnapshotData::DumpRecord::Writer
	{
	public:
		DumpWriter(MonitoringData* data, AttNumber att_id, const char* user_name, ULONG generation)
			: dump(data), offset(dump->setup(att_id, user_name, generation))
		{
			fb_assert(offset);
		}

		void write(const SnapshotData::DumpRecord& record)
		{
			const ULONG length = record.getLength();
			dump->write(offset, sizeof(ULONG), &length);
			dump->write(offset, length, record.getData());
		}

	private:
		MonitoringData* const dump;
		const ULONG offset;
	};

	class TempWriter : public SnapshotData::DumpRecord::Writer
	{
	public:
		TempWriter(TempSpace& temp)
			: tempSpace(temp)
		{}

		void write(const SnapshotData::DumpRecord& record)
		{
			const offset_t offset = tempSpace.getSize();
			const ULONG length = record.getLength();
			tempSpace.write(offset, &length, sizeof(ULONG));
			tempSpace.write(offset + sizeof(ULONG), record.getData(), length);
		}

	private:
		TempSpace& tempSpace;
	};

	const ULONG HEADER_SIZE = (ULONG) FB_ALIGN(sizeof(MonitoringHeader), FB_ALIGNMENT);

} // namespace


const Format* MonitoringTableScan::getFormat(thread_db* tdbb, jrd_rel* relation) const
{
	MonitoringSnapshot* const snapshot = MonitoringSnapshot::create(tdbb);
	return snapshot->getData(relation)->getFormat();
}


bool MonitoringTableScan::retrieveRecord(thread_db* tdbb, jrd_rel* relation,
										 FB_UINT64 position, Record* record) const
{
	MonitoringSnapshot* const snapshot = MonitoringSnapshot::create(tdbb);
	if (!snapshot->getData(relation)->fetch(position, record))
		return false;

	if (relation->rel_id == rel_mon_attachments || relation->rel_id == rel_mon_statements)
	{
		const USHORT fieldId = (relation->rel_id == rel_mon_attachments) ?
			(USHORT) f_mon_att_idle_timer : (USHORT) f_mon_stmt_timer;

		dsc desc;
		if (EVL_field(relation, record, fieldId, &desc))
		{
			SINT64 clock;
			memcpy(&clock, desc.dsc_address, sizeof(clock));

			ISC_TIMESTAMP_TZ* ts = reinterpret_cast<ISC_TIMESTAMP_TZ*> (desc.dsc_address);
			ts->utc_timestamp = TimeZoneUtil::getCurrentGmtTimeStamp().utc_timestamp;

			if (relation->rel_id == rel_mon_attachments)
			{
				const SINT64 currClock = fb_utils::query_performance_counter() / fb_utils::query_performance_frequency();
				NoThrowTimeStamp::add10msec(&ts->utc_timestamp, clock - currClock, ISC_TIME_SECONDS_PRECISION);
				NoThrowTimeStamp::round_time(ts->utc_timestamp.timestamp_time, 0);
			}
			else
			{
				const SINT64 currClock = fb_utils::query_performance_counter() * 1000 / fb_utils::query_performance_frequency();
				NoThrowTimeStamp::add10msec(&ts->utc_timestamp, clock - currClock, ISC_TIME_SECONDS_PRECISION / 1000);
			}

			// hvlad: this will assign local system (server) time zone that was actual
			// when current attachment created.
			Attachment* att = tdbb->getAttachment();
			ts->time_zone = att->att_timestamp.time_zone;
		}
	}

	return true;
}


// MonitoringData class

MonitoringData::MonitoringData(Database* dbb)
	: PermanentStorage(*dbb->dbb_permanent),
	  m_dbId(dbb->getUniqueFileId())
{
	initSharedFile();
}


MonitoringData::~MonitoringData()
{
	Guard guard(this);

	try
	{
		if (m_sharedMemory->getHeader() &&
			m_sharedMemory->getHeader()->used == HEADER_SIZE)
		{
			m_sharedMemory->removeMapFile();
		}
	}
	catch (const Exception&)
	{} // no-op
}


void MonitoringData::initSharedFile()
{
	PathName name;
	name.printf(MONITOR_FILE, m_dbId.c_str());

	try
	{
		m_sharedMemory.reset(FB_NEW_POOL(getPool())
			SharedMemory<MonitoringHeader>(name.c_str(), DEFAULT_SIZE, this));

		const auto header = m_sharedMemory->getHeader();
		checkHeader(header);
	}
	catch (const Exception& ex)
	{
		iscLogException("MonitoringData: Cannot initialize the shared memory region", ex);
		throw;
	}
}


void MonitoringData::acquire()
{
	m_localMutex.enter(FB_FUNCTION);
	m_sharedMemory->mutexLock();

	// Reattach if someone has just deleted the shared file

	while (m_sharedMemory->getHeader()->isDeleted())
	{
		// Shared memory must be empty at this point
		fb_assert(m_sharedMemory->getHeader()->used == HEADER_SIZE);

		m_sharedMemory->mutexUnlock();
		m_sharedMemory.reset();

		Thread::yield();

		initSharedFile();
		m_sharedMemory->mutexLock();
	}

	if (m_sharedMemory->getHeader()->allocated > m_sharedMemory->sh_mem_length_mapped)
	{
#ifdef HAVE_OBJECT_MAP
		FbLocalStatus statusVector;
		if (!m_sharedMemory->remapFile(&statusVector, m_sharedMemory->getHeader()->allocated, false))
		{
			release();
			status_exception::raise(&statusVector);
		}
#else
		release();
		status_exception::raise(Arg::Gds(isc_montabexh));
#endif
	}
}


void MonitoringData::release()
{
	m_sharedMemory->mutexUnlock();
	m_localMutex.leave();
}


void MonitoringData::enumerate(const char* userName, ULONG generation, SessionList& sessions)
{
	const bool init = sessions.isEmpty();

	// When initializing, collect all sessions older than the given generation.
	// Otherwise, remove sessions that have updated their generation.

	for (ULONG offset = HEADER_SIZE; offset < m_sharedMemory->getHeader()->used;)
	{
		const auto ptr = (UCHAR*) m_sharedMemory->getHeader() + offset;
		const auto element = (Element*) ptr;
		const ULONG length = element->getBlockLength();

		if (!userName || !strcmp(element->userName, userName)) // permitted
		{
			if (init)
			{
				if (element->generation < generation)
					sessions.add(element->attId);
			}
			else if (element->generation >= generation)
				sessions.findAndRemove(element->attId);
		}

		offset += length;
	}
}


void MonitoringData::read(const char* userName, TempSpace& temp)
{
	offset_t position = temp.getSize();

	// Copy data of all permitted sessions

	for (ULONG offset = HEADER_SIZE; offset < m_sharedMemory->getHeader()->used;)
	{
		const auto ptr = (UCHAR*) m_sharedMemory->getHeader() + offset;
		const auto element = (Element*) ptr;
		const ULONG length = element->getBlockLength();

		if (!userName || !strcmp(element->userName, userName)) // permitted
		{
			temp.write(position, ptr + sizeof(Element), element->length);
			position += element->length;
		}

		offset += length;
	}
}


ULONG MonitoringData::setup(AttNumber att_id, const char* userName, ULONG generation)
{
	const FB_UINT64 offset = FB_ALIGN(m_sharedMemory->getHeader()->used, FB_ALIGNMENT);
	const ULONG delta = offset + sizeof(Element) - m_sharedMemory->getHeader()->used;

	ensureSpace(delta);

	// Prepare for writing new data at the tail

	const auto ptr = (UCHAR*) m_sharedMemory->getHeader() + offset;
	const auto element = (Element*) ptr;
	element->attId = att_id;
	snprintf(element->userName, sizeof(element->userName), "%s", userName);
	element->generation = generation;
	element->length = 0;
	m_sharedMemory->getHeader()->used += delta;
	return offset;
}


void MonitoringData::write(ULONG offset, ULONG length, const void* buffer)
{
	ensureSpace(length);

	// Write data item at the tail

	const auto ptr = (UCHAR*) m_sharedMemory->getHeader() + offset;
	const auto element = (Element*) ptr;
	memcpy(ptr + sizeof(Element) + element->length, buffer, length);
	element->length += length;
	m_sharedMemory->getHeader()->used += length;
}


void MonitoringData::cleanup(AttNumber att_id)
{
	// Remove information about the given session

	for (ULONG offset = HEADER_SIZE; offset < m_sharedMemory->getHeader()->used;)
	{
		const auto ptr = (UCHAR*) m_sharedMemory->getHeader() + offset;
		const auto element = (Element*) ptr;
		const ULONG length = element->getBlockLength();

		if (element->attId == att_id)
		{
			if (offset + length < m_sharedMemory->getHeader()->used)
			{
				memmove(ptr, ptr + length, m_sharedMemory->getHeader()->used - offset - length);
				m_sharedMemory->getHeader()->used -= length;
			}
			else
			{
				m_sharedMemory->getHeader()->used = offset;
			}

			break;
		}

		offset += length;
	}
}


void MonitoringData::ensureSpace(ULONG length)
{
	FB_UINT64 newSize = m_sharedMemory->getHeader()->used + length;

	if (newSize > m_sharedMemory->getHeader()->allocated)
	{
		if (newSize > MAX_ULONG)
		{
			(Arg::Gds(isc_montabexh) <<
				Arg::Gds(isc_random) << Arg::Str("storage size exceeds limit")).raise();
		}

		FB_UINT64 remapSize = FB_ALIGN(newSize, DEFAULT_SIZE);
		if (remapSize > MAX_ULONG)
			remapSize = newSize;

#ifdef HAVE_OBJECT_MAP
		FbLocalStatus statusVector;
		if (!m_sharedMemory->remapFile(&statusVector, (ULONG) remapSize, true))
		{
			status_exception::raise(&statusVector);
		}
		m_sharedMemory->getHeader()->allocated = m_sharedMemory->sh_mem_length_mapped;
#else
		status_exception::raise(Arg::Gds(isc_montabexh));
#endif
	}
}


void MonitoringData::mutexBug(int osErrorCode, const char* s)
{
	string msg;
	msg.printf("MONITOR: mutex %s error, status = %d", s, osErrorCode);
	fb_utils::logAndDie(msg.c_str());
}


bool MonitoringData::initialize(SharedMemoryBase* sm, bool initialize)
{
	if (initialize)
	{
		MonitoringHeader* const header = reinterpret_cast<MonitoringHeader*>(sm->sh_mem_header);

		// Initialize the shared data header
		initHeader(header);

		header->used = HEADER_SIZE;
		header->allocated = sm->sh_mem_length_mapped;
	}

	return true;
}


// MonitoringSnapshot class


MonitoringSnapshot* MonitoringSnapshot::create(thread_db* tdbb)
{
	const auto transaction = tdbb->getTransaction();
	fb_assert(transaction);

	if (!transaction->tra_mon_snapshot)
	{
		// Create a database snapshot and store it
		// in the transaction block
		MemoryPool& pool = *transaction->tra_pool;
		transaction->tra_mon_snapshot = FB_NEW_POOL(pool) MonitoringSnapshot(tdbb, pool);
	}

	return transaction->tra_mon_snapshot;
}


MonitoringSnapshot::MonitoringSnapshot(thread_db* tdbb, MemoryPool& pool)
	: SnapshotData(pool)
{
	PAG_header(tdbb, true);

	const auto dbb = tdbb->getDatabase();
	const auto attachment = tdbb->getAttachment();

	const auto selfAttId = attachment->att_attachment_id;

	// Initialize record buffers
	const auto dbb_buffer = allocBuffer(tdbb, pool, rel_mon_database);
	const auto att_buffer = allocBuffer(tdbb, pool, rel_mon_attachments);
	const auto tra_buffer = allocBuffer(tdbb, pool, rel_mon_transactions);
	const auto cmp_stmt_buffer = dbb->getEncodedOdsVersion() >= ODS_13_1 ?
		allocBuffer(tdbb, pool, rel_mon_compiled_statements) :
		nullptr;
	const auto stmt_buffer = allocBuffer(tdbb, pool, rel_mon_statements);
	const auto call_buffer = allocBuffer(tdbb, pool, rel_mon_calls);
	const auto io_stat_buffer = allocBuffer(tdbb, pool, rel_mon_io_stats);
	const auto rec_stat_buffer = allocBuffer(tdbb, pool, rel_mon_rec_stats);
	const auto ctx_var_buffer = allocBuffer(tdbb, pool, rel_mon_ctx_vars);
	const auto mem_usage_buffer = allocBuffer(tdbb, pool, rel_mon_mem_usage);
	const auto tab_stat_buffer = allocBuffer(tdbb, pool, rel_mon_tab_stats);

	// Increment the global monitor generation

	const auto generation = dbb->newMonitorGeneration();

	// Dump state of our own attachment

	Monitoring::dumpAttachment(tdbb, attachment, generation);

	// Enumerate active sessions and ensure they have dumped their state.
	// Check that by comparing the session generation with the current one.

	const auto locksmith = attachment->locksmith(tdbb, MONITOR_ANY_ATTACHMENT);
	const auto userName = attachment->getEffectiveUserName();
	const auto userNamePtr = locksmith ? nullptr : userName.c_str();

	Lock temp_lock(tdbb, sizeof(AttNumber), LCK_monitor), *lock = &temp_lock;
	MonitoringData::SessionList sessions(pool);

	do
	{
		ThreadStatusGuard tempStatus(tdbb);

		{ // scope for the guard

			MonitoringData::Guard guard(dbb->dbb_monitoring_data);
			dbb->dbb_monitoring_data->enumerate(userNamePtr, generation, sessions);
		}

		if (!sessions.hasData())
			break;

		const auto attId = sessions.pop();
		fb_assert(attId != selfAttId);
		lock->setKey(attId);

		// Try getting an exclusive lock first.
		// Success means session is dead and must be garbage collected.

		if (LCK_lock(tdbb, lock, LCK_EX, LCK_NO_WAIT))
		{
			LCK_release(tdbb, lock);

			MonitoringData::Guard guard(dbb->dbb_monitoring_data);
			dbb->dbb_monitoring_data->cleanup(attId);

			continue;
		}

		// Ping the session via AST to dump its state

		if (!LCK_lock(tdbb, lock, LCK_SR, LCK_WAIT))
		{
			fb_assert(false);
			ERR_punt();
		}

		LCK_release(tdbb, lock);

	} while (sessions.hasData());

	// Collect monitoring data. Start by gathering database-level info,
	// it goes directly to the temporary space (as it's not stored in the shared dump).

	TempSpace temp_space(pool, SCRATCH);

	{ // scope for putDatabase and its utilities

		TempWriter writer(temp_space);
		SnapshotData::DumpRecord tempRecord(pool, writer);

		Monitoring::putDatabase(tdbb, tempRecord);
	}

	// Read the dump into a temporary space

	{ // scope for the guard

		MonitoringData::Guard guard(dbb->dbb_monitoring_data);
		dbb->dbb_monitoring_data->read(userNamePtr, temp_space);
	}

	// Parse the dump

	MonitoringData::Reader reader(pool, temp_space);

	SnapshotData::DumpRecord dumpRecord(pool);
	while (reader.getRecord(dumpRecord))
	{
		const int rid = dumpRecord.getRelationId();

		RecordBuffer* buffer = nullptr;
		Record* record = nullptr;

		switch (rid)
		{
		case rel_mon_database:
			buffer = dbb_buffer;
			break;
		case rel_mon_attachments:
			buffer = att_buffer;
			break;
		case rel_mon_transactions:
			buffer = tra_buffer;
			break;
		case rel_mon_compiled_statements:
			buffer = cmp_stmt_buffer;
			break;
		case rel_mon_statements:
			buffer = stmt_buffer;
			break;
		case rel_mon_calls:
			buffer = call_buffer;
			break;
		case rel_mon_io_stats:
			buffer = io_stat_buffer;
			break;
		case rel_mon_rec_stats:
			buffer = rec_stat_buffer;
			break;
		case rel_mon_ctx_vars:
			buffer = ctx_var_buffer;
			break;
		case rel_mon_mem_usage:
			buffer = mem_usage_buffer;
			break;
		case rel_mon_tab_stats:
			buffer = tab_stat_buffer;
			break;
		default:
			fb_assert(false);
		}

		if (buffer)
		{
			record = buffer->getTempRecord();
			record->nullify();
		}

		bool store_record = false;

		SnapshotData::DumpField dumpField;
		while (dumpRecord.getField(dumpField))
		{
			if (record)
			{
				putField(tdbb, record, dumpField);
				store_record = true;
			}
		}

		if (store_record)
			buffer->store(record);
	}
}


void SnapshotData::clearSnapshot()
{
	for (FB_SIZE_T i = 0; i < m_snapshot.getCount(); i++)
		delete m_snapshot[i].data;

	m_snapshot.clear();
}


RecordBuffer* SnapshotData::getData(const jrd_rel* relation) const
{
	fb_assert(relation);

	return getData(relation->rel_id);
}


RecordBuffer* SnapshotData::getData(int id) const
{
	for (FB_SIZE_T i = 0; i < m_snapshot.getCount(); i++)
	{
		if (m_snapshot[i].rel_id == id)
			return m_snapshot[i].data;
	}

	return nullptr;
}


RecordBuffer* SnapshotData::allocBuffer(thread_db* tdbb, MemoryPool& pool, int rel_id)
{
	jrd_rel* const relation = MET_lookup_relation_id(tdbb, rel_id, false);
	fb_assert(relation);
	MET_scan_relation(tdbb, relation);
	fb_assert(relation->isVirtual());

	const Format* const format = MET_current(tdbb, relation);
	fb_assert(format);

	RecordBuffer* const buffer = FB_NEW_POOL(pool) RecordBuffer(pool, format);
	const RelationData data = {relation->rel_id, buffer};
	m_snapshot.add(data);

	return buffer;
}


void SnapshotData::putField(thread_db* tdbb, Record* record, const DumpField& field)
{
	jrd_tra* const transaction = tdbb->getTransaction();

	fb_assert(record);

	const Format* const format = record->getFormat();
	fb_assert(format);

	dsc to_desc;

	if (field.id < format->fmt_count)
		to_desc = format->fmt_desc[field.id];

	if (to_desc.isUnknown())
		return;

	to_desc.dsc_address += (IPTR) record->getData();

	if (field.type == VALUE_GLOBAL_ID)
	{
		// special case: translate 64-bit global ID into 32-bit local ID
		fb_assert(field.length == sizeof(SINT64));
		SINT64 global_id;
		memcpy(&global_id, field.data, field.length);

		SLONG local_id;
		if (!m_map.get(global_id, local_id))
		{
			local_id = ++m_counter;
			m_map.put(global_id, local_id);
		}

		dsc from_desc;
		from_desc.makeLong(0, &local_id);
		MOV_move(tdbb, &from_desc, &to_desc);
	}
	else if (field.type == VALUE_TABLE_ID_OBJECT_NAME || field.type == VALUE_TABLE_ID_SCHEMA_NAME)
	{
		// special case: translate relation ID into name
		fb_assert(field.length == sizeof(SLONG));
		SLONG rel_id;
		memcpy(&rel_id, field.data, field.length);

		const jrd_rel* const relation = MET_lookup_relation_id(tdbb, rel_id, false);
		if (!relation || relation->rel_name.object.isEmpty())
			return;

		const auto& name = field.type == VALUE_TABLE_ID_OBJECT_NAME ?
			relation->rel_name.object : relation->rel_name.schema;

		dsc from_desc;
		from_desc.makeText(name.length(), CS_METADATA, (UCHAR*) name.c_str());
		MOV_move(tdbb, &from_desc, &to_desc);
	}
	else if (field.type == VALUE_INTEGER)
	{
		fb_assert(field.length == sizeof(SINT64));
		SINT64 value;
		memcpy(&value, field.data, field.length);

		dsc from_desc;
		from_desc.makeInt64(0, &value);
		MOV_move(tdbb, &from_desc, &to_desc);
	}
	else if (field.type == VALUE_TIMESTAMP)
	{
		fb_assert(field.length == sizeof(ISC_TIMESTAMP));
		ISC_TIMESTAMP value;
		memcpy(&value, field.data, field.length);

		dsc from_desc;
		from_desc.makeTimestamp(&value);
		MOV_move(tdbb, &from_desc, &to_desc);
	}
	else if (field.type == VALUE_TIMESTAMP_TZ)
	{
		fb_assert(field.length == sizeof(ISC_TIMESTAMP_TZ));
		ISC_TIMESTAMP_TZ value;
		memcpy(&value, field.data, field.length);

		dsc from_desc;
		from_desc.makeTimestampTz(&value);
		MOV_move(tdbb, &from_desc, &to_desc);
	}
	else if (field.type == VALUE_STRING)
	{
		if (to_desc.isBlob())
		{
			const UCHAR bpb[] = {
				isc_bpb_version1,
				isc_bpb_type, 1, isc_bpb_type_stream,
				isc_bpb_storage, 1, isc_bpb_storage_temp
			};

			bid blob_id;
			blb* const blob = blb::create2(tdbb, transaction, &blob_id, sizeof(bpb), bpb);
			blob->BLB_put_data(tdbb, (UCHAR*) field.data, field.length);
			blob->BLB_close(tdbb);

			dsc from_desc;
			from_desc.makeBlob(isc_blob_text, CS_METADATA, (ISC_QUAD*) &blob_id);
			MOV_move(tdbb, &from_desc, &to_desc);
		}
		else
		{
			dsc from_desc;
			from_desc.makeText(field.length, CS_METADATA, (UCHAR*) field.data);

			TruncateCallbacks tcb(isc_truncate_monitor);
			CVT_move_common(&from_desc, &to_desc, 0, &tcb);	// no need in decimal status for string=>string move
		}
	}
	else if (field.type == VALUE_BOOLEAN)
	{
		fb_assert(field.length == sizeof(UCHAR));
		UCHAR value;
		memcpy(&value, field.data, field.length);
		dsc from_desc;
		from_desc.makeBoolean(&value);
		MOV_move(tdbb, &from_desc, &to_desc);
	}
	else
	{
		fb_assert(false);
	}

	// hvlad: detach just created temporary blob from request to bound its
	// lifetime to transaction. This is necessary as this blob belongs to
	// the MON$ table and must be accessible until transaction ends.
	if (to_desc.isBlob())
	{
		bid* blob_id = reinterpret_cast<bid*>(to_desc.dsc_address);

		if (!transaction->tra_blobs->locate(blob_id->bid_temp_id()))
			fb_assert(false);

		BlobIndex& blobIdx = transaction->tra_blobs->current();
		fb_assert(!blobIdx.bli_materialized);

		if (blobIdx.bli_request)
		{
			if (!blobIdx.bli_request->req_blobs.locate(blobIdx.bli_temp_id))
				fb_assert(false);

			blobIdx.bli_request->req_blobs.fastRemove();
			blobIdx.bli_request = nullptr;
		}
	}

	record->clearNull(field.id);
}


// Monitoring class


SINT64 Monitoring::getGlobalId(int value)
{
	return ((SINT64) getpid() << BITS_PER_LONG) + value;
}


void Monitoring::putDatabase(thread_db* tdbb, SnapshotData::DumpRecord& record)
{
	const auto dbb = tdbb->getDatabase();

	record.reset(rel_mon_database);

	// Determine the backup state
	int backup_state = backup_state_unknown;

	BackupManager* const bm = dbb->dbb_backup_manager;

	if (bm && !bm->isShutDown())
	{
		BackupManager::StateReadGuard holder(tdbb);

		switch (bm->getState())
		{
		case Ods::hdr_nbak_normal:
			backup_state = backup_state_normal;
			break;
		case Ods::hdr_nbak_stalled:
			backup_state = backup_state_stalled;
			break;
		case Ods::hdr_nbak_merge:
			backup_state = backup_state_merge;
			break;
		}
	}

	PathName databaseName(dbb->dbb_database_name);
	ISC_systemToUtf8(databaseName);

	// database name or alias (MUST BE ALWAYS THE FIRST ITEM PASSED!)
	record.storeString(f_mon_db_name, databaseName);
	// page size
	record.storeInteger(f_mon_db_page_size, dbb->dbb_page_size);
	// major ODS version
	record.storeInteger(f_mon_db_ods_major, dbb->dbb_ods_version);
	// minor ODS version
	record.storeInteger(f_mon_db_ods_minor, dbb->dbb_minor_version);
	// oldest interesting transaction
	record.storeInteger(f_mon_db_oit, dbb->dbb_oldest_transaction);
	// oldest active transaction
	record.storeInteger(f_mon_db_oat, dbb->dbb_oldest_active);
	// oldest snapshot transaction
	record.storeInteger(f_mon_db_ost, dbb->dbb_oldest_snapshot);
	// next transaction
	record.storeInteger(f_mon_db_nt, dbb->dbb_next_transaction);
	// number of page buffers
	record.storeInteger(f_mon_db_page_bufs, dbb->dbb_bcb->bcb_count);

	int temp;

	// SQL dialect
	temp = (dbb->dbb_flags & DBB_DB_SQL_dialect_3) ? 3 : 1;
	record.storeInteger(f_mon_db_dialect, temp);

	// shutdown mode
	if (dbb->dbb_ast_flags & DBB_shutdown_full)
		temp = shut_mode_full;
	else if (dbb->dbb_ast_flags & DBB_shutdown_single)
		temp = shut_mode_single;
	else if (dbb->dbb_ast_flags & DBB_shutdown)
		temp = shut_mode_multi;
	else
		temp = shut_mode_online;
	record.storeInteger(f_mon_db_shut_mode, temp);

	// sweep interval
	record.storeInteger(f_mon_db_sweep_int, dbb->dbb_sweep_interval);
	// read only flag
	temp = dbb->readOnly() ? 1 : 0;
	record.storeInteger(f_mon_db_read_only, temp);
	// forced writes flag
	temp = (dbb->dbb_flags & DBB_force_write) ? 1 : 0;
	record.storeInteger(f_mon_db_forced_writes, temp);
	// reserve space flag
	temp = (dbb->dbb_flags & DBB_no_reserve) ? 0 : 1;
	record.storeInteger(f_mon_db_res_space, temp);
	// creation date
	record.storeTimestampTz(f_mon_db_created, dbb->dbb_creation_date);
	// database size
	record.storeInteger(f_mon_db_pages, PageSpace::actAlloc(dbb));
	// database backup state
	record.storeInteger(f_mon_db_backup_state, backup_state);

	// crypt thread status
	if (dbb->dbb_crypto_manager)
	{
		record.storeInteger(f_mon_db_crypt_page, dbb->dbb_crypto_manager->getCurrentPage(tdbb));
		record.storeInteger(f_mon_db_crypt_state, dbb->dbb_crypto_manager->getCurrentState(tdbb));
	}

	// database owner
	record.storeString(f_mon_db_owner, dbb->dbb_owner);

	// security database type
	PathName secDbName;
	string secDbType = "Other";
	expandDatabaseName(dbb->dbb_config->getSecurityDatabase(), secDbName, nullptr);
	if (secDbName == dbb->dbb_filename)
		secDbType = "Self";
	else
	{
		PathName defDbName;
		expandDatabaseName(Config::getDefaultConfig()->getSecurityDatabase(), defDbName, nullptr);
		if (secDbName == defDbName)
			secDbType = "Default";
	}
	record.storeString(f_mon_db_secdb, secDbType);

	record.storeInteger(f_mon_db_na, dbb->getLatestAttachmentId());
	record.storeInteger(f_mon_db_ns, dbb->getLatestStatementId());

	record.storeString(f_mon_db_guid, dbb->dbb_guid.value().toString());
	record.storeString(f_mon_db_file_id, dbb->getUniqueFileId());

	record.storeInteger(f_mon_db_repl_mode, dbb->dbb_replica_mode);

	// statistics
	const int stat_id = fb_utils::genUniqueId();
	record.storeGlobalId(f_mon_db_stat_id, getGlobalId(stat_id));

	record.write();

	if (dbb->dbb_flags & DBB_shared)
	{
		MutexLockGuard guard(dbb->dbb_stats_mutex, FB_FUNCTION);
		putStatistics(record, dbb->dbb_stats, stat_id, stat_database);
		putMemoryUsage(record, dbb->dbb_memory_stats, stat_id, stat_database);
	}
	else
	{
		RuntimeStatistics zero_rt_stats;
		MemoryStats zero_mem_stats;
		putStatistics(record, zero_rt_stats, stat_id, stat_database);
		putMemoryUsage(record, zero_mem_stats, stat_id, stat_database);
	}
}


void Monitoring::putAttachment(SnapshotData::DumpRecord& record, const Jrd::Attachment* attachment)
{
	fb_assert(attachment);
	if (!attachment->att_user)
		return;

	const auto dbb = attachment->att_database;

	record.reset(rel_mon_attachments);

	PathName attName(attachment->att_filename);
	ISC_systemToUtf8(attName);

	// user (MUST BE ALWAYS THE FIRST ITEM PASSED!)
	record.storeString(f_mon_att_user, attachment->getUserName());
	// attachment id
	record.storeInteger(f_mon_att_id, attachment->att_attachment_id);
	// process id
	record.storeInteger(f_mon_att_server_pid, getpid());
	// state
	int temp = attachment->hasActiveRequests() ? mon_state_active : mon_state_idle;
	record.storeInteger(f_mon_att_state, temp);
	// attachment name
	record.storeString(f_mon_att_name, attName);
	// role
	record.storeString(f_mon_att_role, attachment->getSqlRole());
	// remote protocol
	record.storeString(f_mon_att_remote_proto, attachment->att_network_protocol);
	// remote address
	record.storeString(f_mon_att_remote_addr, attachment->att_remote_address);
	// remote process id
	if (attachment->att_remote_pid)
		record.storeInteger(f_mon_att_remote_pid, attachment->att_remote_pid);
	// remote process name
	record.storeString(f_mon_att_remote_process, attachment->att_remote_process);
	// remote connection flags
	if (attachment->att_remote_address.hasData())
	{
		record.storeBoolean(f_mon_att_wire_compressed,
			attachment->att_remote_flags & isc_dpb_addr_flag_conn_compressed);
		record.storeBoolean(f_mon_att_wire_encrypted,
			attachment->att_remote_flags & isc_dpb_addr_flag_conn_encrypted);
	}
	// charset
	record.storeInteger(f_mon_att_charset_id, attachment->att_charset);
	// timestamp
	record.storeTimestampTz(f_mon_att_timestamp, attachment->att_timestamp);
	// garbage collection flag
	temp = (attachment->att_flags & ATT_no_cleanup) ? 0 : 1;
	record.storeInteger(f_mon_att_gc, temp);
	// client library version
	record.storeString(f_mon_att_client_version, attachment->att_client_version);
	// remote protocol version
	record.storeString(f_mon_att_remote_version, attachment->att_remote_protocol);
	// wire encryption plugin
	record.storeString(f_mon_att_remote_crypt, attachment->att_remote_crypt);
	// remote host name
	record.storeString(f_mon_att_remote_host, attachment->att_remote_host);
	// OS user name
	record.storeString(f_mon_att_remote_os_user, attachment->att_remote_os_user);
	// authentication method
	record.storeString(f_mon_att_auth_method, attachment->att_user->usr_auth_method);
	// statistics
	const int stat_id = fb_utils::genUniqueId();
	record.storeGlobalId(f_mon_att_stat_id, getGlobalId(stat_id));
	// system flag
	temp = (attachment->att_flags & ATT_system) ? 1 : 0;
	record.storeInteger(f_mon_att_sys_flag, temp);

	// session idle timeout, seconds
	record.storeInteger(f_mon_att_idle_timeout, attachment->getIdleTimeout());
	// when idle timer expires, NULL if not running
	SINT64 clock;
	if (attachment->getIdleTimerClock(clock))
	{
		ISC_TIMESTAMP_TZ idleTimer;
		static_assert(sizeof(clock) <= sizeof(idleTimer), "timer clock value not fits into timestamp field");

		memcpy(&idleTimer, &clock, sizeof(clock));
		record.storeTimestampTz(f_mon_att_idle_timer, idleTimer);
	}
	// statement timeout, milliseconds
	record.storeInteger(f_mon_att_stmt_timeout, attachment->getStatementTimeout());

	char timeZoneBuffer[TimeZoneUtil::MAX_SIZE];
	TimeZoneUtil::format(timeZoneBuffer, sizeof(timeZoneBuffer), attachment->att_current_timezone);
	record.storeString(f_mon_att_session_tz, string(timeZoneBuffer));

	record.storeInteger(f_mon_att_par_workers, attachment->att_parallel_workers);

	if (const auto& searchPath = *attachment->att_schema_search_path; searchPath.hasData())
	{
		record.storeString(f_mon_att_search_path,
			std::accumulate(
				std::next(searchPath.begin()),
				searchPath.end(),
				searchPath.front().toQuotedString(),
				[](const auto& str, const auto& name) {
					return str + ", " + name.toQuotedString();
				}
			));
	}

	record.write();

	if (attachment->att_database->dbb_flags & DBB_shared)
	{
		putStatistics(record, attachment->att_stats, stat_id, stat_attachment);
		putMemoryUsage(record, attachment->att_memory_stats, stat_id, stat_attachment);
	}
	else
	{
		MutexLockGuard guard(attachment->att_database->dbb_stats_mutex, FB_FUNCTION);
		putStatistics(record, attachment->att_database->dbb_stats, stat_id, stat_attachment);
		putMemoryUsage(record, attachment->att_database->dbb_memory_stats, stat_id, stat_attachment);
	}

	// context vars
	putContextVars(record, attachment->att_context_vars, attachment->att_attachment_id, true);
}


void Monitoring::putTransaction(SnapshotData::DumpRecord& record, const jrd_tra* transaction)
{
	fb_assert(transaction);

	record.reset(rel_mon_transactions);

	int temp = mon_state_idle;
	for (const Request* request = transaction->tra_requests;
		request; request = request->req_tra_next)
	{
		if (request->req_transaction && (request->req_flags & req_active))
		{
			temp = mon_state_active;
			break;
		}
	}

	// transaction id
	record.storeInteger(f_mon_tra_id, transaction->tra_number);
	// attachment id
	record.storeInteger(f_mon_tra_att_id, transaction->tra_attachment->att_attachment_id);
	// state
	record.storeInteger(f_mon_tra_state, temp);
	// timestamp
	record.storeTimestampTz(f_mon_tra_timestamp, transaction->tra_timestamp);
	// top transaction
	record.storeInteger(f_mon_tra_top, transaction->tra_top);
	// oldest transaction
	record.storeInteger(f_mon_tra_oit, transaction->tra_oldest);
	// oldest active transaction
	record.storeInteger(f_mon_tra_oat, transaction->tra_oldest_active);
	// isolation mode
	if (transaction->tra_flags & TRA_degree3)
		temp = iso_mode_consistency;
	else if (transaction->tra_flags & TRA_read_committed)
	{
		temp = (transaction->tra_flags & TRA_read_consistency) ?
			iso_mode_rc_read_consistency :
			(transaction->tra_flags & TRA_rec_version) ?
				iso_mode_rc_version : iso_mode_rc_no_version;
	}
	else
		temp = iso_mode_concurrency;
	record.storeInteger(f_mon_tra_iso_mode, temp);
	// lock timeout
	record.storeInteger(f_mon_tra_lock_timeout, transaction->tra_lock_timeout);
	// read only flag
	temp = (transaction->tra_flags & TRA_readonly) ? 1 : 0;
	record.storeInteger(f_mon_tra_read_only, temp);
	// autocommit flag
	temp = (transaction->tra_flags & TRA_autocommit) ? 1 : 0;
	record.storeInteger(f_mon_tra_auto_commit, temp);
	// auto undo flag
	temp = (transaction->tra_flags & TRA_no_auto_undo) ? 0 : 1;
	record.storeInteger(f_mon_tra_auto_undo, temp);
	// statistics
	const int stat_id = fb_utils::genUniqueId();
	record.storeGlobalId(f_mon_tra_stat_id, getGlobalId(stat_id));
	// auto release temp blobid flag
	temp = (transaction->tra_flags & TRA_auto_release_temp_blobid) ? 1 : 0;
	record.storeInteger(f_mon_tra_auto_release_temp_blobid, temp);

	record.write();

	putStatistics(record, transaction->tra_stats, stat_id, stat_transaction);
	putMemoryUsage(record, transaction->tra_memory_stats, stat_id, stat_transaction);

	// context vars
	putContextVars(record, transaction->tra_context_vars, transaction->tra_number, false);
}


void Monitoring::putStatement(SnapshotData::DumpRecord& record, const Statement* statement, const string& plan)
{
	fb_assert(statement);

	record.reset(rel_mon_compiled_statements);

	// compiled statement id
	record.storeInteger(f_mon_cmp_stmt_id, statement->getStatementId());

	// sql text
	if (statement->sqlText)
		record.storeString(f_mon_cmp_stmt_sql_text, *statement->sqlText);

	// explained plan
	if (plan.hasData())
		record.storeString(f_mon_cmp_stmt_expl_plan, plan);

	// object name/type
	if (const auto routine = statement->getRoutine())
	{
		if (routine->getName().package.hasData())
			record.storeString(f_mon_cmp_stmt_pkg_name, routine->getName().package);

		if (routine->getName().schema.hasData())
			record.storeString(f_mon_cmp_sch_name, routine->getName().schema);

		record.storeString(f_mon_cmp_stmt_name, routine->getName().object);
		record.storeInteger(f_mon_cmp_stmt_type, routine->getObjectType());
	}
	else if (statement->triggerName.object.hasData())
	{
		record.storeString(f_mon_cmp_sch_name, statement->triggerName.schema);
		record.storeString(f_mon_cmp_stmt_name, statement->triggerName.object);
		record.storeInteger(f_mon_cmp_stmt_type, obj_trigger);
	}

	// statistics
	const int stat_id = fb_utils::genUniqueId();
	record.storeGlobalId(f_mon_cmp_stmt_stat_id, getGlobalId(stat_id));

	record.write();

	putMemoryUsage(record, statement->pool->getStatsGroup(), stat_id, stat_cmp_statement);
}


void Monitoring::putRequest(SnapshotData::DumpRecord& record, const Request* request,
							const string& plan)
{
	fb_assert(request);

	const auto dbb = request->req_attachment->att_database;

	record.reset(rel_mon_statements);

	// request id
	record.storeInteger(f_mon_stmt_id, request->getRequestId());
	// attachment id
	if (request->req_attachment)
		record.storeInteger(f_mon_stmt_att_id, request->req_attachment->att_attachment_id);
	// state, transaction ID, timestamp
	if (request->req_transaction && (request->req_flags & req_active))
	{
		const bool is_stalled = (request->req_flags & req_stall);
		record.storeInteger(f_mon_stmt_state, is_stalled ? mon_state_stalled : mon_state_active);
		record.storeInteger(f_mon_stmt_tra_id, request->req_transaction->tra_number);
		record.storeTimestampTz(f_mon_stmt_timestamp, request->getTimeStampTz());

		SINT64 clock;
		if (request->req_timer && request->req_timer->getExpireClock(clock))
		{
			ISC_TIMESTAMP_TZ ts;
			memcpy(&ts, &clock, sizeof(clock));
			record.storeTimestampTz(f_mon_stmt_timer, ts);
		}
	}
	else
		record.storeInteger(f_mon_stmt_state, mon_state_idle);

	const Statement* const statement = request->getStatement();

	// sql text
	if (statement->sqlText)
		record.storeString(f_mon_stmt_sql_text, *statement->sqlText);

	// explained plan
	if (plan.hasData())
		record.storeString(f_mon_stmt_expl_plan, plan);

	// statistics
	const int stat_id = fb_utils::genUniqueId();
	record.storeGlobalId(f_mon_stmt_stat_id, getGlobalId(stat_id));

	// statement timeout, milliseconds
	record.storeInteger(f_mon_stmt_timeout, request->req_timeout);

	if (dbb->getEncodedOdsVersion() >= ODS_13_1)
		record.storeInteger(f_mon_stmt_cmp_stmt_id, statement->getStatementId());

	record.write();

	putStatistics(record, request->req_stats, stat_id, stat_statement);
	putMemoryUsage(record, request->req_memory_stats, stat_id, stat_statement);
}


void Monitoring::putCall(SnapshotData::DumpRecord& record, const Request* request)
{
	fb_assert(request);

	const auto dbb = request->req_attachment->att_database;
	auto initialRequest = request->req_caller;

	while (initialRequest->req_caller)
		initialRequest = initialRequest->req_caller;

	fb_assert(initialRequest);

	record.reset(rel_mon_calls);

	// call id
	record.storeInteger(f_mon_call_id, request->getRequestId());
	// statement id
	record.storeInteger(f_mon_call_stmt_id, initialRequest->getRequestId());
	// caller id
	if (initialRequest != request->req_caller)
		record.storeInteger(f_mon_call_caller_id, request->req_caller->getRequestId());

	const auto statement = request->getStatement();
	const auto routine = statement->getRoutine();

	// object name/type
	if (routine)
	{
		if (routine->getName().package.hasData())
			record.storeString(f_mon_call_pkg_name, routine->getName().package);

		if (routine->getName().schema.hasData())
			record.storeString(f_mon_call_sch_name, routine->getName().schema);

		record.storeString(f_mon_call_name, routine->getName().object);
		record.storeInteger(f_mon_call_type, routine->getObjectType());
	}
	else if (statement->triggerName.object.hasData())
	{
		record.storeString(f_mon_call_sch_name, statement->triggerName.schema);
		record.storeString(f_mon_call_name, statement->triggerName.object);
		record.storeInteger(f_mon_call_type, obj_trigger);
	}
	else
	{
		// we should never be here...
		fb_assert(false);
	}

	// timestamp
	record.storeTimestampTz(f_mon_call_timestamp, request->getTimeStampTz());
	// source line/column
	if (request->req_src_line)
	{
		record.storeInteger(f_mon_call_src_line, request->req_src_line);
		record.storeInteger(f_mon_call_src_column, request->req_src_column);
	}

	if (dbb->getEncodedOdsVersion() >= ODS_13_1)
		record.storeInteger(f_mon_call_cmp_stmt_id, statement->getStatementId());

	// statistics
	const auto stat_id = fb_utils::genUniqueId();
	record.storeGlobalId(f_mon_call_stat_id, getGlobalId(stat_id));

	record.write();

	putStatistics(record, request->req_stats, stat_id, stat_call);
	putMemoryUsage(record, request->req_memory_stats, stat_id, stat_call);
}


void Monitoring::putStatistics(SnapshotData::DumpRecord& record, const RuntimeStatistics& statistics,
							   int stat_id, int stat_group)
{
	// statistics id
	const auto id = getGlobalId(stat_id);

	// physical I/O statistics
	record.reset(rel_mon_io_stats);
	record.storeGlobalId(f_mon_io_stat_id, id);
	record.storeInteger(f_mon_io_stat_group, stat_group);
	record.storeInteger(f_mon_io_page_reads, statistics.getValue(RuntimeStatistics::PAGE_READS));
	record.storeInteger(f_mon_io_page_writes, statistics.getValue(RuntimeStatistics::PAGE_WRITES));
	record.storeInteger(f_mon_io_page_fetches, statistics.getValue(RuntimeStatistics::PAGE_FETCHES));
	record.storeInteger(f_mon_io_page_marks, statistics.getValue(RuntimeStatistics::PAGE_MARKS));
	record.write();

	// logical I/O statistics (global)
	record.reset(rel_mon_rec_stats);
	record.storeGlobalId(f_mon_rec_stat_id, id);
	record.storeInteger(f_mon_rec_stat_group, stat_group);
	record.storeInteger(f_mon_rec_seq_reads, statistics.getValue(RuntimeStatistics::RECORD_SEQ_READS));
	record.storeInteger(f_mon_rec_idx_reads, statistics.getValue(RuntimeStatistics::RECORD_IDX_READS));
	record.storeInteger(f_mon_rec_inserts, statistics.getValue(RuntimeStatistics::RECORD_INSERTS));
	record.storeInteger(f_mon_rec_updates, statistics.getValue(RuntimeStatistics::RECORD_UPDATES));
	record.storeInteger(f_mon_rec_deletes, statistics.getValue(RuntimeStatistics::RECORD_DELETES));
	record.storeInteger(f_mon_rec_backouts, statistics.getValue(RuntimeStatistics::RECORD_BACKOUTS));
	record.storeInteger(f_mon_rec_purges, statistics.getValue(RuntimeStatistics::RECORD_PURGES));
	record.storeInteger(f_mon_rec_expunges, statistics.getValue(RuntimeStatistics::RECORD_EXPUNGES));
	record.storeInteger(f_mon_rec_locks, statistics.getValue(RuntimeStatistics::RECORD_LOCKS));
	record.storeInteger(f_mon_rec_waits, statistics.getValue(RuntimeStatistics::RECORD_WAITS));
	record.storeInteger(f_mon_rec_conflicts, statistics.getValue(RuntimeStatistics::RECORD_CONFLICTS));
	record.storeInteger(f_mon_rec_bkver_reads, statistics.getValue(RuntimeStatistics::RECORD_BACKVERSION_READS));
	record.storeInteger(f_mon_rec_frg_reads, statistics.getValue(RuntimeStatistics::RECORD_FRAGMENT_READS));
	record.storeInteger(f_mon_rec_rpt_reads, statistics.getValue(RuntimeStatistics::RECORD_RPT_READS));
	record.storeInteger(f_mon_rec_imgc, statistics.getValue(RuntimeStatistics::RECORD_IMGC));
	record.write();

	// logical I/O statistics (table wise)

	for (RuntimeStatistics::Iterator iter = statistics.begin(); iter != statistics.end(); ++iter)
	{
		const auto rec_stat_id = getGlobalId(fb_utils::genUniqueId());

		record.reset(rel_mon_tab_stats);
		record.storeGlobalId(f_mon_tab_stat_id, id);
		record.storeInteger(f_mon_tab_stat_group, stat_group);
		record.storeTableIdSchemaName(f_mon_tab_sch_name, (*iter).getRelationId());
		record.storeTableIdObjectName(f_mon_tab_name, (*iter).getRelationId());
		record.storeGlobalId(f_mon_tab_rec_stat_id, rec_stat_id);
		record.write();

		record.reset(rel_mon_rec_stats);
		record.storeGlobalId(f_mon_rec_stat_id, rec_stat_id);
		record.storeInteger(f_mon_rec_stat_group, stat_group);
		record.storeInteger(f_mon_rec_seq_reads, (*iter).getCounter(RuntimeStatistics::RECORD_SEQ_READS));
		record.storeInteger(f_mon_rec_idx_reads, (*iter).getCounter(RuntimeStatistics::RECORD_IDX_READS));
		record.storeInteger(f_mon_rec_inserts, (*iter).getCounter(RuntimeStatistics::RECORD_INSERTS));
		record.storeInteger(f_mon_rec_updates, (*iter).getCounter(RuntimeStatistics::RECORD_UPDATES));
		record.storeInteger(f_mon_rec_deletes, (*iter).getCounter(RuntimeStatistics::RECORD_DELETES));
		record.storeInteger(f_mon_rec_backouts, (*iter).getCounter(RuntimeStatistics::RECORD_BACKOUTS));
		record.storeInteger(f_mon_rec_purges, (*iter).getCounter(RuntimeStatistics::RECORD_PURGES));
		record.storeInteger(f_mon_rec_expunges, (*iter).getCounter(RuntimeStatistics::RECORD_EXPUNGES));
		record.storeInteger(f_mon_rec_locks, (*iter).getCounter(RuntimeStatistics::RECORD_LOCKS));
		record.storeInteger(f_mon_rec_waits, (*iter).getCounter(RuntimeStatistics::RECORD_WAITS));
		record.storeInteger(f_mon_rec_conflicts, (*iter).getCounter(RuntimeStatistics::RECORD_CONFLICTS));
		record.storeInteger(f_mon_rec_bkver_reads, (*iter).getCounter(RuntimeStatistics::RECORD_BACKVERSION_READS));
		record.storeInteger(f_mon_rec_frg_reads, (*iter).getCounter(RuntimeStatistics::RECORD_FRAGMENT_READS));
		record.storeInteger(f_mon_rec_rpt_reads, (*iter).getCounter(RuntimeStatistics::RECORD_RPT_READS));
		record.storeInteger(f_mon_rec_imgc, (*iter).getCounter(RuntimeStatistics::RECORD_IMGC));
		record.write();
	}
}


void Monitoring::putContextVars(SnapshotData::DumpRecord& record, const StringMap& variables,
								SINT64 object_id, bool is_attachment)
{
	StringMap::ConstAccessor accessor(&variables);

	for (bool found = accessor.getFirst(); found; found = accessor.getNext())
	{
		record.reset(rel_mon_ctx_vars);

		const int field_id = is_attachment ? f_mon_ctx_var_att_id : f_mon_ctx_var_tra_id;
		record.storeInteger(field_id, object_id);

		record.storeString(f_mon_ctx_var_name, accessor.current()->first);
		record.storeString(f_mon_ctx_var_value, accessor.current()->second);

		record.write();
	}
}


void Monitoring::putMemoryUsage(SnapshotData::DumpRecord& record, const MemoryStats& stats,
								int stat_id, int stat_group)
{
	// statistics id
	const auto id = getGlobalId(stat_id);

	// memory usage
	record.reset(rel_mon_mem_usage);
	record.storeGlobalId(f_mon_mem_stat_id, id);
	record.storeInteger(f_mon_mem_stat_group, stat_group);
	record.storeInteger(f_mon_mem_cur_used, stats.getCurrentUsage());
	record.storeInteger(f_mon_mem_cur_alloc, stats.getCurrentMapping());
	record.storeInteger(f_mon_mem_max_used, stats.getMaximumUsage());
	record.storeInteger(f_mon_mem_max_alloc, stats.getMaximumMapping());

	record.write();
}


void Monitoring::checkState(thread_db* tdbb)
{
	const auto dbb = tdbb->getDatabase();
	const auto attachment = tdbb->getAttachment();

	if (!(attachment && (attachment->att_flags & ATT_monitor_init)))
		return;

	if (const auto generation = checkGeneration(dbb, attachment))
	{
		// Dump attachment state
		dumpAttachment(tdbb, attachment, generation);
	}

	if (attachment->att_flags & ATT_monitor_disabled)
	{
		// Enable signal handler for the monitoring stuff
		attachment->att_flags &= ~ATT_monitor_disabled;
		LCK_convert(tdbb, attachment->att_monitor_lock, LCK_EX, LCK_WAIT);
	}
}


void Monitoring::dumpAttachment(thread_db* tdbb, Attachment* attachment, ULONG generation)
{
	if (!attachment->att_user)
		return;

	const auto dbb = tdbb->getDatabase();
	auto& pool = *dbb->dbb_permanent;

	attachment->mergeStats();

	const auto attId = attachment->att_attachment_id;
	const auto userName = attachment->getUserName();

	fb_assert(dbb->dbb_monitoring_data);

	attachment->att_monitor_generation = generation;

	MonitoringData::Guard guard(dbb->dbb_monitoring_data);
	dbb->dbb_monitoring_data->cleanup(attId);

	DumpWriter writer(dbb->dbb_monitoring_data, attId, userName.c_str(), generation);
	SnapshotData::DumpRecord record(pool, writer);

	putAttachment(record, attachment);

	jrd_tra* transaction = nullptr;

	// Transaction information

	for (transaction = attachment->att_transactions; transaction;
		 transaction = transaction->tra_next)
	{
		putTransaction(record, transaction);
	}

	// Call stack information

	for (transaction = attachment->att_transactions; transaction;
		 transaction = transaction->tra_next)
	{
		for (Request* request = transaction->tra_requests;
			request && (request->req_flags & req_active) && (request->req_transaction == transaction);
			request = request->req_caller)
		{
			request->adjustCallerStats();

			if (!(request->getStatement()->flags &
					(Statement::FLAG_INTERNAL | Statement::FLAG_SYS_TRIGGER)) &&
				request->req_caller)
			{
				putCall(record, request);
			}
		}
	}

	if (dbb->getEncodedOdsVersion() >= ODS_13_1)
	{
		// Statement information

		for (const auto statement : attachment->att_statements)
		{
			if (!(statement->flags & (Statement::FLAG_INTERNAL | Statement::FLAG_SYS_TRIGGER)))
			{
				const string plan = Optimizer::getPlan(tdbb, statement, true);
				putStatement(record, statement, plan);
			}
		}
	}

	// Request information

	for (const auto request : attachment->att_requests)
	{
		const auto statement = request->getStatement();

		if (!(statement->flags & (Statement::FLAG_INTERNAL | Statement::FLAG_SYS_TRIGGER)))
		{
			const string plan = Optimizer::getPlan(tdbb, statement, true);
			putRequest(record, request, plan);
		}
	}
}


void Monitoring::publishAttachment(thread_db* tdbb)
{
	const auto dbb = tdbb->getDatabase();
	const auto attachment = tdbb->getAttachment();

	const auto userName = attachment->getUserName().c_str();

	fb_assert(dbb->dbb_monitoring_data);

	const auto generation = attachment->att_monitor_generation =
		dbb->getMonitorGeneration();

	MonitoringData::Guard guard(dbb->dbb_monitoring_data);
	dbb->dbb_monitoring_data->setup(attachment->att_attachment_id, userName, generation);

	attachment->att_flags |= ATT_monitor_init;
}


void Monitoring::cleanupAttachment(thread_db* tdbb)
{
	const auto dbb = tdbb->getDatabase();
	const auto attachment = tdbb->getAttachment();

	if (attachment->att_flags & ATT_monitor_init)
	{
		attachment->att_flags &= ~ATT_monitor_init;

		if (dbb->dbb_monitoring_data)
		{
			MonitoringData::Guard guard(dbb->dbb_monitoring_data);
			dbb->dbb_monitoring_data->cleanup(attachment->att_attachment_id);
		}
	}
}
