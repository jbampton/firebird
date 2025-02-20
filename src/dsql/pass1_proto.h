/*
 *	PROGRAM:	Dynamic SQL runtime support
 *	MODULE:		pass1_proto.h
 *	DESCRIPTION:	Prototype Header file for pass1.cpp
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

#ifndef DSQL_PASS1_PROTO_H
#define DSQL_PASS1_PROTO_H

namespace Jrd
{
	class CompoundStmtNode;
	class DeclareCursorNode;
	class DsqlMapNode;
	class ExprNode;
	class RecordSourceNode;
	class RseNode;
	class SelectExprNode;
	class SelectNode;
	class ValueExprNode;
	class ValueListNode;
}

void PASS1_ambiguity_check(Jrd::DsqlCompilerScratch*, const Jrd::MetaName&, const Jrd::DsqlContextStack&);
bool PASS1_compare_alias(const Jrd::QualifiedName& contextAlias, const Jrd::QualifiedName& lookupAlias);
bool PASS1_compare_alias(const Firebird::ObjectsArray<Jrd::QualifiedName>& contextAlias,
	const Firebird::ObjectsArray<Jrd::QualifiedName>& lookupAlias);
Jrd::BoolExprNode* PASS1_compose(Jrd::BoolExprNode*, Jrd::BoolExprNode*, UCHAR);
Jrd::DeclareCursorNode* PASS1_cursor_name(Jrd::DsqlCompilerScratch*, const Jrd::MetaName&, USHORT, bool);
Jrd::RseNode* PASS1_derived_table(Jrd::DsqlCompilerScratch*, Jrd::SelectExprNode*, const char*,
	const Jrd::SelectNode* = nullptr);
void PASS1_expand_contexts(Jrd::DsqlContextStack& contexts, Jrd::dsql_ctx* context);
Jrd::ValueListNode* PASS1_expand_select_list(Jrd::DsqlCompilerScratch*, Jrd::ValueListNode*, Jrd::RecSourceListNode*);
void PASS1_expand_select_node(Jrd::DsqlCompilerScratch*, Jrd::ExprNode*, Jrd::ValueListNode*, bool);
void PASS1_field_unknown(const TEXT*, const TEXT*, const Jrd::ExprNode*);
void PASS1_limit(Jrd::DsqlCompilerScratch*, NestConst<Jrd::ValueExprNode>,
	NestConst<Jrd::ValueExprNode>, Jrd::RseNode*);
Jrd::ValueExprNode* PASS1_lookup_alias(Jrd::DsqlCompilerScratch*, const Jrd::MetaName&,
	Jrd::ValueListNode*, bool);
Jrd::dsql_ctx* PASS1_make_context(Jrd::DsqlCompilerScratch* statement, Jrd::RecordSourceNode* relationNode);
bool PASS1_node_match(Jrd::DsqlCompilerScratch*, const Jrd::ExprNode*, const Jrd::ExprNode*, bool);
Jrd::DsqlMapNode* PASS1_post_map(Jrd::DsqlCompilerScratch*, Jrd::ValueExprNode*, Jrd::dsql_ctx*,
	Jrd::WindowClause*);
Jrd::RecordSourceNode* PASS1_relation(Jrd::DsqlCompilerScratch*, Jrd::RecordSourceNode*);
Jrd::RseNode* PASS1_rse(Jrd::DsqlCompilerScratch*, Jrd::SelectExprNode*, const Jrd::SelectNode* = nullptr);
bool PASS1_set_parameter_type(Jrd::DsqlCompilerScratch*, Jrd::ValueExprNode*, std::function<void (dsc*)>, bool);
bool PASS1_set_parameter_type(Jrd::DsqlCompilerScratch*, Jrd::ValueExprNode*, NestConst<Jrd::ValueExprNode>, bool);
Jrd::ValueListNode* PASS1_sort(Jrd::DsqlCompilerScratch*, Jrd::ValueListNode*, Jrd::ValueListNode*);

#endif // DSQL_PASS1_PROTO_H
