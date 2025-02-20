/*
 *	PROGRAM:	Dynamic SQL runtime support
 *	MODULE:		ddl.cpp
 *	DESCRIPTION:	Utilities for generating ddl
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
 * 2001.5.20 Claudio Valderrama: Stop null pointer that leads to a crash,
 * caused by incomplete yacc syntax that allows ALTER DOMAIN dom SET;
 *
 * 2001.07.06 Sean Leyne - Code Cleanup, removed "#ifdef READONLY_DATABASE"
 *                         conditionals, as the engine now fully supports
 *                         readonly databases.
 * December 2001 Mike Nordell - Attempt to make it C++
 *
 * 2001.5.20 Claudio Valderrama: Stop null pointer that leads to a crash,
 * caused by incomplete yacc syntax that allows ALTER DOMAIN dom SET;
 * 2001.5.29 Claudio Valderrama: Check for view v/s relation in DROP
 * command will stop a user that uses DROP VIEW and drops a table by
 * accident and vice-versa.
 * 2001.5.30 Claudio Valderrama: alter column should use 1..N for the
 * position argument since the call comes from SQL DDL.
 * 2001.6.27 Claudio Valderrama: DDL_resolve_intl_type() was adding 2 to the
 * length of varchars instead of just checking that len+2<=MAX_COLUMN_SIZE.
 * It required a minor change to put_field() where it was decremented, too.
 * 2001.6.27 Claudio Valderrama: Finally stop users from invoking the same option
 * several times when altering a domain. Specially dangerous with text data types.
 * Ex: alter domain d type char(5) type varchar(5) default 'x' default 'y';
 * Bear in mind that if DYN functions are addressed directly, this protection
 * becomes a moot point.
 * 2001.6.30 Claudio Valderrama: revert changes from 2001.6.26 because the code
 * is called from several places and there are more functions, even in metd.c,
 * playing the same nonsense game with the field's length, so it needs more
 * careful examination. For now, the new checks in DYN_MOD should catch most anomalies.
 * 2001.7.3 Claudio Valderrama: fix Firebird Bug #223059 with mismatch between number
 * of declared fields for a VIEW and effective fields in the SELECT statement.
 * 2001.07.22 Claudio Valderrama: minor fixes and improvements.
 * 2001.08.18 Claudio Valderrama: RECREATE PROCEDURE.
 * 2001.10.01 Claudio Valderrama: modify_privilege() should recognize that a ROLE can
 *   now be made an explicit grantee.
 * 2001.10.08 Claudio Valderrama: implement fb_sysflag enum values for autogenerated
 *   non-system triggers so DFW can recognize them easily.
 * 2001.10.26 Claudio Valderrama: added a call to the new METD_drop_function()
 *   in DDL_execute() so the metadata cache for udfs can be refreshed.
 * 2001.12.06 Claudio Valderrama: DDL_resolve_intl_type should calculate field length
 * 2002.08.04 Claudio Valderrama: allow declaring and defining variables at the same time
 * 2002.08.04 Dmitry Yemanov: ALTER VIEW
 * 2002.08.31 Dmitry Yemanov: allowed user-defined index names for PK/FK/UK constraints
 * 2002.09.01 Dmitry Yemanov: RECREATE VIEW
 * 2002.09.12 Nickolay Samofatov: fixed cached metadata errors
 * 2004.01.16 Vlad Horsun: added support for default parameters and
 *   EXECUTE BLOCK statement
 * Adriano dos Santos Fernandes
 */

#include "firebird.h"
#include "dyn_consts.h"
#include <stdio.h>
#include <string.h>
#include "../jrd/SysFunction.h"
#include "../jrd/MetaName.h"
#include "../dsql/dsql.h"
#include "../dsql/ExprNodes.h"
#include "ibase.h"
#include "../jrd/Attachment.h"
#include "../jrd/RecordSourceNodes.h"
#include "../jrd/intl.h"
#include "../jrd/intl_classes.h"
#include "../jrd/jrd.h"
#include "../jrd/flags.h"
#include "../jrd/constants.h"
#include "../dsql/errd_proto.h"
#include "../dsql/ddl_proto.h"
#include "../dsql/gen_proto.h"
#include "../dsql/make_proto.h"
#include "../dsql/metd_proto.h"
#include "../dsql/pass1_proto.h"
#include "../dsql/utld_proto.h"
#include "../jrd/intl_proto.h"
#include "../jrd/met_proto.h"
#include "../yvalve/gds_proto.h"
#include "../jrd/jrd_proto.h"
#include "../jrd/vio_proto.h"
#include "../yvalve/why_proto.h"
#include "../common/utils_proto.h"
#include "../dsql/DdlNodes.h"
#include "../dsql/DSqlDataTypeUtil.h"
#include "../common/StatusArg.h"

#ifdef DSQL_DEBUG
#include "../common/prett_proto.h"
#endif

using namespace Jrd;
using namespace Firebird;


static void assign_field_length(dsql_fld*, USHORT);
static void post_607(const Arg::StatusVector& v);


///const int DEFAULT_BLOB_SEGMENT_SIZE = 80; // bytes


// Determine whether ids or names should be referenced when generating blr for fields and relations.
bool DDL_ids(const DsqlCompilerScratch* scratch)
{
	return !(scratch->flags & DsqlCompilerScratch::FLAG_DDL);
}


void DDL_resolve_intl_type(DsqlCompilerScratch* dsqlScratch, dsql_fld* field,
	QualifiedName& collation_name, bool modifying)
{
/**************************************
 *
 *  D D L _ r e s o l v e _ i n t l _ t y p e
 *
 **************************************
 *
 * Function

 *	If the field is defined with a character set or collation,
 *	resolve the names to a subtype now.
 *
 *	Also resolve the field length & whatnot.
 *
 *  If the field is being created, it will pick the db-wide charset
 *  and collation if not specified. If the field is being modified,
 *  since we don't allow changes to those attributes, we'll go and
 *  calculate the correct old length from the field itself so DYN
 *  can validate the change properly.
 *
 *	For International text fields, this is a good time to calculate
 *	their actual size - when declared they were declared in
 *	lengths of CHARACTERs, not BYTES.
 *
 **************************************/

	if (field->typeOfName.object.hasData())
	{
		if (field->typeOfTable.object.hasData())
		{
			dsqlScratch->qualifyExistingName(field->typeOfTable, obj_relation);

			dsql_rel* relation = METD_get_relation(dsqlScratch->getTransaction(), dsqlScratch, field->typeOfTable);
			const dsql_fld* fld = NULL;

			if (relation)
			{
				const MetaName fieldName(field->typeOfName.object);

				for (fld = relation->rel_fields; fld; fld = fld->fld_next)
				{
					if (fieldName == fld->fld_name)
					{
						field->dimensions = fld->dimensions;
						field->fieldSource = fld->fieldSource;
						field->length = fld->length;
						field->scale = fld->scale;
						field->subType = fld->subType;
						field->charSetId = fld->charSetId;
						field->collationId = fld->collationId;
						field->charLength = fld->charLength;
						field->flags = fld->flags;
						field->dtype = fld->dtype;
						field->segLength = fld->segLength;
						break;
					}
				}
			}

			if (!fld)
			{
				// column @1 does not exist in table/view @2
				post_607(Arg::Gds(isc_dyn_column_does_not_exist) <<
						 		field->typeOfName.toQuotedString() <<
								field->typeOfTable.toQuotedString());
			}
		}
		else
		{
			dsqlScratch->qualifyExistingName(field->typeOfName, obj_field);

			if (!METD_get_domain(dsqlScratch->getTransaction(), field, field->typeOfName))
			{
				// Specified domain or source field does not exist
				post_607(Arg::Gds(isc_dsql_domain_not_found) << field->typeOfName.toQuotedString());
			}
		}

		if (field->dimensions != 0)
		{
			ERRD_post(Arg::Gds(isc_wish_list) <<
				Arg::Gds(isc_random) <<
				Arg::Str("Usage of domain or TYPE OF COLUMN of array type in PSQL"));
		}

		if (field->dtype <= dtype_any_text ||
			(field->dtype == dtype_blob && field->subType == isc_blob_text))
		{
			field->charSet = METD_get_charset_name(dsqlScratch->getTransaction(), field->charSetId.value_or(CS_NONE));
		}
	}

	if ((field->dtype > dtype_any_text) && field->dtype != dtype_blob)
	{
		if (field->charSet.object.hasData() || collation_name.object.hasData() || (field->flags & FLD_national))
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
					  Arg::Gds(isc_dsql_datatype_err) << Arg::Gds(isc_collation_requires_text));
		}
		return;
	}

	if (field->dtype == dtype_blob)
	{
		if (field->subTypeName.hasData())
		{
			SSHORT blob_sub_type;
			if (!METD_get_type(dsqlScratch->getTransaction(), field->subTypeName,
					"RDB$FIELD_SUB_TYPE", &blob_sub_type))
			{
				ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
						  Arg::Gds(isc_dsql_datatype_err) <<
						  Arg::Gds(isc_dsql_blob_type_unknown) <<
						  field->subTypeName);
			}
			field->subType = blob_sub_type;
		}

		if (field->subType > isc_blob_text)
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
					  Arg::Gds(isc_dsql_datatype_err) <<
					  Arg::Gds(isc_subtype_for_internal_use));
		}

		if (field->charSet.object.hasData() && (field->subType == isc_blob_untyped))
			field->subType = isc_blob_text;

		if (field->charSet.object.hasData() && (field->subType != isc_blob_text))
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
					  Arg::Gds(isc_dsql_datatype_err) <<
                      Arg::Gds(isc_collation_requires_text));
		}

		if (collation_name.object.hasData() && (field->subType != isc_blob_text))
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
					  Arg::Gds(isc_dsql_datatype_err) <<
                      Arg::Gds(isc_collation_requires_text));
		}

		if (field->subType != isc_blob_text)
			return;
	}

	if (field->charSetId.has_value() && collation_name.object.isEmpty())
	{
		// This field has already been resolved once, and the collation
		// hasn't changed.  Therefore, no need to do it again.
		return;
	}

	if (modifying && field->charSet.object.isEmpty() && field->collate.object.isEmpty())
	{
		// Use charset and collation from already existing field if any
		const dsql_fld* afield = field->fld_next;
		USHORT bpc = 0;

		while (afield)
		{
			// The first test is redundant.
			if (afield != field && afield->fld_relation && afield->fld_name == field->fld_name)
			{
				fb_assert(afield->fld_relation == dsqlScratch->relation || !dsqlScratch->relation);
				break;
			}

			afield = afield->fld_next;
		}

		if (afield)
		{
			field->charSetId = afield->charSetId;
			bpc = METD_get_charset_bpc(dsqlScratch->getTransaction(), field->charSetId.value_or(CS_NONE));
			field->collationId = afield->collationId;
			field->textType = afield->textType;

			if (afield->flags & FLD_national)
				field->flags |= FLD_national;
			else
				field->flags &= ~FLD_national;

			assign_field_length(field, bpc);
			return;
		}
	}

	if (!modifying && !(field->charSet.object.hasData() || field->charSetId.has_value() ||	// set if a domain
		(field->flags & FLD_national)))
	{
		// Attach the database default character set to the new field, if not otherwise specified

		QualifiedName defaultCharSet;

		if (dsqlScratch->ddlSchema.hasData())
			defaultCharSet = METD_get_schema_charset(dsqlScratch->getTransaction(), dsqlScratch->ddlSchema);
		else if (dsqlScratch->flags & DsqlCompilerScratch::FLAG_DDL)
			defaultCharSet = METD_get_database_charset(dsqlScratch->getTransaction());
		else
		{
			USHORT charSet = dsqlScratch->getAttachment()->dbb_attachment->att_charset;
			if (charSet != CS_NONE)
				defaultCharSet = METD_get_charset_name(dsqlScratch->getTransaction(), charSet);
		}

		if (defaultCharSet.object.hasData())
			field->charSet = defaultCharSet;
		else
		{
			// If field is not specified with NATIONAL, or CHARACTER SET
			// treat it as a single-byte-per-character field of character set NONE.
			assign_field_length(field, 1);
			field->textType = 0;

			if (collation_name.object.isEmpty())
				return;
		}
	}

	QualifiedName charset_name;

	if (field->flags & FLD_national)
		charset_name = QualifiedName(NATIONAL_CHARACTER_SET, SYSTEM_SCHEMA);
	else if (field->charSet.object.hasData())
	{
		dsqlScratch->qualifyExistingName(field->charSet, obj_charset);
		charset_name = field->charSet;
	}

	// Find an intlsym for any specified character set name & collation name
	const dsql_intlsym* resolved_type = NULL;

	if (charset_name.object.hasData())
	{
		const dsql_intlsym* resolved_charset = METD_get_charset(dsqlScratch->getTransaction(), charset_name);

		// Error code -204 (IBM's DB2 manual) is close enough
		if (!resolved_charset)
		{
			// specified character set not found
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
					  Arg::Gds(isc_dsql_datatype_err) <<
                      Arg::Gds(isc_charset_not_found) << charset_name.toQuotedString());
		}

		field->charSetId = resolved_charset->intlsym_charset_id;
		resolved_type = resolved_charset;
	}

	if (collation_name.object.hasData())
	{
		dsqlScratch->qualifyExistingName(collation_name, obj_collation);

		const dsql_intlsym* resolved_collation = METD_get_collation(dsqlScratch->getTransaction(),
			collation_name, field->charSetId.value_or(CS_NONE));

		if (!resolved_collation)
		{
			QualifiedName charSetName;

			if (charset_name.object.hasData())
				charSetName = charset_name;
			else
			{
				charSetName = METD_get_charset_name(dsqlScratch->getTransaction(),
					field->charSetId.value_or(CS_NONE));
			}

			// Specified collation not found
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
					  ///Arg::Gds(isc_dsql_datatype_err) <<	// (too large status vector)
                      Arg::Gds(isc_collation_not_found) << collation_name.toQuotedString() << charSetName.toQuotedString());
		}

		// If both specified, must be for same character set
		// A "literal constant" must be handled (charset as ttype_dynamic)

		resolved_type = resolved_collation;

		if ((field->charSetId.value_or(CS_NONE) != resolved_type->intlsym_charset_id) &&
			(field->charSetId.value_or(CS_NONE) != ttype_dynamic))
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
					  Arg::Gds(isc_dsql_datatype_err) <<
                      Arg::Gds(isc_collation_not_for_charset) << collation_name.toQuotedString());
		}

		field->explicitCollation = true;
	}

	assign_field_length(field, resolved_type->intlsym_bytes_per_char);

	field->textType = resolved_type->intlsym_ttype;
	field->charSetId = resolved_type->intlsym_charset_id;
	field->collationId = resolved_type->intlsym_collate_id;
}


static void assign_field_length(dsql_fld* field, USHORT bytes_per_char)
{
/**************************************
 *
 *  a s s i g n _ f i e l d _ l e n g t h
 *
 **************************************
 *
 * Function
 *  We'll see if the field's length fits in the maximum
 *  allowed field, including charset and space for varchars.
 *  Either we raise an error or assign the field's length.
 *  If the charlen comes as zero, we do nothing, although we
 *  know that DYN, MET and DFW will blindly set field length
 *  to zero if they don't catch charlen or another condition.
 *
 **************************************/

	if (field->charLength)
	{
		ULONG field_length = (ULONG) bytes_per_char * field->charLength;

		if (field->dtype == dtype_varying)
			field_length += sizeof(USHORT);

		if (field_length > MAX_COLUMN_SIZE)
		{
			ERRD_post(Arg::Gds(isc_sqlerr) << Arg::Num(-204) <<
					  Arg::Gds(isc_dsql_datatype_err) <<
                      Arg::Gds(isc_imp_exc) <<
					  Arg::Gds(isc_field_name) << Arg::Str(field->fld_name));
		}

		field->length = (USHORT) field_length;
	}

}


// post very often used error - avoid code duplication
static void post_607(const Arg::StatusVector& v)
{
	Arg::Gds err(isc_sqlerr);
	err << Arg::Num(-607) << Arg::Gds(isc_dsql_command_err);

	err.append(v);
	ERRD_post(err);
}
