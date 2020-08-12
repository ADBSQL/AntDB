/*
 * Oracle Functions
 *
 * Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Copyright (c) 2014-2016, ADB Development Group
 *
 * src/backend/oraschema/oracle_proc.sql
 */

/*
 * Function: bitand
 * Parameter Type: : (numeric, numeric)
 */
CREATE OR REPLACE FUNCTION oracle.bitand(bigint, bigint)
    RETURNS bigint
    AS $$select $1 & $2;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: nanvl
 * Parameter Type: (float8, float8)
 */
CREATE OR REPLACE FUNCTION oracle.nanvl(float8, float8)
    RETURNS float8
    AS $$SELECT CASE WHEN $1 = 'NaN' THEN $2 ELSE $1 END;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: nanvl
 * Parameter Type: (numeric, numeric)
 */
CREATE OR REPLACE FUNCTION oracle.nanvl(numeric, numeric)
    RETURNS numeric
    AS $$SELECT CASE WHEN $1 = 'NaN' THEN $2 ELSE $1 END;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: sinh
 * sinh x = (e ^ x - e ^ (-x))/2
 * Parameter Type: : (numeric)
 */
CREATE OR REPLACE FUNCTION oracle.sinh(numeric)
    RETURNS numeric
    AS $$SELECT (exp($1) - exp(-$1)) / 2;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: sinh
 * sinh x = (e ^ x - e ^ (-x))/2
 * Parameter Type: : (float8)
 */
CREATE OR REPLACE FUNCTION oracle.sinh(float8)
    RETURNS float8
    AS $$SELECT (exp($1) - exp(-$1)) / 2;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: cosh
 * cosh x = (e ^ x + e ^ (-x))/2
 * Parameter Type: (numeric)
 */
CREATE OR REPLACE FUNCTION oracle.cosh(numeric)
    RETURNS numeric
    AS $$SELECT (exp($1) + exp(-$1)) / 2;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: cosh
 * cosh x = (e ^ x + e ^ (-x))/2
 * Parameter Type: (float8)
 */
CREATE OR REPLACE FUNCTION oracle.cosh(float8)
    RETURNS float8
    AS $$SELECT (exp($1) + exp(-$1)) / 2;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: tanh
 * tanh x = sinh x / cosh x = (e ^ x - e ^ (-x)) / (e ^ x + e ^ (-x))
 * Parameter Type: (numeric)
 */
CREATE OR REPLACE FUNCTION oracle.tanh(numeric)
    RETURNS numeric
    AS $$SELECT (exp($1) - exp(-$1)) / (exp($1) + exp(-$1));$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: tanh
 * tanh x = sinh x / cosh x = (e ^ x - e ^ (-x)) / (e ^ x + e ^ (-x))
 * Parameter Type: (float8)
 */
CREATE OR REPLACE FUNCTION oracle.tanh(float8)
    RETURNS float8
    AS $$SELECT (exp($1) - exp(-$1)) / (exp($1) + exp(-$1));$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: INSTR
 */
CREATE OR REPLACE FUNCTION oracle.instr(str text, patt text, start int default 1, nth int default 1)
    RETURNS int
    AS 'orastr_instr4'
    LANGUAGE INTERNAL
--ADBONLY CLUSTER SAFE
    IMMUTABLE STRICT;

/*
 * Function: ADD_MONTHS
 */
CREATE OR REPLACE FUNCTION oracle.add_months(TIMESTAMP WITH TIME ZONE, INTEGER)
     RETURNS TIMESTAMP
     AS $$SELECT oracle.add_months($1::pg_catalog.date, $2) + $1::pg_catalog.time;$$
     LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: LAST_DAY
 */
CREATE OR REPLACE FUNCTION oracle.last_day(TIMESTAMP WITH TIME ZONE)
    RETURNS oracle.date
    AS $$SELECT (oracle.last_day($1::pg_catalog.date) + $1::time)::oracle.date;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: months_between
 */
CREATE OR REPLACE FUNCTION oracle.months_between(TIMESTAMP WITH TIME ZONE, TIMESTAMP WITH TIME ZONE)
    RETURNS NUMERIC
    AS $$SELECT oracle.months_between($1::pg_catalog.date, $2::pg_catalog.date);$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: new_time
 * Parameter Type: (timestamp, text, text)
 */
CREATE OR REPLACE FUNCTION oracle.new_time(tt timestamp with time zone, z1 text, z2 text)
    RETURNS timestamp
    AS $$
    DECLARE
    src_interval INTERVAL;
    dst_interval INTERVAL;
    BEGIN
        SELECT utc_offset INTO src_interval FROM pg_timezone_abbrevs WHERE abbrev = z1;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'Invalid time zone: %', z1;
        END IF;
        SELECT utc_offset INTO dst_interval FROM pg_timezone_abbrevs WHERE abbrev = z2;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'Invalid time zone: %', z2;
        END IF;
        RETURN tt - src_interval + dst_interval;
    END;
    $$
    LANGUAGE plpgsql
    IMMUTABLE
    STRICT;

/*
 * Function: next_day
 * Parameter Type: (oracle.date, text)
 * Parameter Type: (timestamptz, text)
 */
CREATE OR REPLACE FUNCTION oracle.next_day(oracle.date, text)
    RETURNS oracle.date
    AS $$SELECT (oracle.ora_next_day($1::date, $2) + $1::time)::oracle.date;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.next_day(timestamptz, text)
    RETURNS oracle.date
    AS $$SELECT (oracle.ora_next_day($1::date, $2) + $1::time)::oracle.date;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: round
 */
CREATE OR REPLACE FUNCTION oracle.round(pg_catalog.date, text default 'DDD')
    RETURNS pg_catalog.date
    AS 'ora_date_round'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

CREATE OR REPLACE FUNCTION oracle.round(timestamptz, text default 'DDD')
    RETURNS oracle.date
    AS $$select oracle.ora_timestamptz_round($1, $2)::oracle.date;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: trunc
 * Parameter Type: (date, text)
 * Parameter Type: (date)
 * Parameter Type: (timestamp with time zone, text)
 * Parameter Type: (timestamp with time zone)
 */
CREATE OR REPLACE FUNCTION oracle.trunc(pg_catalog.date, text default 'DDD')
    RETURNS date
    AS 'ora_date_trunc'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

CREATE OR REPLACE FUNCTION oracle.trunc(oracle.date, text default 'DDD')
    RETURNS oracle.date
    AS 'ora_timestamptz_trunc'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

CREATE OR REPLACE FUNCTION oracle.trunc(timestamptz, text default 'DDD')
    RETURNS oracle.date
    AS $$select oracle.ora_timestamptz_trunc($1, $2)::oracle.date;$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: first
 * Parameter Type: (text)
 */
CREATE OR REPLACE FUNCTION oracle.first(str text)
    RETURNS text
    AS 'orachr_first'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: last
 * Parameter Type: (text)
 */
CREATE OR REPLACE FUNCTION oracle.last(str text)
    RETURNS text
    AS 'orachr_last'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: to_date
 * Parameter Type: (text)
 * Parameter Type: (text, text)
 */
CREATE OR REPLACE FUNCTION oracle.to_date(text)
    RETURNS oracle.date
    AS 'text_todate'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_date(text, text)
    RETURNS oracle.date
    AS 'text_todate'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: to_timestamp
 * Parameter Type: (text)
 * Parameter Type: (text, text)
 */
CREATE OR REPLACE FUNCTION oracle.to_timestamp(text)
    RETURNS timestamp
    AS 'text_totimestamp'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_timestamp(text, text)
    RETURNS timestamp
    AS 'text_totimestamp'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: to_timestamp_tz
 * Parameter Type: (text)
 * Parameter Type: (text, text)
 */
CREATE OR REPLACE FUNCTION oracle.to_timestamp_tz(text)
    RETURNS timestamptz
    AS 'text_totimestamptz'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_timestamp_tz(text, text)
    RETURNS timestamptz
    AS 'text_totimestamptz'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: to_char
 * Parameter Type: (smallint)
 * Parameter Type: (smallint, text)
 * Parameter Type: (int)
 * Parameter Type: (int, text)
 * Parameter Type: (bigint)
 * Parameter Type: (bigint, text)
 * Parameter Type: (real)
 * Parameter Type: (real, text)
 * Parameter Type: (double precision)
 * Parameter Type: (double precision, text)
 * Parameter Type: (numeric)
 * Parameter Type: (numeric, text)
 * Parameter Type: (text)
 * Parameter Type: (timestamp)
 * Parameter Type: (timestamp, text)
 * Parameter Type: (timestamptz)
 * Parameter Type: (timestamptz, text)
 * Parameter Type: (interval)
 * Parameter Type: (interval, text)
 */
CREATE OR REPLACE FUNCTION oracle.to_char(smallint)
    RETURNS text
    AS 'int4_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(smallint, text)
    RETURNS text
    AS 'int4_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(int)
    RETURNS text
    AS 'int4_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(int, text)
    RETURNS text
    AS 'int4_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(bigint)
    RETURNS text
    AS 'int8_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(bigint, text)
    RETURNS text
    AS 'int8_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(real)
    RETURNS text
    AS 'float4_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(real, text)
    RETURNS text
    AS 'float4_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(double precision)
    RETURNS text
    AS 'float8_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(double precision, text)
    RETURNS text
    AS 'float8_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(numeric)
    RETURNS text
    AS 'numeric_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_char(numeric, text)
    RETURNS text
    AS 'numeric_tochar'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE FUNCTION oracle.to_char(text)
    RETURNS TEXT
    AS 'text_tochar'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE FUNCTION oracle.to_char(timestamp)
    RETURNS TEXT
    AS 'timestamp_tochar'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE FUNCTION oracle.to_char(timestamp, text)
    RETURNS TEXT
    AS 'timestamp_tochar'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE FUNCTION oracle.to_char(timestamptz)
    RETURNS TEXT
    AS 'timestamptz_tochar'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE FUNCTION oracle.to_char(timestamptz, text)
    RETURNS TEXT
    AS 'timestamptz_tochar'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE FUNCTION oracle.to_char(interval)
    RETURNS TEXT
    AS 'interval_tochar'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE FUNCTION oracle.to_char(interval, text)
    RETURNS TEXT
    AS 'interval_tochar'
    LANGUAGE INTERNAL
    STABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: to_number
 * Parameter Type: (text)
 * Parameter Type: (text, text)
 * Parameter Type: (float4)
 * Parameter Type: (float4, text)
 * Parameter Type: (float8)
 * Parameter Type: (float8, text)
 * Parameter Type: (numeric)
 * Parameter Type: (numeric, text)
 */
CREATE OR REPLACE FUNCTION oracle.to_number(text)
    RETURNS numeric
    AS 'text_tonumber'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_number(text, text)
    RETURNS numeric
    AS 'text_tonumber'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_number(float4)
    RETURNS numeric
    AS 'float4_tonumber'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_number(float4, text)
    RETURNS numeric
    AS 'float4_tonumber'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_number(float8)
    RETURNS numeric
    AS 'float8_tonumber'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_number(float8, text)
    RETURNS numeric
    AS 'float8_tonumber'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_number(numeric)
    RETURNS numeric
    AS 'select $1'
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;
CREATE OR REPLACE FUNCTION oracle.to_number(numeric, text)
    RETURNS numeric
    AS 'select oracle.to_number($1::text, $2)'
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: to_multi_byte
 * Parameter Type: (text)
 */
CREATE OR REPLACE FUNCTION oracle.to_multi_byte(text)
    RETURNS text
    AS 'ora_to_multi_byte'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: to_single_byte
 * Parameter Type: (text)
 */
CREATE OR REPLACE FUNCTION oracle.to_single_byte(text)
    RETURNS text
    AS 'ora_to_single_byte'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: substr
 * Parameter Type: (text, int)
 * Parameter Type: (text, int, int)
 * Parameter Type: (numeric, numeric)
 * Parameter Type: (numeric, numeric, numeric)
 * Parameter Type: (varchar, numeric)
 * Parameter Type: (varchar, numeric, numeric)
 */
CREATE OR REPLACE FUNCTION oracle.substr(text, integer)
    RETURNS text
    AS 'orastr_substr2'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

CREATE OR REPLACE FUNCTION oracle.substr(text, integer, integer)
    RETURNS text
    AS 'orastr_substr3'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: dump
 * Parameter Type: (text, int)
 */
CREATE OR REPLACE FUNCTION oracle.dump(text, integer default 10)
    RETURNS varchar
    AS 'ora_dump'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: length
 * Parameter Type: (char)
 * Parameter Type: (varchar)
 * Parameter Type: (varchar2)
 * Parameter Type: (text)
 */
CREATE OR REPLACE FUNCTION oracle.length(char)
    RETURNS integer
    AS 'orastr_bpcharlen'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: lengthb
 * Parameter Type: (varchar2)
 * Parameter Type: (varchar)
 * Parameter Type: (text)
 */
CREATE OR REPLACE FUNCTION oracle.lengthb(oracle.varchar2)
    RETURNS integer
    AS 'byteaoctetlen'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/* string functions for varchar2 type
 * these are 'byte' versions of corresponsing text/varchar functions
 */
CREATE OR REPLACE FUNCTION oracle.substrb(oracle.varchar2, integer)
    RETURNS oracle.varchar2
    AS 'bytea_substr_no_len'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

CREATE OR REPLACE FUNCTION oracle.substrb(oracle.varchar2, integer, integer)
    RETURNS oracle.varchar2
    AS 'bytea_substr'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

CREATE OR REPLACE FUNCTION oracle.strposb(oracle.varchar2, oracle.varchar2)
    RETURNS integer
    AS 'byteapos'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: remainder
 * Parameter Type: (numeric, numeric)
 */
CREATE OR REPLACE FUNCTION oracle.remainder(n2 numeric, n1 numeric)
    RETURNS numeric
    AS $$select abs(n2 - n1*round(n2/n1));$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: sys_extract_utc
 */
CREATE OR REPLACE FUNCTION oracle.sys_extract_utc(timestamp with time zone)
    RETURNS timestamp
    AS $$select $1 at time zone 'UTC';$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: mode
 * Parameter Type: (numeric, numeric)
 * Add oracle.mod(numeric, numeric) to make sure find oracle.mod if
 * current grammar is oracle;
 */
CREATE OR REPLACE FUNCTION oracle.mod(numeric, numeric)
    RETURNS numeric
    AS 'numeric_mod'
    LANGUAGE INTERNAL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    RETURNS NULL ON NULL INPUT;

/*
 * Function: replace(text, text)
 */
CREATE OR REPLACE FUNCTION oracle.replace(text, text)
    RETURNS text
    AS $$select oracle.replace($1, $2, NULL);$$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    CALLED ON NULL INPUT;

/*
 * dbms_output schema
 * add put_line function
 */
create schema IF NOT EXISTS dbms_output;

create or replace function dbms_output.put_line(putout in text)
RETURNS void AS $$
  DECLARE
    ret_val text;
  BEGIN
    ret_val:=putout;
    RAISE NOTICE  '%',ret_val;
  end;
  $$
  IMMUTABLE
--ADBONLY CLUSTER SAFE
  LANGUAGE PLPGSQL;

create schema IF NOT EXISTS  dbms_lock;
create or replace function dbms_lock.sleep(sleep_second in double precision) 
  RETURNS void  AS
    'select pg_sleep($1);'
  IMMUTABLE
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION round(interval, int default 0)
RETURNS float8 AS 
$$select round(EXTRACT(EPOCH FROM $1)::numeric/86400, $2)::float8;$$
  IMMUTABLE
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION trunc(interval, int default 0)
RETURNS float8 AS
  $$ select trunc(EXTRACT(EPOCH FROM $1)::numeric/86400, $2)::float8; $$
  IMMUTABLE
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION sign(interval)
RETURNS float8 AS 
  $$ select sign(EXTRACT(EPOCH FROM $1)::numeric/86400)::float8; $$
  IMMUTABLE
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION mod(text, numeric) RETURNS numeric AS
  $$ select mod($1::numeric, $2); $$
  IMMUTABLE
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION oracle.is_prefix(varchar, varchar)
RETURNS boolean AS
  $$select oracle.is_prefix($1,$2,true)$$
  IMMUTABLE PARALLEL SAFE STRICT
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION oracle.rvrs(varchar, integer DEFAULT 1)
RETURNS varchar AS
  $$select oracle.rvrs($1,$2,NULL)$$
  IMMUTABLE PARALLEL SAFE
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION oracle.swap(varchar, varchar, integer DEFAULT 1)
RETURNS varchar AS
  $$select oracle.swap($1,$2,$3,NULL)$$
  IMMUTABLE PARALLEL SAFE
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION oracle.betwn(varchar, integer, integer)
RETURNS varchar AS
  $$select oracle.betwn($1,$2,$3,true)$$
  IMMUTABLE PARALLEL SAFE STRICT
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION oracle.betwn(varchar, varchar,
  varchar DEFAULT NULL,
  INTEGER DEFAULT 1,
  INTEGER DEFAULT 1,
  BOOLEAN DEFAULT true)
RETURNS varchar AS
  $$select oracle.betwn($1,$2,$3,$4,$5,$6,false)$$
  IMMUTABLE PARALLEL SAFE
--ADBONLY CLUSTER SAFE
LANGUAGE SQL;

/*
 * Function: oracle mod
 * Parameter Type: (smallint, smallint)
 */
CREATE OR REPLACE FUNCTION oracle.mod(smallint, smallint)
    RETURNS smallint
    AS $$ 
	    SELECT CASE WHEN $2 = 0 THEN $1 ELSE (SELECT pg_catalog.mod($1, $2)) END;
    $$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: oracle mod
 * Parameter Type: (integer, integer)
 */
CREATE OR REPLACE FUNCTION oracle.mod(integer, integer)
    RETURNS integer
    AS $$
	    SELECT CASE WHEN $2 = 0 THEN $1 ELSE (SELECT pg_catalog.mod($1, $2)) END;
    $$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: oracle od
 * Parameter Type: (bigint, bigint)
 */
CREATE OR REPLACE FUNCTION oracle.mod(bigint, bigint)
    RETURNS bigint
    AS $$
	    SELECT CASE WHEN $2 = 0 THEN $1 ELSE (SELECT pg_catalog.mod($1, $2)) END;
    $$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;

/*
 * Function: oracle od
 * Parameter Type: (numeric, numeric)
 */
CREATE OR REPLACE FUNCTION oracle.mod(numeric, numeric)
    RETURNS numeric
    AS $$
	    SELECT CASE WHEN $2 = 0 THEN $1 ELSE (SELECT pg_catalog.mod($1, $2)) END;
    $$
    LANGUAGE SQL
    IMMUTABLE
--ADBONLY CLUSTER SAFE
    STRICT;
