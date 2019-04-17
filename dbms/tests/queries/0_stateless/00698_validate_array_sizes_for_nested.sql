SET send_logs_level = 'none';

DROP TABLE IF EXISTS mergetree;
CREATE TABLE mergetree (k UInt32, `n.x` Array(UInt64), `n.y` Array(UInt64)) ENGINE = MergeTree ORDER BY k;

INSERT INTO mergetree VALUES (3, [], [1, 2, 3]), (1, [111], []), (2, [], []); -- { serverError 190 }
SELECT * FROM mergetree;

INSERT INTO mergetree VALUES (3, [4, 5, 6], [1, 2, 3]), (1, [111], [222]), (2, [], []);
SELECT * FROM mergetree;

DROP TABLE mergetree;
