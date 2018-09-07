/*-------------------------------------------------------------------------
 *
 * inter-comm.c
 *	  Internode query routines
 *
 *
 * Portions Copyright (c) 2016-2017, ADB Development Group
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/intercomm/inter-query.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/heapam.h"
#include "access/relscan.h"
#include "access/tuptypeconvert.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "agtm/agtm.h"
#include "catalog/pgxc_node.h"
#include "commands/prepare.h"
#include "executor/clusterReceiver.h"
#include "executor/executor.h"
#include "executor/execCluster.h"
#include "intercomm/inter-comm.h"
#include "libpq/libpq-fe.h"
#include "libpq/libpq-int.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/pgxcplan.h"
#include "parser/parse_coerce.h"
#include "pgxc/locator.h"
#include "pgxc/pgxc.h"
#include "pgxc/pgxcnode.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"

#define REMOTE_FETCH_SIZE	64

typedef struct RemoteQueryContext
{
	RemoteQueryState   *node;
	TupleTableSlot	   *dest_slot;
	bool				fetch_batch;
	uint64				fetch_count;
} RemoteQueryContext;

static List *RewriteExecNodes(RemoteQueryState *planstate, ExecNodes *exec_nodes);
static TupleTableSlot *InterXactQuery(InterXactState state, RemoteQueryState *node, TupleTableSlot *destslot);
static bool HandleStartRemoteQuery(NodeHandle *handle, RemoteQueryState *node);
static TupleTableSlot *RestoreRemoteSlot(const char *buf, int len, TupleTableSlot *slot, Oid node_id);
static bool StoreRemoteSlot(RemoteQueryContext *context, TupleTableSlot *iterslot, TupleTableSlot *destslot);
static bool HandleCopyOutData(RemoteQueryContext *context, PGconn *conn, const char *buf, int len);
static bool RemoteQueryFinishHook(void *context, struct pg_conn *conn, PQNHookFuncType type, ...);
static TupleDesc CreateRemoteTupleDesc(MemoryContext context, const char *msg, int len);

static int HandleRowDescriptionMsg(PGconn *conn, int msgLength);
static int HandleQueryCompleteMsg(PGconn *conn);
static int ExtractProcessedNumber(const char *buf, int len, uint64 *nprocessed);
static void deparseStringLiteral(StringInfo buf, const char *val);
static void deparseCnAnalyzeSizeSql(StringInfo buf, Relation rel);
static void deparseCnAnalyzeSampleSql(StringInfo buf, Relation rel, int targper);

static PGcustumFuns QueryCustomFuncs = {
	HandleRowDescriptionMsg,
	NULL,
	HandleQueryCompleteMsg,
	HandleInterUnknownMsg
};

PGcustumFuns *InterQueryCustomFuncs = &QueryCustomFuncs;

int
HandleInterUnknownMsg(PGconn *conn, char c, int msgLength)
{
	char msgid = '\0';
	int msglen = msgLength;

	if (c != 'U')
		return -1;

	if (pqGetc(&msgid, conn))
		return 1;
	msglen--;

	switch (msgid)
	{
		case 'M':		/* command id change */
			{
				CommandId	cid;

				if (msglen != sizeof(cid) ||
					pqGetInt((int *) &cid, sizeof(cid), conn))
				{
					appendPQExpBuffer(&conn->errorMessage,
									  "Invalid command id length %d, expect %lu bytes",
									  msglen, sizeof(cid));
					return 1;
				}

				if (cid > GetReceivedCommandId())
					SetReceivedCommandId(cid);

				return 0;
			}

		case CLUSTER_MSG_TABLE_STAT:	/* table stat change */
			{
				ClusterRecvTableStat(conn->inBuffer + conn->inCursor, msglen);
				conn->inCursor += msglen;

				return 0;
			}

		default:
			break;
	}

	return -1;
}

static List *
RewriteExecNodes(RemoteQueryState *planstate, ExecNodes *exec_nodes)
{
	ExprState	   *estate;
	bool			isnull;
	Datum			partvalue;
	int				nelems, idx;
	ListCell	   *lc;
	Datum		   *en_expr_values;
	bool		   *en_expr_nulls;
	Oid			   *en_expr_types;
	Node		   *en_expr_node;
	List		   *result = NIL;
	RelationLocInfo*rel_loc = NULL;
	Oid			   *argtypes = NULL;
	int				nargs;

	if (!exec_nodes || !exec_nodes->en_expr)
		return NIL;

	rel_loc = GetRelationLocInfo(exec_nodes->en_relid);
	Assert(rel_loc);

	/*
	 * en_expr is set by pgxc_set_en_expr only for distributed
	 * relations while planning DMLs, hence a select for update
	 * on a replicated table here is an assertion
	 */
	Assert(!(exec_nodes->accesstype == RELATION_ACCESS_READ_FOR_UPDATE &&
			IsRelationReplicated(rel_loc)));

	nelems = list_length(exec_nodes->en_expr);
	en_expr_values = (Datum *) palloc0(sizeof(Datum) * nelems);
	en_expr_nulls = (bool *) palloc0(sizeof(bool) * nelems);
	en_expr_types = (Oid *) palloc0(sizeof(Oid) * nelems);

	if (IsRelationDistributedByUserDefined(rel_loc))
	{
		Assert(OidIsValid(rel_loc->funcid));
		Assert(rel_loc->funcAttrNums);
		(void) get_func_signature(rel_loc->funcid, &argtypes, &nargs);
		Assert(nelems == nargs);
	}

	idx = 0;
	foreach (lc, exec_nodes->en_expr)
	{
		en_expr_node = (Node *)lfirst(lc);
		if (IsRelationDistributedByUserDefined(rel_loc) && en_expr_node)
		{
			en_expr_node = coerce_to_target_type(NULL, en_expr_node,
												exprType(en_expr_node),
												argtypes[idx],
												-1,
												COERCION_IMPLICIT,
												COERCE_IMPLICIT_CAST,
												-1);
		}
		if (en_expr_node)
		{
			estate = ExecInitExpr((Expr*)en_expr_node, (PlanState *) planstate);
			partvalue = ExecEvalExpr(estate,
									 planstate->ss.ps.ps_ExprContext,
									 &isnull,
									 NULL);
			en_expr_values[idx] = isnull ? (Datum)0 : partvalue;
			en_expr_nulls[idx] = isnull;
			en_expr_types[idx] = exprType(en_expr_node);
		} else
		{
			en_expr_values[idx] = (Datum)0;
			en_expr_nulls[idx] = true;
			en_expr_types[idx] = InvalidOid;
		}
		idx++;
	}

	if (argtypes)
		pfree(argtypes);

	result = GetInvolvedNodes(rel_loc, nelems, en_expr_values, en_expr_nulls,
							  en_expr_types, exec_nodes->accesstype);
	pfree(en_expr_values);
	pfree(en_expr_nulls);
	pfree(en_expr_types);
	FreeRelationLocInfo(rel_loc);

	return result;
}

List *
GetRemoteNodeList(RemoteQueryState *planstate, ExecNodes *exec_nodes, RemoteQueryExecType exec_type)
{
	List   *node_list = NIL;

	if (exec_nodes)
	{
		if (exec_nodes->en_expr)
			node_list = RewriteExecNodes(planstate, exec_nodes);
		else
		if (OidIsValid(exec_nodes->en_relid))
		{
			RelationLocInfo	   *rel_loc = GetRelationLocInfo(exec_nodes->en_relid);
			Datum				value = (Datum)0;
			bool				null = true;
			Oid					type = InvalidOid;

			node_list = GetInvolvedNodes(rel_loc, 1, &value, &null, &type, exec_nodes->accesstype);

			/*
			 * en_relid is set only for DMLs, hence a select for update on a
			 * replicated table here is an assertion
			 */
			Assert(!(exec_nodes->accesstype == RELATION_ACCESS_READ_FOR_UPDATE &&
					IsRelationReplicated(rel_loc)));

			/*
			 * what the hell!!! Copy these code from old PGXC but i don't understand.
			 *
			 * Special handling for ROUND ROBIN distributed tables. The target
			 * node must be determined at the execution time
			 */
			if (!(rel_loc->locatorType == LOCATOR_TYPE_RROBIN && node_list))
			{
				if (exec_type == EXEC_ON_DATANODES || exec_type == EXEC_ON_ALL_NODES)
				{
					list_free(node_list);
					node_list = list_copy(exec_nodes->nodeids);
				}
			}

			FreeRelationLocInfo(rel_loc);
		}
		else
		{
			node_list = list_copy(exec_nodes->nodeids);
		}
	}
	/* get node list by exec_type */
	if (list_length(node_list) == 0)
	{
		switch (exec_type)
		{
			case EXEC_ON_COORDS:
				node_list = GetAllCnIDL(false);
				break;
			case EXEC_ON_DATANODES:
				node_list = GetAllDnIDL(false);
				break;
			case EXEC_ON_ALL_NODES:
				node_list = GetAllNodeIDL(false);
				break;
			default:
				Assert(false);
				break;
		}
	}

	return node_list;
}

TupleTableSlot *
StartRemoteQuery(RemoteQueryState *node, TupleTableSlot *destslot)
{
	RemoteQuery	   *step;
	List		   *node_list;
	InterXactState	state;
	bool			need_xact_block;

	Assert(node);
	/*
	 * A Postgres-XC node cannot run transactions while in recovery as
	 * this operation needs transaction IDs. This is more a safety guard than anything else.
	 */
	if (RecoveryInProgress())
		elog(ERROR, "cannot run transaction to remote nodes during recovery");

	state = GetCurrentInterXactState();
	step = (RemoteQuery *) node->ss.ps.plan;

	node_list = GetRemoteNodeList(node, step->exec_nodes, step->exec_type);
	state = MakeInterXactState2(state, node_list);

	if (step->force_autocommit || step->read_only)
		need_xact_block = false;
	else
		need_xact_block = true;
	if (need_xact_block)
		state->need_xact_block = true;

	/* save handle list for current RemoteQueryState */
	if (node->cur_handles)
		list_free(node->cur_handles);
	node->cur_handles = list_copy(state->cur_handle->handles);
	node->all_handles = list_concat_unique_ptr(node->all_handles, node->cur_handles);

	return InterXactQuery(state, node, destslot);
}

static TupleTableSlot *
InterXactQuery(InterXactState state, RemoteQueryState *node, TupleTableSlot *destslot)
{
	NodeMixHandle	   *cur_handle;
	NodeHandle		   *handle;
	NodeHandle		   *pr_handle;
	ListCell		   *lc_handle;
	bool				need_xact_block;
	bool				already_begin;
	GlobalTransactionId	gxid;
	TimestampTz			timestamp = GetCurrentTransactionStartTimestamp();

	Assert(state && node);
	cur_handle = state->cur_handle;
	need_xact_block = state->need_xact_block;
	pr_handle = cur_handle->pr_handle;

	if (need_xact_block)
	{
		agtm_BeginTransaction();
		gxid = GetCurrentTransactionId();
	} else
		gxid = GetCurrentTransactionIdIfAny();

	PG_TRY();
	{
		if (pr_handle)
		{
			Tuplestorestate	   *tuplestorestate = node->tuplestorestate;
			bool				eof_tuplestore;

			Assert(tuplestorestate);

			if (!HandleBegin(state, pr_handle, gxid, timestamp, need_xact_block, &already_begin) ||
				!HandleStartRemoteQuery(pr_handle, node))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Fail to start query on primary node"),
						 errnode(NameStr(pr_handle->node_name)),
						 errhint("%s", HandleGetError(pr_handle))));
			}

			/*
			 * Here we must check eof of the tuplestore, otherwise
			 * the first tuple slot of the primary handle will be
			 * got once again from the tuplestore.
			 */
			eof_tuplestore = tuplestore_ateof(tuplestorestate);
			if (!eof_tuplestore)
			{
				if (!tuplestore_get_remotetupleslot(tuplestorestate, true, false, destslot))
					eof_tuplestore = true;
			}
			/* try to get the first no-null slot */
			if (eof_tuplestore)
				destslot = HandleFetchRemote(pr_handle, node, destslot, true, false);
		}

		foreach (lc_handle, cur_handle->handles)
		{
			handle = (NodeHandle *) lfirst(lc_handle);
			if (handle == pr_handle)
				continue;

			if (!HandleBegin(state, handle, gxid, timestamp, need_xact_block, &already_begin) ||
				!HandleStartRemoteQuery(handle, node))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Fail to start query on remote node"),
						 errnode(NameStr(handle->node_name)),
						 errhint("%s", HandleGetError(handle))));
			}
		}
	} PG_CATCH();
	{
		InterXactGCCurrent(state);
		PG_RE_THROW();
	} PG_END_TRY();

	return destslot;
}

static bool
HandleStartRemoteQuery(NodeHandle *handle, RemoteQueryState *node)
{
	CommandId	cid;
	Snapshot	snapshot;
	RemoteQuery*step;

	Assert(handle && node);
	snapshot = GetActiveSnapshot();
	step = (RemoteQuery *) node->ss.ps.plan;

	/*
	 * mark the current owner of the handle, if the node is another owner,
	 * it is best to cache data from the handle and make it clean for the
	 * new owner.
	 */
	if (!handle->node_owner || handle->node_owner != node)
		HandleCacheOrGC(handle);
	handle->node_owner = node;

	/*
	 * Scan descriptor would be valid and would contain a valid snapshot
	 * in cases when we need to send out of order command id to data node
	 * e.g. in case of a fetch
	 */

	if (node->cursor != NULL &&
		node->cursor[0] != '\0' &&
		node->ss.ss_currentScanDesc != NULL &&
		node->ss.ss_currentScanDesc->rs_snapshot != NULL)
		cid = node->ss.ss_currentScanDesc->rs_snapshot->curcid;
	else
	{
		/*
		 * An insert into a child by selecting form its parent gets translated
		 * into a multi-statement transaction in which first we select from parent
		 * and then insert into child, then select form child and insert into child.
		 * The select from child should not see the just inserted rows.
		 * The command id of the select from child is therefore set to
		 * the command id of the insert-select query saved earlier.
		 * Similarly a WITH query that updates a table in main query
		 * and inserts a row in the same table in the WITH query
		 * needs to make sure that the row inserted by the WITH query does
		 * not get updated by the main query.
		 */
		if (step->exec_nodes &&
			step->exec_nodes->accesstype == RELATION_ACCESS_READ &&
			step->rq_save_command_id)
			cid = node->rqs_cmd_id;
		else
			cid = GetCurrentCommandId(false);
	}

	if (step->statement || step->cursor || node->rqs_num_params)
	{
		/* need to use Extended Query Protocol */
		int		fetch = 0;
		bool	prepared = false;
		bool	send_desc = false;

		if (step->base_tlist != NULL ||
			step->exec_nodes->accesstype == RELATION_ACCESS_READ ||
			step->has_row_marks)
			send_desc = true;

		/* if prepared statement is referenced see if it is already exist */
		if (step->statement)
			prepared = ActivateDatanodeStatementOnNode(step->statement, handle->node_id);
		/*
		 * execute and fetch rows only if they will be consumed
		 * immediately by the sorter
		 */
		if (step->cursor)
			fetch = 1;

		if (!HandleSendQueryExtend(handle,
								   cid,
								   snapshot,
								   prepared ? NULL : step->sql_statement,
								   step->statement,
								   step->cursor,
								   send_desc,
								   fetch,
								   node->rqs_num_params,
								   node->rqs_param_types,
								   NULL,
								   node->paramval_data,
								   node->paramval_len,
								   0,
								   NULL))
			return false;

	} else
	{
		if (!HandleSendQueryTree(handle, cid, snapshot, step->sql_statement, step->sql_node))
			return false;
	}

	return true;
}

TupleTableSlot *
FetchRemoteQuery(RemoteQueryState *node, TupleTableSlot *destslot)
{
	RemoteQueryContext	context;
	Tuplestorestate	   *tuplestorestate;
	bool				eof_tuplestore;
	ListCell		   *lc_handle;
	NodeHandle		   *handle;
	List			   *handle_list = NIL;

	Assert(node && destslot);
	ExecClearTuple(destslot);

	tuplestorestate = node->tuplestorestate;
	Assert(tuplestorestate);

	eof_tuplestore = tuplestore_ateof(tuplestorestate);
	if (!eof_tuplestore)
	{
		if (!tuplestore_get_remotetupleslot(tuplestorestate, true, false, destslot))
			eof_tuplestore = true;
	}

	if (eof_tuplestore)
	{
		handle_list = node->cur_handles;
		context.node = node;
		context.dest_slot = destslot;
		if (node->eflags & EXEC_FLAG_REWIND)
			context.fetch_batch = true;
		else
			context.fetch_batch = false;
		context.fetch_count = 0;

		/* check sanity for the owner of the current handles */
		foreach (lc_handle, handle_list)
		{
			handle = (NodeHandle *) lfirst(lc_handle);
			/* if the owner is not "node" now, cache it */
			if (!handle->node_owner || handle->node_owner != node)
				HandleCacheOrGC(handle);
			handle->node_owner = node;
		}

		PQNListExecFinish(handle_list, HandleGetPGconn, RemoteQueryFinishHook, &context, true);
	}

	return destslot;
}

TupleTableSlot *
HandleFetchRemote(NodeHandle *handle, RemoteQueryState *node, TupleTableSlot *destslot, bool blocking, bool batch)
{
	RemoteQueryContext	context;

	Assert(handle && node && destslot);
	Assert(handle->node_owner && handle->node_owner == node);
	Assert(handle->node_conn);

	ExecClearTuple(destslot);

	context.node = node;
	context.dest_slot = destslot;
	context.fetch_batch = batch;
	context.fetch_count = 0;

	PQNOneExecFinish(handle->node_conn, RemoteQueryFinishHook, &context, blocking);

	return destslot;
}

static TupleTableSlot *
RestoreRemoteSlot(const char *buf, int len, TupleTableSlot *slot, Oid node_id)
{
	MinimalTuple tup;
	uint32 t_len = *(uint32*)buf;
	if(t_len > len)
		ereport(ERROR, (errmsg("invalid tuple message length")));
	tup = MemoryContextAlloc(slot->tts_mcxt, t_len + sizeof(node_id));
	memcpy(tup, buf, t_len);
	MiniTupSetRemoteNode(tup, node_id);

	return ExecStoreMinimalTuple(tup, slot, true);
}

static bool
StoreRemoteSlot(RemoteQueryContext *context, TupleTableSlot *iterslot, TupleTableSlot *destslot)
{
	Tuplestorestate	   *tuplestorestate;
	RemoteQueryState   *node;
	uint64				fetch_limit = 1;
	bool				ret = false;

	if (TupIsNull(iterslot))
	{
		ExecClearTuple(destslot);
		return false;
	}

	Assert(context);
	node = context->node;
	Assert(node);
	tuplestorestate = node->tuplestorestate;
	Assert(tuplestorestate);

	context->fetch_count++;
	/*
	 * We copy the first iterslot to the destslot and it will be returned to caller.
	 *
	 * If the destslot is just the iterslot, it means the caller no need to obtain
	 * the slot until now, it should be cached in the Tuplestorestate. see HandleCacheOrGC.
	 */
	if (context->fetch_count == 1 && destslot != iterslot)
	{
		destslot = ExecCopySlot(destslot, iterslot);
		destslot->tts_xcnodeoid = iterslot->tts_xcnodeoid;
	} else
	{
		if (context->fetch_batch)
			fetch_limit = REMOTE_FETCH_SIZE;

		/*
		 * backward if at the end of tuplestore, so that we can fetch tuple
		 * from tuplestore next time.
		 */
		if (tuplestore_ateof(tuplestorestate))
			(void) tuplestore_advance(tuplestorestate, false);

		if (context->fetch_count >= fetch_limit)
			ret = true;
	}

	tuplestore_put_remotetupleslot(tuplestorestate, iterslot);

	return ret;
}

static bool
HandleCopyOutData(RemoteQueryContext *context, PGconn *conn, const char *buf, int len)
{
	RemoteQueryState   *node;
	TupleTableSlot	   *destSlot;
	TupleTableSlot	   *scanSlot;
	TupleTableSlot	   *iterSlot;
	PlanState		   *ps;
	NodeHandle		   *handle;
	bool				ret = false;

	Assert(context && buf);
	node = context->node;
	destSlot = context->dest_slot;
	ps = &(node->ss.ps);
	Assert(node && node->recvState);
	scanSlot = node->ss.ss_ScanTupleSlot;
	iterSlot = node->iterSlot;
	handle = (NodeHandle *) (conn->custom);

	switch (buf[0])
	{
		/*
		 * Tuple description of scan slot of RemoteQueryState may be not set
		 * correctly when ExecInitRemoteQuery, such as, select count(1) from x.
		 *
		 * so, we are care about tuple description message from other node and
		 * reset it at right time. iterSlot and convertSlot are the same.
		 */
		case CLUSTER_MSG_TUPLE_DESC:
			if (node->description_count++ == 0)
			{
				TupleDesc desc = CreateRemoteTupleDesc(destSlot->tts_mcxt, buf, len);
				ExecSetSlotDescriptor(destSlot, desc);
				if (destSlot == scanSlot)
				{
					ExecSetSlotDescriptor(iterSlot, desc);

					/* construct cluster receive state */
					Assert(node->recvState && !node->recvState->convert);
					node->recvState->convert = create_type_convert(destSlot->tts_tupleDescriptor, false, true);
					if (node->recvState->convert)
					{
						node->recvState->convert_slot = node->convertSlot;
						/*
						 * Make a copy of descriptor of convert to avoid
						 * function ReleaseTupleDesc release twice.
						 */
						ExecSetSlotDescriptor(node->convertSlot,
											  CreateTupleDescCopy(node->recvState->convert->out_desc));
					}
				}
			} else
			{
				compare_slot_head_message(buf + 1, len - 1, destSlot->tts_tupleDescriptor);
			}
			break;
		case CLUSTER_MSG_CONVERT_DESC:
			{
				if (!node->recvState->convert || !node->recvState->convert_slot)
					ereport(ERROR,
							(errmsg("It is not sane when we got convert tuple description "
									"but convert was not set.")));
				compare_slot_head_message(buf + 1, len - 1,
										  node->recvState->convert_slot->tts_tupleDescriptor);
			}
			break;
		case CLUSTER_MSG_TUPLE_DATA:
			{
				(void) RestoreRemoteSlot(buf + 1, len - 1, iterSlot, handle->node_id);
				iterSlot->tts_xcnodeoid = handle->node_id;
				ret = StoreRemoteSlot(context, iterSlot, destSlot);
			}
			break;
		case CLUSTER_MSG_CONVERT_TUPLE:
			{
				if (!node->recvState->convert || !node->recvState->convert_slot)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("Can not parse convert tuple as convert was not set")));

				restore_slot_message(buf + 1, len - 1, node->recvState->convert_slot);
				do_type_convert_slot_in(node->recvState->convert,
										node->recvState->convert_slot,
										iterSlot,
										node->recvState->slot_need_copy_datum);
				iterSlot->tts_xcnodeoid = handle->node_id;
				ret = StoreRemoteSlot(context, iterSlot, destSlot);
			}
			break;
		default:
			ret = clusterRecvTuple(destSlot, buf, len, ps, conn);
			Assert(!ret);
			break;
	}

	return ret;
}

static bool
RemoteQueryFinishHook(void *context, struct pg_conn *conn, PQNHookFuncType type, ...)
{
	va_list args;

	switch(type)
	{
		case PQNHFT_ERROR:
			return PQNEFHNormal(NULL, conn, type);
		case PQNHFT_COPY_OUT_DATA:
			{
				int				len;
				const char		*buf;

				va_start(args, type);
				buf = va_arg(args, const char*);
				len = va_arg(args, int);

				if(HandleCopyOutData(context, conn, buf, len))
				{
					va_end(args);
					return true;
				}
				va_end(args);
			}
			break;
		case PQNHFT_COPY_IN_ONLY:
			PQputCopyEnd(conn, NULL);
			break;
		case PQNHFT_RESULT:
			{
				PGresult	   *res;
				ExecStatusType	status;

				va_start(args, type);
				res = va_arg(args, PGresult*);
				if(res)
				{
					status = PQresultStatus(res);
					if(status == PGRES_FATAL_ERROR)
					{
						RemoteQueryState   *node;
						node = ((RemoteQueryContext *) context)->node;
						node->command_error_count++;
						PQNReportResultError(res, conn, ERROR, true);
					}
					else if(status == PGRES_COPY_IN)
						PQputCopyEnd(conn, NULL);
				}
				va_end(args);
			}
			break;
		default:
			break;
	}
	return false;
}

static TupleDesc
CreateRemoteTupleDesc(MemoryContext context, const char *msg, int len)
{
	StringInfoData	buf;
	TupleDesc		desc;
	int				i, natts;
	Oid				atttypid;
	char		   *attname;
	int32			atttypmod;
	int32			attndims;
	MemoryContext	oldContext;

	Assert(msg[0] == CLUSTER_MSG_TUPLE_DESC);

	oldContext = MemoryContextSwitchTo(context);

	natts = *(int *) &(msg[2]);
	desc = CreateTemplateTupleDesc(natts, (bool) msg[1]);

	buf.data = (char *) msg;
	buf.len = buf.maxlen = len;
	buf.cursor = 6;
	for (i = 1; i <= natts; i++)
	{
		/* attname */
		attname = load_node_string(&buf, false);
		/* atttypmod */
		atttypmod = *(int32 *)(buf.data + buf.cursor);
		buf.cursor += sizeof(atttypmod);
		/* attndims */
		attndims = *(int32 *)(buf.data + buf.cursor);
		buf.cursor += sizeof(attndims);
		/* atttypid */
		atttypid = load_oid_type(&buf);

		TupleDescInitEntry(desc, (AttrNumber) i, attname, atttypid, atttypmod, attndims);
	}

	(void) MemoryContextSwitchTo(oldContext);

	return desc;
}

void
CloseRemoteStatement(const char *stmt_name, Oid *nodes, int nnodes)
{
	NodeMixHandle  *cur_handle;
	List		   *oid_list;

	if (!stmt_name)
		return;
	oid_list = OidArraryToList(NULL, nodes, nnodes);
	if (!oid_list)
		return ;

	cur_handle = GetMixedHandles(oid_list, NULL);
	Assert(cur_handle);
	HandleListClose(cur_handle->handles, true, stmt_name);
}

/*-------------------------------------------------------------------------------------
 *
 * Define custom functions for PGconn of Handle
 *
 *-------------------------------------------------------------------------------------*/

/*
 * HandleRowDescriptionMsg
 *
 * deal with 'T' message which contained in parseInput.
 *
 * row descriptions will be handled in COPY protocol.
 * If we see 'T' message, just silently drop it. it will
 * be handled in HandleCopyOutData.
 */
static int
HandleRowDescriptionMsg(PGconn *conn, int msgLength)
{
	Assert(conn);
	conn->inCursor += msgLength;
	conn->inStart = conn->inCursor;
	return 0;
}

/*
 * HandleQueryCompleteMsg
 *
 * deal with 'C' message which contained in parseInput.
 *
 */
static int
HandleQueryCompleteMsg(PGconn *conn)
{
	NodeHandle		   *handle;
	void			   *owner;

	Assert(conn);
	handle = (NodeHandle *) conn->custom;
	owner = handle->node_owner;

	if (!owner)
		return 0;

	if (IsA(owner, RemoteQueryState))
	{
		RemoteQueryState   *node = (RemoteQueryState *) owner;
		RemoteQuery		   *step = (RemoteQuery *) node->ss.ps.plan;
		bool				non_fqs_dml;

		/* Is this a DML query that is not FQSed ? */
		non_fqs_dml = (step  && step ->rq_params_internal);

		/* Extract number of processed */
		if (node->combine_type != COMBINE_TYPE_NONE)
		{
			uint64	nprocessed;
			int		digits;

			digits = ExtractProcessedNumber(conn->workBuffer.data, conn->workBuffer.len, &nprocessed);
			if (digits > 0)
			{
				/* Replicated write, make sure they are the same */
				if (node->combine_type == COMBINE_TYPE_SAME)
				{
					if (node->command_complete_count)
					{
						/* For FQS, check if there is a consistency issue with replicated table. */
						if (nprocessed != node->rqs_processed && !non_fqs_dml)
							ereport(ERROR,
									(errcode(ERRCODE_DATA_CORRUPTED),
									 errmsg("Write to replicated table returned"
											" different results from the Datanodes")));
					}
					/* Always update the row count. We have initialized it to 0 */
					node->rqs_processed = nprocessed;
				}
				else
					node->rqs_processed += nprocessed;
			} else
			{
				/* what to do by this case? */
			}
		}

		/* If response checking is enable only then do further processing */
		node->command_complete_count++;
	}

	return 0;
}

static int
ExtractProcessedNumber(const char *buf, int len, uint64 *nprocessed)
{
	int			digits = 0;
	const char *ptr = buf + len;

	if (len <= 0)
		return digits;

	ptr--;	/* skip \0 */
	while (ptr >= buf)
	{
		if (!isdigit(*ptr))
			break;
		digits++;
		ptr--;
	}

	if (digits && nprocessed)
		*nprocessed = strtoul(ptr, NULL, 10);

	return digits;
}

static void
deparseStringLiteral(StringInfo buf, const char *val)
{
	const char *valptr;

	/*
	 * Rather than making assumptions about the remote server's value of
	 * standard_conforming_strings, always use E'foo' syntax if there are any
	 * backslashes.  This will fail on remote servers before 8.1, but those
	 * are long out of support.
	 */
	if (strchr(val, '\\') != NULL)
		appendStringInfoChar(buf, ESCAPE_STRING_SYNTAX);
	appendStringInfoChar(buf, '\'');
	for (valptr = val; *valptr; valptr++)
	{
		char		ch = *valptr;

		if (SQL_STR_DOUBLE(ch, true))
			appendStringInfoChar(buf, ch);
		appendStringInfoChar(buf, ch);
	}
	appendStringInfoChar(buf, '\'');
}

static void
deparseCnAnalyzeSizeSql(StringInfo buf, Relation rel)
{
	StringInfoData	namebuf;
	char		   *nspname;
	char		   *relname;

	initStringInfo(&namebuf);
	nspname = get_namespace_name(RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);
	initStringInfo(&namebuf);
	appendStringInfo(&namebuf, "%s.%s",
					 quote_identifier(nspname), quote_identifier(relname));

	appendStringInfoString(buf, "SELECT pg_catalog.pg_relation_size(");
	deparseStringLiteral(buf, namebuf.data);
	appendStringInfo(buf, "::pg_catalog.regclass) / %d AS blocknum", BLCKSZ);
	pfree(namebuf.data);
}

BlockNumber
CnGetRelationNumberOfBlocks(Relation relation)
{
	List			   *dn_list;
	RemoteQuery		   *rquery;
	RemoteQueryState   *rstate;
	ExecNodes		   *rnodes;
	EState			   *estate;
	TupleTableSlot	   *slot;
	MemoryContext		oldcontext;
	StringInfoData		sqlbuf;
	Datum				value;
	bool				isnull;
	BlockNumber			totalpages;
	int					tuplecnt;

	/* Sancity check */
	Assert(IsCnNode());
	Assert(RelationGetLocInfo(relation));

	/* Get all datanode */
	if ((dn_list = GetAllDnIDL(false)) == NIL)
		return 0;

	/* Get preferred datanode if replicate */
	if (IsRelationReplicated(RelationGetLocInfo(relation)))
		dn_list = GetPreferredRepNodes((const List *) dn_list);

	/* Construct analyze query */
	initStringInfo(&sqlbuf);
	deparseCnAnalyzeSizeSql(&sqlbuf, relation);

	/* Build ExecNodes */
	rnodes = MakeExecNodesByOids(RelationGetLocInfo(relation),
								 dn_list,
								 RELATION_ACCESS_READ);

	/* Build up RemoteQuery */
	rquery = makeNode(RemoteQuery);
	rquery->combine_type = COMBINE_TYPE_NONE;
	rquery->exec_nodes = rnodes;
	rquery->sql_statement = sqlbuf.data;
	rquery->force_autocommit = true;
	rquery->exec_type = EXEC_ON_DATANODES;

	/* Execute query on the data nodes */
	estate = CreateExecutorState();
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
	estate->es_snapshot = GetActiveSnapshot();
	rstate = ExecInitRemoteQuery(rquery, estate, 0);
	MemoryContextSwitchTo(oldcontext);

	totalpages = 0;
	tuplecnt = 0;
	for (;;)
	{
		/* Reset the per-output-tuple exprcontext */
		ResetPerTupleExprContext(estate);

		/*
		 * Execute the plan and obtain a tuple
		 */
		slot = ExecProcNode((PlanState *) rstate);
		if (TupIsNull(slot))
			break;

		/* tuple count */
		tuplecnt++;

		/* Extract blocknumm  */
		value = slot_getattr(slot, 1, &isnull); /* blocknum */
		if (!isnull)
			totalpages += DatumGetInt64(value);
	}

	ExecEndRemoteQuery(rstate);

	FreeExecutorState(estate);

	if (tuplecnt != list_length(dn_list))
		elog(ERROR,
			 "unexpected result from deparseCnAnalyzeSizeSql query");

	return totalpages;
}

static void
deparseCnAnalyzeSampleSql(StringInfo buf, Relation rel, int targper)
{
	StringInfoData	namebuf;
	char		   *nspname;
	char		   *relname;

	initStringInfo(&namebuf);
	nspname = get_namespace_name(RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);
	initStringInfo(&namebuf);
	appendStringInfo(&namebuf, "%s.%s",
					 quote_identifier(nspname), quote_identifier(relname));

	appendStringInfo(buf, "SELECT * FROM %s TABLESAMPLE BERNOULLI(%d)",
					 namebuf.data, targper);
	pfree(namebuf.data);
}

int
CnAcquireSampleRowsFunc(Relation relation, int elevel,
						HeapTuple *rows, int targrows,
						double *totalrows, double *totaldeadrows)
{
	List			   *dn_list;
	RemoteQuery		   *rquery;
	RemoteQueryState   *rstate;
	ExecNodes		   *rnodes;
	EState			   *estate;
	TupleTableSlot	   *slot;
	MemoryContext		oldcontext;
	StringInfoData		sqlbuf;
	HeapTuple			tuple;
	ReservoirStateData	rs;
	int					numrows = 0;	/* # rows now in reservoir */
	double				samplerows = 0; /* total # rows collected */
	double				rowstoskip = -1;	/* -1 means not set yet */
	int					sampleper = 1;	/* 1% smaple rows */

	/* Sancity check */
	Assert(IsCnNode());
	Assert(RelationGetLocInfo(relation));

	/* Get all datanode */
	if ((dn_list = GetAllDnIDL(false)) == NIL)
		return 0;

	/* Prepare for sampling rows */
	reservoir_init_selection_state(&rs, targrows);

	/* Get preferred datanode if replicate */
	if (IsRelationReplicated(RelationGetLocInfo(relation)))
		dn_list = GetPreferredRepNodes((const List *) dn_list);

	/* Construct analyze query */
	initStringInfo(&sqlbuf);
	deparseCnAnalyzeSampleSql(&sqlbuf, relation, sampleper);

	/* Build ExecNodes */
	rnodes = MakeExecNodesByOids(RelationGetLocInfo(relation),
								 dn_list,
								 RELATION_ACCESS_READ);

	/* Build up RemoteQuery */
	rquery = makeNode(RemoteQuery);
	rquery->combine_type = COMBINE_TYPE_NONE;
	rquery->exec_nodes = rnodes;
	rquery->sql_statement = sqlbuf.data;
	rquery->force_autocommit = true;
	rquery->exec_type = EXEC_ON_DATANODES;

	/* Execute query on the data nodes */
	estate = CreateExecutorState();
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
	estate->es_snapshot = GetActiveSnapshot();
	rstate = ExecInitRemoteQuery(rquery, estate, 0);
	MemoryContextSwitchTo(oldcontext);

	for (;;)
	{
		/* Reset the per-output-tuple exprcontext */
		ResetPerTupleExprContext(estate);

		/*
		 * Execute the plan and obtain a tuple
		 */
		slot = ExecProcNode((PlanState *) rstate);
		if (TupIsNull(slot))
			break;

		tuple = ExecFetchSlotTuple(slot);

		/*
		 * The first targrows sample rows are simply copied into the
		 * reservoir. Then we start replacing tuples in the sample
		 * until we reach the end of the relation.  This algorithm is
		 * from Jeff Vitter's paper (see full citation below). It
		 * works by repeatedly computing the number of tuples to skip
		 * before selecting a tuple, which replaces a randomly chosen
		 * element of the reservoir (current set of tuples).  At all
		 * times the reservoir is a true random sample of the tuples
		 * we've passed over so far, so when we fall off the end of
		 * the relation we're done.
		 */
		if (numrows < targrows)
			rows[numrows++] = heap_copytuple(tuple);
		else
		{
			/*
			 * t in Vitter's paper is the number of records already
			 * processed.  If we need to compute a new S value, we
			 * must use the not-yet-incremented value of samplerows as
			 * t.
			 */
			if (rowstoskip < 0)
				rowstoskip = reservoir_get_next_S(&rs, samplerows, targrows);

			if (rowstoskip <= 0)
			{
				/*
				 * Found a suitable tuple, so save it, replacing one
				 * old tuple at random
				 */
				int			k = (int) (targrows * sampler_random_fract(rs.randstate));

				Assert(k >= 0 && k < targrows);
				heap_freetuple(rows[k]);
				rows[k] = heap_copytuple(tuple);
			}

			rowstoskip -= 1;
		}

		samplerows += 1;
	}

	ExecEndRemoteQuery(rstate);

	FreeExecutorState(estate);

	/* We assume that we have no dead tuple. */
	*totaldeadrows = 0.0;

	/*
	 * We've retrieved "sampleper" percent living tuples from
	 * remote nodes. So we estimate that there are
	 *
	 *		samplerows * (100.0 / sampleper)
	 *
	 * rows at most.
	 */
	*totalrows = floor(samplerows * (100.0 / sampleper) + 0.5);

	/*
	 * Emit some interesting relation info
	 */
	ereport(elevel,
			(errmsg("\"%s\": containing %d rows in sample, %.0f estimated total rows",
					RelationGetRelationName(relation),
					numrows, *totalrows)));

	return numrows;
}
