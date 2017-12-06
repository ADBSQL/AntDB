set grammar to oracle;
SELECT regexp_substr('abc1def2', '[[:digit:]]',1,2) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',1,'1') output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',1,to_number(2)) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',1,2.1) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',1,to_char(1)) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',1,power(2,1)) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',4) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',5) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',1000000) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]','5') output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',5.5) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',to_char(5)) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]',power(5,1)) output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]D',1,1,'i') output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]]D',1,1,'c') output  from dual;
SELECT regexp_substr('abc1
def2', '[[:digit:]].d',1,1,'n') output  from dual;
SELECT regexp_substr('abc1
def2', '[[:digit:]].d',1,1,'i') output  from dual;
SELECT regexp_substr('abc1def2', '[[:digit:]] d',1,1,'x') output  from dual;
select regexp_substr('abcxxx#%
adfbc','^a',1,2,'m') from dual;
select regexp_substr('abcxxx#%
adfbc','^a',1,2,'n') from dual;
select regexp_substr('abcxxx#%
adfbc','^a',1,2,'i') from dual;
select regexp_substr('abcxxx#%
adfbc','^a',1,2,'x') from dual;
select regexp_substr('abcxxx#%
adfbc','^a',1,2,'c') from dual;
SELECT REGEXP_SUBSTR(123, 12, 1, 1)  FROM DUAL;
SELECT REGEXP_SUBSTR('1234567890', '(123)(4(56)(78))', 1, 1, 'i', 4)  FROM DUAL;
SELECT REGEXP_SUBSTR('1234567890', '(123)(4(56)(78))', 1, 1, 'i', 1)  FROM DUAL;
SELECT REGEXP_SUBSTR('1234567890', '(123)(4(56)(78))', 1, 1, 'i', 9)  FROM DUAL;
SELECT REGEXP_SUBSTR('1234567890', '(123)(4(56)(78))', 1, 1, 'i', 0)  FROM DUAL;
SELECT REGEXP_SUBSTR('1234567890', '(123)(4(56)(78))', 1, 1, 'i', 4.5) FROM DUAL;
SELECT REGEXP_SUBSTR('1234567890', '(123)(4(56)(78))', 1, 1, 'i', to_char(4))  FROM DUAL;
SELECT REGEXP_SUBSTR('1234567890', '(123)(4(56)(78))', 1, 1, 'i',power(2,2))  FROM DUAL;
SELECT REGEXP_SUBSTR('1234567890', '(123)(4(56)(78))', 1, 1, 'i', '4')  FROM DUAL;
SELECT REGEXP_SUBSTR('1234567890abcdefg', '(1)(2)(3)(4)(5)(6)(7)(8)(9)(0)(a)(b)', 1, 1, 'i', 10)  FROM DUAL;
SELECT REGEXP_SUBSTR('', '', 1, 1, 0) FROM DUAL;
SELECT REGEXP_SUBSTR(null, null, 1, 1, 0) FROM DUAL;
