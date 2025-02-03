/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		btr_proto.h
 *	DESCRIPTION:	Prototype header file for btr.cpp
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

#ifndef JRD_BTR_PROTO_H
#define JRD_BTR_PROTO_H

#include "../jrd/btr.h"
#include "../jrd/ods.h"
#include "../jrd/req.h"
#include "../jrd/exe.h"

void	BTR_all(Jrd::thread_db*, Jrd::Cached::Relation*, Jrd::IndexDescList&, Jrd::RelationPages*);
void	BTR_activate_index(Jrd::thread_db*, Jrd::Cached::Relation*, MetaId);
void	BTR_complement_key(Jrd::temporary_key*);
void	BTR_create(Jrd::thread_db*, Jrd::IndexCreation&, Jrd::SelectivityList&);
bool	BTR_delete_index(Jrd::thread_db*, Jrd::win*, MetaId);
bool	BTR_description(Jrd::thread_db*, Jrd::Cached::Relation*, const Ods::index_root_page*, Jrd::index_desc*, MetaId);
DSC*	BTR_eval_expression(Jrd::thread_db*, Jrd::index_desc*, Jrd::Record*);
void	BTR_evaluate(Jrd::thread_db*, const Jrd::IndexRetrieval*, Jrd::RecordBitmap**, Jrd::RecordBitmap*);
UCHAR*	BTR_find_leaf(Ods::btree_page*, Jrd::temporary_key*, UCHAR*, USHORT*, bool, int);
Ods::btree_page*	BTR_find_page(Jrd::thread_db*, const Jrd::IndexRetrieval*, Jrd::win*, Jrd::index_desc*,
	Jrd::temporary_key*, Jrd::temporary_key*);
void	BTR_insert(Jrd::thread_db*, Jrd::win*, Jrd::index_insertion*);
USHORT	BTR_key_length(Jrd::thread_db*, Jrd::jrd_rel*, Jrd::index_desc*);
Ods::btree_page*	BTR_left_handoff(Jrd::thread_db*, Jrd::win*, Ods::btree_page*, SSHORT);
bool	BTR_lookup(Jrd::thread_db*, Jrd::Cached::Relation*, MetaId, Jrd::index_desc*, Jrd::RelationPages*);
bool	BTR_make_bounds(Jrd::thread_db*, const Jrd::IndexRetrieval*, Jrd::IndexScanListIterator*,
						Jrd::temporary_key*, Jrd::temporary_key*, USHORT&);
Jrd::idx_e	BTR_make_key(Jrd::thread_db*, USHORT, const Jrd::ValueExprNode* const*, const SSHORT*,
						 const Jrd::index_desc*, Jrd::temporary_key*, USHORT, bool*);
void	BTR_make_null_key(Jrd::thread_db*, const Jrd::index_desc*, Jrd::temporary_key*);
void	BTR_mark_index_for_delete(Jrd::thread_db* tdbb, Jrd::Cached::Relation*, MetaId id);
bool	BTR_next_index(Jrd::thread_db*, Jrd::Cached::Relation*, Jrd::jrd_tra*, Jrd::index_desc*, Jrd::win*);
void	BTR_remove(Jrd::thread_db*, Jrd::win*, Jrd::index_insertion*);
void	BTR_reserve_slot(Jrd::thread_db*, Jrd::IndexCreation&, Jrd::IndexCreateLock&);
void	BTR_selectivity(Jrd::thread_db*, Jrd::Cached::Relation*, MetaId, Jrd::SelectivityList&);
bool	BTR_types_comparable(const dsc& target, const dsc& source);
Ods::index_root_page* BTR_fetch_root_for_update(const char* from, Jrd::thread_db* tdbb, Jrd::win* window);
const Ods::index_root_page* BTR_fetch_root(const char* from, Jrd::thread_db* tdbb, Jrd::win* window);

#endif // JRD_BTR_PROTO_H
