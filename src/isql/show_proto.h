/*
 *	PROGRAM:	Interactive SQL utility
 *	MODULE:		show_proto.h
 *	DESCRIPTION:	Prototype header file for show.epp
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

#ifndef ISQL_SHOW_PROTO_H
#define ISQL_SHOW_PROTO_H

#include "../common/classes/fb_string.h"
#include "../common/classes/QualifiedMetaString.h"
#include <firebird/Interface.h>
#include "../isql/FrontendParser.h"
#include "../jrd/obj.h"
#include <optional>

void	SHOW_comments(bool force);
void	SHOW_dbb_parameters (Firebird::IAttachment*, const UCHAR*, unsigned, bool, const char*);
processing_state	SHOW_ddl_grants(const std::optional<Firebird::QualifiedMetaString>&,
	const SCHAR*, ObjectType, const TEXT*);
processing_state	SHOW_grants(const std::optional<Firebird::QualifiedMetaString>&,
	const SCHAR*, ObjectType, const TEXT*);
void	SHOW_grant_roles(const SCHAR*, bool*, const TEXT*);
void	SHOW_print_metadata_text_blob(FILE*, ISC_QUAD*, bool escape_squote = false,
	bool avoid_end_in_single_line_comment = false);
processing_state	SHOW_metadata(const FrontendParser::AnyShowNode& node);
void	SHOW_read_owner();
const Firebird::string SHOW_trigger_action(SINT64);
processing_state	SHOW_maps(bool extract, const std::optional<Firebird::MetaString>& name);
bool	SHOW_system_privileges(const Firebird::MetaString& name, const char* prfx, bool lf);

#endif // ISQL_SHOW_PROTO_H
