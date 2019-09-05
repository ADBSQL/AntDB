/*-------------------------------------------------------------------------
 *
 * dynamicreduce.h
 *	  Dynamic reduce tuples in cluster
 *
 * Portions Copyright (c) 2019, AntDB Development Group
 *
 * src/include/utils/dynamicreduce.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_REDUCE_H_
#define DYNAMIC_REDUCE_H_

#include "access/attnum.h"
#include "executor/tuptable.h"
#include "lib/oidbuffer.h"
#include "lib/stringinfo.h"
#include "storage/buffile.h"
#include "storage/shm_mq.h"

#define ADB_DYNAMIC_REDUCE_QUERY_SIZE	(64*1024)	/* 64K */

#define DR_MSG_SEND	0x1
#define DR_MSG_RECV	0x2

typedef struct DynamicReduceNodeInfo
{
	Oid			node_oid;
	int			pid;
	uint16		port;
	NameData	host;
	NameData	name;
}DynamicReduceNodeInfo;

typedef struct DynamicReduceMQData
{
	char	worker_sender_mq[ADB_DYNAMIC_REDUCE_QUERY_SIZE];
	char	reduce_sender_mq[ADB_DYNAMIC_REDUCE_QUERY_SIZE];
}DynamicReduceMQData,*DynamicReduceMQ;

/* for SharedFileSet plan */
typedef struct DynamicReduceSFSData
{
	DynamicReduceMQData	mq;
	SharedFileSet		sfs;
}DynamicReduceSFSData, *DynamicReduceSFS;
#define DRSFSD_SIZE(n) (offsetof(DynamicReduceSFSData, nodes) + sizeof(Oid)*(n))

typedef struct DynamicReduceIOBuffer
{
	shm_mq_handle		   *mqh_sender;
	shm_mq_handle		   *mqh_receiver;
	TupleTableSlot		   *slot_remote;
	struct ExprContext	   *econtext;
	struct ReduceExprState *expr_state;
	TupleTableSlot		   *(*FetchLocal)(void *user_data, struct ExprContext *econtext);
	void				   *user_data;
	OidBufferData			tmp_buf;
	StringInfoData			send_buf;
	StringInfoData			recv_buf;
	bool					eof_local;
	bool					eof_remote;
}DynamicReduceIOBuffer;

extern PGDLLIMPORT bool is_reduce_worker;

#define IsDynamicReduceWorker()		(is_reduce_worker)

extern void DynamicReduceWorkerMain(Datum main_arg);
extern uint16 StartDynamicReduceWorker(void);
extern void StopDynamicReduceWorker(void);
extern void ResetDynamicReduceWork(void);
extern void DynamicReduceConnectNet(const DynamicReduceNodeInfo *info, uint32 count);
extern const Oid* DynamicReduceGetCurrentWorkingNodes(uint32 *count);

extern void DynamicReduceStartNormalPlan(int plan_id, struct dsm_segment *seg, DynamicReduceMQ mq, TupleDesc desc, List *work_nodes);
extern void DynamicReduceStartMergePlan(int plan_id, struct dsm_segment *seg, DynamicReduceMQ mq, TupleDesc desc, List *work_nodes,
										int numCols, AttrNumber *sortColIdx, Oid *sortOperators, Oid *collations, bool *nullsFirst);
extern void DynamicReduceStartParallelPlan(int plan_id, struct dsm_segment *seg, DynamicReduceMQ mq, TupleDesc desc, List *work_nodes, int parallel_max);

extern void DynamicReduceStartSharedFileSetPlan(int plan_id, struct dsm_segment *seg, DynamicReduceSFS sfs, TupleDesc desc, List *work_nodes);
extern char* DynamicReduceSFSFileName(char *name, Oid nodeoid);
extern TupleTableSlot *DynamicReduceReadSFSTuple(TupleTableSlot *slot, BufFile *file, StringInfo buf);
extern void DynamicReduceWriteSFSTuple(TupleTableSlot *slot, BufFile *file);

extern bool DynamicReduceRecvTuple(shm_mq_handle *mqh, TupleTableSlot *slot, StringInfo buf,
								   Oid *nodeoid, bool nowait);
extern int DynamicReduceSendOrRecvTuple(shm_mq_handle *mqsend, shm_mq_handle *mqrecv,
										StringInfo send_buf, TupleTableSlot *slot_recv, StringInfo recv_buf);
extern bool DynamicReduceSendMessage(shm_mq_handle *mqh, Size nbytes, void *data, bool nowait);

extern void SerializeEndOfPlanMessage(StringInfo buf);
extern bool SendEndOfPlanMessageToMQ(shm_mq_handle *mqh, bool nowait);
extern bool SendRejectPlanMessageToMQ(shm_mq_handle *mqh, bool nowait);

extern void SerializeDynamicReducePlanData(StringInfo buf, const void *data, uint32 len, struct OidBufferData *target);
extern void SerializeDynamicReduceSlot(StringInfo buf, TupleTableSlot *slot, struct OidBufferData *target);

extern void SerializeDynamicReduceNodeInfo(StringInfo buf, const DynamicReduceNodeInfo *info, uint32 count);
extern uint32 RestoreDynamicReduceNodeInfo(StringInfo buf, DynamicReduceNodeInfo **info);

/* in dr_fetch.c */
extern void DynamicReduceInitFetch(DynamicReduceIOBuffer *io, dsm_segment *seg, TupleDesc desc,
								   void *send_addr, Size send_size, void *recv_addr, Size recv_size);
extern void DynamicReduceClearFetch(DynamicReduceIOBuffer *io);
extern TupleTableSlot* DynamicReduceFetchSlot(DynamicReduceIOBuffer *io);
extern TupleTableSlot* DynamicReduceFetchLocal(DynamicReduceIOBuffer *io);

#endif /* DYNAMIC_REDUCE_H_ */
