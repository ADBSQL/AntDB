/*-------------------------------------------------------------------------
 *
 * nodemgr.h
 *  Routines for node management
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 2010-2012, Postgres-XC Development Group
 * Portions Copyright (c) 2014-2017, ADB Development Group
 *
 * IDENTIFICATION
 * 		src/include/pgxc/nodemgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMGR_H
#define NODEMGR_H

#include "nodes/parsenodes.h"

/* GUC parameters, limit for number of nodes */
extern int 	MaxDataNodes;
extern int 	MaxCoords;

extern uint32 adb_get_all_coord_oid_array(Oid **pparr, bool order_name);
extern List* adb_get_all_coord_oid_list(bool order_name);
extern uint32 adb_get_all_datanode_oid_array(Oid **pparr, bool order_name);
extern List* adb_get_all_datanode_oid_list(bool order_name);
extern void adb_get_all_node_oid_array(Oid **pparr, uint32 *ncoord, uint32 *ndatanode, bool order_name);
extern void adb_get_all_node_oid_list(List **list_coord, List **list_datanode, bool order_name);

extern void PgxcNodeAlter(AlterNodeStmt *stmt);
extern void PgxcNodeCreate(CreateNodeStmt *stmt);
extern void PgxcNodeRemove(DropNodeStmt *stmt);

extern void InitPGXCNodeIdentifier(void);

#endif	/* NODEMGR_H */
