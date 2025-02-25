# Parametric aggregate functions {#aggregate_functions_parametric}

Some aggregate functions can accept not only argument columns (used for compression), but a set of parameters – constants for initialization. The syntax is two pairs of brackets instead of one. The first is for parameters, and the second is for arguments.

## histogram

Calculates an adaptive histogram. It doesn't guarantee precise results.

```
histogram(number_of_bins)(values)
```
 
The functions uses [A Streaming Parallel Decision Tree Algorithm](http://jmlr.org/papers/volume11/ben-haim10a/ben-haim10a.pdf). The borders of histogram bins are adjusted as a new data enters a function, and in common case the widths of bins are not equal.

**Parameters**

`number_of_bins` — Upper limit for a number of bins for the histogram. Function automatically calculates the number of bins. It tries to reach the specified number of bins, but if it fails, it uses less number of bins.
`values` — [Expression](../syntax.md#syntax-expressions) resulting in input values.

**Returned values**

- [Array](../../data_types/array.md) of [Tuples](../../data_types/tuple.md) of the following format:

    ```
    [(lower_1, upper_1, height_1), ... (lower_N, upper_N, height_N)]
    ```

    - `lower` — Lower bound of the bin.
    - `upper` — Upper bound of the bin.
    - `height` — Calculated height of the bin.

**Example**

```sql
SELECT histogram(5)(number + 1) 
FROM (
    SELECT * 
    FROM system.numbers 
    LIMIT 20
)
```
```text
┌─histogram(5)(plus(number, 1))───────────────────────────────────────────┐
│ [(1,4.5,4),(4.5,8.5,4),(8.5,12.75,4.125),(12.75,17,4.625),(17,20,3.25)] │
└─────────────────────────────────────────────────────────────────────────┘
```

You can visualize a histogram with the [bar](../other_functions.md#function-bar) function, for example:

```sql
WITH histogram(5)(rand() % 100) AS hist
SELECT 
    arrayJoin(hist).3 AS height, 
    bar(height, 0, 6, 5) AS bar
FROM 
(
    SELECT *
    FROM system.numbers
    LIMIT 20
)
```
```text
┌─height─┬─bar───┐
│  2.125 │ █▋    │
│   3.25 │ ██▌   │
│  5.625 │ ████▏ │
│  5.625 │ ████▏ │
│  3.375 │ ██▌   │
└────────┴───────┘
```

In this case you should remember, that you don't know the borders of histogram bins.

## sequenceMatch(pattern)(time, cond1, cond2, ...)

Pattern matching for event chains.

`pattern` is a string containing a pattern to match. The pattern is similar to a regular expression.

`time` is the time of the event, type support: `Date`,`DateTime`, and other unsigned integer types.

`cond1`, `cond2` ... is from one to 32 arguments of type UInt8 that indicate whether a certain condition was met for the event.

The function collects a sequence of events in RAM. Then it checks whether this sequence matches the pattern.
It returns UInt8: 0 if the pattern isn't matched, or 1 if it matches.

Example: `sequenceMatch ('(?1).*(?2)')(EventTime, URL LIKE '%company%', URL LIKE '%cart%')`

- whether there was a chain of events in which a pageview with 'company' in the address occurred earlier than a pageview with 'cart' in the address.

This is a singular example. You could write it using other aggregate functions:

```
minIf(EventTime, URL LIKE '%company%') < maxIf(EventTime, URL LIKE '%cart%').
```

However, there is no such solution for more complex situations.

Pattern syntax:

`(?1)` refers to the condition (any number can be used in place of 1).

`.*` is any number of any events.

`(?t>=1800)` is a time condition.

Any quantity of any type of events is allowed over the specified time.

Instead of `>=`, the following operators can be used:`<`, `>`, `<=`.

Any number may be specified in place of 1800.

Events that occur during the same second can be put in the chain in any order. This may affect the result of the function.

## sequenceCount(pattern)(time, cond1, cond2, ...)

Works the same way as the sequenceMatch function, but instead of returning whether there is an event chain, it returns UInt64 with the number of event chains found.
Chains are searched for without overlapping. In other words, the next chain can start only after the end of the previous one.

## windowFunnel(window)(timestamp, cond1, cond2, cond3, ...)

Searches for event chains in a sliding time window and calculates the maximum number of events that occurred from the chain.

```
windowFunnel(window)(timestamp, cond1, cond2, cond3, ...)
```

**Parameters:**

- `window` — Length of the sliding window in seconds.
- `timestamp` — Name of the column containing the timestamp. Data type support: `Date`,`DateTime`, and other unsigned integer types(Note that though timestamp support `UInt64` type, there is a limitation it's value can't overflow maximum of Int64, which is 2^63 - 1).
- `cond1`, `cond2`... — Conditions or data describing the chain of events. Data type: `UInt8`. Values can be 0 or 1.

**Algorithm**

- The function searches for data that triggers the first condition in the chain and sets the event counter to 1. This is the moment when the sliding window starts.
- If events from the chain occur sequentially within the window, the counter is incremented. If the sequence of events is disrupted, the counter isn't incremented.
- If the data has multiple event chains at varying points of completion, the function will only output the size of the longest chain.

**Returned value**

- Integer. The maximum number of consecutive triggered conditions from the chain within the sliding time window. All the chains in the selection are analyzed.

**Example**

Determine if one hour is enough for the user to select a phone and purchase it in the online store.

Set the following chain of events:

1. The user logged in to their account on the store (`eventID=1001`).
2. The user searches for a phone (`eventID = 1003, product = 'phone'`).
3. The user placed an order (`eventID = 1009`).

To find out how far the user `user_id` could get through the chain in an hour in January of 2017, make the query:

```
SELECT
    level,
    count() AS c
FROM
(
    SELECT
        user_id,
        windowFunnel(3600)(timestamp, eventID = 1001, eventID = 1003 AND product = 'phone', eventID = 1009) AS level
    FROM trend_event
    WHERE (event_date >= '2017-01-01') AND (event_date <= '2017-01-31')
    GROUP BY user_id
)
GROUP BY level
ORDER BY level
```

Simply, the level value could only be 0, 1, 2, 3, it means the maxium event action stage that one user could reach.

## retention(cond1, cond2, ...)

Retention refers to the ability of a company or product to retain its customers over some specified periods.

`cond1`, `cond2` ... is from one to 32 arguments of type UInt8 that indicate whether a certain condition was met for the event

Example:

Consider you are doing a website analytics, intend to calculate the retention of customers

This could be easily calculate by `retention`

```
SELECT
    sum(r[1]) AS r1,
    sum(r[2]) AS r2,
    sum(r[3]) AS r3
FROM
(
    SELECT
        uid,
        retention(date = '2018-08-10', date = '2018-08-11', date = '2018-08-12') AS r
    FROM events
    WHERE date IN ('2018-08-10', '2018-08-11', '2018-08-12')
    GROUP BY uid
)
```

Simply, `r1` means the number of unique visitors who met the `cond1` condition, `r2` means the number of unique visitors who met `cond1` and `cond2` conditions, `r3` means the number of unique visitors who met `cond1` and `cond3` conditions.


## uniqUpTo(N)(x)

Calculates the number of different argument values ​​if it is less than or equal to N. If the number of different argument values is greater than N, it returns N + 1.

Recommended for use with small Ns, up to 10. The maximum value of N is 100.

For the state of an aggregate function, it uses the amount of memory equal to 1 + N \* the size of one value of bytes.
For strings, it stores a non-cryptographic hash of 8 bytes. That is, the calculation is approximated for strings.

The function also works for several arguments.

It works as fast as possible, except for cases when a large N value is used and the number of unique values is slightly less than N.

Usage example:

```
Problem: Generate a report that shows only keywords that produced at least 5 unique users.
Solution: Write in the GROUP BY query SearchPhrase HAVING uniqUpTo(4)(UserID) >= 5
```

[Original article](https://clickhouse.yandex/docs/en/query_language/agg_functions/parametric_functions/) <!--hide-->


## sumMapFiltered(keys_to_keep)(keys, values)

Same behavior as [sumMap](reference.md#agg_functions-summap) except that an array of keys is passed as a parameter. This can be especially useful when working with a high cardinality of keys.
