
#ifndef MGR_CNDNNODE_H
#define MGR_CNDNNODE_H

#include "catalog/genbki.h"
#include "catalog/mgr_node_d.h"

CATALOG(mgr_node,4813,NodeRelationId)
{
	/* node name */
	NameData	nodename;

	/* node hostoid from host*/
	Oid			nodehost;

	/* node type */
	char		nodetype;

	/* node sync for slave */
	NameData		nodesync;

	/* node port */
	int32		nodeport;

	/* is initialized */
	bool		nodeinited;

	/* 0 stands for the node is not slave*/
	Oid			nodemasternameoid;

	/*check the node in cluster*/
	bool		nodeincluster;

	/* check the node is read only */
	bool		nodereadonly;

#ifdef CATALOG_VARLEN

	text		nodepath;		/* node data path */
#endif						/* CATALOG_VARLEN */
} FormData_mgr_node;

/* ----------------
 *		Form_mgr_node corresponds to a pointer to a tuple with
 *		the format of mgr_nodenode relation.
 * ----------------
 */
typedef FormData_mgr_node *Form_mgr_node;

#ifdef EXPOSE_TO_CLIENT_CODE

#define CNDN_TYPE_COORDINATOR_MASTER		'c'
#define CNDN_TYPE_COORDINATOR_SLAVE			's'
#define CNDN_TYPE_DATANODE_MASTER			'd'
#define CNDN_TYPE_DATANODE_SLAVE			'b'

#define GTM_TYPE_GTM_MASTER			'g'
#define GTM_TYPE_GTM_SLAVE			'p'

/*CNDN_TYPE_DATANODE include : datanode master,slave*/
#define CNDN_TYPE_COORDINATOR		'C'
#define CNDN_TYPE_DATANODE		'D'
#define CNDN_TYPE_GTM			'G'

#define SHUTDOWN_S  "smart"
#define SHUTDOWN_F  "fast"
#define SHUTDOWN_I  "immediate"
#define TAKEPLAPARM_N  "none"

/*adb_slot*/
#define SELECT_ADB_SLOT_TABLE_COUNT		"select count(*) from pg_catalog.adb_slot;"

#endif							/* EXPOSE_TO_CLIENT_CODE */

typedef enum AGENT_STATUS
{
	AGENT_DOWN = 4, /*the number is enum PGPing max_value + 1*/
	AGENT_RUNNING
}agent_status;

struct enum_sync_state
{
	int type;
	char *name;
};

typedef enum SYNC_STATE
{
	SYNC_STATE_SYNC,
	SYNC_STATE_ASYNC,
	SYNC_STATE_POTENTIAL,
}sync_state;

typedef enum{
	PGXC_CONFIG,
	PGXC_APPEND,
	PGXC_FAILOVER,
	PGXC_REMOVE
}pgxc_node_operator;

/*the values see agt_cmd.c, used for pg_hba.conf add content*/
typedef enum ConnectType
{
	CONNECT_LOCAL=1,
	CONNECT_HOST,
	CONNECT_HOSTSSL,
	CONNECT_HOSTNOSSL
}ConnectType;

typedef struct nodeInfo
{
	Oid tupleOid;
	Oid hostOid;
	int port;
	NameData name;
	bool isPreferred;
}nodeInfo;

extern bool with_data_checksums;

#define DEFAULT_DB "postgres"

#endif /* MGR_CNDNNODE_H */
