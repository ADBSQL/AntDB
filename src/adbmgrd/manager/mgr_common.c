
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "mgr/mgr_cmds.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "mgr/mgr_msg_type.h"
#include "catalog/mgr_host.h"
#include "catalog/mgr_cndnnode.h"
#include "utils/rel.h"
#include "utils/array.h"
#include "utils/tqual.h"
#include "executor/spi.h"
#include "../../interfaces/libpq/libpq-fe.h"
#include "utils/fmgroids.h"
#include "executor/spi.h"

#define MAXLINE (8192-1)
#define MAXPATH (512-1)
#define RETRY 3
#define SLEEP_MICRO 100*1000     /* 100 millisec */

static TupleDesc common_command_tuple_desc = NULL;
static TupleDesc common_list_acl_tuple_desc = NULL;
static TupleDesc showparam_command_tuple_desc = NULL;
static void myUsleep(long microsec);

TupleDesc get_common_command_tuple_desc(void)
{
	if(common_command_tuple_desc == NULL)
	{
		MemoryContext volatile old_context = MemoryContextSwitchTo(TopMemoryContext);
		TupleDesc volatile desc = NULL;
		PG_TRY();
		{
			desc = CreateTemplateTupleDesc(3, false);
			TupleDescInitEntry(desc, (AttrNumber) 1, "name",
							   NAMEOID, -1, 0);
			TupleDescInitEntry(desc, (AttrNumber) 2, "success",
							   BOOLOID, -1, 0);
			TupleDescInitEntry(desc, (AttrNumber) 3, "message",
							   TEXTOID, -1, 0);
			common_command_tuple_desc = BlessTupleDesc(desc);
		}PG_CATCH();
		{
			if(desc)
				FreeTupleDesc(desc);
			PG_RE_THROW();
		}PG_END_TRY();
		(void)MemoryContextSwitchTo(old_context);
	}
	Assert(common_command_tuple_desc);
	return common_command_tuple_desc;
}

HeapTuple build_common_command_tuple(const Name name, bool success, const char *message)
{
	Datum datums[3];
	bool nulls[3];
	TupleDesc desc;
	AssertArg(name && message);
	desc = get_common_command_tuple_desc();

	AssertArg(desc && desc->natts == 3
		&& desc->attrs[0]->atttypid == NAMEOID
		&& desc->attrs[1]->atttypid == BOOLOID
		&& desc->attrs[2]->atttypid == TEXTOID);

	datums[0] = NameGetDatum(name);
	datums[1] = BoolGetDatum(success);
	datums[2] = CStringGetTextDatum(message);
	nulls[0] = nulls[1] = nulls[2] = false;
	return heap_form_tuple(desc, datums, nulls);
}

HeapTuple build_list_acl_command_tuple(const Name name, const char *message)
{
	Datum datums[2];
	bool nulls[2];
	TupleDesc desc;
	AssertArg(name && message);
	desc = get_list_acl_command_tuple_desc();

	AssertArg(desc && desc->natts == 2
		&& desc->attrs[0]->atttypid == NAMEOID
		&& desc->attrs[1]->atttypid == TEXTOID);

	datums[0] = NameGetDatum(name);
	datums[1] = CStringGetTextDatum(message);
	nulls[0] = nulls[1] = false;
	return heap_form_tuple(desc, datums, nulls);
}

TupleDesc get_list_acl_command_tuple_desc(void)
{
	if(common_list_acl_tuple_desc == NULL)
	{
		MemoryContext volatile old_context = MemoryContextSwitchTo(TopMemoryContext);
		TupleDesc volatile desc = NULL;
		PG_TRY();
		{
			desc = CreateTemplateTupleDesc(2, false);
			TupleDescInitEntry(desc, (AttrNumber) 1, "name",
							   NAMEOID, -1, 0);
			TupleDescInitEntry(desc, (AttrNumber) 2, "message",
							   TEXTOID, -1, 0);
			common_list_acl_tuple_desc = BlessTupleDesc(desc);
		}PG_CATCH();
		{
			if(desc)
				FreeTupleDesc(desc);
			PG_RE_THROW();
		}PG_END_TRY();
		(void)MemoryContextSwitchTo(old_context);
	}
	Assert(common_list_acl_tuple_desc);
	return common_list_acl_tuple_desc;
}

/*get the the address of host in table host*/
char *get_hostaddress_from_hostoid(Oid hostOid)
{
	Relation rel;
	HeapTuple tuple;
	Datum host_addr;
	char *hostaddress;
	bool isNull = false;
	
	rel = heap_open(HostRelationId, AccessShareLock);
	tuple = SearchSysCache1(HOSTHOSTOID, ObjectIdGetDatum(hostOid));
	/*check the host exists*/
	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION)
		,errmsg("cache lookup failed for relation %u", hostOid)));
	}
	host_addr = heap_getattr(tuple, Anum_mgr_host_hostaddr, RelationGetDescr(rel), &isNull);
	if(isNull)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR)
			, err_generic_string(PG_DIAG_TABLE_NAME, "mgr_host")
			, errmsg("column hostaddr is null")));
	hostaddress = pstrdup(TextDatumGetCString(host_addr));
	ReleaseSysCache(tuple);
	heap_close(rel, AccessShareLock);
	return hostaddress;
}

/*get the the hostname in table host*/
char *get_hostname_from_hostoid(Oid hostOid)
{
	Relation rel;
	HeapTuple tuple;
	Datum host_name;
	char *hostname;
	bool isNull = false;
	
	rel = heap_open(HostRelationId, AccessShareLock);
	tuple = SearchSysCache1(HOSTHOSTOID, ObjectIdGetDatum(hostOid));
	/*check the host exists*/
	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION)
		,errmsg("cache lookup failed for relation %u", hostOid)));
	}
	host_name = heap_getattr(tuple, Anum_mgr_host_hostname, RelationGetDescr(rel), &isNull);
	if(isNull)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR)
			, err_generic_string(PG_DIAG_TABLE_NAME, "mgr_host")
			, errmsg("column hostname is null")));
	hostname = pstrdup(NameStr(*DatumGetName(host_name)));
	ReleaseSysCache(tuple);
	heap_close(rel, AccessShareLock);
	return hostname;
}

/*get the the user in table host*/
char *get_hostuser_from_hostoid(Oid hostOid)
{
	Relation rel;
	HeapTuple tuple;
	Datum host_user;
	char *hostuser;
	bool isNull = false;
	
	rel = heap_open(HostRelationId, AccessShareLock);
	tuple = SearchSysCache1(HOSTHOSTOID, ObjectIdGetDatum(hostOid));
	/*check the host exists*/
	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION)
		,errmsg("cache lookup failed for relation %u", hostOid)));
	}
	host_user = heap_getattr(tuple, Anum_mgr_host_hostuser, RelationGetDescr(rel), &isNull);
	if(isNull)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR)
			, err_generic_string(PG_DIAG_TABLE_NAME, "mgr_host")
			, errmsg("column hostuser is null")));
	hostuser = pstrdup(NameStr(*DatumGetName(host_user)));
	ReleaseSysCache(tuple);
	heap_close(rel, AccessShareLock);
	return hostuser;
}

/*
* get msg from agent
*/
bool mgr_recv_msg(ManagerAgent	*ma, GetAgentCmdRst *getAgentCmdRst)
{
	char			msg_type;
	StringInfoData recvbuf;
	bool initdone = false;
	initStringInfo(&recvbuf);
	for(;;)
	{
		resetStringInfo(&recvbuf);
		msg_type = ma_get_message(ma, &recvbuf);
		if(msg_type == AGT_MSG_IDLE)
		{
			/* message end */
			break;
		}else if(msg_type == '\0')
		{
			/* has an error */
			break;
		}else if(msg_type == AGT_MSG_ERROR)
		{
			/* error message */
			getAgentCmdRst->ret = false;
			appendStringInfoString(&(getAgentCmdRst->description), ma_get_err_info(&recvbuf, AGT_MSG_RESULT));
			ereport(LOG, (errmsg("receive msg: %s", ma_get_err_info(&recvbuf, AGT_MSG_RESULT))));
			break;
		}else if(msg_type == AGT_MSG_NOTICE)
		{
			/* ignore notice message */
			ereport(LOG, (errmsg("receive msg: %s", recvbuf.data)));
		}
		else if(msg_type == AGT_MSG_RESULT)
		{
			getAgentCmdRst->ret = true;
			appendStringInfoString(&(getAgentCmdRst->description), run_success);
			ereport(DEBUG1, (errmsg("receive msg: %s", recvbuf.data)));
			initdone = true;
			break;
		}
	}
	pfree(recvbuf.data);
	return initdone;
}

/*
* get host info from agent for [ADB monitor]
*/
bool mgr_recv_msg_for_monitor(ManagerAgent *ma, bool *ret, StringInfo agentRstStr)
{
	char msg_type;
	bool initdone = false;
	
	for (;;)
	{
		msg_type = ma_get_message(ma, agentRstStr);
		if (msg_type == AGT_MSG_IDLE)
		{
			/* message end */
			break;
		}else if (msg_type == '\0')
		{
			/* has an error */
			break;
		}else if (msg_type == AGT_MSG_ERROR)
		{
			/* error message */
			*ret = false;
			appendStringInfoString(agentRstStr, ma_get_err_info(agentRstStr, AGT_MSG_RESULT));
			ereport(LOG, (errmsg("receive msg: %s", ma_get_err_info(agentRstStr, AGT_MSG_RESULT))));
			break;
		}else if (msg_type == AGT_MSG_NOTICE)
		{
			/* ignore notice message */
			ereport(LOG, (errmsg("receive msg: %s", agentRstStr->data)));
		}
		else if (msg_type == AGT_MSG_RESULT)
		{
			*ret = true;
			ereport(LOG, (errmsg("receive msg: %s", agentRstStr->data)));
			initdone = true;
			break;
		}
	}
	return initdone;
}

bool is_valid_ip(char *ip)
{
	FILE *pPipe;
	char psBuffer[1024];
	char ping_str[1024];
	struct hostent *hptr;

	if ((hptr = gethostbyname(ip)) == NULL)
	{
		return false;
	}

	snprintf(ping_str, sizeof(ping_str), "ping -c 1 %s", ip);

	if((pPipe = popen(ping_str, "r" )) == NULL )
	{
		return false;
	}
	while(fgets(psBuffer, 1024, pPipe))
	{
		if (strstr(psBuffer, "0 received") != NULL ||
			strstr(psBuffer, "Unreachable") != NULL)
		{
			pclose(pPipe);
			return false;
		}
	}
	pclose(pPipe);
	return true;
}

/* ping someone node for monitor */
int pingNode_user(char *host, char *port, char *user)
{
	PGPing ret;
	char conninfo[MAXLINE + 1] = {0};
	char editBuf[MAXPATH + 1] = {0};
	int retry;

	if (host)
	{
		snprintf(editBuf, MAXPATH, "host='%s' ", host);
		strncat(conninfo, editBuf, MAXLINE);
	}

	if (port)
	{
		snprintf(editBuf, MAXPATH, "port=%d ", atoi(port));
		strncat(conninfo, editBuf, MAXLINE);
	}

	if (user)
	{
		snprintf(editBuf, MAXPATH, "user=%s ", user);
		strncat(conninfo, editBuf, MAXLINE);
	}

	/*timeout set 2s*/
	snprintf(editBuf, MAXPATH,"connect_timeout=2");
	strncat(conninfo, editBuf, MAXLINE);

	if (conninfo[0])
	{
		elog(DEBUG1, "Ping node string: %s.\n", conninfo);
		for (retry = RETRY; retry; retry--)
		{
			ret = PQping(conninfo);
			switch (ret)
			{
				case PQPING_OK:
					return PQPING_OK;
				case PQPING_REJECT:
					return PQPING_REJECT;
				case PQPING_NO_ATTEMPT:
					return PQPING_NO_ATTEMPT;
				case PQPING_NO_RESPONSE:
					return PQPING_NO_RESPONSE;
				default:
					myUsleep(SLEEP_MICRO);
					continue;
			}
		}
	}

	return -1;
}

static void
myUsleep(long microsec)
{
    struct timeval delay;

    if (microsec <= 0)
        return; 

    delay.tv_sec = microsec / 1000000L;
    delay.tv_usec = microsec % 1000000L;
    (void) select(0, NULL, NULL, NULL, &delay);
}

/*check the host in use or not*/
bool mgr_check_host_in_use(Oid hostoid)
{
	HeapScanDesc rel_scan;
	HeapTuple tuple =NULL;
	Form_mgr_node mgr_node;
	Relation rel;
	
	rel = heap_open(NodeRelationId, RowExclusiveLock);
	rel_scan = heap_beginscan_catalog(rel, 0, NULL);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		/* check this tuple incluster or not, if it has incluster, cannot be dropped/alter. */
		mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
		Assert(mgr_node);
		if(mgr_node->nodehost == hostoid)
		{
			heap_endscan(rel_scan);
			heap_close(rel, RowExclusiveLock);
			return true;
		}
	}
	heap_endscan(rel_scan);
	heap_close(rel, RowExclusiveLock);
	return false;
}

/*
* get database namelist
*/
List *monitor_get_dbname_list(char *user, char *address, int port)
{
	StringInfoData constr;
	PGconn* conn;
	PGresult *res;
	char *oneCoordValueStr;
	List *nodenamelist =NIL;
	int iN = 0;
	char *sqlstr = "select datname from pg_database  where datname != \'template0\' and datname != \'template1\' order by 1;";
		
	initStringInfo(&constr);
	appendStringInfo(&constr, "postgresql://%s@%s:%d/postgres", user, address, port);
	conn = PQconnectdb(constr.data);
	/* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK) 
	{
		ereport(LOG, 
			(errmsg("Connection to database failed: %s\n", PQerrorMessage(conn))));
		PQfinish(conn);
		pfree(constr.data);
		return NULL;
	}
	res = PQexec(conn, sqlstr);
	if(PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		ereport(LOG, 
			(errmsg("Select failed: %s\n" , PQresultErrorMessage(res))));
		PQclear(res);
		PQfinish(conn);
		pfree(constr.data);
		return NULL;
	}
	/*check column number*/
	Assert(1 == PQnfields(res));
	for (iN=0; iN < PQntuples(res); iN++)
	{
		oneCoordValueStr = PQgetvalue(res, iN, 0 );
		nodenamelist = lappend(nodenamelist, pstrdup(oneCoordValueStr));
	}
	PQclear(res);
	PQfinish(conn);
	pfree(constr.data);
	
	return nodenamelist;
}

/*
* get user, hostaddress from coordinator
*/
void monitor_get_one_node_user_address_port(Relation rel_node, int *agentport, char **user, char **address, int *coordport, char nodetype)
{
	HeapScanDesc rel_scan;
	ScanKeyData key[1];
	HeapTuple tuple;
	HeapTuple tup;
	Form_mgr_node mgr_node;
	Form_mgr_host mgr_host;

	ScanKeyInit(&key[0],
		Anum_mgr_node_nodetype
		,BTEqualStrategyNumber
		,F_CHAREQ
		,CharGetDatum(nodetype));
	rel_scan = heap_beginscan_catalog(rel_node, 1, key);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
		Assert(mgr_node);
		*coordport = mgr_node->nodeport;
		*address = get_hostaddress_from_hostoid(mgr_node->nodehost);
		*user = get_hostuser_from_hostoid(mgr_node->nodehost);
		tup = SearchSysCache1(HOSTHOSTOID, ObjectIdGetDatum(mgr_node->nodehost));
		if(!(HeapTupleIsValid(tup)))
		{
			ereport(ERROR, (errmsg("host oid \"%u\" not exist", mgr_node->nodehost)
				, err_generic_string(PG_DIAG_TABLE_NAME, "mgr_host")
				, errcode(ERRCODE_INTERNAL_ERROR)));
		}
		mgr_host = (Form_mgr_host)GETSTRUCT(tup);
		Assert(mgr_host);
		*agentport = mgr_host->hostagentport;
		ReleaseSysCache(tup);
		break;
	}
	heap_endscan(rel_scan);
}

/*
* get len values to iarray, the values get from the given sqlstr's result
*/
bool monitor_get_sqlvalues_one_node(int agentport, char *sqlstr, char *user, char *address, int port, char * dbname, int iarray[], int len)
{
	int iloop = 0;
	char *pstr;
	char strtmp[64];
	StringInfoData resultstrdata;

	initStringInfo(&resultstrdata);
	monitor_get_stringvalues(AGT_CMD_GET_SQL_STRINGVALUES, agentport, sqlstr, user, address, port, dbname, &resultstrdata);
	if (resultstrdata.len == 0)
	{
		return false;
	}
	pstr = resultstrdata.data;
	while(pstr != NULL && iloop < len)
	{
		strcpy(strtmp, pstr);
		iarray[iloop] = atoi(strtmp);
		pstr = pstr + strlen(strtmp) + 1;
		iloop++;
	}
	pfree(resultstrdata.data);
	return true;
}

/*
* to make the columns name for the result of command: show nodename parameter
*/
TupleDesc get_showparam_command_tuple_desc(void)
{
	if(showparam_command_tuple_desc == NULL)
	{
		MemoryContext volatile old_context = MemoryContextSwitchTo(TopMemoryContext);
		TupleDesc volatile desc = NULL;
		PG_TRY();
		{
			desc = CreateTemplateTupleDesc(3, false);
			TupleDescInitEntry(desc, (AttrNumber) 1, "type",
							   NAMEOID, -1, 0);
			TupleDescInitEntry(desc, (AttrNumber) 2, "success",
							   BOOLOID, -1, 0);
			TupleDescInitEntry(desc, (AttrNumber) 3, "message",
							   TEXTOID, -1, 0);
			common_command_tuple_desc = BlessTupleDesc(desc);
		}PG_CATCH();
		{
			if(desc)
				FreeTupleDesc(desc);
			PG_RE_THROW();
		}PG_END_TRY();
		(void)MemoryContextSwitchTo(old_context);
	}
	Assert(common_command_tuple_desc);
	return common_command_tuple_desc;
}

List * DecodeTextArrayToValueList(Datum textarray)
{
	ArrayType  *array = NULL;
	Datum *elemdatums;
	int num_elems;
	List *value_list = NIL;
	int i;

	Assert( PointerIsValid(DatumGetPointer(textarray)));

	array = DatumGetArrayTypeP(textarray);
	Assert(ARR_ELEMTYPE(array) == TEXTOID);

	deconstruct_array(array, TEXTOID, -1, false, 'i', &elemdatums, NULL, &num_elems);
	for (i = 0; i < num_elems; ++i)
	{
		value_list = lappend(value_list, makeString(TextDatumGetCString(elemdatums[i])));
	}

	return value_list;
}

void check_nodename_isvalid(char *nodename)
{
	HeapTuple tuple;
	ScanKeyData key[2];
	Relation rel;
	HeapScanDesc rel_scan;

	rel = heap_open(NodeRelationId, AccessShareLock);

	ScanKeyInit(&key[0]
			,Anum_mgr_node_nodename
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(nodename));
	ScanKeyInit(&key[1]
			,Anum_mgr_node_nodeincluster
			,BTEqualStrategyNumber
			,F_BOOLEQ
			,BoolGetDatum(true));
	rel_scan = heap_beginscan_catalog(rel, 2, key);

	tuple = heap_getnext(rel_scan, ForwardScanDirection);
	if (tuple == NULL)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
			,errmsg("node name \"%s\" does not exist", nodename)));

	heap_endscan(rel_scan);
	heap_close(rel, AccessShareLock);
}

bool mgr_has_function_privilege_name(char *funcname, char *priv_type)
{
	Datum aclresult;
	aclresult = DirectFunctionCall2(has_function_privilege_name,
							CStringGetTextDatum(funcname),
							CStringGetTextDatum(priv_type));

	return DatumGetBool(aclresult);
}

bool mgr_has_table_privilege_name(char *tablename, char *priv_type)
{
	Datum aclresult;
	aclresult = DirectFunctionCall2(has_table_privilege_name,
							CStringGetTextDatum(tablename),
							CStringGetTextDatum(priv_type));

	return DatumGetBool(aclresult);
}

/*
* get msg from agent
*/
void mgr_recv_sql_stringvalues_msg(ManagerAgent	*ma, StringInfo resultstrdata)
{
	char			msg_type;
	StringInfoData recvbuf;
	initStringInfo(&recvbuf);
	for(;;)
	{
		msg_type = ma_get_message(ma, &recvbuf);
		if(msg_type == AGT_MSG_IDLE)
		{
			/* message end */
			break;
		}else if(msg_type == '\0')
		{
			/* has an error */
			break;
		}else if(msg_type == AGT_MSG_ERROR)
		{
			/* error message */
			ereport(WARNING, (errmsg("%s", ma_get_err_info(&recvbuf, AGT_MSG_RESULT))));
			break;
		}else if(msg_type == AGT_MSG_NOTICE)
		{
			/* ignore notice message */
			ereport(NOTICE, (errmsg("%s", recvbuf.data)));
		}
		else if(msg_type == AGT_MSG_RESULT)
		{
			appendBinaryStringInfo(resultstrdata, recvbuf.data, recvbuf.len);
			ereport(DEBUG1, (errmsg("%s", recvbuf.data)));
			break;
		}
	}
	pfree(recvbuf.data);
}

/*
* get active coordinator node name
*/
bool mgr_get_active_node(Name nodename, char nodetype)
{
	int ret;
	bool bresult = false;
	StringInfoData sqlstr;

	/*check all node stop*/
	if ((ret = SPI_connect()) < 0)
		ereport(ERROR, (errmsg("ADB Manager SPI_connect failed: error code %d", ret)));
	initStringInfo(&sqlstr);
	appendStringInfo(&sqlstr, "select nodename from mgr_monitor_nodetype_all(%d) where status = true;", nodetype);
	ret = SPI_execute(sqlstr.data, false, 0);
	pfree(sqlstr.data);
	if (ret != SPI_OK_SELECT)
		ereport(ERROR, (errmsg("ADB Manager SPI_execute failed: error code %d", ret)));
	if (SPI_processed > 0 && SPI_tuptable != NULL)
	{
		namestrcpy(nodename, SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1));
		bresult = true;
	}
	SPI_freetuptable(SPI_tuptable);
	SPI_finish();
	
	return bresult;
}