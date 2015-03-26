/*
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
 * Stephen W. Boyd						- Added support for new features.
 */

enum kwwords_t {
	KW_none = 0,
	KW_start_actions,
	KW_ACTIVE,
	KW_ANY,
	KW_AT,
	KW_BACK_SLASH, // ???
	KW_BASED,
	KW_BEGIN,
	KW_BUFFERS,
	KW_CACHE,
	KW_CANCEL_BLOB,
	KW_CASE,
	KW_CHAR,
	KW_CLEAR_HANDLES,
	KW_CLOSE_BLOB,
	KW_COLLATE,
	KW_COMMIT,
	KW_CONSTRAINT,
	KW_CREATE_BLOB,
	KW_DATABASE,
	KW_DERIVED_FROM,
	KW_DOMAIN,
	KW_DOUBLE,
	KW_ELEMENT,
	KW_ELSE,
	KW_END,
	KW_END_ERROR,
	KW_END_FETCH,
	KW_END_FOR,
	KW_END_MODIFY,
	KW_END_STORE,
	KW_END_STORE_SPECIAL,
	KW_END_STREAM,
	KW_ERASE,
	KW_ESCAPE,
	KW_EVENT_INIT,
	KW_EVENT_WAIT,
	KW_EXEC,
	KW_EXTERNAL,
	KW_EXTRACT,
	KW_FETCH,
	KW_FINISH,
	KW_FLOAT,
	KW_FOR,
	KW_FUNCTION,
	KW_GET_SEGMENT,
	KW_GET_SLICE,
	KW_INACTIVE,
	KW_INT,
	KW_KEY,
	KW_LONG,
	KW_L_BRACE,
	KW_MODIFY,
	KW_MONTH,
	KW_NAMESPACE,
	KW_NATIONAL,
	KW_NCHAR,
	KW_ON,
	KW_ON_ERROR,
	KW_OPEN_BLOB,
	KW_PREPARE,
	KW_PROC,
	KW_PROCEDURE,
	KW_PUT_SEGMENT,
	KW_PUT_SLICE,
	KW_READY,
	KW_RELEASE,
	KW_RELEASE_REQUESTS,
	KW_RETURNING,
	KW_RETURNING_VALUES,
	KW_ROLE,
	KW_ROLLBACK,
	KW_R_BRACE,
	KW_SAVE,
	KW_SHORT,
	KW_START_STREAM,
	KW_START_TRANSACTION,
	KW_STATISTICS,
	KW_STORE,
	KW_SUB, KW_SUBROUTINE,
	//KW_SUSPEND_WINDOW,
	KW_end_actions,
	KW_ABNORMAL,
#ifdef SCROLLABLE_CURSORS
	KW_ABSOLUTE,
#endif
	KW_ACCEPTING, // ???
	KW_ACTION, KW_ADD, KW_ALL,
	KW_ALLOCATION,  // ???
	KW_ALTER, KW_AMPERSAND, KW_AND, KW_ANYCASE,
	KW_ARE,	// SQL: NAMES ARE
	KW_AS,
	KW_ASCENDING,
	KW_ASTERISK,
	KW_AUTO,
	KW_AUTOCOMMIT,
	KW_AVERAGE,
	KW_BASE_NAME,
	KW_BETWEEN,
	KW_BLOB,
	KW_BUFFERCOUNT, // ???
	KW_BUFFERSIZE, // ???
	KW_BY,
	KW_CARAT,
	KW_CASCADE,
	KW_CAST,
	KW_CHECK,
	KW_CHECK_POINT_LEN,
	KW_CLOSE,
	KW_COLON,
	KW_COMMA,
	KW_COMMENT,
	KW_COMMITTED,
	KW_COMPILETIME,
	KW_COMPUTED,
	KW_CONCURRENCY,
	KW_CONDITIONAL,
	KW_CONNECT,
	KW_CONSISTENCY,
	KW_CONTAINING,
	KW_CONTINUE,
	KW_COUNT,
	KW_CREATE,
	KW_CROSS,
	KW_CSTRING,
	KW_CURRENT,
	KW_CURRENT_DATE,
	KW_CURRENT_TIME,
	KW_CURRENT_TIMESTAMP,
	KW_CURSOR,
	KW_DATE,
	KW_DAY,
	KW_DBA, // ???
	KW_DBKEY,
	KW_DEC,
	KW_DECIMAL,
	KW_DECLARE,
	KW_DEFAULT,
	KW_DELETE,
	KW_DESCENDING,
	KW_DESCRIBE,
	KW_DESCRIPTOR,
	KW_DIALECT,
	KW_DISCONNECT,
	KW_DISTINCT,
	KW_DOT,
	KW_DOT_DOT, // ???
	KW_DROP,
	KW_END_EXEC,
	KW_ENTRY_POINT,
	KW_EQ,
	KW_EQUALS,
	KW_EQUIV,
	KW_ERROR,
	KW_EVENT,
	KW_EXACTCASE,
	KW_EXCLUSIVE,
	KW_EXECUTE,
	KW_EXISTS,
	KW_EXTERN,
	KW_FILE,
	KW_FILENAME,
	KW_FILTER,
	KW_FIRST,
	KW_FOREIGN,
	KW_FORWARD,
	KW_FOUND,
	KW_FROM,
	KW_FULL,
	KW_GE,
	KW_GEN_ID,
	KW_GENERATOR,
	KW_GLOBAL, // ???
	KW_GO,
	KW_GOTO,
	KW_GRANT,
	KW_GROUP,
	KW_GROUP_COMMIT_WAIT,
	KW_GT,
	KW_HANDLES,
	KW_HAVING,
	//KW_HEIGHT,
	//KW_HORIZONTAL,
	KW_HOUR,
	KW_IMMEDIATE,
	KW_IN,
	KW_INC,
	KW_INCLUDE,
	KW_INDEX,
	KW_INDICATOR,
	KW_INIT,
	KW_INNER,
	KW_INPUT,
	KW_INPUT_TYPE,
	KW_INSERT,
	KW_INTEGER, KW_INTERNAL, KW_INTO, KW_IS, KW_ISOLATION, KW_JOIN,
#ifdef SCROLLABLE_CURSORS
	KW_LAST,
#endif
	KW_LC_CTYPE,
	KW_LC_MESSAGES,
	KW_LE,
	KW_LEFT,
	KW_LEFT_PAREN,
	KW_LENGTH,
	KW_LEVEL,
	KW_LIKE,
	KW_LOCK,
	KW_LOG_BUF_SIZE,
	KW_LOG_FILE,
	KW_LT,
	KW_L_BRCKET,
	KW_MAIN,
	KW_MANUAL,
	KW_MATCHES,
	KW_MAX,
	KW_MAX_SEGMENT,
	//KW_MENU_HANDLE,
	KW_MERGE,
	KW_MIN,
	KW_MINUTE,
	KW_MINUS,
	KW_MISSING, KW_MODULE_NAME, KW_NAME, KW_NAMES, KW_NATURAL, KW_NE,
#ifdef SCROLLABLE_CURSORS
	KW_NEXT,
#endif
	KW_NO,
	KW_NOT,
	KW_NO_AUTO_UNDO,
	KW_NO_WAIT,
	KW_NULL,
	KW_NUMERIC,
	KW_NUM_LOG_BUFS,
	KW_OF,
	KW_ONLY,
	KW_OPAQUE, // ???
	KW_OPEN,
	KW_OPTION,
	KW_OPTIONS,
	KW_OR,
	KW_OR1,
	KW_ORDER,
	KW_OUTER,
	KW_OUTPUT,
	KW_OUTPUT_TYPE,
	KW_OVER,
	KW_OVERFLOW,
	KW_OVERRIDING, // ???
	KW_PAGE,
	KW_PAGES,
	KW_PAGESIZE,
	KW_PAGE_SIZE,
	KW_PARAMETER,
	KW_PASSWORD,
	KW_PATHNAME, // ???
	KW_PLAN, KW_PLUS, KW_POINTS, KW_PRECISION, KW_PRIMARY,
#ifdef SCROLLABLE_CURSORS
	KW_PRIOR,
#endif
	KW_PRIVILEGES,
	KW_PROTECTED,
	KW_PUBLIC,
	KW_QUAD,
	KW_RAW_PARTITIONS, // ???
	KW_READ,
	KW_READ_COMMITTED,
	KW_READ_ONLY, KW_READ_WRITE, KW_REAL, KW_REDUCED, KW_REFERENCES,
#ifdef SCROLLABLE_CURSORS
	KW_RELATIVE,
#endif
	KW_REM,
	KW_REQUEST_HANDLE,
	KW_RESERVING,
	KW_RESOURCE, // ???
	KW_RESTRICT,
	KW_RETAIN,
	KW_RETURNS,
	KW_REVOKE,
	KW_RIGHT,
	KW_RIGHT_PAREN,
	KW_ROUTINE_PTR,
	KW_RUN, // ???
	KW_RUNTIME, KW_R_BRCKET, KW_SCALE,
	KW_SCHEDULE, // ???
	KW_SCHEMA,
#ifdef SCROLLABLE_CURSORS
	KW_SCROLL,
#endif
	KW_SECOND, KW_SECTION, KW_SEGMENT, KW_SELECT, KW_SEMI_COLON, KW_SET, KW_SHARED, KW_SHADOW,
	KW_SINGULAR, KW_SIZE, KW_SLASH, KW_SMALLINT, KW_SNAPSHOT, KW_SORT, KW_SORTED,
	KW_SQL, KW_SQLERROR, KW_SQLWARNING, KW_STABILITY, KW_STARTING, KW_STARTING_WITH, KW_STARTS,
	KW_STATE, // ???
	KW_STATEMENT, KW_STATIC, KW_STOGROUP, KW_STREAM, KW_STRING, KW_SUB_TYPE, KW_SUM,
	KW_SYNONYM, KW_TABLE, KW_TABLESPACE,
	KW_TAG, // ???
	KW_TERMINATING_FIELD, // ???
	KW_TERMINATOR, // ???
	KW_TIME, KW_TIMESTAMP,
	KW_TITLE_LENGTH, // ???
	KW_TITLE_TEXT,  // ???
	KW_TO, KW_TOTAL, KW_TRANSACTION, KW_TRANSACTION_HANDLE,
	KW_TRANSPARENT, // ???
	KW_TRIGGER, KW_UNCOMMITTED, KW_UNION, KW_UNIQUE, KW_UPDATE,
	KW_UPPER,	// SQL UPPER operation
	KW_UPPERCASE,
	KW_LOWER, KW_LOWERCASE,
	KW_USER,
	KW_USERS,
	KW_USER_NAME,
	KW_USING,
	KW_VALUE,
	KW_VALUES,
	KW_VAL_PARAM,
	KW_VARCHAR,
	KW_VARIABLE,
	KW_VARYING,
	KW_VERSION,
	//KW_VERTICAL,
	KW_VIEW,
	KW_WAIT,
	KW_WAKING, // ???
	KW_WARNING, // ???
	KW_WEEKDAY,
	KW_WHENEVER,
	//KW_WIDTH,
	KW_WITH, KW_WORK, KW_WRITE, KW_YEAR, KW_YEARDAY,
	KW_NULLIF,
	KW_SKIP,
	KW_CURRENT_CONNECTION,
	KW_CURRENT_ROLE,
	KW_CURRENT_TRANSACTION,
	KW_CURRENT_USER,
	KW_COALESCE,
	KW_WHEN,
	KW_THEN,
	KW_SUBSTRING,
	KW_max
};
