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
 *  The Original Code was created by Adriano dos Santos Fernandes
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2008 Adriano dos Santos Fernandes <adrianosf@uol.com.br>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "firebird/impl/consts_pub.h"
#include "iberror.h"
#include "firebird/impl/inf_pub.h"
#include "../jrd/ExtEngineManager.h"
#include "firebird/impl/sqlda_pub.h"
#include "../common/dsc.h"
#include "../jrd/align.h"
#include "../jrd/jrd.h"
#include "../jrd/exe.h"
#include "../jrd/req.h"
#include "../jrd/status.h"
#include "../jrd/tra.h"
#include "../dsql/BoolNodes.h"
#include "../dsql/StmtNodes.h"
#include "../common/os/path_utils.h"
#include "../jrd/cmp_proto.h"
#include "../jrd/cvt_proto.h"
#include "../jrd/evl_proto.h"
#include "../jrd/exe_proto.h"
#include "../jrd/intl_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/par_proto.h"
#include "../jrd/Function.h"
#include "../jrd/TimeZone.h"
#include "../jrd/SystemPackages.h"
#include "../common/isc_proto.h"
#include "../common/classes/auto.h"
#include "../common/classes/fb_pair.h"
#include "../common/classes/fb_string.h"
#include "../common/classes/init.h"
#include "../common/classes/objects_array.h"
#include "../common/config/config.h"
#include "../common/ScanDir.h"
#include "../common/utils_proto.h"
#include "../common/classes/GetPlugins.h"

using namespace Firebird;
using namespace Jrd;


static EngineCheckout::Type checkoutType(IExternalEngine* engine);


namespace
{
	// Compare two formats for equivalence, excluding fmt_defaults field.
	bool sameFormats(const Format* fmt1, const Format* fmt2)
	{
		return fmt1->fmt_length == fmt2->fmt_length &&
			fmt1->fmt_count == fmt2->fmt_count &&
			fmt1->fmt_version == fmt2->fmt_version &&
			fmt1->fmt_desc == fmt2->fmt_desc;
	}

	// Copy message between different formats.
	void copyMessage(thread_db* tdbb,
		const Format* srcFormat, UCHAR* srcMsg,
		const Format* dstFormat, UCHAR* dstMsg)
	{
		fb_assert(srcFormat->fmt_desc.getCount() == dstFormat->fmt_desc.getCount());

		const auto srcDescEnd = srcFormat->fmt_desc.begin() + (srcFormat->fmt_desc.getCount() / 2 * 2);
		auto srcDescIt = srcFormat->fmt_desc.begin();
		auto dstDescIt = dstFormat->fmt_desc.begin();

		while (srcDescIt < srcDescEnd)
		{
			fb_assert(srcDescIt[1].dsc_dtype == dtype_short);
			fb_assert(dstDescIt[1].dsc_dtype == dtype_short);

			const auto srcArgOffset = (IPTR) srcDescIt[0].dsc_address;
			const auto srcNullOffset = (IPTR) srcDescIt[1].dsc_address;
			const auto srcNullPtr = reinterpret_cast<const SSHORT*>(srcMsg + srcNullOffset);

			const auto dstArgOffset = (IPTR) dstDescIt[0].dsc_address;
			const auto dstNullOffset = (IPTR) dstDescIt[1].dsc_address;
			const auto dstNullPtr = reinterpret_cast<SSHORT*>(dstMsg + dstNullOffset);

			if (!*srcNullPtr)
			{
				dsc srcDesc = srcDescIt[0];
				srcDesc.dsc_address = srcMsg + srcArgOffset;

				dsc dstDesc = dstDescIt[0];
				dstDesc.dsc_address = dstMsg + dstArgOffset;

				MOV_move(tdbb, &srcDesc, &dstDesc);
			}

			*dstNullPtr = *srcNullPtr;

			srcDescIt += 2;
			dstDescIt += 2;
		}
	}

	// Internal message node.
	class IntMessageNode : public MessageNode
	{
	public:
		IntMessageNode(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, USHORT message,
				Array<NestConst<Parameter>>& aParameters, const Format* aFormat)
			: MessageNode(pool),
			  parameters(aParameters),
			  format(aFormat)
		{
			setup(tdbb, csb, message, format->fmt_count);
		}

		USHORT setupDesc(thread_db* tdbb, CompilerScratch* csb, USHORT index,
			dsc* desc, ItemInfo* itemInfo) override
		{
			*desc = format->fmt_desc[index];

			if (index % 2 == 0 && index / 2u < parameters.getCount())
			{
				const Parameter* param = parameters[index / 2];

				if (param->prm_mechanism != prm_mech_type_of &&
					!fb_utils::implicit_domain(param->prm_field_source.object.c_str()))
				{
					QualifiedNameMetaNamePair entry(param->prm_field_source, {});

					FieldInfo fieldInfo;
					bool exist = csb->csb_map_field_info.get(entry, fieldInfo);
					MET_get_domain(tdbb, csb->csb_pool, param->prm_field_source, desc,
						(exist ? NULL : &fieldInfo));

					if (!exist)
						csb->csb_map_field_info.put(entry, fieldInfo);

					itemInfo->field = entry;
					itemInfo->nullable = fieldInfo.nullable;
					itemInfo->fullDomain = true;
				}

				itemInfo->name = param->prm_name;

				if (!param->prm_nullable)
					itemInfo->nullable = false;
			}

			return type_alignments[desc->dsc_dtype];
		}

	public:
		Array<NestConst<Parameter>>& parameters;
		const Format* const format;
	};

	// External message node.
	class ExtMessageNode : public MessageNode
	{
	public:
		ExtMessageNode(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb, USHORT message, const Format* aFormat)
			: MessageNode(pool),
			  format(aFormat)
		{
			setup(tdbb, csb, message, format->fmt_count);
		}

		USHORT setupDesc(thread_db* tdbb, CompilerScratch* csb, USHORT index,
			dsc* desc, ItemInfo* itemInfo) override
		{
			*desc = format->fmt_desc[index];
			return type_alignments[desc->dsc_dtype];
		}

		const StmtNode* execute(thread_db* tdbb, Request* request, ExeState* exeState) const override
		{
			if (request->req_operation == Request::req_evaluate)
			{
				// Clear the message. This is important for external routines.
				UCHAR* msg = request->getImpure<UCHAR>(impureOffset);
				memset(msg, 0, format->fmt_length);
			}

			return MessageNode::execute(tdbb, request, exeState);
		}

	public:
		const Format* const format;
	};

	// Initialize output parameters with their domains default value or NULL.
	// Kind of blr_init_variable, but for parameters.
	class InitParametersNode final : public TypedNode<StmtNode, StmtNode::TYPE_EXT_INIT_PARAMETERS>
	{
	public:
		InitParametersNode(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
				Array<NestConst<Parameter>>& parameters, MessageNode* aMessage)
			: TypedNode<StmtNode, StmtNode::TYPE_EXT_INIT_PARAMETERS>(pool),
			  message(aMessage)
		{
			// Iterate over the format items, except the EOF item.
			const unsigned paramCount = message->format->fmt_count / 2;

			defaultValuesNode = FB_NEW_POOL(pool) ValueListNode(pool, paramCount);

			for (unsigned paramIndex = 0; paramIndex < paramCount; ++paramIndex)
			{
				const auto parameter = parameters[paramIndex];

				if (parameter->prm_mechanism != prm_mech_type_of &&
					!fb_utils::implicit_domain(parameter->prm_field_source.object.c_str()))
				{
					QualifiedNameMetaNamePair entry(parameter->prm_field_source, {});

					FieldInfo fieldInfo;
					bool exist = csb->csb_map_field_info.get(entry, fieldInfo);

					if (exist && fieldInfo.defaultValue)
						defaultValuesNode->items[paramIndex] = CMP_clone_node(tdbb, csb, fieldInfo.defaultValue);
				}
			}
		}

		string internalPrint(NodePrinter& printer) const override
		{
			StmtNode::internalPrint(printer);

			NODE_PRINT(printer, message);
			NODE_PRINT(printer, defaultValuesNode);

			return "InitParametersNode";
		}

		void genBlr(DsqlCompilerScratch* /*dsqlScratch*/) override
		{
		}

		InitParametersNode* pass1(thread_db* tdbb, CompilerScratch* csb) override
		{
			doPass1(tdbb, csb, &defaultValuesNode);
			return this;
		}

		InitParametersNode* pass2(thread_db* tdbb, CompilerScratch* csb) override
		{
			ExprNode::doPass2(tdbb, csb, &defaultValuesNode);
			return this;
		}

		const StmtNode* execute(thread_db* tdbb, Request* request, ExeState* /*exeState*/) const override
		{
			if (request->req_operation == Request::req_evaluate)
			{
				const auto msg = request->getImpure<UCHAR>(message->impureOffset);
				const auto paramCount = defaultValuesNode->items.getCount();

				for (unsigned paramIndex = 0; paramIndex < paramCount; ++paramIndex)
				{
					const auto defaultValueNode = defaultValuesNode->items[paramIndex];
					dsc* defaultDesc = nullptr;

					if (defaultValueNode)
						defaultDesc = EVL_expr(tdbb, request, defaultValueNode);

					const auto formatIndex = paramIndex * 2;
					const auto& nullDesc = message->format->fmt_desc[formatIndex + 1];
					fb_assert(nullDesc.dsc_dtype == dtype_short);

					if (defaultDesc)
					{
						// Initialize the value. The null flag is already initialized to not-null.
						fb_assert(!*(SSHORT*) (msg + (IPTR) nullDesc.dsc_address));

						dsc desc = message->format->fmt_desc[formatIndex];
						desc.dsc_address = msg + (IPTR) desc.dsc_address;
						MOV_move(tdbb, defaultDesc, &desc);
					}
					else
						*(SSHORT*) (msg + (IPTR) nullDesc.dsc_address) = FB_TRUE;
				}

				request->req_operation = Request::req_return;
			}

			return parentStmt;
		}

		private:
			MessageNode* const message;
			ValueListNode* defaultValuesNode;
	};

	// Move parameters from a message to another, validating theirs values.
	class MessageMoverNode : public CompoundStmtNode
	{
	public:
		MessageMoverNode(MemoryPool& pool, MessageNode* fromMessage, MessageNode* toMessage,
					MessageNode* aCheckMessageEof = nullptr)
			: CompoundStmtNode(pool),
			  checkMessageEof(aCheckMessageEof)
		{
			// Iterate over the format items, except the EOF item.
			for (unsigned i = 0; i < (fromMessage->format->fmt_count / 2) * 2; i += 2)
			{
				auto flag = FB_NEW_POOL(pool) ParameterNode(pool);
				flag->messageNumber = fromMessage->messageNumber;
				flag->message = fromMessage;
				flag->argNumber = i + 1;

				auto param = FB_NEW_POOL(pool) ParameterNode(pool);
				param->messageNumber = fromMessage->messageNumber;
				param->message = fromMessage;
				param->argNumber = i;
				param->argFlag = flag;

				AssignmentNode* assign = FB_NEW_POOL(pool) AssignmentNode(pool);
				assign->asgnFrom = param;
				statements.add(assign);

				flag = FB_NEW_POOL(pool) ParameterNode(pool);
				flag->messageNumber = toMessage->messageNumber;
				flag->message = toMessage;
				flag->argNumber = i + 1;

				param = FB_NEW_POOL(pool) ParameterNode(pool);
				param->messageNumber = toMessage->messageNumber;
				param->message = toMessage;
				param->argNumber = i;
				param->argFlag = flag;

				assign->asgnTo = param;
			}
		}

		const StmtNode* execute(thread_db* tdbb, Request* request, ExeState* exeState) const override
		{
			if (checkMessageEof &&
				request->req_operation == Request::req_evaluate &&
				(request->req_flags & req_proc_select))
			{
				const auto msg = request->getImpure<UCHAR>(checkMessageEof->impureOffset);
				const auto eof = (SSHORT*) (msg + (IPTR) checkMessageEof->format->fmt_desc.back().dsc_address);

				if (!*eof)
					request->req_operation = Request::req_return;
			}

			return CompoundStmtNode::execute(tdbb, request, exeState);
		}

	private:
		MessageNode* checkMessageEof;
	};

	// External procedure node.
	class ExtProcedureNode : public CompoundStmtNode
	{
	public:
		ExtProcedureNode(MemoryPool& pool, MessageNode* aExtInMessageNode, MessageNode* aExtOutMessageNode,
				MessageNode* aIntOutMessageNode, const ExtEngineManager::Procedure* aProcedure)
			: CompoundStmtNode(pool),
			  extInMessageNode(aExtInMessageNode),
			  extOutMessageNode(aExtOutMessageNode),
			  intOutMessageNode(aIntOutMessageNode),
			  procedure(aProcedure)
		{
			SuspendNode* suspend = FB_NEW_POOL(pool) SuspendNode(pool);
			suspend->message = intOutMessageNode;
			suspend->statement = FB_NEW_POOL(pool) MessageMoverNode(pool,
				extOutMessageNode, intOutMessageNode, intOutMessageNode);

			statements.add(suspend);
			statements.add(FB_NEW_POOL(pool) StallNode(pool));
		}

		const StmtNode* execute(thread_db* tdbb, Request* request, ExeState* exeState) const override
		{
			impure_state* const impure = request->getImpure<impure_state>(impureOffset);
			ExtEngineManager::ResultSet*& resultSet = request->req_ext_resultset;
			UCHAR* extInMsg = extInMessageNode ? request->getImpure<UCHAR>(extInMessageNode->impureOffset) : NULL;
			UCHAR* extOutMsg = extOutMessageNode ? request->getImpure<UCHAR>(extOutMessageNode->impureOffset) : NULL;
			UCHAR* intOutMsg = intOutMessageNode ? request->getImpure<UCHAR>(intOutMessageNode->impureOffset) : NULL;
			SSHORT* eof = intOutMsg ?
				(SSHORT*) (intOutMsg + (IPTR) intOutMessageNode->format->fmt_desc.back().dsc_address) : NULL;

			switch (request->req_operation)
			{
				case Request::req_evaluate:
					impure->sta_state = 0;
					*eof = 0;

					fb_assert(!resultSet);
					resultSet = procedure->open(tdbb, extInMsg, extOutMsg);

					if (!resultSet)
						break;
					// fall into

				case Request::req_proceed:
				case Request::req_sync:
					*eof = 0;
					if (resultSet)
					{
						if (resultSet->fetch(tdbb) && (request->req_flags & req_proc_fetch))
							*eof = -1;
						else
						{
							delete resultSet;
							resultSet = NULL;
						}
					}

					impure->sta_state = 0;	// suspend node

					if (!*eof)
						request->req_operation = Request::req_return;
					else
						request->req_operation = Request::req_sync;

					break;

				case Request::req_unwind:
					delete resultSet;
					resultSet = NULL;
					break;
			}

			return CompoundStmtNode::execute(tdbb, request, exeState);
		}

	private:
		MessageNode* extInMessageNode;
		MessageNode* extOutMessageNode;
		MessageNode* intOutMessageNode;
		const ExtEngineManager::Procedure* procedure;
	};

	// External trigger node.
	class ExtTriggerNode final : public TypedNode<StmtNode, StmtNode::TYPE_EXT_TRIGGER>
	{
	public:
		ExtTriggerNode(MemoryPool& pool, const ExtEngineManager::Trigger* aTrigger)
			: TypedNode<StmtNode, StmtNode::TYPE_EXT_TRIGGER>(pool),
			  trigger(aTrigger)
		{
		}

		string internalPrint(NodePrinter& printer) const override
		{
			StmtNode::internalPrint(printer);
			return "ExtTriggerNode";
		}

		void genBlr(DsqlCompilerScratch* /*dsqlScratch*/) override
		{
		}

		ExtTriggerNode* pass1(thread_db* /*tdbb*/, CompilerScratch* /*csb*/) override
		{
			return this;
		}

		ExtTriggerNode* pass2(thread_db* /*tdbb*/, CompilerScratch* /*csb*/) override
		{
			return this;
		}

		const StmtNode* execute(thread_db* tdbb, Request* request, ExeState* /*exeState*/) const override
		{
			if (request->req_operation == Request::req_evaluate)
			{
				trigger->execute(tdbb, request, request->req_trigger_action,
					getRpb(request, 0), getRpb(request, 1));

				request->req_operation = Request::req_return;
			}

			return parentStmt;
		}

	private:
		static record_param* getRpb(Request* request, USHORT n)
		{
			return request->req_rpb.getCount() > n && request->req_rpb[n].rpb_number.isValid() ?
				&request->req_rpb[n] : NULL;
		}

	private:
		const ExtEngineManager::Trigger* trigger;
	};
}


template <typename T> class ExtEngineManager::ContextManager
{
public:
	ContextManager(thread_db* tdbb, EngineAttachmentInfo* aAttInfo, T* obj,
				CallerName aCallerName = CallerName())
		: attInfo(aAttInfo),
		  attachment(tdbb->getAttachment()),
		  transaction(tdbb->getTransaction()),
		  charSet(attachment->att_charset),
		  attInUse(attachment->att_in_use),
		  traInUse(transaction ? transaction->tra_in_use : false)
	{
		// !!!!!  needs async lock to be safe
		attachment->att_in_use = true;

		if (transaction)
		{
			callerName = transaction->tra_caller_name;
			transaction->tra_caller_name = aCallerName;
			++transaction->tra_callback_count;
			transaction->tra_in_use = true;
		}

		attInfo->context->setTransaction(tdbb);

		setCharSet(tdbb, attInfo, obj);
	}

	ContextManager(thread_db* tdbb, EngineAttachmentInfo* aAttInfo, USHORT aCharSet,
				CallerName aCallerName = CallerName())
		: attInfo(aAttInfo),
		  attachment(tdbb->getAttachment()),
		  transaction(tdbb->getTransaction()),
		  charSet(attachment->att_charset),
		  attInUse(attachment->att_in_use),
		  traInUse(transaction ? transaction->tra_in_use : false)
	{
		attachment->att_charset = aCharSet;
		// !!!!!  needs async lock to be safe
		attachment->att_in_use = true;

		if (transaction)
		{
			callerName = transaction->tra_caller_name;
			transaction->tra_caller_name = aCallerName;
			++transaction->tra_callback_count;
			transaction->tra_in_use = true;
		}

		attInfo->context->setTransaction(tdbb);
	}

	~ContextManager()
	{
		if (transaction)
		{
			--transaction->tra_callback_count;
			transaction->tra_in_use = traInUse;
			transaction->tra_caller_name = callerName;
		}

		// !!!!!  needs async lock to be safe
		attachment->att_in_use = attInUse;
		attachment->att_charset = charSet;
	}

private:
	void setCharSet(thread_db* tdbb, EngineAttachmentInfo* attInfo, T* obj)
	{
		attachment->att_charset = attInfo->adminCharSet;

		if (!obj)
			return;

		char charSetNameBuffer[MAX_QUALIFIED_NAME_TO_STRING_LEN];

		{	// scope
			EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));

			FbLocalStatus status;
			obj->getCharSet(&status, attInfo->context, charSetNameBuffer, sizeof(charSetNameBuffer));
			status.check();
		}

		charSetNameBuffer[sizeof(charSetNameBuffer) - 1] = '\0';
		QualifiedName charSetName;

		if (charSetNameBuffer[0])
		{
			charSetName = QualifiedName::parseSchemaObject(charSetNameBuffer);
			attachment->qualifyExistingName(tdbb, charSetName, obj_charset);
		}

		USHORT charSetId;

		if (!MET_get_char_coll_subtype(tdbb, &charSetId, charSetName))
			status_exception::raise(Arg::Gds(isc_charset_not_found) << charSetName.toQuotedString());

		attachment->att_charset = charSetId;
	}

private:
	EngineAttachmentInfo* attInfo;
	Jrd::Attachment* attachment;
	jrd_tra* transaction;
	// These data members are to restore the original information.
	const USHORT charSet;
	const bool attInUse;
	const bool traInUse;
	CallerName callerName;
};


//---------------------


ExtEngineManager::ExternalContextImpl::ExternalContextImpl(thread_db* tdbb,
		IExternalEngine* aEngine)
	: engine(aEngine),
	  internalAttachment(tdbb->getAttachment()),
	  internalTransaction(NULL),
	  externalAttachment(NULL),
	  externalTransaction(NULL),
	  miscInfo(*internalAttachment->att_pool)
{
	//// TODO: admin rights

	clientCharSet = INTL_charset_lookup(tdbb, internalAttachment->att_client_charset)->getName();

	externalAttachment = MasterInterfacePtr()->registerAttachment
		(internalAttachment->getProvider(), internalAttachment->getInterface());
}

ExtEngineManager::ExternalContextImpl::~ExternalContextImpl()
{
	releaseTransaction();

	if (externalAttachment)
	{
		externalAttachment->release();
		externalAttachment = NULL;
	}
}

void ExtEngineManager::ExternalContextImpl::releaseTransaction()
{
	if (externalTransaction)
	{
		externalTransaction->release();
		externalTransaction = NULL;
	}

	internalTransaction = NULL;
}

void ExtEngineManager::ExternalContextImpl::setTransaction(thread_db* tdbb)
{
	ITransaction* newTransaction = tdbb->getTransaction() ? tdbb->getTransaction()->getInterface(true) : NULL;

	if (newTransaction == internalTransaction)
		return;

	releaseTransaction();
	fb_assert(!externalTransaction && !internalTransaction);

	if ((internalTransaction = newTransaction))
		externalTransaction = MasterInterfacePtr()->registerTransaction(externalAttachment, internalTransaction);
}

IMaster* ExtEngineManager::ExternalContextImpl::getMaster()
{
	MasterInterfacePtr master;
	return master;
}

IExternalEngine* ExtEngineManager::ExternalContextImpl::getEngine(CheckStatusWrapper* /*status*/)
{
	return engine;
}

Firebird::IAttachment* ExtEngineManager::ExternalContextImpl::getAttachment(
	CheckStatusWrapper* /*status*/)
{
	externalAttachment->addRef();
	return externalAttachment;
}

Firebird::ITransaction* ExtEngineManager::ExternalContextImpl::getTransaction(
	CheckStatusWrapper* /*status*/)
{
	externalTransaction->addRef();
	return externalTransaction;
}

const char* ExtEngineManager::ExternalContextImpl::getUserName()
{
	return internalAttachment->att_user ? internalAttachment->att_user->getUserName().c_str() : "";
}

const char* ExtEngineManager::ExternalContextImpl::getDatabaseName()
{
	return internalAttachment->att_database->dbb_database_name.c_str();
}

const char* ExtEngineManager::ExternalContextImpl::getClientCharSet()
{
	return clientCharSet.c_str();
}

int ExtEngineManager::ExternalContextImpl::obtainInfoCode()
{
	static AtomicCounter counter;
	return ++counter;
}

void* ExtEngineManager::ExternalContextImpl::getInfo(int code)
{
	void* value = NULL;
	miscInfo.get(code, value);
	return value;
}

void* ExtEngineManager::ExternalContextImpl::setInfo(int code, void* value)
{
	void* oldValue = getInfo(code);
	miscInfo.put(code, value);
	return oldValue;
}


//---------------------


ExtEngineManager::ExtRoutine::ExtRoutine(thread_db* tdbb, ExtEngineManager* aExtManager,
		IExternalEngine* aEngine, RoutineMetadata* aMetadata)
	: extManager(aExtManager),
	  engine(aEngine),
	  metadata(aMetadata),
	  database(tdbb->getDatabase())
{
	engine->addRef();
}

void ExtEngineManager::ExtRoutine::PluginDeleter::operator()(IPluginBase* ptr)
{
	if (ptr)
		PluginManagerInterfacePtr()->releasePlugin(ptr);
}


//---------------------


struct ExtEngineManager::Function::Impl final
{
	Impl(MemoryPool& pool)
		: inValidations(pool),
		  outValidations(pool),
		  outDefaults(pool)
	{
	}

	Array<NonPooledPair<Item, ItemInfo*>> inValidations;
	Array<NonPooledPair<Item, ItemInfo*>> outValidations;
	Array<unsigned> outDefaults;
};


ExtEngineManager::Function::Function(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
		ExtEngineManager* aExtManager, IExternalEngine* aEngine,
		RoutineMetadata* aMetadata, IExternalFunction* aFunction,
		RefPtr<IMessageMetadata> extInputParameters, RefPtr<IMessageMetadata> extOutputParameters,
		const Jrd::Function* aUdf)
	: ExtRoutine(tdbb, aExtManager, aEngine, aMetadata),
	  function(aFunction),
	  udf(aUdf),
	  impl(FB_NEW_POOL(pool) Impl(pool))
{
	extInputFormat.reset(Routine::createFormat(pool, extInputParameters, false));
	extOutputFormat.reset(Routine::createFormat(pool, extOutputParameters, true));

	const bool useExtInMessage = udf->getInputFields().hasData() &&
		!sameFormats(extInputFormat, udf->getInputFormat());

	if (useExtInMessage)
		extInputImpureOffset = csb->allocImpure(FB_ALIGNMENT, extInputFormat->fmt_length);
	else
		extInputFormat.reset();

	const bool useExtOutMessage = udf->getOutputFields().hasData() &&
		!sameFormats(extOutputFormat, udf->getOutputFormat());

	if (useExtOutMessage)
		extOutputImpureOffset = csb->allocImpure(FB_ALIGNMENT, extOutputFormat->fmt_length);
	else
		extOutputFormat.reset();

	// Get input parameters that have validation expressions.
	for (const auto param : udf->getInputFields())
	{
		FieldInfo fieldInfo;
		ItemInfo itemInfo;

		if (param->prm_mechanism != prm_mech_type_of &&
			!fb_utils::implicit_domain(param->prm_field_source.object.c_str()))
		{
			const QualifiedNameMetaNamePair entry(param->prm_field_source, {});
			const bool exist = csb->csb_map_field_info.get(entry, fieldInfo);

			if (!exist)
			{
				dsc dummyDesc;
				MET_get_domain(tdbb, csb->csb_pool, param->prm_field_source, &dummyDesc, &fieldInfo);
				csb->csb_map_field_info.put(entry, fieldInfo);
			}

			itemInfo.field = entry;
			itemInfo.nullable = fieldInfo.nullable;
			itemInfo.fullDomain = true;
		}

		itemInfo.name = param->prm_name;

		if (!param->prm_nullable)
			itemInfo.nullable = false;

		if (itemInfo.isSpecial())
		{
			Item item(Item::TYPE_PARAMETER, 0, (param->prm_number - 1) * 2);
			csb->csb_map_item_info.put(item, itemInfo);

			impl->inValidations.ensureCapacity(udf->getInputFields().getCount() - (param->prm_number - 1));
			impl->inValidations.push({item, CMP_pass2_validation(tdbb, csb, item)});
		}
	}

	// Get output parameters that have default or validation expressions.
	for (const auto param : udf->getOutputFields())
	{
		FieldInfo fieldInfo;
		ItemInfo itemInfo;

		if (param->prm_mechanism != prm_mech_type_of &&
			!fb_utils::implicit_domain(param->prm_field_source.object.c_str()))
		{
			const QualifiedNameMetaNamePair entry(param->prm_field_source, {});
			const bool exist = csb->csb_map_field_info.get(entry, fieldInfo);

			if (!exist)
			{
				dsc dummyDesc;
				MET_get_domain(tdbb, csb->csb_pool, param->prm_field_source, &dummyDesc, &fieldInfo);
				csb->csb_map_field_info.put(entry, fieldInfo);
			}

			if (fieldInfo.defaultValue)
			{
				impl->outDefaults.ensureCapacity(udf->getOutputFields().getCount() - param->prm_number);
				impl->outDefaults.push(param->prm_number);
			}

			itemInfo.field = entry;
			itemInfo.nullable = fieldInfo.nullable;
			itemInfo.fullDomain = true;
		}

		itemInfo.name = param->prm_name;

		if (!param->prm_nullable)
			itemInfo.nullable = false;

		if (itemInfo.isSpecial())
		{
			Item item(Item::TYPE_PARAMETER, 1, param->prm_number * 2);
			csb->csb_map_item_info.put(item, itemInfo);

			impl->outValidations.ensureCapacity(udf->getOutputFields().getCount());
			impl->outValidations.push({item, CMP_pass2_validation(tdbb, csb, item)});
		}
	}
}


ExtEngineManager::Function::~Function()
{
	//Database::Checkout dcoHolder(database);
	function->dispose();
}


// Execute an external function starting with an inactive request and ending with an active one.
void ExtEngineManager::Function::execute(thread_db* tdbb, Request* request, jrd_tra* transaction,
	unsigned inMsgLength, UCHAR* inMsg, unsigned outMsgLength, UCHAR* outMsg) const
{
	fb_assert(inMsgLength == udf->getInputFormat()->fmt_length);
	fb_assert(outMsgLength == udf->getOutputFormat()->fmt_length);

	// Validate input parameters (internal message).
	validateParameters(tdbb, inMsg, true);

	// If there is a need for an external input message, copy the internal message to it and switch the message.
	if (extInputImpureOffset.has_value())
	{
		const auto extInMsg = request->getImpure<UCHAR>(extInputImpureOffset.value());
		copyMessage(tdbb, udf->getInputFormat(), inMsg, extInputFormat, extInMsg);
		inMsg = extInMsg;
		///inMsgLength = extInputFormat->fmt_length;
	}

	// Initialize outputs in the internal message.
	{
		fb_assert(udf->getOutputFormat()->fmt_desc.getCount() / 2 == udf->getOutputFields().getCount());

		// Initialize everything to NULL (FB_TRUE).
		memset(outMsg, FB_TRUE, udf->getOutputFormat()->fmt_length);

		for (const auto paramNumber : impl->outDefaults)
		{
			const auto param = udf->getOutputFields()[paramNumber];
			const QualifiedNameMetaNamePair entry(param->prm_field_source, {});
			FieldInfo fieldInfo;

			dsc* defaultValue = nullptr;

			if (request->getStatement()->mapFieldInfo.get(entry, fieldInfo) && fieldInfo.defaultValue)
				defaultValue = EVL_expr(tdbb, request, fieldInfo.defaultValue);

			const auto& paramDesc = udf->getOutputFormat()->fmt_desc[paramNumber * 2];
			const auto& nullDesc = udf->getOutputFormat()->fmt_desc[paramNumber * 2 + 1];

			fb_assert(nullDesc.dsc_dtype == dtype_short);

			if (defaultValue)
			{
				dsc desc = paramDesc;
				desc.dsc_address = outMsg + (IPTR) desc.dsc_address;
				MOV_move(tdbb, defaultValue, &desc);

				*(SSHORT*) (outMsg + (IPTR) nullDesc.dsc_address) = FB_FALSE;
			}
			else
				*(SSHORT*) (outMsg + (IPTR) nullDesc.dsc_address) = FB_TRUE;
		}
	}

	const auto extOutMsg = extOutputImpureOffset.has_value() ?
		request->getImpure<UCHAR>(extOutputImpureOffset.value()) : nullptr;

	// If there is a need for an external output message, copy the internal message to it.
	if (extOutMsg)
		copyMessage(tdbb, udf->getOutputFormat(), outMsg, extOutputFormat, extOutMsg);

	// Call external.
	{	// scope
		EngineAttachmentInfo* attInfo = extManager->getEngineAttachment(tdbb, engine.get());
		const MetaString& userName = udf->invoker ? udf->invoker->getUserName() : "";
		ContextManager<IExternalFunction> ctxManager(tdbb, attInfo, function,
			(udf->getName().package.isEmpty() ?
			CallerName(obj_udf, udf->getName(), userName) :
			CallerName(obj_package_header, QualifiedName(udf->getName().package, udf->getName().schema), userName)));

		EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));

		FbLocalStatus status;
		function->execute(&status, attInfo->context, inMsg, (extOutMsg ? extOutMsg : outMsg));
		status.check();
	}

	// If there is an external output message, copy it to the internal message.
	if (extOutMsg)
		copyMessage(tdbb, extOutputFormat, extOutMsg, udf->getOutputFormat(), outMsg);

	// Validate output parameters (internal message).
	validateParameters(tdbb, outMsg, false);
}

void ExtEngineManager::Function::validateParameters(thread_db* tdbb, UCHAR* msg, bool input) const
{
	const auto format = input ? udf->getInputFormat() : udf->getOutputFormat();
	const auto& validations = input ? impl->inValidations : impl->outValidations;
	const UCHAR messageNumber = input ? 0 : 1;

	for (const auto& [item, itemInfo] : validations)
	{
		const unsigned paramNumber = item.index / 2;
		const auto& paramDesc = format->fmt_desc[paramNumber * 2];
		const auto& nullDesc = format->fmt_desc[paramNumber * 2 + 1];

		fb_assert(nullDesc.dsc_dtype == dtype_short);

		dsc value = paramDesc;
		value.dsc_address = msg + (IPTR) value.dsc_address;

		const bool isNull = *(SSHORT*) (msg + (IPTR) nullDesc.dsc_address);

		EVL_validate(tdbb, Item(Item::TYPE_PARAMETER, messageNumber, paramNumber), itemInfo, &value, isNull);
	}
}


//---------------------


ExtEngineManager::Procedure::Procedure(thread_db* tdbb, ExtEngineManager* aExtManager,
	    IExternalEngine* aEngine, RoutineMetadata* aMetadata, IExternalProcedure* aProcedure,
		const jrd_prc* aPrc)
	: ExtRoutine(tdbb, aExtManager, aEngine, aMetadata),
	  procedure(aProcedure),
	  prc(aPrc)
{
}


ExtEngineManager::Procedure::~Procedure()
{
	//Database::Checkout dcoHolder(database);
	procedure->dispose();
}


ExtEngineManager::ResultSet* ExtEngineManager::Procedure::open(thread_db* tdbb,
	UCHAR* inMsg, UCHAR* outMsg) const
{
	return FB_NEW_POOL(*tdbb->getDefaultPool()) ResultSet(tdbb, inMsg, outMsg, this);
}


//---------------------


ExtEngineManager::ResultSet::ResultSet(thread_db* tdbb, UCHAR* inMsg, UCHAR* outMsg,
		const ExtEngineManager::Procedure* aProcedure)
	: procedure(aProcedure),
	  attachment(tdbb->getAttachment()),
	  firstFetch(true)
{
	attInfo = procedure->extManager->getEngineAttachment(tdbb, procedure->engine.get());
	const MetaString& userName = procedure->prc->invoker ? procedure->prc->invoker->getUserName() : "";
	ContextManager<IExternalProcedure> ctxManager(tdbb, attInfo, procedure->procedure,
		(procedure->prc->getName().package.isEmpty() ?
			CallerName(obj_procedure, procedure->prc->getName(), userName) :
			CallerName(obj_package_header,
				QualifiedName(procedure->prc->getName().package, procedure->prc->getName().schema), userName)));

	charSet = attachment->att_charset;

	EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));

	FbLocalStatus status;
	resultSet = procedure->procedure->open(&status, attInfo->context, inMsg, outMsg);
	status.check();
}


ExtEngineManager::ResultSet::~ResultSet()
{
	if (resultSet)
	{
		EngineCheckout cout(JRD_get_thread_data(), FB_FUNCTION, checkoutType(attInfo->engine));
		resultSet->dispose();
	}
}


bool ExtEngineManager::ResultSet::fetch(thread_db* tdbb)
{
	bool wasFirstFetch = firstFetch;
	firstFetch = false;

	if (!resultSet)
		return wasFirstFetch;

	const MetaString& userName = procedure->prc->invoker ? procedure->prc->invoker->getUserName() : "";
	ContextManager<IExternalProcedure> ctxManager(tdbb, attInfo, charSet,
		(procedure->prc->getName().package.isEmpty() ?
			CallerName(obj_procedure, procedure->prc->getName(), userName) :
			CallerName(obj_package_header,
				QualifiedName(procedure->prc->getName().package, procedure->prc->getName().schema), userName)));

	EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));

	FbLocalStatus status;
	bool ret = resultSet->fetch(&status);
	status.check();

	return ret;
}


//---------------------


ExtEngineManager::Trigger::Trigger(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb,
			ExtEngineManager* aExtManager, IExternalEngine* aEngine, RoutineMetadata* aMetadata,
			IExternalTrigger* aTrigger, const Jrd::Trigger* aTrg)
	: ExtRoutine(tdbb, aExtManager, aEngine, aMetadata),
	  computedStatements(pool),
	  trigger(aTrigger),
	  trg(aTrg),
	  fieldsPos(pool),
	  varDecls(pool),
	  computedCount(0)
{
	jrd_rel* relation = trg->relation;

	if (relation)
	{
		GenericMap<Left<MetaName, USHORT> > fieldsMap;

		for (FB_SIZE_T i = 0; i < relation->rel_fields->count(); ++i)
		{
			jrd_fld* field = (*relation->rel_fields)[i];

			if (field)
				fieldsMap.put(field->fld_name, (USHORT) i);
		}

		format = Routine::createFormat(pool, metadata->triggerFields, false);

		FbLocalStatus status;

		for (unsigned i = 0; i < format->fmt_count / 2u; ++i)
		{
			const char* fieldName = metadata->triggerFields->getField(&status, i);
			status.check();

			USHORT pos;

			if (!fieldsMap.get(fieldName, pos))
				fb_assert(false);
			else
				fieldsPos.add(pos);
		}

		setupComputedFields(tdbb, pool, csb);
	}
}


ExtEngineManager::Trigger::~Trigger()
{
	trigger->dispose();
}


void ExtEngineManager::Trigger::execute(thread_db* tdbb, Request* request, unsigned action,
	record_param* oldRpb, record_param* newRpb) const
{
	EngineAttachmentInfo* attInfo = extManager->getEngineAttachment(tdbb, engine.get());
	const MetaString& userName = trg->ssDefiner.asBool() ? trg->owner.c_str() : "";
	ContextManager<IExternalTrigger> ctxManager(tdbb, attInfo, trigger,
		CallerName(obj_trigger, trg->name, userName));

	// ASF: Using Array instead of HalfStaticArray to not need to do alignment hacks here.
	Array<UCHAR> oldMsg;
	Array<UCHAR> newMsg;

	if (oldRpb)
		setValues(tdbb, request, oldMsg, oldRpb);

	if (newRpb)
		setValues(tdbb, request, newMsg, newRpb);

	{	// scope
		EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));

		FbLocalStatus status;
		trigger->execute(&status, attInfo->context, action,
			(oldMsg.hasData() ? oldMsg.begin() : NULL), (newMsg.hasData() ? newMsg.begin() : NULL));
		status.check();
	}

	if (newRpb)
	{
		// Move data back from the message to the record.

		Record* record = newRpb->rpb_record;
		UCHAR* p = newMsg.begin();

		for (unsigned i = 0; i < format->fmt_count / 2u; ++i)
		{
			USHORT fieldPos = fieldsPos[i];

			dsc target;
			bool readonly = !EVL_field(newRpb->rpb_relation, record, fieldPos, &target) &&
				target.dsc_address && !(target.dsc_flags & DSC_null);

			if (!readonly && target.dsc_address)
			{
				SSHORT* nullSource = (SSHORT*) (p + (IPTR) format->fmt_desc[i * 2 + 1].dsc_address);

				if (*nullSource == 0)
				{
					dsc source = format->fmt_desc[i * 2];
					source.dsc_address += (IPTR) p;
					MOV_move(tdbb, &source, &target);
					record->clearNull(fieldPos);
				}
				else
					record->setNull(fieldPos);
			}
		}
	}
}


void ExtEngineManager::Trigger::setupComputedFields(thread_db* tdbb, MemoryPool& pool, CompilerScratch* csb)
{
	SET_TDBB(tdbb);

	USHORT varId = 0;

	static_assert(NEW_CONTEXT_VALUE == OLD_CONTEXT_VALUE + 1, "OLD/NEW context assumption.");

	for (unsigned context = OLD_CONTEXT_VALUE; context <= NEW_CONTEXT_VALUE; ++context)	// OLD (0), NEW (1)
	{
		for (FB_SIZE_T i = 0; i < trg->relation->rel_fields->count(); ++i)
		{
			jrd_fld* field = (*trg->relation->rel_fields)[i];
			if (!field || !field->fld_computation)
				continue;

			if (context == OLD_CONTEXT_VALUE)	// count once
				++computedCount;

			DeclareVariableNode* declareNode = FB_NEW_POOL(pool) DeclareVariableNode(pool);
			declareNode->varId = varId;

			declareNode->varDesc = trg->relation->rel_current_format->fmt_desc[i];

			// For CHAR fields, change variable type to VARCHAR to avoid manual INTL adjustments for multi-byte.
			if (declareNode->varDesc.isText())
			{
				declareNode->varDesc.dsc_dtype = dtype_varying;
				declareNode->varDesc.dsc_length += sizeof(USHORT);
			}

			varDecls.push(declareNode);

			csb->csb_variables = vec<DeclareVariableNode*>::newVector(
				*tdbb->getDefaultPool(), csb->csb_variables, varId);

			ValueExprNode* exprNode = FB_NEW_POOL(*tdbb->getDefaultPool()) FieldNode(*tdbb->getDefaultPool(), context, i, true);

			VariableNode* varNode = FB_NEW_POOL(pool) VariableNode(pool);
			varNode->varId = varId;

			AssignmentNode* assignNode = FB_NEW_POOL(pool) AssignmentNode(pool);
			assignNode->asgnFrom = exprNode;
			assignNode->asgnTo = varNode;

			// Do not run the assignment for invalid RPBs (NEW in DELETE, OLD in INSERT).

			SLONG* actionPtr = FB_NEW_POOL(pool) SLONG(INFO_TYPE_TRIGGER_ACTION);

			LiteralNode* actionLiteral = FB_NEW_POOL(pool) LiteralNode(pool);
			actionLiteral->litDesc.dsc_dtype = dtype_long;
			actionLiteral->litDesc.dsc_length = sizeof(SLONG);
			actionLiteral->litDesc.dsc_scale = 0;
			actionLiteral->litDesc.dsc_sub_type = 0;
			actionLiteral->litDesc.dsc_address = reinterpret_cast<UCHAR*>(actionPtr);

			InternalInfoNode* internalInfo = FB_NEW_POOL(pool) InternalInfoNode(pool, actionLiteral);

			SLONG* comparePtr = FB_NEW_POOL(pool) SLONG(context == OLD_CONTEXT_VALUE ? TRIGGER_INSERT : TRIGGER_DELETE);

			LiteralNode* compareLiteral = FB_NEW_POOL(pool) LiteralNode(pool);
			compareLiteral->litDesc.dsc_dtype = dtype_long;
			compareLiteral->litDesc.dsc_length = sizeof(SLONG);
			compareLiteral->litDesc.dsc_scale = 0;
			compareLiteral->litDesc.dsc_sub_type = 0;
			compareLiteral->litDesc.dsc_address = reinterpret_cast<UCHAR*>(comparePtr);

			ComparativeBoolNode* cmp = FB_NEW_POOL(pool) ComparativeBoolNode(pool,
				blr_neq, internalInfo, compareLiteral);

			IfNode* ifNode = FB_NEW_POOL(pool) IfNode(pool);
			ifNode->condition = cmp;
			ifNode->trueAction = assignNode;

			computedStatements.add(declareNode);
			computedStatements.add(ifNode);

			++varId;
		}
	}
}


void ExtEngineManager::Trigger::setValues(thread_db* tdbb, Request* request, Array<UCHAR>& msgBuffer,
	record_param* rpb) const
{
	if (!rpb || !rpb->rpb_record)
		return;

	UCHAR* p = msgBuffer.getBuffer(format->fmt_length);
	memset(p, 0, format->fmt_length);

	// NEW variables comes after OLD ones.
	USHORT computedVarId =
		request->req_rpb.getCount() >= NEW_CONTEXT_VALUE && rpb == &request->req_rpb[NEW_CONTEXT_VALUE] ?
			computedCount : 0;

	for (unsigned i = 0; i < format->fmt_count / 2u; ++i)
	{
		USHORT fieldPos = fieldsPos[i];
		SSHORT* nullTarget = (SSHORT*) (p + (IPTR) format->fmt_desc[i * 2 + 1].dsc_address);

		dsc source;
		dsc target = format->fmt_desc[i * 2];
		target.dsc_address += (IPTR) p;

		const jrd_fld* field = (*rpb->rpb_relation->rel_fields)[fieldPos];

		if (field->fld_computation)
		{
			const DeclareVariableNode* varDecl = varDecls[computedVarId++];
			impure_value* varImpure = request->getImpure<impure_value>(varDecl->impureOffset);

			*nullTarget = (varImpure->vlu_desc.dsc_flags & DSC_null) ? FB_TRUE : FB_FALSE;

			if (!*nullTarget)
				MOV_move(tdbb, &varImpure->vlu_desc, &target);
		}
		else
		{
			if (!EVL_field(rpb->rpb_relation, rpb->rpb_record, fieldPos, &source))
				source.dsc_flags |= DSC_null;

			*nullTarget = (source.dsc_flags & DSC_null) ? FB_TRUE : FB_FALSE;

			if (!*nullTarget)
				MOV_move(tdbb, &source, &target);
		}
	}
}


//---------------------


ExtEngineManager::~ExtEngineManager()
{
	fb_assert(enginesAttachments.count() == 0);
/*
AP: Commented out this code due to later AV.

When engine is released, it does dlclose() plugin module (libudr_engine.so),
but that module is not actually unloaded - because UDR module (libudrcpp_example.so) is using
symbols from plugin module, therefore raising plugin module's reference count.
UDR module can be unloaded only from plugin module's global variable (ModuleMap modules) dtor,
which is not called as long as plugin module is not unloaded. As the result all this will be
unloaded only on program exit, causing at that moment AV if this code is active: it happens that
~ModuleMap dlcloses itself.

	PluginManagerInterfacePtr pi;

	EnginesMap::Accessor accessor(&engines);
	for (bool found = accessor.getFirst(); found; found = accessor.getNext())
	{
		IExternalEngine* engine = accessor.current()->second;
		pi->releasePlugin(engine);
	}
 */
}


//---------------------


namespace
{
	class SystemEngine : public StdPlugin<IExternalEngineImpl<SystemEngine, ThrowStatusExceptionWrapper> >
	{
	public:
		explicit SystemEngine()
		{
		}

	public:
		int release() override
		{
			// Never delete static instance of SystemEngine
			return 1;
		}

		void open(ThrowStatusExceptionWrapper* status, IExternalContext* context,
			char* name, unsigned nameSize) override
		{
		}

		void openAttachment(ThrowStatusExceptionWrapper* status, IExternalContext* context) override
		{
		}

		void closeAttachment(ThrowStatusExceptionWrapper* status, IExternalContext* context) override
		{
		}

		IExternalFunction* makeFunction(ThrowStatusExceptionWrapper* status, IExternalContext* context,
			IRoutineMetadata* metadata, IMetadataBuilder* inBuilder, IMetadataBuilder* outBuilder) override
		{
			const char* packageName = metadata->getPackage(status);
			const char* routineName = metadata->getName(status);

			for (auto& package : SystemPackage::get())
			{
				if (strcmp(package.name, packageName) == 0)
				{
					for (auto& routine : package.functions)
					{
						if (strcmp(routine.name, routineName) == 0)
							return routine.factory(status, context, metadata, inBuilder, outBuilder);
					}
				}
			}

			fb_assert(false);
			return nullptr;
		}

		IExternalProcedure* makeProcedure(ThrowStatusExceptionWrapper* status, IExternalContext* context,
			IRoutineMetadata* metadata, IMetadataBuilder* inBuilder, IMetadataBuilder* outBuilder) override
		{
			const char* packageName = metadata->getPackage(status);
			const char* routineName = metadata->getName(status);

			for (auto& package : SystemPackage::get())
			{
				if (strcmp(package.name, packageName) == 0)
				{
					for (auto& routine : package.procedures)
					{
						if (strcmp(routine.name, routineName) == 0)
							return routine.factory(status, context, metadata, inBuilder, outBuilder);
					}
				}
			}

			fb_assert(false);
			return nullptr;
		}

		IExternalTrigger* makeTrigger(ThrowStatusExceptionWrapper* status, IExternalContext* context,
			IRoutineMetadata* metadata, IMetadataBuilder* fieldsBuilder) override
		{
			fb_assert(false);
			return nullptr;
		}

	public:
		static SystemEngine* INSTANCE;
	};

	SystemEngine* SystemEngine::INSTANCE = nullptr;
}


//---------------------


void ExtEngineManager::initialize()
{
	SystemEngine::INSTANCE = FB_NEW SystemEngine();
}


ExtEngineManager::ExtEngineManager(MemoryPool& p)
	: PermanentStorage(p),
	  engines(p),
	  enginesAttachments(p)
{
	engines.put("SYSTEM", SystemEngine::INSTANCE);
}


void ExtEngineManager::closeAttachment(thread_db* tdbb, Attachment* attachment)
{
	EnginesMap enginesCopy;

	{	// scope
		ReadLockGuard readGuard(enginesLock, FB_FUNCTION);

		EnginesMap::Accessor accessor(&engines);
		for (bool found = accessor.getFirst(); found; found = accessor.getNext())
			enginesCopy.put(accessor.current()->first, accessor.current()->second);
	}

	EngineCheckout cout(tdbb, FB_FUNCTION, EngineCheckout::UNNECESSARY);

	EnginesMap::Accessor accessor(&enginesCopy);
	for (bool found = accessor.getFirst(); found; found = accessor.getNext())
	{
		IExternalEngine* engine = accessor.current()->second;
		EngineAttachmentInfo* attInfo = getEngineAttachment(tdbb, engine, true);

		if (attInfo)
		{
			{	// scope
				ContextManager<IExternalFunction> ctxManager(tdbb, attInfo, attInfo->adminCharSet);
				FbLocalStatus status;
				engine->closeAttachment(&status, attInfo->context);	//// FIXME: log status

				// Check whether a non-SYSTEM engine is used by other attachments.
				// If no one uses, release it.
				if (engine != SystemEngine::INSTANCE)
				{
					bool close = true;
					WriteLockGuard writeGuard(enginesLock, FB_FUNCTION);

					EnginesAttachmentsMap::Accessor ea_accessor(&enginesAttachments);
					for (bool ea_found = ea_accessor.getFirst(); ea_found; ea_found = ea_accessor.getNext())
					{
						if (ea_accessor.current()->first.engine == engine)
						{
							close = false; // engine is in use, no need to release
							break;
						}
					}

					if (close)
					{
						if (engines.remove(accessor.current()->first)) // If engine has already been deleted - nothing to do
							PluginManagerInterfacePtr()->releasePlugin(engine);
					}
				}
			}

			delete attInfo;
		}
	}
}


void ExtEngineManager::makeFunction(thread_db* tdbb, CompilerScratch* csb, Jrd::Function* udf,
	const MetaName& engine, const string& entryPoint, const string& body)
{
	string entryPointTrimmed = entryPoint;
	entryPointTrimmed.trim();

	EngineAttachmentInfo* attInfo = getEngineAttachment(tdbb, engine);
	const MetaString& userName = udf->invoker ? udf->invoker->getUserName() : "";
	ContextManager<IExternalFunction> ctxManager(tdbb, attInfo, attInfo->adminCharSet,
		(udf->getName().package.isEmpty() ?
			CallerName(obj_udf, udf->getName(), userName) :
			CallerName(obj_package_header, QualifiedName(udf->getName().package, udf->getName().schema), userName)));

	MemoryPool& pool = *tdbb->getAttachment()->att_pool;

	AutoPtr<RoutineMetadata> metadata(FB_NEW_POOL(pool) RoutineMetadata(pool));
	metadata->name = udf->getName();
	metadata->entryPoint = entryPointTrimmed;
	metadata->body = body;
	metadata->inputParameters.assignRefNoIncr(Routine::createMetadata(udf->getInputFields(), true));
	metadata->outputParameters.assignRefNoIncr(Routine::createMetadata(udf->getOutputFields(), true));

	udf->setInputFormat(Routine::createFormat(pool, metadata->inputParameters, false));
	udf->setOutputFormat(Routine::createFormat(pool, metadata->outputParameters, true));

	FbLocalStatus status;

	RefPtr<IMetadataBuilder> inBuilder(REF_NO_INCR, metadata->inputParameters->getBuilder(&status));
	status.check();

	RefPtr<IMetadataBuilder> outBuilder(REF_NO_INCR, metadata->outputParameters->getBuilder(&status));
	status.check();

	IExternalFunction* externalFunction;
	RefPtr<IMessageMetadata> extInputParameters, extOutputParameters;

	{	// scope
		EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));

		externalFunction = attInfo->engine->makeFunction(&status, attInfo->context, metadata,
			inBuilder, outBuilder);

		try
		{
			status.check();

			if (!externalFunction)
			{
				status_exception::raise(
					Arg::Gds(isc_eem_func_not_returned) << udf->getName().toQuotedString() << engine);
			}
		}
		catch (const Exception&)
		{
			if (tdbb->getAttachment()->isGbak())
				return;
			else
				throw;
		}

		extInputParameters.assignRefNoIncr(inBuilder->getMetadata(&status));
		status.check();

		extOutputParameters.assignRefNoIncr(outBuilder->getMetadata(&status));
		status.check();
	}

	try
	{
		udf->fun_external = FB_NEW_POOL(pool) Function(tdbb, pool, csb, this, attInfo->engine,
			metadata.release(), externalFunction, extInputParameters, extOutputParameters, udf);

		// This is necessary for compilation, but will never be executed.
		const auto dummyNode = FB_NEW_POOL(csb->csb_pool) CompoundStmtNode(csb->csb_pool);

		auto statement = udf->getStatement();
		PAR_preparsed_node(tdbb, nullptr, dummyNode, nullptr, &csb, &statement, false, 0);
		udf->setStatement(statement);
	}
	catch (...)
	{
		EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));
		externalFunction->dispose();
		throw;
	}
}


void ExtEngineManager::makeProcedure(thread_db* tdbb, CompilerScratch* csb, jrd_prc* prc,
	const MetaName& engine, const string& entryPoint, const string& body)
{
	string entryPointTrimmed = entryPoint;
	entryPointTrimmed.trim();

	EngineAttachmentInfo* attInfo = getEngineAttachment(tdbb, engine);
	const MetaString& userName = prc->invoker ? prc->invoker->getUserName() : "";
	ContextManager<IExternalProcedure> ctxManager(tdbb, attInfo, attInfo->adminCharSet,
		(prc->getName().package.isEmpty() ?
			CallerName(obj_procedure, prc->getName(), userName) :
			CallerName(obj_package_header,
				QualifiedName(prc->getName().package, prc->getName().schema), userName)));

	MemoryPool& pool = *tdbb->getAttachment()->att_pool;

	AutoPtr<RoutineMetadata> metadata(FB_NEW_POOL(pool) RoutineMetadata(pool));
	metadata->name = prc->getName();
	metadata->entryPoint = entryPointTrimmed;
	metadata->body = body;
	metadata->inputParameters.assignRefNoIncr(Routine::createMetadata(prc->getInputFields(), true));
	metadata->outputParameters.assignRefNoIncr(Routine::createMetadata(prc->getOutputFields(), true));

	prc->setInputFormat(Routine::createFormat(pool, metadata->inputParameters, false));
	prc->setOutputFormat(Routine::createFormat(pool, metadata->outputParameters, true));

	FbLocalStatus status;

	RefPtr<IMetadataBuilder> inBuilder(REF_NO_INCR, metadata->inputParameters->getBuilder(&status));
	status.check();

	RefPtr<IMetadataBuilder> outBuilder(REF_NO_INCR, metadata->outputParameters->getBuilder(&status));
	status.check();

	IExternalProcedure* externalProcedure;
	RefPtr<IMessageMetadata> extInputParameters, extOutputParameters;

	{	// scope
		EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));

		externalProcedure = attInfo->engine->makeProcedure(&status, attInfo->context, metadata,
			inBuilder, outBuilder);

		try
		{
			status.check();

			if (!externalProcedure)
			{
				status_exception::raise(
					Arg::Gds(isc_eem_proc_not_returned) <<
						prc->getName().toQuotedString() << engine);
			}
		}
		catch (const Exception&)
		{
			if (tdbb->getAttachment()->isGbak())
				return;
			else
				throw;
		}

		extInputParameters.assignRefNoIncr(inBuilder->getMetadata(&status));
		status.check();

		extOutputParameters.assignRefNoIncr(outBuilder->getMetadata(&status));
		status.check();
	}

	const Format* extInputFormat = Routine::createFormat(pool, extInputParameters, false);
	const Format* extOutputFormat = Routine::createFormat(pool, extOutputParameters, true);

	try
	{
		prc->setExternal(FB_NEW_POOL(pool) Procedure(tdbb, this, attInfo->engine,
			metadata.release(), externalProcedure, prc));

		MemoryPool& csbPool = csb->csb_pool;

		CompoundStmtNode* mainNode = FB_NEW_POOL(csbPool) CompoundStmtNode(csbPool);

		IntMessageNode* intInMessageNode = prc->getInputFields().hasData() ?
			FB_NEW_POOL(csbPool) IntMessageNode(tdbb, csbPool, csb, 0,
				prc->getInputFields(), prc->getInputFormat()) :
			NULL;
		ExtMessageNode* extInMessageNode = NULL;

		if (intInMessageNode)
		{
			mainNode->statements.add(intInMessageNode);

			extInMessageNode = FB_NEW_POOL(csbPool) ExtMessageNode(tdbb, csbPool, csb, 2, extInputFormat);
			mainNode->statements.add(extInMessageNode);
		}

		IntMessageNode* intOutMessageNode = FB_NEW_POOL(csbPool) IntMessageNode(tdbb, csbPool, csb, 1,
			prc->getOutputFields(), prc->getOutputFormat());
		mainNode->statements.add(intOutMessageNode);

		ExtMessageNode* extOutMessageNode = FB_NEW_POOL(csbPool) ExtMessageNode(tdbb, csbPool,
			csb, 3, extOutputFormat);
		mainNode->statements.add(extOutMessageNode);

		// Initialize the output fields into the external message.
		InitParametersNode* initParametersNode = FB_NEW_POOL(csbPool) InitParametersNode(
			tdbb, csbPool, csb, prc->getOutputFields(), extOutMessageNode);
		mainNode->statements.add(initParametersNode);

		ReceiveNode* receiveNode = intInMessageNode ?
			FB_NEW_POOL(csbPool) ReceiveNode(csbPool) : NULL;

		if (intInMessageNode)
		{
			CompoundStmtNode* receiveSubStatement = FB_NEW_POOL(csbPool) CompoundStmtNode(csbPool);
			receiveSubStatement->statements.add(FB_NEW_POOL(csbPool) MessageMoverNode(
				csbPool, intInMessageNode, extInMessageNode));
			receiveSubStatement->statements.add(FB_NEW_POOL(csbPool) StallNode(csbPool));

			receiveNode->statement = receiveSubStatement;
			receiveNode->message = intInMessageNode;

			mainNode->statements.add(receiveNode);
		}
		else
			mainNode->statements.add(FB_NEW_POOL(csbPool) StallNode(csbPool));

		ExtProcedureNode* extProcedureNode = FB_NEW_POOL(csbPool) ExtProcedureNode(csbPool,
			extInMessageNode, extOutMessageNode, intOutMessageNode, prc->getExternal());
		mainNode->statements.add(extProcedureNode);

		Statement* statement = prc->getStatement();
		PAR_preparsed_node(tdbb, NULL, mainNode, NULL, &csb, &statement, false, 0);
		prc->setStatement(statement);
	}
	catch (...)
	{
		EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));
		externalProcedure->dispose();
		throw;
	}
}


void ExtEngineManager::makeTrigger(thread_db* tdbb, CompilerScratch* csb, Jrd::Trigger* trg,
	const MetaName& engine, const string& entryPoint, const string& body,
	unsigned type)
{
	string entryPointTrimmed = entryPoint;
	entryPointTrimmed.trim();

	EngineAttachmentInfo* attInfo = getEngineAttachment(tdbb, engine);
	const MetaString& userName = trg->ssDefiner.asBool() ? trg->owner.c_str() : "";
	ContextManager<IExternalTrigger> ctxManager(tdbb, attInfo, attInfo->adminCharSet,
		CallerName(obj_trigger, trg->name, userName));

	MemoryPool& pool = *tdbb->getAttachment()->att_pool;

	AutoPtr<RoutineMetadata> metadata(FB_NEW_POOL(pool) RoutineMetadata(pool));
	metadata->name = trg->name;
	metadata->entryPoint = entryPointTrimmed;
	metadata->body = body;
	metadata->triggerType = type;

	jrd_rel* relation = trg->relation;

	if (relation)
	{
		metadata->triggerTable = relation->rel_name;

		MsgMetadata* fieldsMsg = FB_NEW MsgMetadata;
		metadata->triggerFields = fieldsMsg;

		Format* relFormat = relation->rel_current_format;

		for (FB_SIZE_T i = 0; i < relation->rel_fields->count(); ++i)
		{
			jrd_fld* field = (*relation->rel_fields)[i];
			if (field)
			{
				dsc d(relFormat->fmt_desc[i]);
				fieldsMsg->addItem(field->fld_name, !field->fld_not_null, d);
			}
		}
	}

	FbLocalStatus status;

	RefPtr<IMetadataBuilder> fieldsBuilder(REF_NO_INCR, relation ?
		metadata->triggerFields->getBuilder(&status) : NULL);
	if (relation)
	{
		status.check();
	}

	IExternalTrigger* externalTrigger;

	{	// scope
		EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));

		FbLocalStatus status;
		externalTrigger = attInfo->engine->makeTrigger(&status, attInfo->context, metadata,
			fieldsBuilder);
		status.check();

		if (!externalTrigger)
		{
			status_exception::raise(
				Arg::Gds(isc_eem_trig_not_returned) << trg->name.toQuotedString() << engine);
		}

		if (relation)
		{
			metadata->triggerFields = fieldsBuilder->getMetadata(&status);
			status.check();
		}
	}

	try
	{
		const auto extTrigger = FB_NEW_POOL(pool) Trigger(tdbb, pool, csb, this, attInfo->engine,
			metadata.release(), externalTrigger, trg);

		trg->extTrigger.reset(extTrigger);

		MemoryPool& csbPool = csb->csb_pool;

		CompoundStmtNode* mainNode = FB_NEW_POOL(csbPool) CompoundStmtNode(csbPool);
		mainNode->statements.append(extTrigger->computedStatements);

		const auto extTriggerNode = FB_NEW_POOL(csbPool) ExtTriggerNode(csbPool, extTrigger);
		mainNode->statements.add(extTriggerNode);

		PAR_preparsed_node(tdbb, trg->relation, mainNode, NULL, &csb, &trg->statement, true, 0);
	}
	catch (...)
	{
		EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));
		externalTrigger->dispose();
		throw;
	}
}


IExternalEngine* ExtEngineManager::getEngine(thread_db* tdbb, const MetaName& name)
{
	ReadLockGuard readGuard(enginesLock, FB_FUNCTION);
	IExternalEngine* engine = NULL;

	if (!engines.get(name, engine))
	{
		readGuard.release();
		WriteLockGuard writeGuard(enginesLock, FB_FUNCTION);

		if (!engines.get(name, engine))
		{
			GetPlugins<IExternalEngine> engineControl(IPluginManager::TYPE_EXTERNAL_ENGINE, name.c_str());

			if (engineControl.hasData())
			{
				EngineAttachment key(NULL, NULL);
				AutoPtr<EngineAttachmentInfo> attInfo;

				try
				{
					EngineCheckout cout(tdbb, FB_FUNCTION);

					engine = engineControl.plugin();
					if (engine)
					{
						Attachment::SyncGuard attGuard(tdbb->getAttachment(), FB_FUNCTION);

						key = EngineAttachment(engine, tdbb->getAttachment());
						attInfo = FB_NEW_POOL(getPool()) EngineAttachmentInfo();
						attInfo->engine = engine;
						attInfo->context = FB_NEW_POOL(getPool()) ExternalContextImpl(tdbb, engine);

						setupAdminCharSet(tdbb, engine, attInfo);

						ContextManager<IExternalFunction> ctxManager(tdbb, attInfo, attInfo->adminCharSet);
						FbLocalStatus status;
						engine->openAttachment(&status, attInfo->context);	//// FIXME: log status
					}
				}
				catch (...)
				{
					if (engine)
					{
						PluginManagerInterfacePtr()->releasePlugin(engine);
					}

					throw;
				}

				if (engine)
				{
					engine->addRef();
					engines.put(name, engine);
					enginesAttachments.put(key, attInfo);
					attInfo.release();
				}
			}
		}
	}

	if (!engine)
	{
		status_exception::raise(Arg::Gds(isc_eem_engine_notfound) << name);
	}

	return engine;
}


ExtEngineManager::EngineAttachmentInfo* ExtEngineManager::getEngineAttachment(
	thread_db* tdbb, const MetaName& name)
{
	IExternalEngine* engine = getEngine(tdbb, name);
	return getEngineAttachment(tdbb, engine);
}


ExtEngineManager::EngineAttachmentInfo* ExtEngineManager::getEngineAttachment(
	thread_db* tdbb, IExternalEngine* engine, bool closing)
{
	EngineAttachment key(engine, tdbb->getAttachment());
	EngineAttachmentInfo* attInfo = NULL;

	ReadLockGuard readGuard(&enginesLock, FB_FUNCTION);

	if (!enginesAttachments.get(key, attInfo) && !closing)
	{
		readGuard.release();
		WriteLockGuard writeGuard(enginesLock, FB_FUNCTION);

		if (!enginesAttachments.get(key, attInfo))
		{
			attInfo = FB_NEW_POOL(getPool()) EngineAttachmentInfo();
			attInfo->engine = engine;
			attInfo->context = FB_NEW_POOL(getPool()) ExternalContextImpl(tdbb, engine);

			setupAdminCharSet(tdbb, engine, attInfo);

			enginesAttachments.put(key, attInfo);

			ContextManager<IExternalFunction> ctxManager(tdbb, attInfo, attInfo->adminCharSet);
			EngineCheckout cout(tdbb, FB_FUNCTION, checkoutType(attInfo->engine));
			FbLocalStatus status;
			engine->openAttachment(&status, attInfo->context);	//// FIXME: log status
		}

		return attInfo;
	}

	if (closing && attInfo)
	{
		readGuard.release();
		WriteLockGuard writeGuard(enginesLock, FB_FUNCTION);
		enginesAttachments.remove(key);
	}

	return attInfo;
}


void ExtEngineManager::setupAdminCharSet(thread_db* tdbb, IExternalEngine* engine,
	EngineAttachmentInfo* attInfo)
{
	ContextManager<IExternalFunction> ctxManager(tdbb, attInfo, CS_UTF8);

	QualifiedName charSetName;
	char charSetNameBuffer[MAX_QUALIFIED_NAME_TO_STRING_LEN] = DEFAULT_DB_CHARACTER_SET_NAME;

	FbLocalStatus status;
	engine->open(&status, attInfo->context, charSetNameBuffer, sizeof(charSetNameBuffer));
	status.check();

	charSetNameBuffer[sizeof(charSetNameBuffer) - 1] = '\0';

	if (charSetNameBuffer[0])
	{
		charSetName = QualifiedName::parseSchemaObject(charSetNameBuffer);
		tdbb->getAttachment()->qualifyExistingName(tdbb, charSetName, obj_charset);
	}

	if (!MET_get_char_coll_subtype(tdbb, &attInfo->adminCharSet, charSetName))
		status_exception::raise(Arg::Gds(isc_charset_not_found) << charSetName.toQuotedString());
}


//---------------------


static EngineCheckout::Type checkoutType(IExternalEngine* engine)
{
	return engine == SystemEngine::INSTANCE ? EngineCheckout::AVOID : EngineCheckout::REQUIRED;
}
