#include "postgres.h"

#include "mgr/mgr_cmds.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "parser/mgr_node.h"
#include "tcop/utility.h"


const char *mgr_CreateCommandTag(Node *parsetree)
{
	const char *tag;
	AssertArg(parsetree);

	switch(nodeTag(parsetree))
	{
	case T_MGRAddHost:
		tag = "ADD HOST";
		break;
	case T_MGRDropHost:
		tag = "DROP HOST";
		break;
	case T_MGRAlterHost:
		tag = "ALTER HOST";
		break;
	case T_MGRAddNode:
		tag = "ADD NODE";
		break;
	case T_MGRAlterNode:
		tag = "ALTER NODE";
		break;
	case T_MGRDropNode:
		tag = "DROP NODE";
		break;
	case T_MGRUpdateparm:
		tag = "SET PARAM";
		break;
	case T_MGRUpdateparmReset:
		tag = "RESET PARAM";
		break;
	case T_MGRStartAgent:
		tag = "START AGENT";
		break;
	case T_MGRFlushHost:
		tag = "FLUSH HOST";
		break;
	case T_MGRDoctorSet: 
		tag = "SET DOCTOR";
		break;
	case T_MonitorJobitemAdd:
		tag = "ADD ITEM";
		break;
	case T_MonitorJobitemAlter:
		tag = "ALTER ITEM";
		break;
	case T_MonitorJobitemDrop:
		tag = "DROP ITEM";
		break;
	case T_MonitorJobAdd:
		tag = "ADD JOB";
		break;
	case T_MonitorJobAlter:
		tag = "ALTER JOB";
		break;
	case T_MonitorJobDrop:
		tag = "DROP JOB";
		break;
	case T_MgrExtensionAdd:
		tag = "ADD EXTENSION";
		break;
	case T_MgrExtensionDrop:
		tag = "DROP EXTENSION";
		break;
	case T_MgrRemoveNode:
		tag = "REMOVE NODE";
		break;
	case T_MGRSetClusterInit:
		tag = "SET CLUSTER INIT";
		break;
	case T_MonitorDeleteData:
		tag = "CLEAN MONITOR DATA";
		break;
	case T_ClusterSlotInitStmt:
		tag = "CLUSTER SLOT INIT";
		break;
	case T_MGRFlushParam:
		tag = "FLUSH PARAM";
		break;
	default:
		ereport(WARNING, (errmsg("unrecognized node type: %d", (int)nodeTag(parsetree))));
		tag = "???";
		break;
	}
	return tag;
}

void mgr_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
									ProcessUtilityContext context, ParamListInfo params,
									QueryEnvironment *queryEnv,
									DestReceiver *dest,
									char *completionTag)
{
	Node *parsetree = pstmt->utilityStmt;
	AssertArg(parsetree);
	switch(nodeTag(parsetree))
	{
	case T_MGRAddHost:
		mgr_add_host((MGRAddHost*)parsetree, params, dest);
		break;
	case T_MGRDropHost:
		mgr_drop_host((MGRDropHost*)parsetree, params, dest);
		break;
	case T_MGRAlterHost:
		mgr_alter_host((MGRAlterHost*)parsetree, params, dest);
		break;
	case T_MGRAddNode:
		mgr_add_node((MGRAddNode*)parsetree, params, dest);
		break;
	case T_MGRAlterNode:
		mgr_alter_node((MGRAlterNode*)parsetree, params, dest);
		break;
	case T_MGRDropNode:
		mgr_drop_node((MGRDropNode*)parsetree, params, dest);
		break;
	case T_MGRUpdateparm:
		mgr_add_updateparm((MGRUpdateparm*)parsetree, params, dest);
		break;
	case T_MGRUpdateparmReset:
		mgr_reset_updateparm((MGRUpdateparmReset*)parsetree, params, dest);
		break;
	case T_MGRFlushHost:
		mgr_flushhost((MGRFlushHost*)parsetree, params, dest);
		break;
	case T_MGRDoctorSet: 
		mgr_doctor_set_param((MGRDoctorSet*)parsetree, params, dest);
		break;
	case T_MonitorJobitemAdd:
		monitor_jobitem_add((MonitorJobitemAdd*)parsetree, params, dest);
		break;
	case T_MonitorJobitemAlter:
		monitor_jobitem_alter((MonitorJobitemAlter*)parsetree, params, dest);
		break;
	case T_MonitorJobitemDrop:
		monitor_jobitem_drop((MonitorJobitemDrop*)parsetree, params, dest);
		break;
	case T_MonitorJobAdd:
		monitor_job_add((MonitorJobAdd*)parsetree, params, dest);
		break;
	case T_MonitorJobAlter:
		monitor_job_alter((MonitorJobAlter*)parsetree, params, dest);
		break;
	case T_MonitorJobDrop:
		monitor_job_drop((MonitorJobDrop*)parsetree, params, dest);
		break;
	case T_MgrExtensionAdd:
		mgr_extension((MgrExtensionAdd*)parsetree, params, dest);
		break;
	case T_MgrExtensionDrop:
		mgr_extension((MgrExtensionAdd*)parsetree, params, dest);
		break;
	case T_MgrRemoveNode:
		mgr_remove_node((MgrRemoveNode*)parsetree, params, dest);
		break;
	case T_MGRSetClusterInit:
		mgr_set_init((MGRSetClusterInit*)parsetree, params, dest);
		break;
	case T_MonitorDeleteData:
		monitor_delete_data((MonitorDeleteData*)parsetree, params, dest);
		break;
	case T_ClusterSlotInitStmt:
		mgr_cluster_slot_init((ClusterSlotInitStmt*)parsetree, params, dest, queryString);
		break;
	case T_MGRFlushParam:
		mgr_flushparam((MGRFlushParam*)parsetree, params, dest);
		break;
	default:
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR)
			,errmsg("unrecognized node type: %d", (int)nodeTag(parsetree))));
		break;
	}
}
