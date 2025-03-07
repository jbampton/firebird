/*
 *	PROGRAM:	Engine Code
 *	MODULE:		CacheVector.cpp
 *	DESCRIPTION:	Vector used in shared metadata cache.
 *
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
 *  The Original Code was created by Alexander Peshkov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2021 Alexander Peshkov <peshkoff@mail.ru>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 *
 *
 */

#include "firebird.h"

#include "../jrd/CacheVector.h"
#include "../jrd/jrd.h"
#include "../jrd/Database.h"
#include "../jrd/tra.h"
#include "../jrd/met.h"
#include "../jrd/tpc_proto.h"

using namespace Jrd;
using namespace Firebird;

// class TransactionNumber

TraNumber TransactionNumber::current(thread_db* tdbb)
{
	jrd_tra* tra = tdbb->getTransaction();
	return tra ? tra->tra_number : 0;
}

TraNumber TransactionNumber::oldestActive(thread_db* tdbb)
{
	return tdbb->getDatabase()->dbb_oldest_active;
}

TraNumber TransactionNumber::next(thread_db* tdbb)
{
	return tdbb->getDatabase()->dbb_next_transaction + 1;
}

bool TransactionNumber::isNotActive(thread_db* tdbb, TraNumber traNumber)
{
	auto state = TPC_cache_state(tdbb, traNumber);
	return (state == tra_committed) || (state == tra_dead);
}

ULONG* TransactionNumber::getFlags(thread_db* tdbb)
{
	jrd_tra* tra = tdbb->getTransaction();

	// try to recover missing transaction - sooner of all scan() will use system transaction
	static ULONG pseudoFlag = 0u;
	return tra ? &tra->tra_flags : &pseudoFlag;
}


// class VersionSupport

MdcVersion VersionSupport::next(thread_db* tdbb)
{
	return tdbb->getDatabase()->dbb_mdc->nextVersion();
}


// class ObjectBase

void ObjectBase::lockedExcl [[noreturn]] (thread_db* tdbb)
{
	fatal_exception::raise("Unspecified object locked exclusive for deletion");
}


// class CachePool

MemoryPool& CachePool::get(thread_db* tdbb)
{
	Database* dbb = tdbb->getDatabase();
	return dbb->dbb_mdc->getPool();
}


// class ElementBase

[[noreturn]] void ElementBase::busyError(thread_db* tdbb, MetaId id, const char* name, const char* family)
{
	fatal_exception::raiseFmt("%s %s%sid=%d busy in another thread - operation failed\n",
		family, name ? name : "", name ? " " : "", id);
}

void ElementBase::commitErase(thread_db* tdbb)
{
	auto* mdc = tdbb->getDatabase()->dbb_mdc;
	mdc->objectCleanup(TransactionNumber::current(tdbb), this);
}

ElementBase::~ElementBase()
{ }

