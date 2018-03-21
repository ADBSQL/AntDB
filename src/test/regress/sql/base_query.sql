set grammar to oracle;
set datestyle='ISO,YMD';
set timezone = 8;
--基本查询
create table tt(id int,name varchar(10),sal binary_float, regdt timestamp default(to_date(' 2016-01-01 00:00:00','YYYY-MM-DD hh24:mi:ss')));
insert into tt(id,name,sal) values(1, 'mike',3008.90);
select * from tt;
select name, regdt from tt;
select tt.name from tt;
select regdt,name,sal from tt;
drop table tt;
--WHERE	基本运算符号
create table tt(id int,name varchar(10),sal binary_float, regdt timestamp default(to_date(' 2016-01-01 00:00:00','YYYY-MM-DD hh24:mi:ss')));
insert into tt(id,name,sal) values(1, 'mike',3008.90);
insert into tt values(2, 'Tom',4380,to_timestamp('2016-3-24 15:09:50','YYYY-MM-DD hh24:mi:ss'));
select * from tt where id=2;
select * from tt where id>=1;
select * from tt where id<2;
select * from tt where id*2-1=1;
select name from tt where regdt=to_timestamp('2016-3-24 15:09:50','YYYY-MM-DD hh24:mi:ss');
drop table tt;
--||连接不同列
create table tt(id int,name varchar(10));
insert into tt values(1,'Mike');
insert into tt values(2,'Jack');
select id||name as id_name from tt;
select to_char(id)||name as id_name from tt;
drop table tt;
create table tt(id number,sal number);
insert into tt values(1,100);
insert into tt values(2,200);
select id||sal as id_sal from tt order by id;
drop table tt;
--不同数据类型
--timestamp	
create table tt(id int, dt timestamp default(to_date(' 2016-01-01 00:00:00','YYYY-MM-DD hh24:mi:ss')));
insert into tt(id) values(1);
insert into tt values(2,to_timestamp('2016-03-15 15:09:50','YYYY-MM-DD hh24:mi:ss'));
insert into tt values(3,to_date('2016-03-15 15:09:50','YYYY-MM-DD hh24:mi:ss'));
select * from tt order by id;
select * from tt where dt=to_date('2016-03-15 15:09:50','YYYY-MM-DD hh24:mi:ss') order by id;
select * from tt where dt=to_timestamp('2016-03-15 15:09:50','YYYY-MM-DD hh24:mi:ss') order by id;
select * from tt where dt=to_timestamp('2016-03-16 15:09:50','YYYY-MM-DD hh24:mi:ss')- NUMTODSINTERVAL(1, 'day') order by id;
select * from tt where dt<to_timestamp('2016-03-16','YYYY-MM-DD') order by id;
select * from tt where dt between to_date('2016-03-15','YYYY-MM-DD') and to_date('2016-03-16','YYYY-MM-DD') order by id;
select * from tt where dt between to_date('2016-03-15 03:09:00 pm','YYYY-MM-DD hh:mi:ss pm') and to_date('2016-03-16','YYYY-MM-DD') order by id;
drop table tt;
--timestamp with time zone
create table tt(id int, dt timestamp with time zone);
insert into tt values(1,timestamp'2016-03-15 15:09:50' at time zone '0:00');
insert into tt values(3,to_timestamp_tz('2016-03-15 15:09:50 +8:00','YYYY-MM-DD hh24:mi:ss tzh:tzm'));
select * from tt;
select * from tt where dt<timestamp'2016-03-15 15:09:50' at time zone '4:00';
drop table tt;
--interval
create table tt(id int, itv interval year(1) to month);
insert into tt values(1,numtoyminterval(3,'year')+numtoyminterval(3,'month'));
insert into tt values(2,numtoyminterval(8,'month'));
insert into tt values(2,numtodsinterval(20,'day'));
select * from tt order by id;
select * from tt where itv < numtoyminterval(3,'year')
select * from tt where itv = numtoyminterval(10,'month')
select * from tt where itv between numtoyminterval(10,'month') and numtodsinterval(1500,'day');
drop table tt;
--float：不同精度和类型
create table tt(id int,name varchar(10),sal binary_float, regdt timestamp default(to_date(' 2016-01-01 00:00:00','YYYY-MM-DD hh24:mi:ss')));
insert into tt(id,name,sal) values(1, 'mike',3008.90);
select name,sal from tt;
select name,sal from tt where sal=3008.90;
select name,sal from tt where sal=3008.9;
select name,sal from tt where sal=3008.9000;
select name,sal from tt where sal = 3.009E+003;
select name,sal from tt where sal = to_number(3008.90);
select name,sal from tt where sal = '3008.90';
select name,sal from tt where sal = '3008.9';
select name,sal from tt where sal = '3008.9000';
select name,sal from tt where sal = to_char(3008.90);
select name,sal from tt where sal = abs(-3008.90);
drop table tt;
--int型
create table tt(id int,name varchar(10),sal integer);
insert into tt(id,name,sal) values(1, 'mike',3008.90);
insert into tt(id,name,sal) values(2, 'mike',2500);
select name,sal from tt order by sal;
select name,sal from tt where id=1.0;
select name,sal from tt where sal=3009;
select name,sal from tt where sal=3009.0;
select name,sal from tt where id='1';
select name,sal from tt where id=to_number(1);
select name,sal from tt where id=to_char(1);
select name,sal from tt where id=to_char(1.0);
select name,sal from tt where sal='3009';
select name,sal from tt where sal=to_number(3009);
select name,sal from tt where sal=to_char(25000);
select name,sal from tt where sal=abs(-25000);
select name,sal from tt where sal='';
drop table tt;
--varchar	
create table tt(id int,name varchar(10),info varchar(50));
insert into tt(id,name,info) values(1, 'mike','5000');
insert into tt(id,name,info) values(2, '2',to_date('2015-02-12','YYYY-MM-DD'));
insert into tt(id,name,info) values(3, null,'');
select * from tt order by id;
select name from tt where name='mike';
select name from tt where name=2;
select name from tt where info=to_date('2015-02-12','YYYY-MM-DD');
select name from tt where info='';
select name from tt where info=null;
select name from tt where info is null;
drop table tt;
--number	
create table tt(id number(3,1));
insert into tt(id) values(1);
insert into tt(id) values(2.17);
select * from tt order by id;
select * from tt where id=1.00000000;
select * from tt where id=000001;
select * from tt where id=tanh(100);
select * from tt where id=exp(0);
select * from tt where id='2.2';
select * from tt where id=to_char(2.2);
select * from tt where id=to_char(2.20);
select * from tt where id=to_number(2.200,'9.000');
select * from tt where id= 2.2::binary_float;
select * from tt where id= 2.2::binary_double;
drop table tt;
--为子查询取别名
create table tt(id int,sal binary_float);
insert into tt values(1,1880.23);
insert into tt values(2,17000);
create table aa(id int,sal binary_float);
insert into aa values(1,5000);
insert into aa values(2,1000);
select t.sal money from tt t,aa a where t.sal>a.sal;
select tt.sal money from tt,aa a where tt.sal>a.sal;
select tt.id from tt, (select id,sal from aa where id<2) a where tt.id=a.id;
select id from (select id,sal from aa where id<3) a where a.id>1;
select id from (select id,sal money from aa where id<3) a where a.money<5000;
drop table tt;
drop table aa;