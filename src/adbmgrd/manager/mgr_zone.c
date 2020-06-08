/*
 * commands of zone
 */
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include<pwd.h>

#include "postgres.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/mgr_host.h"
#include "catalog/pg_authid.h"
#include "catalog/mgr_node.h"
#include "catalog/mgr_updateparm.h"
#include "catalog/mgr_parm.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "mgr/mgr_cmds.h"
#include "mgr/mgr_agent.h"
#include "mgr/mgr_msg_type.h"
#include "mgr/mgr_helper.h"
#include "mgr/mgr_switcher.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "parser/mgr_node.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/acl.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "funcapi.h"
#include "fmgr.h"
#include "utils/lsyscache.h"
#include "executor/spi.h"
#include "../../interfaces/libpq/libpq-fe.h"
#include "nodes/makefuncs.h"
#include "access/xlog.h"
#include "nodes/nodes.h"

char *mgr_zone;

#define MGR_PGEXEC_DIRECT_EXE_UTI_RET_COMMAND_OK	0 

static void MgrZoneFailoverGtm(MemoryContext spiContext, char *currentZone);
static void MgrZoneFailoverCoord(MemoryContext spiContext, char *currentZone);
static void MgrZoneFailoverDN(MemoryContext spiContext, char *currentZone);
static void MgrGetOldDnMasterNotZone(MemoryContext spiContext, char *currentZone, char nodeType, dlist_head *masterList);
static MgrNodeWrapper *MgrGetOldGtmMasterNotZone(MemoryContext spiContext, char *currentZone);
static void MgrFailoverCheck(MemoryContext spiContext, char *currentZone);
static void MgrCheckMasterHasSlave(MemoryContext spiContext, char *currentZone);
static void MgrCheckMasterHasSlaveCnDn(MemoryContext spiContext, char *currentZone, char nodeType);
static void MgrMakesureAllSlaveRunning(void);
static void MgrZoneUpdateOtherZoneMgrNode(Relation relNode, char *currentZone);
static bool mgr_zone_has_node(const char *zonename, char nodetype);
static void mgr_zone_has_all_masternode(char *currentZone);
static void mgr_make_sure_allmaster_running(void);

Datum mgr_zone_failover(PG_FUNCTION_ARGS)
{
	HeapTuple 		tupResult = NULL;
	NameData 		name;
	char 			*currentZone;
	int 			spiRes = 0;	
	MemoryContext 	spiContext = NULL;

	if (RecoveryInProgress())
		ereport(ERROR, (errmsg("cannot do the command during recovery")));

	currentZone  = PG_GETARG_CSTRING(0);
	Assert(currentZone);
	if (strcmp(currentZone, mgr_zone) != 0)
		ereport(ERROR, (errmsg("the given zone name \"%s\" is not the same wtih guc parameter mgr_zone \"%s\" in postgresql.conf", currentZone, mgr_zone)));	

	namestrcpy(&name, "zone promote");
	PG_TRY();
	{
		if ((spiRes = SPI_connect()) != SPI_OK_CONNECT){
			ereport(ERROR, (errmsg("SPI_connect failed, connect return:%d",	spiRes)));
		}
		spiContext = CurrentMemoryContext; 		
		MgrFailoverCheck(spiContext, currentZone);

		ereportNoticeLog(errmsg("ZONE FAILOVER %s, step1:failover gtmcoord slave in zone(%s).", currentZone, currentZone));
		MgrZoneFailoverGtm(spiContext, currentZone);

		ereportNoticeLog(errmsg("ZONE FAILOVER %s, step2:failover coordinator slave in zone(%s).", currentZone, currentZone));
		MgrZoneFailoverCoord(spiContext, currentZone);

		ereportNoticeLog(errmsg("ZONE FAILOVER %s, step3:failover datanode slave in zone(%s).", currentZone, currentZone));
		MgrZoneFailoverDN(spiContext, currentZone);
	}PG_CATCH();
	{
		SPI_finish();
		ereport(ERROR, (errmsg(" ZONE FAILOVER zone(%s) failed.", currentZone)));
		PG_RE_THROW();
	}PG_END_TRY();

	SPI_finish();
	ereportNoticeLog(errmsg("the command of \"ZONE FAILOVER %s\" result is %s, description is %s", currentZone,"true", "success"));
	tupResult = build_common_command_tuple(&name, true, "success");
	return HeapTupleGetDatum(tupResult);
}

Datum mgr_zone_config_all(PG_FUNCTION_ARGS)
{
	Relation 		relNode  = NULL;
	HeapTuple 		tupResult= NULL;
	StringInfoData 	resultmsg;
	ScanKeyData 	key[3];
	NameData 		name;	
	char 			*currentZone= NULL;
	bool 			bres 	    = true;
	PGconn 			*gtm_conn   = NULL;
	Oid 			cnoid;
	
	if (RecoveryInProgress())
		ereport(ERROR, (errmsg("cannot do the command during recovery")));
	
	namestrcpy(&name, "zone config");
	currentZone  = PG_GETARG_CSTRING(0);
	Assert(currentZone);

	if (strcmp(currentZone, mgr_zone) !=0)
		ereport(ERROR, (errmsg("the given zone name \"%s\" is not the same wtih guc parameter mgr_zone \"%s\" in postgresql.conf", currentZone, mgr_zone)));
	
	mgr_zone_has_all_masternode(currentZone);
    mgr_make_sure_allmaster_running();
	
	initStringInfo(&resultmsg);
	ScanKeyInit(&key[0]
				,Anum_mgr_node_nodeincluster
				,BTEqualStrategyNumber
				,F_BOOLEQ
				,BoolGetDatum(true));
	ScanKeyInit(&key[1]
			,Anum_mgr_node_nodezone
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(currentZone));

	PG_TRY();
	{
		relNode = table_open(NodeRelationId, RowExclusiveLock);
		mgr_get_gtmcoord_conn(mgr_zone, MgrGetDefDbName(), &gtm_conn, &cnoid);
		hexp_pqexec_direct_execute_utility(gtm_conn, SQL_BEGIN_TRANSACTION, MGR_PGEXEC_DIRECT_EXE_UTI_RET_COMMAND_OK);
 	    hexp_pqexec_direct_execute_utility(gtm_conn, SQL_COMMIT_TRANSACTION, MGR_PGEXEC_DIRECT_EXE_UTI_RET_COMMAND_OK);
		
	}PG_CATCH();
	{
		ClosePgConn(gtm_conn);
		table_close(relNode, RowExclusiveLock);
		MgrFree(resultmsg.data);
		PG_RE_THROW();
	}PG_END_TRY();

	ClosePgConn(gtm_conn);
	table_close(relNode, RowExclusiveLock);	
	tupResult = build_common_command_tuple(&name, bres, resultmsg.len == 0 ? "success":resultmsg.data);
	MgrFree(resultmsg.data);
	ereport(LOG, (errmsg("the command of \"ZONE CONFIG %s\" result is %s, description is %s", currentZone
		,bres ? "true":"false", resultmsg.len == 0 ? "success":resultmsg.data)));
	return HeapTupleGetDatum(tupResult);
}
/*
* mgr_zone_clear
* clear the tuple which is not in the current zone
*/
Datum mgr_zone_clear(PG_FUNCTION_ARGS)
{
	Relation		relNode = NULL;
	TableScanDesc	relScan  = NULL;
	ScanKeyData	key[2];
	HeapTuple		tuple = NULL;
	Form_mgr_node	mgr_node = NULL;
	char		   *zone = NULL;

	if (RecoveryInProgress())
		ereport(ERROR, (errmsg("cannot do the command during recovery")));

	zone  = PG_GETARG_CSTRING(0);
	Assert(zone);

	if (strcmp(zone, mgr_zone) !=0)
		ereport(ERROR, (errmsg("the given zone name \"%s\" is not the same wtih guc parameter mgr_zone \"%s\" in postgresql.conf", zone, mgr_zone)));

	ereportNoticeLog(errmsg("make the special node as master type and set its master name is null, sync_state is null on node table in zone \"%s\"", zone));
	ScanKeyInit(&key[0]
				,Anum_mgr_node_nodeincluster
				,BTEqualStrategyNumber
				,F_BOOLEQ
				,BoolGetDatum(true));
	ScanKeyInit(&key[1]
			,Anum_mgr_node_nodezone
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(zone));

	PG_TRY();
	{
		relNode = table_open(NodeRelationId, RowExclusiveLock);
		relScan = table_beginscan_catalog(relNode, 2, key);
		while((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
		{
			mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
			Assert(mgr_node);
			if (mgr_checknode_in_currentzone(zone, mgr_node->nodemasternameoid))
				continue;
			ereportNoticeLog(errmsg("make the node \"%s\" as master type on node table in zone \"%s\"", NameStr(mgr_node->nodename), zone));
			mgr_node->nodetype = mgr_get_master_type(mgr_node->nodetype);
			namestrcpy(&(mgr_node->nodesync), "");
			mgr_node->nodemasternameoid = 0;
			heap_inplace_update(relNode, tuple);
		}
		EndScan(relScan);

		ereportNoticeLog(errmsg("on node table, drop the node which is not in zone \"%s\"", zone));
		MgrZoneUpdateOtherZoneMgrNode(relNode, zone);
	}PG_CATCH();
	{
		EndScan(relScan);	
		table_close(relNode, RowExclusiveLock);	
		PG_RE_THROW();
	}PG_END_TRY();

	table_close(relNode, RowExclusiveLock);
	PG_RETURN_BOOL(true);
}

Datum mgr_zone_init(PG_FUNCTION_ARGS)
{
	Relation		relNode  = NULL;
	HeapTuple		tuple    = NULL;
	HeapTuple		tupResult= NULL;
	TableScanDesc	relScan  = NULL;
	char		   *coordMaster = NULL;
	char			*currentZone= NULL;
	NameData		name;
	Form_mgr_node	mgrNode;
	ScanKeyData		key[3];
	StringInfoData	strerr;
	bool			res = true;

	if (RecoveryInProgress())
		ereport(ERROR, (errmsg("cannot do the command during recovery")));

	namestrcpy(&name, "zone init");
	currentZone = PG_GETARG_CSTRING(0);
	initStringInfo(&strerr);
	
	ScanKeyInit(&key[0]
				,Anum_mgr_node_nodeinited
				,BTEqualStrategyNumber
				,F_BOOLEQ
				,BoolGetDatum(false));
	ScanKeyInit(&key[1]
				,Anum_mgr_node_nodeincluster
				,BTEqualStrategyNumber
				,F_BOOLEQ
				,BoolGetDatum(false));
	ScanKeyInit(&key[2]
			,Anum_mgr_node_nodezone
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(currentZone));
	PG_TRY();
	{
		relNode = table_open(NodeRelationId, RowExclusiveLock);
		relScan = table_beginscan_catalog(relNode, 3, key);
		while((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
		{
			mgrNode = (Form_mgr_node)GETSTRUCT(tuple);
			Assert(mgrNode);
			if (mgrNode->nodetype == CNDN_TYPE_GTM_COOR_SLAVE)
			{
				if (mgr_append_agtm_slave_func(NameStr(mgrNode->nodename))){
					ereportNoticeLog(errmsg("append gtmcoord slave %s success.", NameStr(mgrNode->nodename)));		
				}
				else{
					res = false;
					ereportWarningLog(errmsg("append gtmcoord slave %s failed.", NameStr(mgrNode->nodename)));		
				}
			}
			else if (mgrNode->nodetype == CNDN_TYPE_COORDINATOR_SLAVE)
			{
				coordMaster = mgr_get_mastername_by_nodename_type(NameStr(mgrNode->nodename), CNDN_TYPE_COORDINATOR_SLAVE);
				if (mgr_append_coord_slave_func(coordMaster, NameStr(mgrNode->nodename), &strerr)){
					ereportNoticeLog(errmsg("append coordinator slave %s success.", NameStr(mgrNode->nodename)));		
				}
				else{
					res = false;
					ereportWarningLog(errmsg("append coordinator slave %s failed.", NameStr(mgrNode->nodename)));
				}
				MgrFree(coordMaster);
			}
			else if (mgrNode->nodetype == CNDN_TYPE_DATANODE_SLAVE)
			{
				if (mgr_append_dn_slave_func(NameStr(mgrNode->nodename))){
					ereportNoticeLog(errmsg("append datanode slave %s success.", NameStr(mgrNode->nodename)));		
				}
				else{
					res = false;
					ereportWarningLog(errmsg("append datanode slave %s failed.", NameStr(mgrNode->nodename)));
				}
			}		
		}
	}PG_CATCH();
	{
		MgrFree(strerr.data);
		EndScan(relScan);
		table_close(relNode, RowExclusiveLock);
		PG_RE_THROW();
	}PG_END_TRY();

	MgrFree(strerr.data);
	EndScan(relScan);
	table_close(relNode, RowExclusiveLock);
	tupResult = build_common_command_tuple(&name, res, res ? "success" : "failed");
	return HeapTupleGetDatum(tupResult);
}

static void MgrCheckMasterHasSlaveCnDn(MemoryContext spiContext, char *currentZone, char nodeType)
{
	dlist_head 			masterList = DLIST_STATIC_INIT(masterList);
	dlist_head 			slaveList  = DLIST_STATIC_INIT(slaveList);
	dlist_iter 			iter;
	MgrNodeWrapper      *mgrNode = NULL;
	Assert(spiContext);
	Assert(currentZone);

	PG_TRY();
	{
		MgrGetOldDnMasterNotZone(spiContext, currentZone, nodeType, &masterList);
		dlist_foreach(iter, &masterList)
		{
			mgrNode = dlist_container(MgrNodeWrapper, link, iter.cur);
			dlist_init(&slaveList);
			selectActiveMgrSlaveNodesInZone(mgrNode->oid, getMgrSlaveNodetype(mgrNode->form.nodetype), currentZone, spiContext, &slaveList);
			if (dlist_is_empty(&slaveList)){
				ereport(ERROR, (errmsg("no %s in zone(%s) can't promote.", mgr_nodetype_str(mgrNode->form.nodetype), currentZone)));
			}
			pfreeMgrNodeWrapperList(&slaveList, NULL);
		}
	}PG_CATCH();
	{
		pfreeMgrNodeWrapperList(&masterList, NULL);
		pfreeMgrNodeWrapperList(&slaveList, NULL);
		PG_RE_THROW();
	}PG_END_TRY();
	pfreeMgrNodeWrapperList(&masterList, NULL);
}
static void MgrCheckMasterHasSlave(MemoryContext spiContext, char *currentZone)
{
	MgrNodeWrapper 	*oldMaster = NULL;
	dlist_head 		activeNodes = DLIST_STATIC_INIT(activeNodes);
	Assert(spiContext);
	Assert(currentZone);

	oldMaster = MgrGetOldGtmMasterNotZone(spiContext, currentZone);
	Assert(oldMaster);
	selectActiveMgrSlaveNodesInZone(oldMaster->oid, getMgrSlaveNodetype(oldMaster->form.nodetype), currentZone, spiContext, &activeNodes);
	if (dlist_is_empty(&activeNodes)){
		ereport(ERROR, (errmsg("no gtmcoord slave in zone(%s) can't promote.", currentZone)));
	}

	MgrCheckMasterHasSlaveCnDn(spiContext, currentZone, CNDN_TYPE_COORDINATOR_MASTER);
	MgrCheckMasterHasSlaveCnDn(spiContext, currentZone, CNDN_TYPE_DATANODE_MASTER);	

    pfreeMgrNodeWrapperList(&activeNodes, NULL);
}
static void MgrFailoverCheck(MemoryContext spiContext, char *currentZone)
{
	PG_TRY();
	{
		MgrCheckMasterHasSlave(spiContext, currentZone);
		MgrMakesureAllSlaveRunning();
	}
	PG_CATCH();
	{
		PG_RE_THROW();
	}PG_END_TRY();
}
static MgrNodeWrapper *MgrGetOldGtmMasterNotZone(MemoryContext spiContext, char *currentZone)
{
	dlist_head 			masterList = DLIST_STATIC_INIT(masterList);
	dlist_iter 			iter;
	MgrNodeWrapper 		*oldMaster = NULL;
	int                 gtmMasterNum = 0;
	dlist_node 			*node = NULL; 

	selectNodeNotZone(spiContext, currentZone, CNDN_TYPE_GTM_COOR_MASTER, &masterList);
	if (dlist_is_empty(&masterList)){
		ereport(ERROR, (errmsg("no master gtmcoord in other zone, current zone(%s).", currentZone)));
	}

	dlist_foreach(iter, &masterList)
	{
		oldMaster = dlist_container(MgrNodeWrapper, link, iter.cur);
		Assert(oldMaster);
		gtmMasterNum++;	
	}	
	if (gtmMasterNum != 1){
		ereport(ERROR, (errmsg("master gtmcoord num should be equal to 1, in not zone(%s).", currentZone)));
	}

	node = dlist_tail_node(&masterList);
	oldMaster = dlist_container(MgrNodeWrapper, link, node);
	return oldMaster;
}
static void MgrGetOldDnMasterNotZone(MemoryContext spiContext, char *currentZone, char nodeType, dlist_head *masterList)
{
	selectNodeNotZone(spiContext, currentZone, nodeType, masterList);
	if (dlist_is_empty(masterList)){
		ereport(ERROR, (errmsg("no %s in other zone, current zone(%s).", mgr_nodetype_str(nodeType), currentZone)));
	}
}
static void MgrZoneFailoverGtm(MemoryContext spiContext, char *currentZone)
{
	MgrNodeWrapper 	*oldGtmMaster = NULL;
	NameData       	newMasterName = {{0}};

	Assert(spiContext);
	Assert(currentZone);

	oldGtmMaster = MgrGetOldGtmMasterNotZone(spiContext, currentZone);
	Assert(oldGtmMaster);
	
	PG_TRY();
	{
		FailOverGtmCoordMaster(NameStr(oldGtmMaster->form.nodename), true, true, &newMasterName, currentZone);	
	}
	PG_CATCH();
	{
		pfreeMgrNodeWrapper(oldGtmMaster);
		PG_RE_THROW();
	}PG_END_TRY();
	
	pfreeMgrNodeWrapper(oldGtmMaster);
	return;
}
static void MgrZoneFailoverCoord(MemoryContext spiContext, char *currentZone)
{
	dlist_head 			masterList = DLIST_STATIC_INIT(masterList);
	MgrNodeWrapper 		*mgrNode;
	dlist_iter 			iter;
	NameData 			newMasterName = {{0}};
	
	PG_TRY();
	{
		MgrGetOldDnMasterNotZone(spiContext, currentZone, CNDN_TYPE_COORDINATOR_MASTER, &masterList);
		dlist_foreach(iter, &masterList)
		{
			mgrNode = dlist_container(MgrNodeWrapper, link, iter.cur);
			Assert(mgrNode);
            memset(&newMasterName, 0x00, sizeof(NameData));
			FailOverCoordMaster(NameStr(mgrNode->form.nodename), true, true, &newMasterName, currentZone);
		}
	}
	PG_CATCH();
	{
		pfreeMgrNodeWrapperList(&masterList, NULL);
		PG_RE_THROW();
	}PG_END_TRY();
	
	pfreeMgrNodeWrapperList(&masterList, NULL);
	return;
}
static void MgrZoneFailoverDN(MemoryContext spiContext, char *currentZone)
{
	dlist_head 			masterList = DLIST_STATIC_INIT(masterList);
	MgrNodeWrapper 		*mgrNode;
	dlist_iter 			iter;
	NameData 			newMasterName = {{0}};

	PG_TRY();
	{
		MgrGetOldDnMasterNotZone(spiContext, currentZone, CNDN_TYPE_DATANODE_MASTER, &masterList);
		dlist_foreach(iter, &masterList)
		{
			mgrNode = dlist_container(MgrNodeWrapper, link, iter.cur);
			Assert(mgrNode);
			memset(&newMasterName, 0x00, sizeof(NameData));
			FailOverDataNodeMaster(NameStr(mgrNode->form.nodename), true, true, &newMasterName, currentZone);
		}
	}PG_CATCH();
	{
		pfreeMgrNodeWrapperList(&masterList, NULL);
		PG_RE_THROW();
	}PG_END_TRY();

	pfreeMgrNodeWrapperList(&masterList, NULL);
	return;
}

/*
* mgr_checknode_in_currentzone
* 
* check given tuple oid, if tuple is in the current zone return true, else return false;
*
*/
bool mgr_checknode_in_currentzone(const char *zone, const Oid TupleOid)
{
	Relation relNode;
	HeapTuple tuple;
	TableScanDesc relScan;
	ScanKeyData key[1];
	bool res = false;

	Assert(zone);
	ScanKeyInit(&key[0]
		,Anum_mgr_node_nodezone
		,BTEqualStrategyNumber
		,F_NAMEEQ
		,CStringGetDatum(zone));

	relNode = table_open(NodeRelationId, AccessShareLock);
	relScan = table_beginscan_catalog(relNode, 1, key);
	while((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{
		if (TupleOid == ((Form_mgr_node)GETSTRUCT(tuple))->oid)
		{
			res = true;
			break;
		}

	}
	table_endscan(relScan);
	table_close(relNode, AccessShareLock);

	return res;
}
/*
* mgr_get_nodetuple_by_name_zone
*
* get the tuple of node according to nodename and zone
*/
HeapTuple mgr_get_nodetuple_by_name_zone(Relation rel, char *nodename, char *nodezone)
{
	ScanKeyData key[2];
	TableScanDesc rel_scan;
	HeapTuple tuple = NULL;
	HeapTuple tupleret = NULL;
	NameData nodenamedata;
	NameData nodezonedata;

	Assert(nodename);
	Assert(nodezone);
	namestrcpy(&nodenamedata, nodename);
	namestrcpy(&nodezonedata, nodezone);
	ScanKeyInit(&key[0]
		,Anum_mgr_node_nodename
		,BTEqualStrategyNumber, F_NAMEEQ
		,NameGetDatum(&nodenamedata));
	ScanKeyInit(&key[1]
		,Anum_mgr_node_nodezone
		,BTEqualStrategyNumber
		,F_NAMEEQ
		,NameGetDatum(&nodezonedata));
	rel_scan = table_beginscan_catalog(rel, 2, key);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		break;
	}
	tupleret = heap_copytuple(tuple);
	table_endscan(rel_scan);
	return tupleret;
}
/*
* mgr_node_has_slave_inzone
* check the oid has been used by slave in given zone
*/
bool mgr_node_has_slave_inzone(Relation rel, char *zone, Oid mastertupleoid)
{
	ScanKeyData key[2];
	HeapTuple tuple;
	TableScanDesc scan;

	ScanKeyInit(&key[0]
		,Anum_mgr_node_nodemasternameoid
		,BTEqualStrategyNumber
		,F_OIDEQ
		,ObjectIdGetDatum(mastertupleoid));
	ScanKeyInit(&key[1]
		,Anum_mgr_node_nodezone
		,BTEqualStrategyNumber
		,F_NAMEEQ
		,CStringGetDatum(zone));
	scan = table_beginscan_catalog(rel, 2, key);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		table_endscan(scan);
		return true;
	}
	table_endscan(scan);
	return false;
}
static void mgr_zone_has_all_masternode(char *currentZone)
{
	if (!mgr_zone_has_node(currentZone, CNDN_TYPE_GTM_COOR_MASTER))
		ereport(ERROR, (errmsg("the zone \"%s\" has not GTMCOORD MASTER in cluster", currentZone)));
	if (!mgr_zone_has_node(currentZone, CNDN_TYPE_COORDINATOR_MASTER))
		ereport(ERROR, (errmsg("the zone \"%s\" has not COORDINATOR MASTER in cluster", currentZone)));
	if (!mgr_zone_has_node(currentZone, CNDN_TYPE_DATANODE_MASTER))
		ereport(ERROR, (errmsg("the zone \"%s\" has not DATANODE MASTER in cluster", currentZone)));
}
static void mgr_make_sure_allmaster_running(void)
{
	mgr_make_sure_all_running(CNDN_TYPE_GTM_COOR_MASTER);
	mgr_make_sure_all_running(CNDN_TYPE_COORDINATOR_MASTER);
	mgr_make_sure_all_running(CNDN_TYPE_DATANODE_MASTER);
}
static void MgrMakesureAllSlaveRunning(void)
{
	PG_TRY();
	{
		mgr_make_sure_all_running(CNDN_TYPE_GTM_COOR_SLAVE);
		mgr_make_sure_all_running(CNDN_TYPE_COORDINATOR_SLAVE);
		mgr_make_sure_all_running(CNDN_TYPE_DATANODE_SLAVE);
	}PG_CATCH();
	{
		PG_RE_THROW();
	}PG_END_TRY();
}
static void MgrZoneUpdateOtherZoneMgrNode(Relation relNode, char *currentZone)
{
	TableScanDesc 	relScan = NULL;
	HeapTuple 		tuple;
	Form_mgr_node 	mgr_node;

	PG_TRY();
	{
		relScan = table_beginscan_catalog(relNode, 0, NULL);
		while((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
		{
			mgr_node = (Form_mgr_node)GETSTRUCT(tuple);
			Assert(mgr_node);			
			if (strcasecmp(NameStr(mgr_node->nodezone), currentZone) == 0)
				continue;            
			namestrcpy(&(mgr_node->nodesync), "");
			mgr_node->nodetype      = getMgrSlaveNodetype(mgr_node->nodetype);	
			mgr_node->nodeinited    = false;
			mgr_node->nodeincluster = false;			
			heap_inplace_update(relNode, tuple);
			ereport(LOG, (errmsg("set %s to not inited, not incluster on mgr node table in zone \"%s\"", NameStr(mgr_node->nodename), NameStr(mgr_node->nodezone))));
		}
		EndScan(relScan);
	}PG_CATCH();
	{
		EndScan(relScan);		
		PG_RE_THROW();
	}PG_END_TRY();
}
/*
* mgr_zone_has_node
* check the zone has given the type of node in cluster
*/
static bool mgr_zone_has_node(const char *zonename, char nodetype)
{
	bool bres = false;
	Relation relNode;
	TableScanDesc relScan;
	ScanKeyData key[3];
	HeapTuple tuple =NULL;

	ScanKeyInit(&key[0]
				,Anum_mgr_node_nodeincluster
				,BTEqualStrategyNumber
				,F_BOOLEQ
				,BoolGetDatum(true));
	ScanKeyInit(&key[1]
			,Anum_mgr_node_nodezone
			,BTEqualStrategyNumber
			,F_NAMEEQ
			,CStringGetDatum(zonename));
	ScanKeyInit(&key[2]
		,Anum_mgr_node_nodetype
		,BTEqualStrategyNumber
		,F_CHAREQ
		,CharGetDatum(nodetype));
	relNode = table_open(NodeRelationId, AccessShareLock);
	relScan = table_beginscan_catalog(relNode, 3, key);
	while((tuple = heap_getnext(relScan, ForwardScanDirection)) != NULL)
	{	
		bres = true;
		break;
	}
	table_endscan(relScan);
	table_close(relNode, AccessShareLock);

	return bres;
}

