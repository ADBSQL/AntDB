
#ifndef MGR_CNDNNODE_H
#define MGR_CNDNNODE_H

#ifdef BUILD_BKI
#include "catalog/buildbki.h"
#else /* BUILD_BKI */
#include "catalog/genbki.h"
#endif /* BUILD_BKI */

#define NodeRelationId 4813

CATALOG(mgr_node,4813)
{
	NameData	nodename;		/* node name */
	Oid			nodehost;		/* node hostoid from host*/
	char		nodetype;		/* node type */
	NameData		nodesync;		/* node sync for slave */
	int32		nodeport;		/* node port */
	bool		nodeinited;		/* is initialized */
	Oid			nodemasternameoid;	/* 0 stands for the node is not slave*/
	bool		nodeincluster;		/*check the node in cluster*/
	bool		nodereadonly;		/* check the node is read only */
	NameData	nodezone;
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

/* ----------------
 *		compiler constants for mgr_node
 * ----------------
 */
#define Natts_mgr_node							11
#define Anum_mgr_node_nodename					1
#define Anum_mgr_node_nodehost					2
#define Anum_mgr_node_nodetype					3
#define Anum_mgr_node_nodesync					4
#define Anum_mgr_node_nodeport					5
#define Anum_mgr_node_nodeinited				6
#define Anum_mgr_node_nodemasternameOid			7
#define Anum_mgr_node_nodeincluster				8
#define Anum_mgr_node_nodereadonly				9
#define Anum_mgr_node_nodezone					10
#define Anum_mgr_node_nodepath					11

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

/* not exist node type */
#define CNDN_TYPE_NONE			'0'

#define SHUTDOWN_S  "smart"
#define SHUTDOWN_F  "fast"
#define SHUTDOWN_I  "immediate"
#define TAKEPLAPARM_N  "none"

/*adb_slot*/
#define SELECT_ADB_SLOT_TABLE_COUNT		"select count(*) from pg_catalog.adb_slot;"

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


struct enum_recovery_status
{
	int type;
	char *name;
};

typedef enum RECOVERY_STATUS
{
	RECOVERY_IN,
	RECOVERY_NOT_IN,
	RECOVERY_UNKNOWN
}recovery_status;

extern bool with_data_checksums;

#define DEFAULT_DB "postgres"

#endif /* MGR_CNDNNODE_H */
