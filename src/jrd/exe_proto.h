/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		exe_proto.h
 *	DESCRIPTION:	Prototype header file for exe.cpp
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
 */

#ifndef JRD_EXE_PROTO_H
#define JRD_EXE_PROTO_H

#include "../jrd/cmp_proto.h"
#include <atomic>

namespace Jrd {
	class Request;
	class jrd_tra;
	class AssignmentNode;

	enum InternalRequest : USHORT;
}

void EXE_assignment(Jrd::thread_db*, const Jrd::AssignmentNode*);
void EXE_assignment(Jrd::thread_db*, const Jrd::ValueExprNode*, const Jrd::ValueExprNode*);
void EXE_assignment(Jrd::thread_db* tdbb, const Jrd::ValueExprNode* to, dsc* from_desc, bool from_null,
	const Jrd::ValueExprNode* missing_node, const Jrd::ValueExprNode* missing2_node);

void EXE_execute_db_triggers(Jrd::thread_db*, Jrd::jrd_tra*, enum TriggerAction);
void EXE_execute_ddl_triggers(Jrd::thread_db* tdbb, Jrd::jrd_tra* transaction,
	bool preTriggers, int action);
void EXE_execute_triggers(Jrd::thread_db*, const Jrd::Triggers&, Jrd::record_param*, Jrd::record_param*,
	enum TriggerAction, Jrd::StmtNode::WhichTrigger, int = 0);
void EXE_execute_function(Jrd::thread_db* tdbb, Jrd::Request* request, Jrd::jrd_tra* transaction,
	ULONG inMsgLength, UCHAR* inMsg, ULONG outMsgLength, UCHAR* outMsg);
bool EXE_get_stack_trace(const Jrd::Request* request, Firebird::string& sTrace);

const Jrd::StmtNode* EXE_looper(Jrd::thread_db* tdbb, Jrd::Request* request,
	const Jrd::StmtNode* in_node);
void EXE_receive(Jrd::thread_db*, Jrd::Request*, USHORT, ULONG, void*, bool = false);
void EXE_release(Jrd::thread_db*, Jrd::Request*);
void EXE_send(Jrd::thread_db*, Jrd::Request*, USHORT, ULONG, const void*);
void EXE_start(Jrd::thread_db*, Jrd::Request*, Jrd::jrd_tra*);
void EXE_unwind(Jrd::thread_db*, Jrd::Request*);

namespace Jrd
{
	class CachedRequestId
	{
	public:
		CachedRequestId()
			: id(generator++)
		{
			fb_assert(id <= MAX_USHORT);
		}

		CachedRequestId(const CachedRequestId&) = delete;
		CachedRequestId& operator=(const CachedRequestId&) = delete;

	public:
		USHORT getId() const
		{
			return id;
		}

	private:
		unsigned id;
		static inline std::atomic_uint generator;
	};

	// ASF: To make this class MT-safe in SS for v3, it should be AutoCacheRequest::release job to
	// inform CMP that the request is available for subsequent usage.
	class AutoCacheRequest
	{
	public:
		AutoCacheRequest(thread_db* tdbb, USHORT aId, InternalRequest aWhich)
			: id(aId),
			  which(aWhich),
			  request(tdbb->getDatabase()->findSystemRequest(tdbb, id, which))
		{
		}

		AutoCacheRequest(thread_db* tdbb, const CachedRequestId& cachedRequestId)
			: id(cachedRequestId.getId()),
			  which(CACHED_REQUESTS),
			  request(tdbb->getDatabase()->findSystemRequest(tdbb, id, which))
		{
		}

		AutoCacheRequest()
			: id(0),
			  which(NOT_REQUEST),
			  request(NULL)
		{
		}

		~AutoCacheRequest()
		{
			release();
		}

	public:
		void reset(thread_db* tdbb, USHORT aId, InternalRequest aWhich)
		{
			release();

			id = aId;
			which = aWhich;
			request = tdbb->getDatabase()->findSystemRequest(tdbb, id, which);
		}

		void reset(thread_db* tdbb, const CachedRequestId& cachedRequestId)
		{
			release();

			id = cachedRequestId.getId();
			which = CACHED_REQUESTS;
			request = tdbb->getDatabase()->findSystemRequest(tdbb, id, which);
		}

		void compile(thread_db* tdbb, const UCHAR* blr, ULONG blrLength)
		{
			if (request)
				return;

			cacheRequest(CMP_compile_request(tdbb, blr, blrLength, true));
		}

		Request* operator ->()
		{
			return request;
		}

		operator Request*()
		{
			return request;
		}

		bool operator !() const
		{
			return !request;
		}

	private:
		void release();
		void cacheRequest(Request* req);

	private:
		USHORT id;
		InternalRequest which;
		Request* request;
	};

	class AutoRequest
	{
	public:
		AutoRequest()
			: request(NULL)
		{
		}

		~AutoRequest()
		{
			release();
		}

	public:
		void reset()
		{
			release();
		}

		void compile(thread_db* tdbb, const UCHAR* blr, ULONG blrLength)
		{
			if (request)
				return;

			request = CMP_compile_request(tdbb, blr, blrLength, true);
			request->setUsed();
		}

		Request* operator ->()
		{
			return request;
		}

		operator Request*()
		{
			return request;
		}

		bool operator !() const
		{
			return !request;
		}

	private:
		void release();

	private:
		Request* request;
	};
}

#endif // JRD_EXE_PROTO_H
