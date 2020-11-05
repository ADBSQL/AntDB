set grammar to oracle;
select to_number(89.987,'99.999') from dual;
select to_number(89.987,'99.9999') from dual;
select to_number(89.987,'99.000') from dual;
select to_number(889.987,'099.000') from dual;
select to_number(889.987,'000.000') from dual;
select to_number(-889.987,'000.000') from dual;
select to_number(-889.987,'900.000') from dual;
select to_number(-889.987,'-999.99') from dual;
select to_number(-0,'9') from dual;
select to_number(-78.87) from dual;
select to_number('889.987','000.000') from dual;
select to_number('-889.987','9999.999') from dual;
select to_number('-889.987','-999.999') from dual;
select to_number('889.987') from dual;
SELECT TO_NUMBER('$12,123.23','$999,999.99') FROM DUAL;
SELECT TO_NUMBER('nan') FROM DUAL;
SELECT TO_NUMBER(123.28,09999.99) FROM DUAL;
SELECT TO_NUMBER(-123.28,999.99) FROM DUAL;
SELECT TO_NUMBER((5+3.14*4)/5,'999.99999') FROM DUAL; 
select to_number(30, 'xxx') from dual;
select to_number('889.987','') from dual;

CREATE TABLE t4test (id int,it int,num varchar);
insert into t4test values(1,1,'2321.123123');
insert into t4test values(2,2,'3243.232');
select to_number(num,'9G999D99') as val from t4test order by val;
update t4test set it=to_number(it+22,'099');
select * from t4test order by id;
drop table t4test;