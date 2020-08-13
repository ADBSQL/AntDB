/*
 * Oracle Random
 *
 * Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Copyright (c) 2014-2016, ADB Development Group
 *
 * src/backend/oraschema/oracle_random.sql
 */

CREATE SCHEMA IF NOT EXISTS dbms_random;
GRANT USAGE ON SCHEMA dbms_random TO PUBLIC;

CREATE OR REPLACE FUNCTION dbms_random.initialize(int)
    RETURNS void
    AS 'dbms_random_initialize'
    LANGUAGE INTERNAL
    IMMUTABLE PARALLEL SAFE
--ADBONLY CLUSTER SAFE
    STRICT;
COMMENT ON FUNCTION dbms_random.initialize(int) IS 'Initialize package with a seed value';

CREATE OR REPLACE FUNCTION dbms_random.normal()
    RETURNS double precision
    AS 'dbms_random_normal'
    LANGUAGE INTERNAL
--ADBONLY CLUSTER SAFE
    STRICT
    VOLATILE PARALLEL RESTRICTED;
COMMENT ON FUNCTION dbms_random.normal() IS 'Returns random numbers in a standard normal distribution';

CREATE OR REPLACE FUNCTION dbms_random.random()
    RETURNS integer
    AS 'dbms_random_random'
    LANGUAGE INTERNAL
--ADBONLY CLUSTER SAFE
    STRICT
    VOLATILE PARALLEL RESTRICTED;
COMMENT ON FUNCTION dbms_random.random() IS 'Generate Random Numeric Values';

CREATE OR REPLACE FUNCTION dbms_random.seed(integer)
    RETURNS void
    AS 'dbms_random_seed_int'
    LANGUAGE INTERNAL
    IMMUTABLE PARALLEL SAFE
--ADBONLY CLUSTER SAFE
    STRICT;
COMMENT ON FUNCTION dbms_random.seed(int) IS 'Reset the seed value';

CREATE OR REPLACE FUNCTION dbms_random.seed(text)
    RETURNS void
    AS 'dbms_random_seed_varchar'
    LANGUAGE INTERNAL
    IMMUTABLE PARALLEL SAFE
--ADBONLY CLUSTER SAFE
    STRICT;
COMMENT ON FUNCTION dbms_random.seed(text) IS 'Reset the seed value';

CREATE OR REPLACE FUNCTION dbms_random.string(opt text, len int)
    RETURNS text
    AS 'dbms_random_string'
    LANGUAGE INTERNAL
--ADBONLY CLUSTER SAFE
    STRICT
    VOLATILE PARALLEL RESTRICTED;
COMMENT ON FUNCTION dbms_random.string(text,int) IS 'Create Random Strings';

CREATE OR REPLACE FUNCTION dbms_random.terminate()
    RETURNS void
    AS 'dbms_random_terminate'
    LANGUAGE INTERNAL
--ADBONLY CLUSTER SAFE
    STRICT
    IMMUTABLE PARALLEL SAFE;
COMMENT ON FUNCTION dbms_random.terminate() IS 'Terminate use of the Package';

CREATE OR REPLACE FUNCTION dbms_random.value(low double precision, high double precision)
    RETURNS double precision
    AS 'dbms_random_value_range'
    LANGUAGE INTERNAL
    STRICT
--ADBONLY CLUSTER SAFE
    VOLATILE PARALLEL RESTRICTED;
COMMENT ON FUNCTION dbms_random.value(double precision, double precision) IS 'Generate Random number x, where x is greather or equal to low and less then high';

CREATE OR REPLACE FUNCTION dbms_random.value(text, double precision)
    RETURNS double precision
    AS 'select dbms_random.value($1::double precision, $2);'
    LANGUAGE SQL
    STRICT
--ADBONLY CLUSTER SAFE
    VOLATILE PARALLEL RESTRICTED;
CREATE OR REPLACE FUNCTION dbms_random.value(double precision, text)
    RETURNS double precision
    AS 'select dbms_random.value($1, $2::double precision);'
    LANGUAGE SQL
    STRICT
--ADBONLY CLUSTER SAFE
    VOLATILE PARALLEL RESTRICTED;
CREATE OR REPLACE FUNCTION dbms_random.value(text, text)
    RETURNS double precision
    AS 'select dbms_random.value($1::double precision, $2::double precision);'
    LANGUAGE SQL
    STRICT
--ADBONLY CLUSTER SAFE
    VOLATILE PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION dbms_random.value()
    RETURNS double precision
    AS 'dbms_random_value'
    LANGUAGE INTERNAL
--ADBONLY CLUSTER SAFE
    STRICT
    VOLATILE PARALLEL RESTRICTED;
COMMENT ON FUNCTION dbms_random.value() IS 'Generate Random number x, where x is greather or equal to 0 and less then 1';

