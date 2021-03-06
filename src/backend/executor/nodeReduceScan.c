
#include "postgres.h"

#include "catalog/pg_collation_d.h"
#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/nodeHash.h"
#include "executor/nodeReduceScan.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/memutils.h"
#include "utils/sharedtuplestore.h"
#include "utils/typcache.h"

typedef struct RedcueScanSharedMemory
{
	SharedFileSet	sfs;
	char			padding[sizeof(SharedFileSet)%MAXIMUM_ALIGNOF ? 
							MAXIMUM_ALIGNOF - sizeof(SharedFileSet)%MAXIMUM_ALIGNOF : 0];
	char			sts_mem[FLEXIBLE_ARRAY_MEMBER];
}RedcueScanSharedMemory;

#define REDUCE_SCAN_SHM_SIZE(nbatch)													\
	(StaticAssertExpr(offsetof(RedcueScanSharedMemory, sts_mem) % MAXIMUM_ALIGNOF == 0,	\
					  "sts_mem not align to max"),										\
	 offsetof(RedcueScanSharedMemory, sts_mem) + MAXALIGN(sts_estimate(1)) * (nbatch))
#define REDUCE_SCAN_STS_ADDR(start, batch)		\
	(SharedTuplestore*)((char*)start + MAXALIGN(sts_estimate(1)) * batch)

int reduce_scan_bucket_size = 1024*1024;	/* 1MB */
int reduce_scan_max_buckets = 1024;

static List* InitHashFuncList(List *list);
static uint32 ExecReduceScanGetHashValue(ExprContext *econtext, List *exprs, List *fcinfos, bool *isnull);
static inline SharedTuplestoreAccessor* ExecGetReduceScanBatch(ReduceScanState *node, uint32 hashval)
{
	return node->batchs[hashval%node->nbatchs];
}

static TupleTableSlot *ExecReduceScan(PlanState *pstate)
{
	ReduceScanState *node = castNode(ReduceScanState, pstate);
	TupleTableSlot *scan_slot = node->scan_slot;
	ExprContext	   *econtext = pstate->ps_ExprContext;
	ProjectionInfo *projInfo = pstate->ps_ProjInfo;
	ExprState	   *qual = pstate->qual;
	MinimalTuple	mtup;
	uint32			hashval;

	if (unlikely(node->cur_batch == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("reduce scan plan %d not ready to scan", pstate->plan->plan_node_id)));

	ResetExprContext(econtext);
	for (;;)
	{
		/* when no hash, sts_scan_next ignore "&hashval" argument */
		mtup = sts_scan_next(node->cur_batch, &hashval);
		if (mtup == NULL)
			return ExecClearTuple(pstate->ps_ResultTupleSlot);

		if (node->scan_hash_exprs != NIL &&
			hashval != node->cur_hashval)
		{
			/* using hash and hash value not equal */
			InstrCountFiltered1(node, 1);
			continue;
		}

		econtext->ecxt_outertuple = ExecStoreMinimalTuple(mtup, scan_slot, false);
		if (ExecQual(qual, econtext))
			return ExecProject(projInfo);

		InstrCountFiltered1(node, 1);
		ResetExprContext(econtext);
	}
}

ReduceScanState *ExecInitReduceScan(ReduceScan *node, EState *estate, int eflags)
{
	Plan	   *outer_plan;
	PlanState  *outer_ps;
	ReduceScanState *rcs = makeNode(ReduceScanState);

	rcs->ps.plan = (Plan*)node;
	rcs->ps.state = estate;
	rcs->ps.ExecProcNode = ExecReduceScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &rcs->ps);

	outer_plan = outerPlan(node);
	outerPlanState(rcs) = outer_ps = ExecInitNode(outer_plan, estate, eflags & ~(EXEC_FLAG_REWIND|EXEC_FLAG_BACKWARD));
	rcs->ps.scandesc = ExecGetResultType(outerPlanState(rcs));

	/*
	 * All we using Var is OUTER_VAR, and we using MinimalTuple except function FetchReduceScanOuter,
	 * so initialize param, qual and projection using TTSOpsMinimalTuple
	 */
	rcs->ps.outerops = &TTSOpsMinimalTuple;
	rcs->ps.outeropsset = rcs->ps.outeropsfixed = true;
	rcs->scan_slot = ExecAllocTableSlot(&estate->es_tupleTable, rcs->ps.scandesc, &TTSOpsMinimalTuple);
	ExecInitResultTupleSlotTL(&rcs->ps, &TTSOpsMinimalTuple);
	rcs->ps.qual = ExecInitQual(node->plan.qual, (PlanState *) rcs);
	ExecConditionalAssignProjectionInfo(&rcs->ps, rcs->ps.scandesc, OUTER_VAR);
	Assert(rcs->ps.ps_ProjInfo != NULL);

	if(node->param_hash_keys != NIL)
	{
		size_t space_allowed;
		int nbuckets;
		int nskew_mcvs;
		int saved_work_mem = work_mem;
		Assert(list_length(node->param_hash_keys) == list_length(node->scan_hash_keys));

		rcs->ncols_hash = list_length(node->param_hash_keys);
		work_mem = reduce_scan_bucket_size;
		ExecChooseHashTableSize(outer_plan->plan_rows,
								outer_plan->plan_width,
								false,
								false,
								0,
								&space_allowed,
								&nbuckets,
								&rcs->nbatchs,
								&nskew_mcvs);
		work_mem = saved_work_mem;
		if (nbuckets > 0 &&
			nbuckets * rcs->nbatchs > rcs->nbatchs)
			rcs->nbatchs *= nbuckets;
		if (rcs->nbatchs > reduce_scan_max_buckets)
			rcs->nbatchs = reduce_scan_max_buckets;
		rcs->param_hash_exprs = ExecInitExprList(node->param_hash_keys, (PlanState*)rcs);
		rcs->param_hash_funs = InitHashFuncList(node->param_hash_keys);

		/* copy ops from outer */
		rcs->ps.outerops = outer_ps->resultops;
		rcs->ps.outeropsfixed = outer_ps->resultopsfixed;
		rcs->ps.outeropsset = outer_ps->resultopsset;
		rcs->scan_hash_exprs = ExecInitExprList(node->scan_hash_keys, (PlanState*)rcs);
		rcs->scan_hash_funs = InitHashFuncList(node->scan_hash_keys);
	}else
	{
		rcs->nbatchs = 1;
	}

	return rcs;
}

static TupleTableSlot *ExecEmptyReduceScan(PlanState *pstate)
{
	return ExecClearTuple(pstate->ps_ResultTupleSlot);
}

void FetchReduceScanOuter(ReduceScanState *node)
{
	TupleTableSlot	   *slot;
	PlanState		   *outer_ps;
	ExprContext		   *econtext;
	MemoryContext		oldcontext;
	RedcueScanSharedMemory *shm;
	MinimalTuple		mtup;
	int					i;
	bool				bool_val;

	if(node->batchs)
		return;

	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(node));

	node->dsm_seg = dsm_create(REDUCE_SCAN_SHM_SIZE(node->nbatchs), 0);
	shm = dsm_segment_address(node->dsm_seg);
	SharedFileSetInit(&shm->sfs, node->dsm_seg);

	node->batchs = palloc(sizeof(node->batchs[0]) * node->nbatchs);
	for (i=0;i<node->nbatchs;++i)
	{
		char name[64];
		sprintf(name, "reduce-scan-%d-b%d", node->ps.plan->plan_node_id, i);
		node->batchs[i] = sts_initialize(REDUCE_SCAN_STS_ADDR(shm->sts_mem, i),
										 1,
										 0,
										 node->scan_hash_funs ? sizeof(uint32) : 0,
										 0,
										 &shm->sfs,
										 name);
	}

	/* we need read all outer slot first */
	outer_ps = outerPlanState(node);
	econtext = node->ps.ps_ExprContext;
	if(node->scan_hash_exprs)
	{
		uint32 hashvalue;
		for(;;)
		{
			slot = ExecProcNode(outer_ps);
			if(TupIsNull(slot))
				break;

			ResetExprContext(econtext);
			econtext->ecxt_outertuple = slot;
			hashvalue = ExecReduceScanGetHashValue(econtext,
												   node->scan_hash_exprs,
												   node->scan_hash_funs,
												   &bool_val);
			if (bool_val)	/* is null */
				continue;

			mtup = ExecFetchSlotMinimalTuple(slot, &bool_val);
			sts_puttuple(ExecGetReduceScanBatch(node, hashvalue),
						 &hashvalue,
						 ExecFetchSlotMinimalTuple(slot, &bool_val));
		}
	}else
	{
		SharedTuplestoreAccessor *accessor = node->batchs[0];
		for(;;)
		{
			slot = ExecProcNode(outer_ps);
			if(TupIsNull(slot))
				break;

			mtup = ExecFetchSlotMinimalTuple(slot, &bool_val);
			sts_puttuple(accessor, NULL, mtup);
			if(bool_val)	/* is null */
				pfree(mtup);
		}
		node->cur_batch = node->batchs[0];
	}

	for (i=0;i<node->nbatchs;++i)
		sts_end_write(node->batchs[i]);

	if (node->cur_batch)
		sts_begin_scan(node->cur_batch);

	MemoryContextSwitchTo(oldcontext);

	if (node->ps.instrument)
		InstrStopNode(node->ps.instrument, 0.0);
}

void ExecEndReduceScan(ReduceScanState *node)
{
	int i;
	if (node->batchs)
	{
		for (i=0;i<node->nbatchs;++i)
		{
			if (node->batchs[i])
				sts_detach(node->batchs[i]);
		}
		pfree(node->batchs);
		node->batchs = NULL;
		node->nbatchs = 0;
	}
	node->cur_batch = NULL;
	if (node->dsm_seg)
	{
		dsm_detach(node->dsm_seg);
		node->dsm_seg = NULL;
	}
	ExecEndNode(outerPlanState(node));
}

void ExecReduceScanMarkPos(ReduceScanState *node)
{
	elog(ERROR, "not support yet!");
}

void ExecReduceScanRestrPos(ReduceScanState *node)
{
	elog(ERROR, "not support yet!");
}

void ExecReScanReduceScan(ReduceScanState *node)
{
	if (node->cur_batch != NULL)
	{
		sts_end_scan(node->cur_batch);
		node->cur_batch = NULL;
	}

	if (node->origin_state &&
		node->batchs == NULL)
	{
		ReduceScanState *origin = node->origin_state;
		MemoryContext	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(node));
		RedcueScanSharedMemory *shm = dsm_segment_address(origin->dsm_seg);
		int				i;
		node->nbatchs = origin->nbatchs;
		node->batchs = palloc0(sizeof(node->batchs[0]) * node->nbatchs);
		for (i=0;i<node->nbatchs;++i)
		{
			node->batchs[i] = sts_attach_read_only(REDUCE_SCAN_STS_ADDR(shm->sts_mem, i),
												   &shm->sfs);
		}
		MemoryContextSwitchTo(oldcontext);
	}

	if (node->batchs == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("reduce scan %d not fetch outer yet", node->ps.plan->plan_node_id)));

	if(node->param_hash_exprs)
	{
		ExprContext *econtext = node->ps.ps_ExprContext;
		node->cur_hashval = ExecReduceScanGetHashValue(econtext,
													   node->param_hash_exprs,
													   node->param_hash_funs,
													   &node->cur_hash_is_null);
		if (node->cur_hash_is_null)
		{
			node->cur_batch = NULL;
			ExecSetExecProcNode(&node->ps, ExecEmptyReduceScan);
		}else
		{
			node->cur_batch = ExecGetReduceScanBatch(node, node->cur_hashval);
		}
	}else
	{
		node->cur_batch = node->batchs[0];
	}

	if (node->cur_batch)
	{
		ExecSetExecProcNode(&node->ps, ExecReduceScan);
		sts_begin_scan(node->cur_batch);
	}
}

static uint32 ExecReduceScanGetHashValue(ExprContext *econtext, List *exprs, List *fcinfos, bool *isnull)
{
	ListCell		   *lc,*lc2;
	FunctionCallInfo	fcinfo;
	ExprState		   *expr_state;
	Datum				key_value;
	uint32				hash_value = 0;
	Assert(list_length(exprs) == list_length(fcinfos));

	forboth(lc, exprs, lc2, fcinfos)
	{
		expr_state = lfirst(lc);
		fcinfo = lfirst(lc2);

		fcinfo->args[0].value = ExecEvalExpr(expr_state, econtext, isnull);
		if (*isnull)
			return 0;

		key_value = FunctionCallInvoke(fcinfo);
		if (unlikely(fcinfo->isnull))
		{
			ereport(ERROR,
					errmsg("hash function %u returned NULL", fcinfo->flinfo->fn_oid));
		}
		hash_value = hash_combine(hash_value, DatumGetUInt32(key_value));
	}

	return hash_value;
}

static bool SetEmptyResultWalker(ReduceScanState *state, void *context)
{
	if (state == NULL)
		return false;

	if (IsA(state, ReduceScanState))
	{
		ExecSetExecProcNode(&state->ps, ExecEmptyReduceScan);
		if (state->cur_batch)
		{
			sts_end_scan(state->cur_batch);
			state->cur_batch = NULL;
		}
		return false;
	}

	return planstate_tree_walker(&state->ps, SetEmptyResultWalker, context);
}

void BeginDriveClusterReduce(PlanState *node)
{
	SetEmptyResultWalker((ReduceScanState*)node, NULL);
}

void ExecSetReduceScanEPQOrigin(ReduceScanState *node, ReduceScanState *origin)
{
	Assert(IsA(node, ReduceScanState) && IsA(origin, ReduceScanState));
	Assert(node->ps.plan->plan_node_id == origin->ps.plan->plan_node_id);

	if (origin->origin_state)
		node->origin_state = origin->origin_state;
	else
		node->origin_state = origin;
}

static List*
InitHashFuncList(List *list)
{
	Oid					collid;
	TypeCacheEntry	   *typeCache;
	FunctionCallInfo    fcinfo;
	FmgrInfo		   *flinfo;
	Node			   *expr;
	ListCell		   *lc;
	List			   *result = NIL;

	foreach (lc, list)
	{
		expr = lfirst(lc);

		typeCache = lookup_type_cache(exprType(expr), TYPECACHE_HASH_PROC);
		Assert(OidIsValid(typeCache->hash_proc));

		flinfo = palloc(MAXALIGN(sizeof(FmgrInfo)) + SizeForFunctionCallInfo(1));
		fcinfo = (FunctionCallInfo)(((char*)flinfo) + MAXALIGN(sizeof(FmgrInfo)));
		fmgr_info(typeCache->hash_proc, flinfo);
		fmgr_info_set_expr(expr, flinfo);
		collid = exprCollation(expr);
		InitFunctionCallInfoData(*fcinfo, flinfo, 1, collid, NULL, NULL);
		fcinfo->args[0].isnull = false;

		result = lappend(result, fcinfo);
	}

	return result;
}