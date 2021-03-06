/*-------------------------------------------------------------------------
 *
 * pgxc.h
 *		Postgres-XC flags and connection control information
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 2010-2012, Postgres-XC Development Group
 * Portions Copyright (c) 2014-2017, ADB Development Group
 *
 * IDENTIFICATION
 * 		src/include/pgxc/pgxc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGXC_H
#define PGXC_H

#include "postgres.h"
#include "miscadmin.h"

typedef enum
{
	ADB_NODE_INVALID = 0,
	ADB_NODE_GTM = 1,
	ADB_NODE_COORDINATOR = 2,
	ADB_NODE_DATANODE = 4,
	ADB_NODE_COORDINATOR_GTM = ADB_NODE_GTM | ADB_NODE_COORDINATOR
} AdbNodeTypes;

extern bool isRestoreMode;
extern bool isRestoreCoordType;

typedef enum
{
	REMOTE_CONN_APP,
	REMOTE_CONN_COORD,
	REMOTE_CONN_AGTM,
	REMOTE_CONN_RXACTMGR,
	REMOTE_CONN_DATANODE
} RemoteConnTypes;

/* Determine remote connection type for a PGXC backend */
extern int		remoteConnType;

extern PGDLLIMPORT int adb_node_type;

/* Local node name and numer */
extern char	*PGXCNodeName;
extern Oid PGXCNodeOid;
extern uint32 PGXCNodeIdentifier;

extern Datum xc_lockForBackupKey1;
extern Datum xc_lockForBackupKey2;

#define IS_PGXC_COORDINATOR		((adb_node_type & ADB_NODE_COORDINATOR) != 0)
#define IS_PGXC_DATANODE		(adb_node_type == ADB_NODE_DATANODE)
#define REMOTE_CONN_TYPE		remoteConnType

#define IsConnFromApp()			(remoteConnType == REMOTE_CONN_APP)
#define IsConnFromGTM()			(remoteConnType == REMOTE_CONN_AGTM)
#define IsConnFromCoord()		(remoteConnType == REMOTE_CONN_COORD || remoteConnType == REMOTE_CONN_AGTM)
#define IsConnFromDatanode()	(remoteConnType == REMOTE_CONN_DATANODE)
#define IsConnFromRxactMgr()	(remoteConnType == REMOTE_CONN_RXACTMGR)

#define IsDnNode()				(adb_node_type == ADB_NODE_DATANODE)
#define IsCnNode()				((adb_node_type & ADB_NODE_COORDINATOR) != 0)
#define IsCnMaster()			(IS_PGXC_COORDINATOR && !IsConnFromCoord())
#define IsCnCandidate()			(IS_PGXC_COORDINATOR && IsConnFromCoord())
#define IsGTMNode()				((adb_node_type & ADB_NODE_GTM) != 0)
#define IsGTMCnNode()			(adb_node_type == ADB_NODE_COORDINATOR_GTM)


/* key pair to be used as object id while using advisory lock for backup */
#define XC_LOCK_FOR_BACKUP_KEY_1	0xFFFF
#define XC_LOCK_FOR_BACKUP_KEY_2	0xFFFF

#define IS_PGXC_REAL_DATANODE		(IS_PGXC_DATANODE&&!useLocalXid&&!isRestoreMode)

#endif   /* PGXC_H */
