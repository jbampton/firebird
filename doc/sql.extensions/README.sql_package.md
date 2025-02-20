# RDB$SQL package (FB 6.0)

`RDB$SQL` is a package with utility routines to work with dynamic SQL.

## Procedure `EXPLAIN`

`RDB$SQL.EXPLAIN` returns tabular information of a query's plan, without execute the query.

Since `SQL` text generally is multi-line string and have quotes, you may use `<alternate string literal>`
(strings prefixed by `Q`) as a way to make escape easy.

Input parameters:
- `SQL` type `BLOB SUB_TYPE TEXT CHARACTER SET UTF8 NOT NULL` - query statement

Output parameters:
- `PLAN_LINE` type `INTEGER NOT NULL` - plan's line order
- `RECORD_SOURCE_ID` type `BIGINT NOT NULL` - record source id
- `PARENT_RECORD_SOURCE_ID` type `BIGINT` - parent record source id
- `LEVEL` type `INTEGER NOT NULL` - indentation level (may have gaps in relation to parent's level)
- `SCHEMA_NAME` type `RDB$SCHEMA_NAME` - schema name of a stored procedure
- `PACKAGE_NAME` type `RDB$PACKAGE_NAME` - package name of a stored procedure
- `OBJECT_NAME` type `RDB$RELATION_NAME` - object (table, procedure) name
- `ALIAS` type `RDB$SHORT_DESCRIPTION` - alias name
- `RECORD_LENGTH` type `INTEGER` - record length for the record source
- `KEY_LENGTH` type `INTEGER` - key length for the record source
- `ACCESS_PATH` type `RDB$DESCRIPTION NOT NULL` - friendly plan description

```sql
select *
  from rdb$sql.explain('select * from employee where emp_no = ?');
```

```sql
select *
  from rdb$sql.explain(q'{
    select *
    from (
      select full_name name from employee
      union all
      select customer name from customer
    )
    where name = ?
  }');
```

## Procedure `PARSE_UNQUALIFIED_NAMES`

`RDB$SQL.PARSE_UNQUALIFIED_NAMES` is a selectable procedure that parses a list of unqualified SQL names and returns
one row for each name. The input must follow parse rules for names and the output of unquoted names are uppercased.

```sql
select *
  from rdb$sql.parse_unqualified_names('schema1, schema2, "schema3", "schema 4", "schema ""5"""');

-- SCHEMA1
-- SCHEMA2
-- schema3
-- "schema 4"
-- "schema "5"
```

# Authors
- Adriano dos Santos Fernandes
