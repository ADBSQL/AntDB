set grammar to oracle;
--1、integer类型是主键
create table tt(id integer primary key,name char(10));
insert into tt values(1,'a');
insert into tt values(1,'b');
drop table tt;
--2、varchar2类型是主键
create table tt(id integer ,name varchar2(10) primary key);
insert into tt values(1,'a');
insert into tt values(2,'a');
drop table tt;
--3、表约束方式创建主键
create table tt(id integer,name char(10),constraint pkk primary key(id));
drop table tt;
--4、联合主键
create table tt(id integer ,name varchar2(10), primary key(id,name));
insert into tt values(1,'b');
insert into tt values(1,'b');
drop table tt;
--5、创建外键
create table tt(id integer unique,name char(10));
insert into tt values(1,'a');
insert into tt values(2,'b');
create table aa(id int, sname char(10),constraint fk FOREIGN KEY(id) REFERENCES tt(id));
insert into aa values(1,'s1');
insert into aa values(2,'s2');
insert into aa values(3,'s2');
drop table aa;
drop table tt;
--6、主键与外键数据类型不同
create table tt(id integer unique,name char(10));
create table aa(fid number(10),sname char(10), constraint fk FOREIGN KEY(fid) REFERENCES tt(id));
drop table aa;
drop table tt;
create table tt(id number unique,name char(10));
create table aa(fid integer,sname char(10), constraint fk FOREIGN KEY(fid) REFERENCES tt(id));
drop table aa;
drop table tt;
create table tt(id float unique,name char(10));
create table aa(fid number,sname char(10), constraint fk FOREIGN KEY(fid) REFERENCES tt(id));
drop table aa;
drop table tt;
--7、联动删除
create table tt(id number(4) primary key,name char(10));
create table aa(id number(3) primary key,name varchar(20));
ALTER TABLE aa ADD CONSTRAINT FK_ID FOREIGN KEY(id) REFERENCES tt(id) ON DELETE CASCADE;  
insert into tt values(1,'Mike');
insert into aa values(1,'Jack');
select * from aa;
delete from tt where id=1;
select * from aa;
drop table aa;
drop table tt;

