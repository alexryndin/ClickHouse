# Enum

Enumerated type storing pairs of the `'string' = integer` format.

ClickHouse supports:

- 8-bit `Enum`. It can contain up to 256 values with enumeration of `[-128, 127]`.
- 16-bit `Enum`. It can contain up to 65536 values with enumeration of `[-32768, 32767]`.

ClickHouse automatically chooses a type for `Enum` at data insertion. Also, you can use `Enum8` or `Enum16` types to be sure in size of storage.

## Usage examples

Here we create a table with an `Enum8('hello' = 1, 'world' = 2)` type column:

```sql
CREATE TABLE t_enum
(
    x Enum('hello' = 1, 'world' = 2)
)
ENGINE = TinyLog
```

This column `x` can only store the values that are listed in the type definition: `'hello'` or `'world'`. If you try to save any other value, ClickHouse will generate an exception. ClickHouse automatically chooses the 8-bit size for enumeration of this `Enum`.

```sql
:) INSERT INTO t_enum VALUES ('hello'), ('world'), ('hello')

INSERT INTO t_enum VALUES

Ok.

3 rows in set. Elapsed: 0.002 sec.

:) insert into t_enum values('a')

INSERT INTO t_enum VALUES


Exception on client:
Code: 49. DB::Exception: Unknown element 'a' for type Enum('hello' = 1, 'world' = 2)
```

When you query data from the table, ClickHouse outputs the string values from `Enum`.

```sql
SELECT * FROM t_enum

┌─x─────┐
│ hello │
│ world │
│ hello │
└───────┘
```

If you need to see the numeric equivalents of the rows, you must cast the `Enum` value to integer type.

```sql
SELECT CAST(x, 'Int8') FROM t_enum

┌─CAST(x, 'Int8')─┐
│               1 │
│               2 │
│               1 │
└─────────────────┘
```

To create an Enum value in a query, you also need to use `CAST`.

```sql
SELECT toTypeName(CAST('a', 'Enum(\'a\' = 1, \'b\' = 2)'))

┌─toTypeName(CAST('a', 'Enum(\'a\' = 1, \'b\' = 2)'))─┐
│ Enum8('a' = 1, 'b' = 2)                             │
└─────────────────────────────────────────────────────┘
```

## General rules and usage

Each of the values is assigned a number in the range `-128 ... 127` for `Enum8` or in the range `-32768 ... 32767` for `Enum16`. All the strings and numbers must be different. An empty string is allowed. If this type is specified (in a table definition), numbers can be in an arbitrary order. However, the order does not matter.

Neither the string nor the numeric value in an `Enum` can be [NULL](../query_language/syntax.md).

An `Enum` can be contained in [Nullable](nullable.md) type. So if you create a table using the query

```
CREATE TABLE t_enum_nullable
(
    x Nullable( Enum8('hello' = 1, 'world' = 2) )
)
ENGINE = TinyLog
```

it can store not only `'hello'` and `'world'`, but `NULL`, as well.

```
INSERT INTO t_enum_nullable Values('hello'),('world'),(NULL)
```

In RAM, an `Enum` column is stored in the same way as `Int8` or `Int16` of the corresponding numerical values.

When reading in text form, ClickHouse parses the value as a string and searches for the corresponding string from the set of Enum values. If it is not found, an exception is thrown. When reading in text format, the string is read and the corresponding numeric value is looked up. An exception will be thrown if it is not found.
When writing in text form, it writes the value as the corresponding string. If column data contains garbage (numbers that are not from the valid set), an exception is thrown. When reading and writing in binary form, it works the same way as for Int8 and Int16 data types.
The implicit default value is the value with the lowest number.

During `ORDER BY`, `GROUP BY`, `IN`, `DISTINCT` and so on, Enums behave the same way as the corresponding numbers. For example, ORDER BY sorts them numerically. Equality and comparison operators work the same way on Enums as they do on the underlying numeric values.

Enum values cannot be compared with numbers. Enums can be compared to a constant string. If the string compared to is not a valid value for the Enum, an exception will be thrown. The IN operator is supported with the Enum on the left hand side and a set of strings on the right hand side. The strings are the values of the corresponding Enum.

Most numeric and string operations are not defined for Enum values, e.g. adding a number to an Enum or concatenating a string to an Enum.
However, the Enum has a natural `toString` function that returns its string value.

Enum values are also convertible to numeric types using the `toT` function, where T is a numeric type. When T corresponds to the enum’s underlying numeric type, this conversion is zero-cost.
The Enum type can be changed without cost using ALTER, if only the set of values is changed. It is possible to both add and remove members of the Enum using ALTER (removing is safe only if the removed value has never been used in the table). As a safeguard, changing the numeric value of a previously defined Enum member will throw an exception.

Using ALTER, it is possible to change an Enum8 to an Enum16 or vice versa, just like changing an Int8 to Int16.


[Original article](https://clickhouse.yandex/docs/en/data_types/enum/) <!--hide-->
