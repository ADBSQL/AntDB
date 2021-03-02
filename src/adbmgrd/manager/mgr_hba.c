	/*
 * commands of hba
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "postgres.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/htup.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/mgr_host.h"
#include "catalog/mgr_node.h"
#include "catalog/mgr_updateparm.h"
#include "catalog/pg_type.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "mgr/mgr_cmds.h"
#include "mgr/mgr_agent.h"
#include "mgr/mgr_msg_type.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "parser/mgr_node.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "funcapi.h"
#include "utils/lsyscache.h"
#include "../../interfaces/libpq/libpq-fe.h"
#include "../../bin/agent/hba_scan.h"

#include "catalog/mgr_hba.h"
#include "utils/formatting.h"


#define SPACE          				" "
#define SPACE_INTERVAL				1
#define HBA_RESULT_COLUMN  			2
#define HBA_ELEM_NUM				6
#define is_digit(c) ((unsigned)(c) - '0' <= 9)
#define SQL_PG_HBA_FILE_RULES "select type,trim(both '{}' from database::varchar), \
		trim(both '{}' from user_name::varchar) \
		,address,netmask,auth_method from pg_hba_file_rules"

typedef enum OperateHbaType
{
	HANDLE_NO = 0,	/*don't do any handle for drop table hba*/
	HBA_ALL=1,		/*drop table hba all*/
	HBA_NODENAME_ALL,/*drop table hba all base on nodename*/
	HBA_ALL_VALUE,
	HBA_NODENAME_VALUE  /*drop table hba all base on hbavalue and nodename*/
}OperateHbaType;

typedef struct TableInfo
{
	Relation rel;
	TableScanDesc rel_scan;
	ListCell  **lcp;
}TableInfo;

#if (Natts_mgr_hba != 3)
#error "need change hba code"
#endif
static TupleDesc common_command_tuple_desc = NULL;
static TupleDesc hba_file_reules_tuple_desc = NULL;

/*--------------------------------------------------------------------*/
void mgr_clean_hba_table(char *coord_name, char *values);
void add_hba_table_to_file(char *coord_name);
/*--------------------------------------------------------------------*/

//extern void mgr_reload_conf(Oid hostoid, char *nodepath);
//extern HbaInfo* parse_hba_file(const char *filename);
/*--------------------------------------------------------------------*/
static void mgr_add_hba_all(char type, char *hbastr, GetAgentCmdRst *err_msg);
static void mgr_add_hba_one(char nodetype, char *nodename, char *zone, char *hbastr, bool record_err_msg,bool is_check_value, GetAgentCmdRst *err_msg);
static void drop_hba_all(GetAgentCmdRst *err_msg);
static void drop_hba_nodename_all(char *coord_name, GetAgentCmdRst *err_msg);
static void drop_hba_all_value(List *args_list, GetAgentCmdRst *err_msg);
static void drop_hba_nodename_valuelist(char *coord_name, List *args_list, GetAgentCmdRst *err_msg);
static void drop_hba_nodename_value(char *coord_name, char *hbavalue, GetAgentCmdRst *err_msg);
static Oid tuple_insert_table_hba(Datum *values, bool *isnull);
static HbaType get_connect_type(char *str_type);
static TupleDesc get_tuple_desc_for_hba(void);
static HeapTuple tuple_form_table_hba(const Name node_name, const char * values);
static void delete_table_hba(char *coord_name, char *values);
static bool check_hba_tuple_exist(char *coord_name, char *values);
static bool check_pghbainfo_vaild(StringInfo hba_info, StringInfo err_msg, bool record_err_msg);
static bool is_auth_method_valid(char *method);
static List *parse_hba_list(List *args_list);
static void joint_hba_send_str(char *hbavalue, StringInfo infosendmsg);
static void joint_hba_table_str(char *hbavalue, StringInfo infomsg);
static bool is_digit_str(char *s_digit);
static bool mgr_type_include(char nodetype, char type);
static int ipv6_mask_to_int (const char *ipstr);
static int ipv4_mask_to_int(const char *prefix);
/*--------------------------------------------------------------------*/
static int bit_count(uint32_t i)
{
	int c = 0;
	unsigned int seen_one = 0;

	while (i > 0) {
		if (i & 1) {
			seen_one = 1;
			c++;
		} else {
			if (seen_one) {
				return -1;
			}
		}
		i >>= 1;
	}

	return c;
}

static int mask2prefix(struct in_addr mask)
{
	return bit_count(ntohl(mask.s_addr));
}

static
int ipv4_mask_to_int(const char *prefix)
{
	int ret;
	struct in_addr in;

	ret = inet_pton(AF_INET, prefix, &in);
	if (ret == 0)
		return -1;

	return mask2prefix(in);
}

static int
ipv6_mask_to_int(const char *ipstr)
{
	int len = 0;
	unsigned char val;
	unsigned char *pnt;
	struct in6_addr ip6;

	if (inet_pton(AF_INET6, ipstr, &ip6) <= 0) {
		ereport(INFO,
			( errmsg("bad IPv6 address: %s\n", ipstr)));
		return -1;
	}

	pnt = (unsigned char *) & ip6;

	while ((*pnt == 0xff) && len < 128)
	{
		len += 8;
		pnt++;
	}

	if (len < 128)
	{
		val = *pnt;
		while (val)
		{
			len++;
			val <<= 1;
		}
	}
	return len;
}

static TupleDesc get_hba_conf_tuple_desc(void)
{
	if(hba_file_reules_tuple_desc == NULL)
	{
		MemoryContext volatile old_context = MemoryContextSwitchTo(TopMemoryContext);
		TupleDesc volatile desc = NULL;
		PG_TRY();
		{
			desc = CreateTemplateTupleDesc(3);
			TupleDescInitEntry(desc, (AttrNumber) 1, "nodetype", NAMEOID, -1, 0);
			TupleDescInitEntry(desc, (AttrNumber) 2, "nodename", NAMEOID, -1, 0);
			TupleDescInitEntry(desc, (AttrNumber) 3, "hbavalue", TEXTOID, -1, 0);
			hba_file_reules_tuple_desc = BlessTupleDesc(desc);
		}PG_CATCH();
		{
			if(desc)
				FreeTupleDesc(desc);
			PG_RE_THROW();
		}PG_END_TRY();
		(void)MemoryContextSwitchTo(old_context);
	}
	Assert(hba_file_reules_tuple_desc);
	return hba_file_reules_tuple_desc;
}

static HeapTuple build_hba_conf_file_tuple(const Form_mgr_node mgr_node, const StringInfoData* resultstrdata)
{
	int i = 0;
	char *value;
	int position = 0;
	StringInfoData hbavaluedata;
	NameData name[2];
	Datum datums[3];
	bool nulls[3];
	TupleDesc desc;
	AssertArg(mgr_node && resultstrdata);
	desc = get_hba_conf_tuple_desc();

	AssertArg(desc && desc->natts == 3
		&& TupleDescAttr(desc, 0)->atttypid == NAMEOID
		&& TupleDescAttr(desc, 1)->atttypid == NAMEOID
		&& TupleDescAttr(desc, 2)->atttypid == TEXTOID);

	if (mgr_node->nodetype == CNDN_TYPE_GTM_COOR_SLAVE || mgr_node->nodetype == CNDN_TYPE_GTM_COOR_MASTER)
		namestrcpy(&name[0], "gtmcoord");
	else if (mgr_node->nodetype == CNDN_TYPE_DATANODE_SLAVE || mgr_node->nodetype == CNDN_TYPE_DATANODE_MASTER)
		namestrcpy(&name[0], "datanode");
	else if (mgr_node->nodetype == CNDN_TYPE_COORDINATOR_SLAVE || mgr_node->nodetype == CNDN_TYPE_COORDINATOR_MASTER)
		namestrcpy(&name[0], "coordinator");
	else
		namestrcpy(&name[0], "unknown nodetype");

	namestrcpy(&name[1], NameStr(mgr_node->nodename));

	initStringInfo(&hbavaluedata);
	position = 0;
	i = 0;
	while(1)
	{
		if(position >= resultstrdata->len)
			break;
		value = &(resultstrdata->data[position]);
		if (*value)
		{
			if (i == 4) /* netmask */
			{
				if(strstr(value, ":"))/* ipv6 */
					appendStringInfo(&hbavaluedata, "%d ", ipv6_mask_to_int(value));
				else
					appendStringInfo(&hbavaluedata, "%d ", ipv4_mask_to_int(value));
				
			}
			else
			{
				appendStringInfo(&hbavaluedata, "%s ", value);	
			}
			
			position = position + strlen(value);
		}
		position = position + 1;
		i++;
		if (i == 6)
		{
			appendStringInfo(&hbavaluedata, "\n");
			i = 0;
		}
	}

	datums[0] = NameGetDatum(&name[0]);
	datums[1] = NameGetDatum(&name[1]);
	datums[2] = CStringGetTextDatum(hbavaluedata.data);

	nulls[0] = nulls[1] = nulls[2] = false;
	pfree(hbavaluedata.data);
	return heap_form_tuple(desc, datums, nulls);
}

Datum mgr_show_hba_all(PG_FUNCTION_ARGS)
{
	HeapTuple tup_result;
	HeapTuple tuple;
	Form_mgr_node mgr_node;
	FuncCallContext *funcctx;
	StringInfoData resultstrdata;
	
	InitNodeInfo *info;
	char *nodestrname;
	List *nodenamelist;
	ScanKeyData key[1];
	char *hostAddr;
	char *user;
	int agentport;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
	
		funcctx = SRF_FIRSTCALL_INIT();

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		info = palloc0(sizeof(*info));

		info->rel_node = table_open(NodeRelationId, AccessShareLock);
		if(!PG_ARGISNULL(0))
		{
			nodenamelist = get_fcinfo_namelist("", 0, fcinfo);
			nodestrname = (char *) lfirst(list_head(nodenamelist));
			ScanKeyInit(&key[0]
				,Anum_mgr_hba_nodename
				,BTEqualStrategyNumber
				,F_NAMEEQ
				,CStringGetDatum(nodestrname));
			info->rel_scan = table_beginscan_catalog(info->rel_node, 1, key);
		}
		else
		{
			info->rel_scan = table_beginscan_catalog(info->rel_node, 0, NULL);
		}

		funcctx->user_fctx = info;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	Assert(funcctx);
	info = funcctx->user_fctx;
	Assert(info);

	//todo rollback
	while ((tuple = heap_getnext(info->rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
		Assert(mgr_node);

		agentport = get_agentPort_from_hostoid(mgr_node->nodehost);
		hostAddr = get_hostaddress_from_hostoid(mgr_node->nodehost);
		user = get_hostuser_from_hostoid(mgr_node->nodehost);
		initStringInfo(&resultstrdata);
		
		monitor_get_stringvalues(AGT_CMD_GET_SQL_STRINGVALUES, agentport, SQL_PG_HBA_FILE_RULES
			, user, hostAddr, mgr_node->nodeport, DEFAULT_DB, &resultstrdata);

		tup_result = build_hba_conf_file_tuple(mgr_node, &resultstrdata);

		pfree(resultstrdata.data);
		pfree(user);
		pfree(hostAddr);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tup_result));
	}

	heap_endscan(info->rel_scan);
	table_close(info->rel_node, AccessShareLock);
	pfree(info);
	SRF_RETURN_DONE(funcctx);
}

Datum mgr_list_hba_by_name(PG_FUNCTION_ARGS)
{
	List *nodenamelist;
	HeapTuple tup_result;
	HeapTuple tuple;
	Form_mgr_hba mgr_hba;
	ScanKeyData key[1];
	FuncCallContext *funcctx;
	TableInfo *info;
	char *nodestrname;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		nodenamelist = NIL;
		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();
		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		/* allocate memory for user context */
		info = palloc(sizeof(*info));
		info->lcp = (ListCell **) palloc(sizeof(ListCell *));
		if(!PG_ARGISNULL(0))
		{
			nodenamelist = get_fcinfo_namelist("", 0, fcinfo);
		}
		*(info->lcp) = list_head(nodenamelist);
		nodestrname = (char *) lfirst(*(info->lcp));
		ScanKeyInit(&key[0]
				,Anum_mgr_hba_nodename
				,BTEqualStrategyNumber
				,F_NAMEEQ
				,CStringGetDatum(nodestrname));
		info->rel = table_open(HbaRelationId, AccessShareLock);
		info->rel_scan = table_beginscan_catalog(info->rel, 1, key);

		funcctx->user_fctx = info;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	Assert(funcctx);
	info = funcctx->user_fctx;
	Assert(info);
	tuple = heap_getnext(info->rel_scan, ForwardScanDirection);
	if(!PointerIsValid(tuple))
	{
		heap_endscan(info->rel_scan);
		table_close(info->rel, AccessShareLock);
		pfree(info);
		SRF_RETURN_DONE(funcctx);
	}
	else
	{
		mgr_hba = (Form_mgr_hba)GETSTRUCT(tuple);
		Assert(mgr_hba);
		tup_result = tuple_form_table_hba(&(mgr_hba->nodename), TextDatumGetCString(&(mgr_hba->hbavalue)));
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tup_result));
	}
}

Datum mgr_add_hba(PG_FUNCTION_ARGS)
{
	GetAgentCmdRst err_msg;
	OperateHbaType handle_type = HANDLE_NO;
	HeapTuple tup_result;
	List *args_list = NIL;
	char *nodename = NULL;
	char *hbastr = NULL;
	char type = CNDN_TYPE_COORDINATOR;
	char nodetype;
	ScanKeyData key[2];
	NameData nodedataname;
	HeapTuple tuple;
	Form_mgr_node mgr_node;
	Relation rel_node;
	TableScanDesc rel_scan;

	err_msg.ret = true;
	initStringInfo(&err_msg.description);
	/*step 1: parase args,and get nodename,hba values;
	if nodename equal '*', add to all the coordinator*/
	if(PG_ARGISNULL(0))
	{
		ereport(ERROR, (errmsg("args is null")));
	}
	args_list = get_fcinfo_namelist("", 0, fcinfo);
	if(args_list->length < 3)
	{
		ereport(ERROR, (errmsg("args is not enough")));
	}
	hbastr = lfirst(list_head(args_list));
	nodename = lsecond(args_list);
	type = llast_int(args_list);

	namestrcpy(&nodedataname, "*");
	if(strcmp(nodename,"*") == 0)
		handle_type = HBA_ALL;
	else
		handle_type = HBA_NODENAME_ALL;
	if(HBA_ALL == handle_type)
	{
		mgr_add_hba_all(type, hbastr, &err_msg);
	}
	else if(HBA_NODENAME_ALL == handle_type)
	{
		rel_node = table_open(NodeRelationId, AccessShareLock);
		namestrcpy(&nodedataname, nodename);
		ScanKeyInit(&key[0]
			,Anum_mgr_node_nodename
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(&nodedataname));
		ScanKeyInit(&key[1]
			,Anum_mgr_node_nodeincluster
			,BTEqualStrategyNumber
			,F_BOOLEQ
			,BoolGetDatum(true));
		rel_scan = table_beginscan_catalog(rel_node, 2, key);
		tuple = heap_getnext(rel_scan, ForwardScanDirection);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR, (errmsg("the node does not exist in cluster")));
		mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
		Assert(mgr_node);
		nodetype = mgr_node->nodetype;
		heap_endscan(rel_scan);
		table_close(rel_node, AccessShareLock);
		if (!mgr_type_include(nodetype, type))
			ereport(ERROR, (errmsg("the node's type is not right")));

		mgr_add_hba_one(nodetype, nodename, NameStr(mgr_node->nodezone), hbastr, true, true, &err_msg);
	}
	/*step 3: show the state of operating drop hba commands */
	tup_result = tuple_form_table_hba(&nodedataname
									,true == err_msg.ret ? "success" : err_msg.description.data);

	pfree(err_msg.description.data);
	return HeapTupleGetDatum(tup_result);
}

static void mgr_add_hba_all(char type, char *hbastr, GetAgentCmdRst *err_msg)
{
	Relation rel;
	TableScanDesc rel_scan;
	Form_mgr_node mgr_node;
	HeapTuple tuple;
	bool record_err_msg = true;

	rel = table_open(NodeRelationId, AccessShareLock);
	rel_scan = table_beginscan_catalog(rel, 0, NULL);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
		Assert(mgr_node);
		if (!mgr_type_include(mgr_node->nodetype, type))
			continue;
		mgr_add_hba_one(mgr_node->nodetype, NameStr(mgr_node->nodename), NameStr(mgr_node->nodezone)
			, hbastr, record_err_msg, true, err_msg);
		record_err_msg = false;
	}

	heap_endscan(rel_scan);
	table_close(rel, AccessShareLock);
}

static void mgr_add_hba_one(char nodetype, char *nodename, char *zone, char *hbastr, bool record_err_msg, bool is_check_exist, GetAgentCmdRst *err_msg)
{
	AppendNodeInfo nodeinfo;

	ListCell *lc, *lc_elem;
	List *list_elem = NIL;
	List *args_list = NIL;
	char *str_elem, *str;
	StringInfoData infosendmsg;
	StringInfoData hbainfomsg;
	StringInfoData hbasendmsg;
	GetAgentCmdRst getAgentCmdRst;
	bool is_exist = false;
	bool is_valid = false;
	Datum datum[Natts_mgr_node];
	bool isnull[Natts_mgr_node];
	memset(datum, 0, sizeof(datum));
	memset(isnull, 0, sizeof(isnull));

	Assert(nodename);
	Assert(hbastr);
	initStringInfo(&getAgentCmdRst.description);
	/*step1: check the nodename is exist in the mgr_node table and make sure it has been initialized*/
	is_valid = get_active_node_info(nodetype, nodename, zone, &nodeinfo);
	if (!is_valid)
	{
		ereport(ERROR, (errmsg("%s \"%s\" is not running normal"
			, mgr_nodetype_str(nodetype), nodename)));
	}

	/*step2: parser the hba values and check whether it's valid*/
	initStringInfo(&infosendmsg);/*send to agent*/
	initStringInfo(&hbasendmsg);/*add to hba value*/
	initStringInfo(&hbainfomsg);/*store one hba contxt*/
	args_list = lappend(args_list, (void *)hbastr);
	foreach(lc, args_list)
	{
		resetStringInfo(&hbainfomsg);
		resetStringInfo(&hbasendmsg);
		str = lfirst(lc);
		joint_hba_send_str(str, &hbasendmsg);
		joint_hba_table_str(str, &hbainfomsg);

		/*check the hba value is valid*/
		is_valid = check_pghbainfo_vaild(&hbasendmsg, &err_msg->description, record_err_msg);
		if(!is_valid)
		{
			if(true == record_err_msg)
			{
				err_msg->ret = false;
				appendStringInfo(&err_msg->description, "in the item \"%s\".\n",str);
			}
			continue;
		}
		if (is_check_exist)
		{
			/*check the value whether exist in the hba table*/
			is_exist = check_hba_tuple_exist(nodename, hbainfomsg.data);
			if(is_exist)
			{
				appendStringInfo(&err_msg->description, "nodename %s with values \"%s\" has existed.\n", nodename, hbainfomsg.data);
				err_msg->ret = false;
				continue;
			}
		}
		/*add to list and remove the same*/
		str_elem = palloc0(hbainfomsg.len + 1);
		memcpy(str_elem, hbainfomsg.data, hbainfomsg.len);
		foreach(lc_elem, list_elem)
		{
			if(strcmp(lfirst(lc_elem), str_elem) == 0)
				break;
		}
		if(PointerIsValid(lc_elem))
		{
			pfree(str_elem);
		}
		else
		{
			appendBinaryStringInfo(&infosendmsg, hbasendmsg.data, hbasendmsg.len);
			list_elem = lappend(list_elem, str_elem);
		}
	}
	if(list_length(list_elem) > 0)
	{
		/*step3: send msg to the specified coordinator to update datanode master's pg_hba.conf*/
		mgr_send_conf_parameters(AGT_CMD_CNDN_REFRESH_PGHBACONF
								,nodeinfo.nodepath
								,&infosendmsg
								,nodeinfo.nodehost
								,&getAgentCmdRst);
		if (!getAgentCmdRst.ret)
		{
			appendStringInfo(&err_msg->description,"add hba %s execute in agent failure\n",nodename);
			appendStringInfo(&err_msg->description,"hba info sync error\n");
			err_msg->ret = false;
		}
		else
		{
			/*step4: execute pgxc_ctl reload to take effect for the new value in the pg_hba.conf  */
			mgr_reload_conf(nodeinfo.nodehost, nodeinfo.nodepath);
		}
	}
	/*check whether insert hba info into table*/
	if (is_check_exist)
	{
		/*step5: add a new tuple to hba table */
		foreach(lc, list_elem)
		{
			str_elem = lfirst(lc);
			datum[Anum_mgr_hba_nodename - 1] = CStringGetDatum(nodename); /* CString compatible Name */
			datum[Anum_mgr_hba_hbavalue - 1] = CStringGetTextDatum(str_elem);
			tuple_insert_table_hba(datum, isnull);
		}
	}
	/*step7: Release an allocated chunk*/
	foreach(lc, list_elem)
	{
		pfree(lfirst(lc));
	}
	list_free(list_elem);
	list_free(args_list);
	pfree(hbainfomsg.data);
	pfree(infosendmsg.data);
	pfree(hbasendmsg.data);
	pfree(getAgentCmdRst.description.data);
}

static Oid tuple_insert_table_hba(Datum *values, bool *isnull)
{
	Relation rel;
	HeapTuple newtuple;
	Oid hba_oid;
	Assert(values && isnull);
	/* now, we can insert record */
	rel = table_open(HbaRelationId, RowExclusiveLock);

	hba_oid = GetNewOidWithIndex(rel, HbaOidIndexId, Anum_mgr_hba_oid);
	values[Anum_mgr_hba_oid-1] = ObjectIdGetDatum(hba_oid);
	isnull[Anum_mgr_hba_oid-1] = false;
	newtuple = heap_form_tuple(RelationGetDescr(rel), values, isnull);
	table_close(rel, RowExclusiveLock);
	heap_freetuple(newtuple);
	return hba_oid;
}

static TupleDesc get_tuple_desc_for_hba(void)
{
	if(common_command_tuple_desc == NULL)
	{
		MemoryContext volatile old_context = MemoryContextSwitchTo(	TopMemoryContext);
		TupleDesc volatile desc = NULL;
		PG_TRY();
		{
			desc = CreateTemplateTupleDesc(HBA_RESULT_COLUMN);

			TupleDescInitEntry(desc, (AttrNumber) 1, "nodename",
							NAMEOID, -1, 0);
			TupleDescInitEntry(desc, (AttrNumber) 2, "values",
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

static HeapTuple tuple_form_table_hba(const Name node_name, const char * values)
{
	Datum datums[HBA_RESULT_COLUMN];
	bool nulls[HBA_RESULT_COLUMN];
	TupleDesc desc;
	AssertArg(node_name && values);
	memset(nulls, false, HBA_RESULT_COLUMN);
	desc = get_tuple_desc_for_hba();

	AssertArg(desc && desc->natts == HBA_RESULT_COLUMN
		&& TupleDescAttr(desc, 0)->atttypid == NAMEOID
		&& TupleDescAttr(desc, 1)->atttypid == TEXTOID);
	datums[0] = NameGetDatum(node_name);
	datums[1] = CStringGetTextDatum(values);
	return heap_form_tuple(desc, datums, nulls);
}

Datum mgr_drop_hba(PG_FUNCTION_ARGS)
{
	GetAgentCmdRst err_msg;
	OperateHbaType handle_type = HANDLE_NO;
	HeapTuple tup_result;
	List *args_list = NIL;
	char *coord_name;
	err_msg.ret = true;
	initStringInfo(&err_msg.description);
	/*step 1: parase args,and get nodename,hba values;
	if nodename equal '*', add to all the coordinator*/
	if(PG_ARGISNULL(0))
	{
		ereport(ERROR, (errmsg("args is null")));
	}
	args_list = get_fcinfo_namelist("", 0, fcinfo);

	if(args_list->length > 0)
	{
		coord_name = llast(args_list);
	}
	else
	{
		ereport(ERROR, (errmsg("args is not enough")));
	}
	if(args_list->length == 1)
	{
		if(strcmp(coord_name,"*") == 0)
			handle_type = HBA_ALL;    /*delete all the hba table*/
		else
			handle_type = HBA_NODENAME_ALL;/*delete the hba table that is own to nodename*/
	}else if(args_list->length > 1)
	{
		if(strcmp(coord_name,"*") == 0)
			handle_type = HBA_ALL_VALUE;         /*delete the hba table which the conext is values*/
		else
			handle_type = HBA_NODENAME_VALUE;    /*delete the hba table which the conext is values and own to nodename*/
	}

	args_list = list_delete(args_list, llast(args_list)); /*remove nodename from list*/

	/*step 2: operating drop table hba  according to the handle_type*/
		/*send drop msg to the agent to delete content of pg_hba.conf */
	initStringInfo(&(err_msg.description));
	switch(handle_type)
	{
		case HBA_ALL: drop_hba_all(&err_msg);
			break;
		case HBA_NODENAME_ALL: drop_hba_nodename_all(coord_name, &err_msg);
			break;
		case HBA_ALL_VALUE: drop_hba_all_value(args_list, &err_msg);
			break;
		case HBA_NODENAME_VALUE: drop_hba_nodename_valuelist(coord_name, args_list, &err_msg);
			break;
		default:ereport(ERROR, (errmsg("operating drop table hba")));
			break;
	}
	/*step 3: show the state of operating drop hba commands */
	tup_result = tuple_form_table_hba((Name)coord_name
									,true == err_msg.ret ? "success" : err_msg.description.data);

	pfree(err_msg.description.data);
	return HeapTupleGetDatum(tup_result);
}

static void drop_hba_all(GetAgentCmdRst *err_msg)
{
	Relation rel;
	HeapTuple tuple;
	TableScanDesc  rel_scan;
	Form_mgr_hba mgr_hba;
	char *coord_name;
	char *hbavalue;
	/*Traverse all the coordinator in the node table*/
	rel = table_open(HbaRelationId, RowExclusiveLock);
	rel_scan = table_beginscan_catalog(rel, 0, NULL);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_hba = (Form_mgr_hba)GETSTRUCT(tuple);
		Assert(mgr_hba);
		coord_name = NameStr(mgr_hba->nodename);
		hbavalue = TextDatumGetCString(&(mgr_hba->hbavalue));
		drop_hba_nodename_value(coord_name, hbavalue, err_msg);
	}
	heap_endscan(rel_scan);
	table_close(rel, RowExclusiveLock);
}
/*
	delete one row form hba talbe base on nodename,
	to delete the nodename which you want.
*/
static void drop_hba_nodename_all(char *coord_name, GetAgentCmdRst *err_msg)
{
	Relation rel;
	ScanKeyData key[1];
	HeapTuple tuple;
	TableScanDesc  rel_scan;
	Form_mgr_hba mgr_hba;
	char *hbavalue;
	Assert(coord_name);

	ScanKeyInit(&key[0]
				,Anum_mgr_hba_nodename
				,BTEqualStrategyNumber
				,F_NAMEEQ
				,CStringGetDatum(coord_name));

	rel = table_open(HbaRelationId, RowExclusiveLock);
	rel_scan = table_beginscan_catalog(rel, 1, key);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_hba = (Form_mgr_hba)GETSTRUCT(tuple);
		Assert(mgr_hba);
		hbavalue = TextDatumGetCString(&(mgr_hba->hbavalue));
		drop_hba_nodename_value(coord_name, hbavalue, err_msg);
	}
	heap_endscan(rel_scan);
	table_close(rel, RowExclusiveLock);
}

static void drop_hba_all_value(List *args_list, GetAgentCmdRst *err_msg)
{
	Relation rel;
	HeapTuple tuple;
	TableScanDesc  rel_scan;
	Form_mgr_hba mgr_hba;
	char *coord_name;
	bool is_exist = false;
	bool tuple_exist = false;
	List *name_list = NIL;
	ListCell *lc_value, *lc_name;
	/*Traverse all the coordinator in the node table*/
	rel = table_open(HbaRelationId, AccessShareLock);
	rel_scan = table_beginscan_catalog(rel, 0, NULL);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_hba = (Form_mgr_hba)GETSTRUCT(tuple);
		Assert(mgr_hba);
		tuple_exist = true;
		coord_name = NameStr(mgr_hba->nodename);
		is_exist = false;
		foreach(lc_name, name_list)
		{
			if(strcmp(lfirst(lc_name), coord_name) == 0)
			{
				is_exist = true;
				break;
			}
		}
		if(false == is_exist)
			name_list = lappend(name_list, coord_name);
	}
	heap_endscan(rel_scan);
	table_close(rel, AccessShareLock);
	if(false == tuple_exist)
	{
		appendStringInfo(&(err_msg->description), "%s", "Error: the hba talbe is empty.\n");
		err_msg->ret = false;
		return;
	}
	foreach(lc_name, name_list)
	{
		coord_name = lfirst(lc_name);
		foreach(lc_value, args_list)
		{
			drop_hba_nodename_value(coord_name, lfirst(lc_value), err_msg);
		}
	}
}
static void drop_hba_nodename_valuelist(char *coord_name, List *args_list, GetAgentCmdRst *err_msg)
{
	ListCell *lc;
	List *hba_list = NIL;
	char *hbavalue;
	hba_list = parse_hba_list(args_list);
	foreach(lc, hba_list)
	{
		hbavalue = lfirst(lc);
		drop_hba_nodename_value(coord_name, hbavalue, err_msg);
	}
	/* Release an allocated chunk*/
	foreach(lc, hba_list)
	{
		pfree(lfirst(lc));
	}
	list_free(hba_list);
}
/*
	delete one row form hba talbe base on nodename and value,
	if success return true,else ruturn false;
*/
static void drop_hba_nodename_value(char *coord_name, char *hbavalue, GetAgentCmdRst *err_msg)
{
	Relation rel;
	HeapTuple tuple;
	Form_mgr_node mgr_node;
	char * node_path;
	Datum datumPath;
	Oid hostoid;
	StringInfoData infosendmsg;
	bool isNull = false;
	GetAgentCmdRst getAgentCmdRst;

	Assert(coord_name);
	initStringInfo(&getAgentCmdRst.description);
	/*step1: check the nodename is exist in the mgr_node table and make sure it has been initialized*/
	rel = table_open(NodeRelationId, AccessShareLock);
	tuple = mgr_get_tuple_node_from_name_type(rel, coord_name);
	if(!(HeapTupleIsValid(tuple)))
	{
		 ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
				 ,errmsg("coordinator\"%s\" does not exist", coord_name)));
	}
	mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
	Assert(mgr_node);
	if(false == mgr_node->nodeinited)
	{
		delete_table_hba(coord_name, hbavalue);
		table_close(rel, AccessShareLock);
		heap_freetuple(tuple);
		return;
	}
	hostoid = mgr_node->nodehost;
	datumPath = heap_getattr(tuple, Anum_mgr_node_nodepath, RelationGetDescr(rel), &isNull);
	if(isNull)
	{
		 ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
				 ,errmsg("coordinator\"%s\" does not exist nodepath", coord_name)));
	}
	node_path = TextDatumGetCString(datumPath);
	table_close(rel, AccessShareLock);
	heap_freetuple(tuple);
	/*step2: parser the hba values and check whether it's valid*/
	initStringInfo(&infosendmsg);/*send to agent*/
	joint_hba_send_str(hbavalue, &infosendmsg);
	if(check_hba_tuple_exist(coord_name, hbavalue) == false)
	{
		pfree(infosendmsg.data);
		appendStringInfo(&err_msg->description, "coordinator \"%s\" with \"%s\" does not exist\n",coord_name, hbavalue);
		err_msg->ret = false;
		return;
	}
	if(check_pghbainfo_vaild(&infosendmsg, &(err_msg->description), true) == false)
	{
		err_msg->ret = false;
		appendStringInfo(&err_msg->description, "in the item \"%s\".\n",hbavalue);
		pfree(infosendmsg.data);
		return;
	}
	/*step3: send msg to the specified coordinator to update datanode master's pg_hba.conf*/
	mgr_send_conf_parameters(AGT_CMD_CNDN_DELETE_PGHBACONF
							,node_path
							,&infosendmsg
							,hostoid
							,&getAgentCmdRst);
	if (!getAgentCmdRst.ret)
	{
		appendStringInfo(&err_msg->description,"drop hba %s failure\n",coord_name);
		appendStringInfo(&err_msg->description, "%s\n",getAgentCmdRst.description.data);
		err_msg->ret = false;
	}
	else
	{
		/*step4: execute pgxc_ctl reload to the specified host */
		mgr_reload_conf(hostoid, node_path);
		/*step5:delete tuple of hba table*/
		delete_table_hba(coord_name, hbavalue);
	}
	pfree(infosendmsg.data);
	pfree(getAgentCmdRst.description.data);
}

/*
if coord_name = "*"
	we delete all the hba table;
else if coord_name is valid and the values is NULL
	we will delete the table where coord_name is equal to coordinator name
else
	we will delete the table by coord_name and values
*/
static void delete_table_hba(char *coord_name, char *values)
{
	Relation rel;
	TableScanDesc rel_scan;
	HeapTuple tuple;
	ScanKeyData scankey[2];
	bool is_check_value = false;
	Assert(coord_name);

	ScanKeyInit(&scankey[0]
			,Anum_mgr_hba_nodename
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(coord_name));
	if(values != NULL)
	{
		is_check_value = true;
		ScanKeyInit(&scankey[1]
				,Anum_mgr_hba_hbavalue
				,BTEqualStrategyNumber
				,F_TEXTEQ
				,CStringGetTextDatum(values));
	}
	rel = table_open(HbaRelationId, RowExclusiveLock);
	if(strcmp(coord_name, "*") == 0)
	{
		if(true == is_check_value)
			rel_scan = table_beginscan_catalog(rel, 1, &scankey[1]);
		else
			rel_scan = table_beginscan_catalog(rel, 0, NULL);
	}
	else
	{
		if(true == is_check_value)
			rel_scan = table_beginscan_catalog(rel, 2, scankey);
		else
			rel_scan = table_beginscan_catalog(rel, 1, &scankey[0]);

	}
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		CatalogTupleDelete(rel, &tuple->t_self);
	}

	heap_endscan(rel_scan);
	table_close(rel, RowExclusiveLock);
}

void mgr_clean_hba_table(char *coord_name, char *values)
{
	delete_table_hba(coord_name, values);
}
/*
To update the data in the hba table to the specified pg_hba.conf file
if coord_name == "*" update all the table
else
	update the specified coordinator

This function is called when the ADB cluster is initialized
*/
void add_hba_table_to_file(char *coord_name)
{
	Relation rel;
	HeapTuple tuple;
	TableScanDesc  rel_scan;
	Form_mgr_hba mgr_hba;
	ScanKeyData scankey[1];
	char *hba_value;
	char nodetype;
	GetAgentCmdRst err_msg;
	NameData nodenamedata;
	NameData zone;

	Assert(coord_name);
	initStringInfo(&(err_msg.description));
	err_msg.ret = true;
	/*Traverse all the coordinator in the node table*/
	ScanKeyInit(&scankey[0]
			,Anum_mgr_hba_nodename
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(coord_name));
	rel = table_open(HbaRelationId, AccessShareLock);
	if(strcmp(coord_name, "*") == 0)
		rel_scan = table_beginscan_catalog(rel, 0, NULL);
	else
		rel_scan = table_beginscan_catalog(rel, 1, &scankey[0]);

	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		mgr_hba = (Form_mgr_hba)GETSTRUCT(tuple);
		Assert(mgr_hba);
		coord_name = NameStr(mgr_hba->nodename);
		hba_value = TextDatumGetCString(&(mgr_hba->hbavalue));
		namestrcpy(&nodenamedata, coord_name);
		nodetype = mgr_get_nodetype(&nodenamedata);
		mgr_get_nodezone(&nodenamedata, &zone);
		if (nodetype == CNDN_TYPE_NONE)
		{
			ereport(ERROR, (errmsg("illegal hba info \"%s\" of \"%s\", please check table mgr_hba", hba_value, NameStr(mgr_hba->nodename))));
		}
		mgr_add_hba_one(nodetype, (char *)coord_name, NameStr(zone), hba_value, true, false, &err_msg);
		if (!err_msg.ret)
		{
			ereport(ERROR, (errmsg("add hba info \"%s\" to coordinator \"%s\"", hba_value, coord_name)));
		}
		resetStringInfo(&(err_msg.description));
		err_msg.ret = true;
	}
	heap_endscan(rel_scan);
	table_close(rel, AccessShareLock);
	pfree(err_msg.description.data);
}

/*
	2,if coord_name == "*",it will check all the table,
	else only select the nodname is coord_name in the table;
	3,if values is NULL,it won't be query
*/
static bool check_hba_tuple_exist(char *coord_name, char *values)
{
	Relation rel;
	TableScanDesc rel_scan;
	HeapTuple tuple;
	ScanKeyData scankey[2];
	bool ret = false;
	bool is_check_value = true;
	Assert(coord_name);

	if(values == NULL)
		is_check_value = false;
	ScanKeyInit(&scankey[0]
			,Anum_mgr_hba_nodename
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(coord_name));
	ScanKeyInit(&scankey[1]
			,Anum_mgr_hba_hbavalue
			,BTEqualStrategyNumber
			,F_TEXTEQ
			,CStringGetTextDatum(values));
	rel = table_open(HbaRelationId, AccessShareLock);
	if(strcmp(coord_name, "*") == 0)
	{
		if(true == is_check_value)
			rel_scan = table_beginscan_catalog(rel, 1, &scankey[1]);
		else
			rel_scan = table_beginscan_catalog(rel, 0, NULL);
	}
	else
	{
		if(true == is_check_value)
			rel_scan = table_beginscan_catalog(rel, 2, scankey);
		else
			rel_scan = table_beginscan_catalog(rel, 1, &scankey[0]);

	}

	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		ret = true;
	}
	heap_endscan(rel_scan);
	table_close(rel, AccessShareLock);
	return ret;
}

static HbaType get_connect_type(char *str_type)
{
	HbaType conntype = HBA_TYPE_EMPTY;
	char *str_lwr ;
//	Assert(str_type);
	if(!PointerIsValid(str_type))
		return conntype;
	str_lwr = str_tolower(str_type, strlen(str_type), DEFAULT_COLLATION_OID);
	if(strcmp(str_lwr, "host") == 0)
		conntype = HBA_TYPE_HOST;
	else if(strcmp(str_lwr, "hostssl") == 0)
		conntype = HBA_TYPE_HOSTSSL;
	else if(strcmp(str_lwr, "hostnossl") == 0)
		conntype = HBA_TYPE_HOSTNOSSL;

	return conntype;
}

static bool check_pghbainfo_vaild(StringInfo hba_info, StringInfo err_msg, bool record_err_msg)
{
	bool is_valid = true;
	HbaInfo *newinfo;
	char *str_elem;
	StringInfoData str_hbainfo;
	int count_elem;
	int ipaddr;
	int str_len = 0;
	bool bipv4 = false;
	bool bipv6 = false;
	MemoryContext pgconf_context;
	MemoryContext oldcontext;
	pgconf_context = AllocSetContextCreate(CurrentMemoryContext,
										   "pghbaadd",
										   ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(pgconf_context);
	newinfo = palloc(sizeof(HbaInfo));
	initStringInfo(&str_hbainfo);
	count_elem = 0;
	str_elem = hba_info->data;
	while(1)
	{
		count_elem = count_elem + 1;
		str_len = strlen(str_elem);
		if(0 == str_len)
			str_len = 1;
		hba_info->cursor = hba_info->cursor + str_len + 1;
		str_elem = &(hba_info->data[hba_info->cursor]);
		if(hba_info->cursor >= hba_info->len)
			break;
	}
	if(count_elem != HBA_ELEM_NUM)
	{
		is_valid = false;
		if(true == record_err_msg)
			appendStringInfoString(err_msg, "Error:\"number of hba item fields is incorrect\"\n");
		goto func_end;
	}
	hba_info->cursor = 0;

	/*type*/
	newinfo->type = hba_info->data[hba_info->cursor];
	hba_info->cursor = hba_info->cursor + sizeof(char) + 1;
	/*database*/
	newinfo->database = &(hba_info->data[hba_info->cursor]);
	hba_info->cursor = hba_info->cursor + strlen(newinfo->database) + 1;
	/*user*/
	newinfo->user = &(hba_info->data[hba_info->cursor]);
	hba_info->cursor = hba_info->cursor + strlen(newinfo->user) + 1;
	/*ip*/
	newinfo->addr = &(hba_info->data[hba_info->cursor]);
	hba_info->cursor = hba_info->cursor + strlen(newinfo->addr) + 1;
	/*mask*/
	if(is_digit_str(&(hba_info->data[hba_info->cursor])))
		newinfo->addr_mark = atoi(&(hba_info->data[hba_info->cursor]));
	else
	{
		is_valid = false;
		if(true == record_err_msg)
			appendStringInfoString(err_msg, "Error:\"the mask is invalid\"\n");
		goto func_end;
	}
	hba_info->cursor = hba_info->cursor + strlen(&(hba_info->data[hba_info->cursor])) + 1;
	/*method*/
	newinfo->auth_method = &(hba_info->data[hba_info->cursor]);
	if(HBA_TYPE_EMPTY == newinfo->type)
	{
		is_valid = false;
		if(true == record_err_msg)
			appendStringInfoString(err_msg, "Error:\"the conntype is invalid\"\n");
		goto func_end;
	}

	if(inet_pton(AF_INET, newinfo->addr, &ipaddr) == 1)
		bipv4 = true;
	else if (inet_pton(AF_INET6, newinfo->addr, &ipaddr) == 1)
		bipv6 = true;
	if ((!bipv6) && (!bipv4))
	{
		is_valid = false;
		if(true == record_err_msg)
			appendStringInfoString(err_msg, "Error:\"the address is invaild\"\n");
		goto func_end;
	}
	if((bipv4 && (newinfo->addr_mark < 0 || newinfo->addr_mark > 32))
		|| (bipv6 && (newinfo->addr_mark < 0 || newinfo->addr_mark > 128)))
	{
		is_valid = false;
		if(true == record_err_msg)
			appendStringInfoString(err_msg, "Error:\"the ip mask is invaild\"\n");
		goto func_end;
	}

	if(!is_auth_method_valid(newinfo->auth_method))
	{
		is_valid = false;
		if(true == record_err_msg)
			appendStringInfoString(err_msg, "Error:\"the auth_method is invaild\"\n");
		goto func_end;
	}
func_end:
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(pgconf_context);
	return is_valid;
}

static bool is_auth_method_valid(char *method)
{
	char *AuthMethod[13] = {"trust", "reject", "md5", "password", "gss", "sspi"
						  , "krb5", "ident", "ldap", "cert", "pam", "peer", "radius"};
	int i = 0;
	char *method_lwr;
	Assert(method);
	method_lwr = str_tolower(method, strlen(method), DEFAULT_COLLATION_OID);
	for(i = 0; i < 13; ++i)
	{
		if(strcmp(method_lwr, AuthMethod[i]) == 0)
			return true;
	}
	return false;
}


static List *parse_hba_list(List *args_list)
{
	StringInfoData hbainfomsg;
	StringInfoData split_str;
	List *list_elem = NIL;
	ListCell *lc, *lc_elem;
	char *str, *str_elem;
	char *str_remain;
	bool is_exist = true;
	/*step2: parser the hba values and check whether it's valid*/
	initStringInfo(&split_str);
	initStringInfo(&hbainfomsg);/*add to hba table*/
	foreach(lc, args_list)
	{
		resetStringInfo(&hbainfomsg);
		resetStringInfo(&split_str);
		str = lfirst(lc);
		appendStringInfo(&split_str, "%s", str);
		str_elem = strtok_r(split_str.data, SPACE, &str_remain);
		appendStringInfo(&hbainfomsg, "%s", str_elem);
		while(str_elem != NULL)
		{
			str_elem = strtok_r(NULL, SPACE, &str_remain);
			if(PointerIsValid(str_elem))
			{
				appendStringInfoSpaces(&hbainfomsg, SPACE_INTERVAL);
				appendStringInfo(&hbainfomsg, "%s",str_elem);
			}
		}
		/*add to list and remove the same*/
		str_elem = palloc0(hbainfomsg.len + 1);
		memcpy(str_elem, hbainfomsg.data, hbainfomsg.len);
		is_exist = false;
		foreach(lc_elem, list_elem)
		{
			str = lfirst(lc_elem);
			if(strcmp(str, str_elem) == 0)
			{
				is_exist = true;
				break;
			}
		}
		if(false == is_exist)
		{
			list_elem = lappend(list_elem, str_elem);
		}
		else
		{
			pfree(str_elem);
		}
	}
	pfree(hbainfomsg.data);
	pfree(split_str.data);
	return list_elem;
}

static void joint_hba_send_str(char *hbavalue, StringInfo infosendmsg)
{
	char *split_str;
	HbaType conntype = HBA_TYPE_EMPTY;
	char *str_remain = NULL;
	char *str_elem = NULL;
	Assert(hbavalue);
	split_str = palloc0(strlen(hbavalue)+1);
	memcpy(split_str, hbavalue, strlen(hbavalue)+1);
	str_elem = strtok_r(split_str, SPACE, &str_remain);
	conntype = get_connect_type(str_elem);
	appendStringInfo(infosendmsg, "%c%c", conntype, '\0');
	while(str_elem != NULL)
	{
		str_elem = strtok_r(NULL, SPACE, &str_remain);
		if(PointerIsValid(str_elem))
		{
			appendStringInfo(infosendmsg,"%s%c", str_elem, '\0');
		}
	}
	pfree(split_str);
}

static void joint_hba_table_str(char *hbavalue, StringInfo infomsg)
{
	char *split_str;
	char *str_remain;
	char *str_elem;
	Assert(hbavalue);
	split_str = palloc0(strlen(hbavalue)+1);
	memcpy(split_str, hbavalue, strlen(hbavalue)+1);
	str_elem = strtok_r(split_str, SPACE, &str_remain);
	appendStringInfo(infomsg, "%s", str_elem);
	while(str_elem != NULL)
	{
		str_elem = strtok_r(NULL, SPACE, &str_remain);
		if(PointerIsValid(str_elem))
		{
			appendStringInfoSpaces(infomsg, SPACE_INTERVAL);
			appendStringInfo(infomsg, "%s", str_elem);
		}
	}
	pfree(split_str);
}

static bool is_digit_str(char *s_digit)
{
	int length = 0;
	int i = 0;
	length = strlen(s_digit);
	for(i = 0; i < length; ++i)
	{
		if(!isdigit(s_digit[i]))
			break;
	}
	if(i < length)
		return false;
	else
		return true;
}

static bool mgr_type_include(char nodetype, char type)
{
	if (type == CNDN_TYPE_GTMCOOR)
	{
		if (nodetype == CNDN_TYPE_GTM_COOR_MASTER
			|| nodetype == CNDN_TYPE_GTM_COOR_SLAVE)
			return true;
	}
	else if (type == CNDN_TYPE_COORDINATOR)
	{
		if (nodetype == CNDN_TYPE_COORDINATOR_MASTER
			|| nodetype == CNDN_TYPE_COORDINATOR_SLAVE)
			return true;
	}
	else
	{
		if (nodetype == CNDN_TYPE_DATANODE_MASTER
			|| nodetype == CNDN_TYPE_DATANODE_SLAVE)
			return true;
	}

	return false;
}
