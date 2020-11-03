#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "executor/execExpr.h"
#include "executor/hashjoin.h"
#include "executor/nodeConnectBy.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeSubplan.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"

#define START_WITH_UNCHECK		0
#define START_WITH_NOT_EMPTY	1
#define START_WITH_HAS_EMPTY	-1

#define CHECK_START_WITH(cbstate_)												\
	do																			\
	{																			\
		if ((cbstate_)->check_start_state == START_WITH_UNCHECK)				\
		{																		\
			ConnectByPlan *plan = castNode(ConnectByPlan, (cbstate_)->ps.plan);	\
			if (IsQualHasEmptySubPlan(&(cbstate_)->ps, plan->start_with))		\
			{																	\
				(cbstate_)->check_start_state = START_WITH_HAS_EMPTY;			\
				return ExecClearTuple(slot);									\
			}else																\
			{																	\
				(cbstate_)->check_start_state = START_WITH_NOT_EMPTY;			\
			}																	\
		}																		\
	}while(0)

typedef enum CBMethod
{
	CB_NEST = 1,
	CB_HASH,
	CB_TUPLESORT,
	CB_SORTHASH
}CBMethod;
#define CONNECT_BY_METHOD(state_) (*((CBMethod*)(state_)->private_state))

/* for no order connect by */
typedef struct NestConnectByState
{
	CBMethod		method;
	Tuplestorestate *scan_ts;
	Tuplestorestate *save_ts;
	int				hash_reader;
	bool			inner_ateof;
}NestConnectByState;

typedef struct HashConnectByState
{
	CBMethod		method;
	BufFile		  **outer_save;
	List		   *outer_HashKeys;
	List		   *inner_HashKeys;
	List		   *hj_hashOperators;
	List		   *hj_Collations;
	ExprState	   *hash_clauses;
	HashJoinTuple	cur_tuple;
	int				cur_skewno;
	int				cur_bucketno;
	uint32			cur_hashvalue;
	bool			inner_ateof;
}HashConnectByState;

typedef struct TuplestoreConnectByLeaf
{
	slist_node		snode;
	MinimalTuple	outer_tup;
	Tuplesortstate *scan_ts;
}TuplestoreConnectByLeaf;

typedef struct TuplesortConnectByState
{
	CBMethod		method;
	slist_head		slist_level;
	slist_head		slist_idle;
	ProjectionInfo *sort_project;
	TupleTableSlot *sort_slot;
}TuplesortConnectByState;

#define InvalidBatchNumber	-1
typedef void (*MaterialSaveRootRow)(ConnectByState *cbstate, TupleTableSlot *slot);
typedef void (*MaterialSaveLevelRow)(ConnectByState *cbstate, TupleTableSlot *slot,
									 uint32 hashval, int batch_no, bool qual);
typedef void (*MaterialEndLevelRow)(ConnectByState *cbstate);

typedef struct MaterialHashConnectByState
{
	HashConnectByState	base;
	void			   *material_state;
	TupleDesc			material_desc;
	TupleTableSlot	   *material_slot;
	ProjectionInfo	   *material_proj;
	MaterialSaveRootRow save_root;
	MaterialEndLevelRow end_root;
	MaterialSaveLevelRow save_level;
	MaterialEndLevelRow end_level;
}MaterialHashConnectByState;

typedef struct HashSortRowData
{
	TupleTableSlot *slot;
	MinimalTuple	mtup;
	uint32			hashval;
	int				batch_no;
	bool			qual;
}HashSortRowData;

typedef struct SortHashConnectByState
{
	MaterialHashConnectByState
						base;
	Tuplesortstate	   *root_sort;
	List			   *final_sort_tlist;
	TupleTableSlot	   *sub_sort_slot;
	TupleDesc			sub_sort_desc;
	ProjectionInfo	   *sub_sort_proj;				/* get sort columns from outer */
	HashSortRowData	   *sub_sort_rows;
	SortSupport			sub_sort_support;
	uint32				sub_sort_row_size;
	uint32				sub_sort_row_space;
	AttrNumber			final_sort_index;			/* final sort column, type is int8[] */
	AttrNumber			outer_sort_index;			/* saved sort column, type is int8[] */
}SortHashConnectByState;
#define HASH_SORT_ROW_STEP_SIZE		32

static TupleTableSlot *ExecNestConnectBy(PlanState *pstate);
static TupleTableSlot *ExecHashConnectBy(PlanState *pstate);
static TupleTableSlot *ExecSortConnectBy(PlanState *pstate);
static TupleTableSlot *ExecFirstSortHashConnectBy(PlanState *state);
static TupleTableSlot *ExecSortHashConnectBy(PlanState *pstate);

static TupleTableSlot* ExecNestConnectByStartWith(ConnectByState *ps);
static TuplestoreConnectByLeaf* GetConnectBySortLeaf(ConnectByState *ps);
static TuplestoreConnectByLeaf *GetNextTuplesortLeaf(ConnectByState *cbstate, TupleTableSlot *parent_slot);
static TupleTableSlot *InsertRootHashValue(ConnectByState *cbstate, TupleTableSlot *slot);
static TupleTableSlot *InsertMaterialHashValue(ConnectByState *cbstate, TupleTableSlot *slot);
static bool ExecHashNewBatch(HashJoinTable hashtable, HashConnectByState *state, TupleTableSlot *slot);
static void ProcessTuplesortRoot(ConnectByState *cbstate, TuplestoreConnectByLeaf *leaf);
static void RestartBufFile(BufFile *file);

static void ExecInitHashConnectBy(HashConnectByState *state, ConnectByState *pstate)
{
	ConnectByPlan  *node = castNode(ConnectByPlan, pstate->ps.plan);
	List		   *rhclause = NIL;
	ListCell	   *lc;
	OpExpr		   *op;
	Oid				left_hash;
	Oid				right_hash;

	foreach (lc, node->hash_quals)
	{
		/* make hash ExprState(s) */
		op = lfirst_node(OpExpr, lc);
		if (get_op_hash_functions(op->opno, &left_hash, &right_hash) == false)
		{
			ereport(ERROR,
					(errmsg("could not find hash function for hash operator %u", op->opno)));
		}
		state->outer_HashKeys = lappend(state->outer_HashKeys,
										ExecInitExpr(linitial(op->args), &pstate->ps));

		state->inner_HashKeys = lappend(state->inner_HashKeys,
										ExecInitExpr(llast(op->args), &pstate->ps));

		rhclause = lappend(rhclause, ExecInitExpr(llast(op->args), outerPlanState(pstate)));

		state->hj_hashOperators = lappend_oid(state->hj_hashOperators, op->opno);
		state->hj_Collations = lappend_oid(state->hj_Collations, op->inputcollid);
	}

	state->hash_clauses = ExecInitQual(node->hash_quals, &pstate->ps);
	castNode(HashState, outerPlanState(pstate))->hashkeys = rhclause;
}

static TupleDesc MakeCycleCheckInputDesc(PlanState *ps)
{
	int i;
	TupleDesc result;
	TupleDesc input = ExecGetResultType(ps);

	result = CreateTemplateTupleDesc(input->natts+1, false);
	for (i=1;i<=input->natts;++i)
		TupleDescCopyEntry(result, i, input, i);
	TupleDescInitEntry(result, input->natts+1, NULL, INT8OID, -1, 0);

	return result;
}

ConnectByState* ExecInitConnectBy(ConnectByPlan *node, EState *estate, int eflags)
{
	ConnectByState *cbstate;
	TupleDesc		input_desc;
	TupleDesc		save_desc;
	List		   *save_tlist;


	cbstate = makeNode(ConnectByState);
	cbstate->ps.plan = (Plan*)node;
	cbstate->ps.state = estate;

	if (node->hash_quals != NIL)
		eflags &= ~(EXEC_FLAG_REWIND|EXEC_FLAG_MARK);
	outerPlanState(cbstate) = ExecInitNode(outerPlan(node), estate, 0);

	ExecAssignExprContext(estate, &cbstate->ps);
	ExecInitResultTupleSlotTL(estate, &cbstate->ps);

	input_desc = MakeCycleCheckInputDesc(outerPlanState(cbstate));
	cbstate->inner_slot = ExecInitExtraTupleSlot(estate, input_desc);
	cbstate->start_with = ExecInitQual(node->start_with, &cbstate->ps);
	cbstate->ps.qual = ExecInitQual(node->plan.qual, &cbstate->ps);
	cbstate->joinclause = ExecInitQual(node->join_quals, &cbstate->ps);

	ExecAssignProjectionInfo(&cbstate->ps, input_desc);
	save_tlist = list_copy(node->save_targetlist);

	if (node->hash_quals != NIL)
	{
		if(node->numCols == 0)
		{
			HashConnectByState *state = palloc0(sizeof(*state));
			ExecInitHashConnectBy(state, cbstate);

			state->method = CB_HASH;
			cbstate->private_state = state;
			cbstate->ps.ExecProcNode = ExecHashConnectBy;
		}else
		{
			SortHashConnectByState *state = palloc0(sizeof(*state));
			Const *sort_col;
			int i;
			ExecInitHashConnectBy(&state->base.base, cbstate);
			state->sub_sort_desc = ExecTypeFromTL(node->sort_targetlist, false);
			state->sub_sort_slot = ExecInitExtraTupleSlot(estate,
														  state->sub_sort_desc);
			state->sub_sort_proj = ExecBuildProjectionInfo(node->sort_targetlist,
														   cbstate->ps.ps_ExprContext,
														   state->sub_sort_slot,
														   &cbstate->ps,
														   input_desc);

			state->sub_sort_support = palloc0(sizeof(state->sub_sort_support[0]) * node->numCols);
			for (i=0;i<node->numCols;++i)
			{
				SortSupport sortkey = &state->sub_sort_support[i];
				sortkey->ssup_cxt = CurrentMemoryContext;
				sortkey->ssup_collation = node->collations[i];
				sortkey->ssup_nulls_first = node->nullsFirst[i];
				sortkey->ssup_attno = node->sortColIdx[i];
				PrepareSortSupportFromOrderingOp(node->sortOperators[i], sortkey);
			}

			/*
			 * make final sort target list, (target list) + int8[]
			 */
			sort_col = makeNullConst(INT8ARRAYOID, -1, InvalidOid);
			state->final_sort_tlist = list_copy(node->plan.targetlist);
			state->final_sort_index = list_length(state->final_sort_tlist);
			state->final_sort_tlist = lappend(state->final_sort_tlist,
											  makeTargetEntry((Expr*)sort_col,
															  state->final_sort_index+1,
															  NULL,
															  true));
			state->base.material_desc = ExecTypeFromTL(state->final_sort_tlist, false);
			state->base.material_slot = ExecInitExtraTupleSlot(estate,
															   state->base.material_desc);
			state->base.material_proj = ExecBuildProjectionInfo(state->final_sort_tlist,
																cbstate->ps.ps_ExprContext,
																state->base.material_slot,
																&cbstate->ps,
																input_desc);
			state->base.base.method = CB_SORTHASH;
			cbstate->private_state = state;
			cbstate->ps.ExecProcNode = ExecFirstSortHashConnectBy;

			/* make save project, add a int8[] */
			state->outer_sort_index = list_length(save_tlist);
			save_tlist = lappend(save_tlist,
								 makeTargetEntry((Expr*)sort_col,
												 state->outer_sort_index,
												 NULL,
												 true));
		}
	}else
	{
		cbstate->ts = tuplestore_begin_heap(false, false, work_mem);
		tuplestore_set_eflags(cbstate->ts, EXEC_FLAG_REWIND);

		if (node->numCols == 0)
		{
			NestConnectByState *state = palloc0(sizeof(NestConnectByState));
			state->method = CB_NEST;
			cbstate->ps.ExecProcNode = ExecNestConnectBy;
			cbstate->private_state = state;
			state->inner_ateof = true;
			state->scan_ts = tuplestore_begin_heap(false, false, work_mem/2);
			state->save_ts = tuplestore_begin_heap(false, false, work_mem/2);
		}else
		{
			TuplestoreConnectByLeaf *leaf;
			TuplesortConnectByState *state = palloc0(sizeof(TuplesortConnectByState));
			state->method = CB_TUPLESORT;
			cbstate->ps.ExecProcNode = ExecSortConnectBy;
			cbstate->private_state = state;
			slist_init(&state->slist_level);
			slist_init(&state->slist_idle);
			state->sort_slot = ExecInitExtraTupleSlot(estate,
													  ExecTypeFromTL(node->sort_targetlist, false));
			state->sort_project = ExecBuildProjectionInfo(node->sort_targetlist,
														  cbstate->ps.ps_ExprContext,
														  state->sort_slot,
														  &cbstate->ps,
														  input_desc);

			leaf = GetConnectBySortLeaf(cbstate);
			slist_push_head(&state->slist_level, &leaf->snode);
		}
	}

	save_desc = ExecTypeFromTL(save_tlist, false);
	cbstate->outer_slot = ExecInitExtraTupleSlot(estate, save_desc);
	cbstate->pj_save_targetlist = ExecBuildProjectionInfo(node->save_targetlist,
														  cbstate->ps.ps_ExprContext,
														  ExecInitExtraTupleSlot(estate, save_desc),
														  &cbstate->ps,
														  input_desc);

	cbstate->level = 1L;
	cbstate->check_start_state = START_WITH_UNCHECK;
	cbstate->processing_root = true;

	return cbstate;
}

static TupleTableSlot *ExecNestConnectBy(PlanState *pstate)
{
	ConnectByState *cbstate = castNode(ConnectByState, pstate);
	NestConnectByState *state = cbstate->private_state;
	TupleTableSlot *outer_slot;
	TupleTableSlot *inner_slot;
	ExprContext *econtext = cbstate->ps.ps_ExprContext;

	if (cbstate->processing_root)
	{
reget_start_with_:
		inner_slot = ExecNestConnectByStartWith(cbstate);
		if (!TupIsNull(inner_slot))
		{
			CHECK_FOR_INTERRUPTS();
			econtext->ecxt_innertuple = inner_slot;
			econtext->ecxt_outertuple = ExecClearTuple(cbstate->outer_slot);
			tuplestore_puttupleslot(state->save_ts,
									ExecProject(cbstate->pj_save_targetlist));
			if (pstate->qual == NULL ||
				ExecQual(pstate->qual, econtext))
			{
				return ExecProject(pstate->ps_ProjInfo);
			}
			InstrCountFiltered1(pstate, 1);
			goto reget_start_with_;
		}

		state->inner_ateof = true;
		cbstate->processing_root = false;
	}

	outer_slot = cbstate->outer_slot;
	inner_slot = cbstate->inner_slot;
	econtext = cbstate->ps.ps_ExprContext;

re_get_tuplestore_connect_by_:
	if (state->inner_ateof)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(pstate));
		tuplestore_gettupleslot(state->scan_ts, true, true, outer_slot);
		MemoryContextSwitchTo(oldcontext);
		if (TupIsNull(outer_slot))
		{
			/* switch work tuplestore */
			Tuplestorestate *ts = state->save_ts;
			state->save_ts = state->scan_ts;
			state->scan_ts = ts;
			tuplestore_clear(state->save_ts);

			/* read new data from last saved tuplestore */
			oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(pstate));
			tuplestore_gettupleslot(state->scan_ts, true, true, outer_slot);
			MemoryContextSwitchTo(oldcontext);

			if (TupIsNull(outer_slot))	/* no more data, end plan */
				return ExecClearTuple(pstate->ps_ProjInfo->pi_state.resultslot);
			++(cbstate->level);
		}

		tuplestore_rescan(cbstate->ts);
		state->inner_ateof = false;
	}

	for(;;)
	{
		CHECK_FOR_INTERRUPTS();
		tuplestore_gettupleslot(cbstate->ts, true, false, inner_slot);
		if (TupIsNull(inner_slot))
			break;

		econtext->ecxt_innertuple = inner_slot;
		econtext->ecxt_outertuple = outer_slot;
		if (ExecQualAndReset(cbstate->joinclause, econtext))
		{
			tuplestore_puttupleslot(state->save_ts,
									ExecProject(cbstate->pj_save_targetlist));
			if (pstate->qual == NULL ||
				ExecQual(pstate->qual, econtext))
			{
				return ExecProject(pstate->ps_ProjInfo);
			}
			InstrCountFiltered1(pstate, 1);
		}
	}

	state->inner_ateof = true;
	goto re_get_tuplestore_connect_by_;
}

static TupleTableSlot *ExecHashConnectBy(PlanState *pstate)
{
	ConnectByState *cbstate = castNode(ConnectByState, pstate);
	HashConnectByState *state = cbstate->private_state;
	ExprContext *econtext = cbstate->ps.ps_ExprContext;
	HashJoinTable hjt = cbstate->hjt;
	TupleTableSlot *inner_slot = cbstate->inner_slot;
	TupleTableSlot *outer_slot = cbstate->outer_slot;
	BufFile *file;
	uint32 hashvalue;

	if (cbstate->processing_root)
	{
		if (hjt == NULL)
		{
			/* initialize */
			HashState *hash = castNode(HashState, outerPlanState(pstate));
			int i;

			hjt = ExecHashTableCreate(hash,
									  state->hj_hashOperators,
									  true);	/* inner join not need keep nulls */
			cbstate->hjt = hjt;
			hash->hashtable = hjt;
			if (hjt->outerBatchFile == NULL)
			{
				Assert(hjt->nbatch == 1);
				hjt->outerBatchFile = MemoryContextAllocZero(hjt->hashCxt, sizeof(BufFile**));
				hjt->innerBatchFile = MemoryContextAllocZero(hjt->hashCxt, sizeof(BufFile**));
			}
			/* I am not sure about the impact of "grow", so disable it */
			hjt->growEnabled = false;
			MultiExecHashEx(hash, InsertRootHashValue, cbstate);

			/* prepare for save joind tuple */
			state->outer_save = MemoryContextAllocZero(GetMemoryChunkContext(hjt->outerBatchFile),
													   sizeof(BufFile*) * hjt->nbatch);

			/* save batch 0 to BufFile, we need rescan */
			for (i=hjt->nbuckets;--i>=0;)
			{
				HashJoinTuple hashTuple = hjt->buckets.unshared[i];
				while (hashTuple != NULL)
				{
					ExecHashJoinSaveTuple(HJTUPLE_MINTUPLE(hashTuple),
										  hashTuple->hashvalue,
										  &hjt->innerBatchFile[0]);
					hashTuple = hashTuple->next.unshared;
				}
			}
			/* seek batch file to start */
			for(i=0;i<hjt->nbatch;++i)
				RestartBufFile(hjt->outerBatchFile[i]);
		}else if (cbstate->is_rescan)
		{
			int i;
			/* clean buf file */
			for(i=0; i<hjt->nbatch; ++i)
			{
				BufFile *file = hjt->outerBatchFile[i];
				if (file == NULL)
					continue;

				BufFileClose(file);
				hjt->outerBatchFile[i] = NULL;
			}
			/* make start with */
			for(i=hjt->nbatch;--i>=0;)
			{
				BufFile *file = hjt->innerBatchFile[i];
				uint32 hashvalue;
				if (file == NULL)
					continue;

				RestartBufFile(file);
				for(;;)
				{
					ExecHashJoinReadTuple(file, &hashvalue, inner_slot);
					if (TupIsNull(inner_slot))
						break;
					InsertRootHashValue(cbstate, inner_slot);
				}
			}
			for (i=hjt->nSkewBuckets;--i>=0;)
			{
				HashJoinTuple hashTuple;
				if (hjt->skewBucket[i])
				{
					hashTuple = hjt->skewBucket[i]->tuples;
					while (hashTuple)
					{
						ExecStoreMinimalTuple(HJTUPLE_MINTUPLE(hashTuple),
											  inner_slot,
											  false);
						InsertRootHashValue(cbstate, inner_slot);
						hashTuple = hashTuple->next.unshared;
					}
				}
			}
			/* seek batch file to start */
			for(i=0;i<hjt->nbatch;++i)
				RestartBufFile(hjt->outerBatchFile[i]);
			if (hjt->nbatch > 1)
			{
				hjt->curbatch = -1;
				ExecHashNewBatch(hjt, state, inner_slot);
			}
			cbstate->is_rescan = false;
		}

reget_start_with_:
		CHECK_FOR_INTERRUPTS();
		if (hjt->curbatch >= hjt->nbatch)
		{
			cbstate->processing_root = false;
			hjt->curbatch = 0;
			RestartBufFile(hjt->outerBatchFile[hjt->curbatch]);
			ExecClearTuple(outer_slot);
			++(cbstate->level);
			goto reget_hash_connect_by_;
		}
		file = hjt->outerBatchFile[hjt->curbatch];
		if (file == NULL)
		{
			++hjt->curbatch;
			goto reget_start_with_;
		}
		for(;;)
		{
			ExecHashJoinReadTuple(file, &hashvalue, inner_slot);
			if (TupIsNull(inner_slot))
			{
				/* end of current batch */
				++hjt->curbatch;
				goto reget_start_with_;
			}
			econtext->ecxt_innertuple = inner_slot;
			econtext->ecxt_outertuple = ExecClearTuple(cbstate->outer_slot);
			if (pstate->qual == NULL ||
				ExecQual(pstate->qual, econtext))
			{
				return ExecProject(pstate->ps_ProjInfo);
			}
			InstrCountFiltered1(pstate, 1);
		}
	}

reget_hash_connect_by_:
	CHECK_FOR_INTERRUPTS();
	if (TupIsNull(outer_slot))
	{
		file = hjt->outerBatchFile[hjt->curbatch];
		if (file == NULL)
			ExecClearTuple(outer_slot);
		else
			ExecHashJoinReadTuple(file, &hashvalue, outer_slot);
		if (TupIsNull(outer_slot))
		{
			if (ExecHashNewBatch(hjt, state, inner_slot) == false)
			{
				/* check prior is empty */
				int i;
				int count = hjt->nbatch;
				bool not_empty = false;
				for (i=0;i<count;++i)
				{
					if (state->outer_save[i] != NULL)
					{
						RestartBufFile(state->outer_save[i]);
						not_empty = true;
					}
				}
				if (not_empty == false)
					return NULL;	/* no more level */

				/* switch outer BufFile */
				{
					BufFile **tmp = hjt->outerBatchFile;
					hjt->outerBatchFile = state->outer_save;
					state->outer_save = tmp;
				}
				/* and reset HashJoinTable */
				if (hjt->curbatch != 0)
				{
					hjt->curbatch = -1;
					not_empty = ExecHashNewBatch(hjt, state, inner_slot);
					Assert(not_empty);
				}
				++(cbstate->level);
			}
			goto reget_hash_connect_by_;
		}

		state->cur_hashvalue = hashvalue;
		state->cur_skewno = ExecHashGetSkewBucket(hjt, hashvalue);
		if (state->cur_skewno == INVALID_SKEW_BUCKET_NO)
		{
			int batch_no;
			ExecHashGetBucketAndBatch(hjt, hashvalue, &state->cur_bucketno, &batch_no);
			Assert(batch_no == hjt->curbatch);
		}
		state->cur_tuple = NULL;
	}

	econtext->ecxt_innertuple = inner_slot;
	econtext->ecxt_outertuple = outer_slot;
	if (ExecScanHashBucketExt(econtext,
							  state->hash_clauses,
							  &state->cur_tuple,
							  state->cur_hashvalue,
							  state->cur_skewno,
							  state->cur_bucketno,
							  hjt,
							  inner_slot) == false)
	{
		ExecClearTuple(outer_slot);
		goto reget_hash_connect_by_;
	}

	if (cbstate->joinclause &&
		ExecQualAndReset(cbstate->joinclause, econtext) == false)
	{
		goto reget_hash_connect_by_;
	}

	/* inner tuple is outer tuple at next level */
	econtext->ecxt_outertuple = inner_slot;
	if (ExecHashGetHashValue(hjt,
							 econtext,
							 state->outer_HashKeys,
							 true,
							 false,
							 &hashvalue))
	{
		TupleTableSlot *save_slot;
		int batch_no;
		int bucket_no;

		econtext->ecxt_outertuple = outer_slot;
		ExecHashGetBucketAndBatch(hjt, hashvalue, &bucket_no, &batch_no);
		if (hjt->innerBatchFile[batch_no] != NULL)
		{
			/* don't need save it when inner batch is empty */
			save_slot = ExecProject(cbstate->pj_save_targetlist);
			ExecHashJoinSaveTuple(ExecFetchSlotMinimalTuple(save_slot),
								hashvalue,
								&state->outer_save[batch_no]);
		}
	}
	econtext->ecxt_outertuple = outer_slot;

	if (pstate->qual == NULL ||
		ExecQualAndReset(pstate->qual, econtext))
	{
		return ExecProject(pstate->ps_ProjInfo);
	}

	InstrCountFiltered1(pstate, 1);
	goto reget_hash_connect_by_;
}

static TupleTableSlot *ExecSortConnectBy(PlanState *pstate)
{
	ConnectByState *cbstate = castNode(ConnectByState, pstate);
	TuplesortConnectByState *state = cbstate->private_state;
	ExprContext *econtext = cbstate->ps.ps_ExprContext;
	TupleTableSlot *outer_slot;
	//TupleTableSlot *inner_slot;
	TupleTableSlot *sort_slot;
	TupleTableSlot *save_slot;
	TupleTableSlot *result_slot;
	TuplestoreConnectByLeaf *leaf;

	if (cbstate->processing_root)
	{
		leaf = slist_head_element(TuplestoreConnectByLeaf, snode, &state->slist_level);
		Assert(leaf->snode.next == NULL);
		ProcessTuplesortRoot(cbstate, leaf);
		tuplesort_performsort(leaf->scan_ts);
		leaf->outer_tup = NULL;
		cbstate->processing_root = false;
	}

	outer_slot = cbstate->outer_slot;
	//inner_slot = cbstate->inner_slot;
	sort_slot = state->sort_slot;

re_get_tuplesort_connect_by_:
	CHECK_FOR_INTERRUPTS();
	if (slist_is_empty(&state->slist_level))
	{
		Assert(cbstate->level == 0L);
		return ExecClearTuple(pstate->ps_ResultTupleSlot);
	}

	leaf = slist_head_element(TuplestoreConnectByLeaf, snode, &state->slist_level);
	if (tuplesort_gettupleslot(leaf->scan_ts, true, false, sort_slot, NULL) == false)
	{
		/* end of current leaf */
		slist_pop_head_node(&state->slist_level);
		slist_push_head(&state->slist_idle, &leaf->snode);
		--(cbstate->level);
		tuplesort_end(leaf->scan_ts);
		leaf->scan_ts = NULL;
		if (leaf->outer_tup)
		{
			pfree(leaf->outer_tup);
			leaf->outer_tup = NULL;
		}
		goto re_get_tuplesort_connect_by_;
	}

	if (leaf->outer_tup)
	{
		Assert(cbstate->level > 1L);
		ExecStoreMinimalTuple(leaf->outer_tup, outer_slot, false);
	}else
	{
		Assert(cbstate->level == 1L);
		ExecClearTuple(outer_slot);
	}
	econtext->ecxt_outertuple = outer_slot;
	econtext->ecxt_innertuple = sort_slot;
	if (pstate->qual == NULL ||
		ExecQual(pstate->qual, econtext))
	{
		result_slot = ExecProject(pstate->ps_ProjInfo);
		/* function GetNextTuplesortLeaf well free Datum, so we need materialize result */
		ExecMaterializeSlot(pstate->ps_ResultTupleSlot);
	}else
	{
		InstrCountFiltered1(pstate, 1);
		result_slot = ExecClearTuple(pstate->ps_ResultTupleSlot);
	}

	save_slot = ExecProject(cbstate->pj_save_targetlist);
	++(cbstate->level);
	ExecMaterializeSlot(save_slot);		/* GetNextTuplesortLeaf will reset memory context */
	leaf = GetNextTuplesortLeaf(cbstate, save_slot);
	if (leaf)
	{
		leaf->outer_tup = ExecCopySlotMinimalTuple(save_slot);
		tuplesort_performsort(leaf->scan_ts);
		slist_push_head(&state->slist_level, &leaf->snode);
	}else
	{
		--(cbstate->level);
	}

	if (TupIsNull(result_slot))
		goto re_get_tuplesort_connect_by_;	/* removed by qual */

	return pstate->ps_ResultTupleSlot;
}

static void FirstMaterialHashConnectBy(ConnectByState *cbstate, MaterialHashConnectByState *state)
{
	ExprContext			   *econtext = cbstate->ps.ps_ExprContext;
	HashJoinTable			hjt = cbstate->hjt;
	TupleTableSlot		   *inner_slot = cbstate->inner_slot;
	TupleTableSlot		   *outer_slot = cbstate->outer_slot;
	BufFile				   *file;
	uint32					hashvalue;
	int						i;

	cbstate->processing_root = true;
	cbstate->level = 1L;

	if (hjt == NULL)
	{
		/* initialize */
		HashState *hash = castNode(HashState, outerPlanState(cbstate));
		hjt = ExecHashTableCreate(hash,
								  state->base.hj_hashOperators,
								  false);	/* inner join not need keep nulls */
		cbstate->hjt = hjt;
		hash->hashtable = hjt;
		if (hjt->outerBatchFile == NULL)
		{
			Assert(hjt->nbatch == 1);
			hjt->outerBatchFile = MemoryContextAllocZero(hjt->hashCxt, sizeof(BufFile**));
			hjt->innerBatchFile = MemoryContextAllocZero(hjt->hashCxt, sizeof(BufFile**));
		}
		/* prepare for save joind tuple */
		state->base.outer_save = MemoryContextAllocZero(GetMemoryChunkContext(hjt->outerBatchFile),
														sizeof(BufFile*) * hjt->nbatch);
		/* I am not sure about the impact of "grow", so disable it */
		hjt->growEnabled = false;
	}

	MultiExecHashEx(castNode(HashState, outerPlanState(cbstate)), InsertMaterialHashValue, cbstate);
	/* save batch 0 to BufFile, we need rescan */
	for (i=hjt->nbuckets;--i>=0;)
	{
		HashJoinTuple hashTuple = hjt->buckets.unshared[i];
		while (hashTuple != NULL)
		{
			ExecHashJoinSaveTuple(HJTUPLE_MINTUPLE(hashTuple),
								  hashTuple->hashvalue,
								  &hjt->innerBatchFile[0]);
			hashTuple = hashTuple->next.unshared;
		}
	}
	state->end_root(cbstate);

	cbstate->processing_root = false;

re_connect_by_:
	cbstate->level++;
	while (hjt->curbatch < hjt->nbatch)
	{
		CHECK_FOR_INTERRUPTS();
		if (hjt->outerBatchFile[hjt->curbatch] == NULL ||
			hjt->innerBatchFile[hjt->curbatch] == NULL)
		{
			if (ExecHashNewBatch(hjt, &state->base, inner_slot) == false)
				break;
		}

		file = hjt->outerBatchFile[hjt->curbatch];
		RestartBufFile(file);
		for(;;)
		{
			CHECK_FOR_INTERRUPTS();
			ExecHashJoinReadTuple(file, &hashvalue, outer_slot);
			if (TupIsNull(outer_slot))
			{
				if (ExecHashNewBatch(hjt, &state->base, inner_slot) == false)
					goto check_is_end_;
				break;
			}

			state->base.cur_hashvalue = hashvalue;
			state->base.cur_skewno = ExecHashGetSkewBucket(hjt, hashvalue);
			if (state->base.cur_skewno == INVALID_SKEW_BUCKET_NO)
			{
				int batch_no;
				ExecHashGetBucketAndBatch(hjt, hashvalue, &state->base.cur_bucketno, &batch_no);
				Assert(batch_no == hjt->curbatch);
			}
			state->base.cur_tuple = NULL;

			econtext->ecxt_innertuple = inner_slot;
			econtext->ecxt_outertuple = outer_slot;

			while(ExecScanHashBucketExt(econtext,
										state->base.hash_clauses,
										&state->base.cur_tuple,
										state->base.cur_hashvalue,
										state->base.cur_skewno,
										state->base.cur_bucketno,
										hjt,
										inner_slot))
			{
				int		batch_no;
				int		bucket_no;
				bool	qual;
				CHECK_FOR_INTERRUPTS();

				if (cbstate->joinclause)
				{
					qual = ExecQualAndReset(cbstate->joinclause, econtext);
					CHECK_FOR_INTERRUPTS();
					if (qual == false)
						continue;
				}

				if (cbstate->ps.qual == NULL ||
					ExecQualAndReset(cbstate->ps.qual, econtext))
				{
					qual = true;
				}else
				{
					qual = false;
					InstrCountFiltered1(cbstate, 1);
				}

				econtext->ecxt_outertuple = inner_slot;
				batch_no = InvalidBatchNumber;
				if (ExecHashGetHashValue(hjt,
										 econtext,
										 state->base.outer_HashKeys,
										 true,
										 false,
										 &hashvalue))
				{
					econtext->ecxt_outertuple = outer_slot;
					ExecHashGetBucketAndBatch(hjt, hashvalue, &bucket_no, &batch_no);
					if (hjt->innerBatchFile[batch_no] == NULL)
					{
						/* don't need save it when inner batch is empty */
						batch_no = InvalidBatchNumber;
					}
				}
				econtext->ecxt_outertuple = outer_slot;
				state->save_level(cbstate, inner_slot, hashvalue, batch_no, qual);
			}
			state->end_level(cbstate);
		}
	}

check_is_end_:
	/* check prior is empty */
	for (i=0;i<hjt->nbatch;++i)
	{
		if (state->base.outer_save[i] != NULL)
		{
			BufFile **swap = hjt->outerBatchFile;
			hjt->outerBatchFile = state->base.outer_save;
			state->base.outer_save = swap;
			if (hjt->curbatch != 0)
			{
				hjt->curbatch = -1;
				ExecHashNewBatch(hjt, &state->base, inner_slot);
				Assert(hjt->curbatch < hjt->nbatch);
			}
			goto re_connect_by_;
		}
	}
}

static TupleTableSlot *ExecSortHashConnectBy(PlanState *pstate)
{
	SortHashConnectByState *state = castNode(ConnectByState, pstate)->private_state;
	TupleTableSlot *sort_slot = state->base.material_slot;
	TupleTableSlot *ret_slot = pstate->ps_ResultTupleSlot;

	ExecClearTuple(ret_slot);
	if (tuplesort_gettupleslot(state->base.material_state, true, false, sort_slot, NULL))
	{
		int natts = ret_slot->tts_tupleDescriptor->natts;
		Assert(natts < sort_slot->tts_tupleDescriptor->natts);

		slot_getsomeattrs(sort_slot, natts);
		memcpy(ret_slot->tts_values, sort_slot->tts_values, sizeof(ret_slot->tts_values[0]) * natts);
		memcpy(ret_slot->tts_isnull, sort_slot->tts_isnull, sizeof(ret_slot->tts_isnull[0]) * natts);
		ExecStoreVirtualTuple(ret_slot);
	}

	return ret_slot;
}

static void SortHashSaveRootRow(ConnectByState *cbstate, TupleTableSlot *slot)
{
	SortHashConnectByState *state = cbstate->private_state;
	ExprContext *econtext = state->sub_sort_proj->pi_exprContext;
	TupleTableSlot *inner_slot = econtext->ecxt_innertuple;
	econtext->ecxt_innertuple = slot;
	tuplesort_puttupleslot(state->root_sort, 
						   ExecProject(state->sub_sort_proj));
	econtext->ecxt_innertuple = inner_slot;
}

static Datum MakeSortHashDatum(TupleTableSlot *outer_slot, AttrNumber prior_attno, int64 **cur_level)
{
	ArrayType  *cur_arr;
	ArrayType  *prior_arr;
	Datum	   *new_arr;
	Datum		tmp_datum;
	int			prior_count;
	uint32		prior_att_index;

	if (AttributeNumberIsValid(prior_attno))
	{
		/* get prior sort values */
		prior_att_index = prior_attno-1;
		slot_getsomeattrs(outer_slot, prior_attno);
		Assert(outer_slot->tts_tupleDescriptor->natts >= prior_attno &&
			   TupleDescAttr(outer_slot->tts_tupleDescriptor, prior_att_index)->atttypid == INT8ARRAYOID);
		Assert(outer_slot->tts_isnull[prior_att_index] == false);
		prior_arr = DatumGetArrayTypeP(outer_slot->tts_values[prior_att_index]);
		Assert(!ARR_HASNULL(prior_arr));
		prior_count = ARR_DIMS(prior_arr)[0];
		new_arr = palloc(sizeof(Datum)*(prior_count+1));
		memcpy(new_arr, ARR_DATA_PTR(prior_arr), sizeof(Datum)*prior_count);
	}else
	{
		prior_count = 0;
		new_arr = &tmp_datum;
	}

	/* make current siblings sort values */
	new_arr[prior_count] = (Datum)0;
	cur_arr = construct_array(new_arr, prior_count+1, INT8OID, sizeof(int64), FLOAT8PASSBYVAL, 'd');

	/* clean and return */
	if (new_arr != &tmp_datum)
		pfree(new_arr);
	*cur_level = ((int64*)ARR_DATA_PTR(cur_arr)) + prior_count;
	return PointerGetDatum(cur_arr);
}

static void SortHashEndRootRow(ConnectByState *cbstate)
{
	SortHashConnectByState *state = cbstate->private_state;
	ExprContext	   *econtext = cbstate->ps.ps_ExprContext;
	TupleTableSlot *sub_sort_slot = state->sub_sort_slot;
	TupleTableSlot *final_sort_slot;
	TupleTableSlot *outer_slot;
	HashJoinTable	hjt = cbstate->hjt;
	int64		   *sort_ptr;
	Datum			sort_datum;
	uint64			filter1 = 0;
	uint32			hashvalue;
	int				bucket_no;
	int				batch_no;

	sort_datum = MakeSortHashDatum(NULL, InvalidAttrNumber, &sort_ptr);
	*sort_ptr = 0;

	tuplesort_performsort(state->root_sort);
	while (tuplesort_gettupleslot(state->root_sort, true, false, sub_sort_slot, NULL))
	{
		/* update sort column vlue */
		++(*sort_ptr);

		/* save outer rows */
		econtext->ecxt_outertuple = sub_sort_slot;
		econtext->ecxt_innertuple = NULL;
		ExecHashGetHashValue(hjt,
							 econtext,
							 state->base.base.outer_HashKeys,
							 true,	/* is outer */
							 true,	/* we need return this tuple, when is null we also need a hashvalue */
							 &hashvalue);
		econtext->ecxt_innertuple = sub_sort_slot;
		econtext->ecxt_outertuple = ExecClearTuple(cbstate->outer_slot);
		outer_slot = ExecProject(cbstate->pj_save_targetlist);
		Assert(outer_slot->tts_tupleDescriptor->natts > state->outer_sort_index &&
			   TupleDescAttr(outer_slot->tts_tupleDescriptor, state->outer_sort_index)->atttypid == INT8ARRAYOID);
		outer_slot->tts_values[state->outer_sort_index] = sort_datum;
		outer_slot->tts_isnull[state->outer_sort_index] = false;
		
		ExecHashGetBucketAndBatch(hjt, hashvalue, &bucket_no, &batch_no);
		Assert(!TupIsNull(outer_slot) &&
			   outer_slot->tts_mintuple == NULL);
		ExecHashJoinSaveTuple(ExecFetchSlotMinimalTuple(outer_slot),
							  hashvalue,
							  &hjt->outerBatchFile[batch_no]);

		if (cbstate->ps.qual == NULL ||
			ExecQual(cbstate->ps.qual, econtext))
		{
			final_sort_slot = ExecProject(state->base.material_proj);
			Assert(final_sort_slot->tts_tupleDescriptor->natts > state->final_sort_index &&
				   TupleDescAttr(final_sort_slot->tts_tupleDescriptor, state->final_sort_index)->atttypid == INT8ARRAYOID);
			Assert(!TupIsNull(final_sort_slot) &&
				   final_sort_slot->tts_mintuple == NULL);

			final_sort_slot->tts_values[state->final_sort_index] = sort_datum;
			final_sort_slot->tts_isnull[state->final_sort_index] = false;
			tuplesort_puttupleslot(state->base.material_state, final_sort_slot);
			ExecClearTuple(sub_sort_slot);
		}else
		{
			++filter1;
		}
	}
	tuplesort_end(state->root_sort);
	state->root_sort = NULL;
	pfree(DatumGetPointer(sort_datum));
	InstrCountFiltered1(cbstate, filter1);
}

static int cmp_sort_hash_rows(const void *a, const void *b, void *arg)
{
	SortHashConnectByState *state = castNode(ConnectByState, arg)->private_state;
	TupleTableSlot		   *s1 = ((HashSortRowData*)a)->slot;
	TupleTableSlot		   *s2 = ((HashSortRowData*)b)->slot;
	int						i,nkey = castNode(ConnectByPlan, ((PlanState*)arg)->plan)->numCols;

	Assert(!TupIsNull(s1));
	Assert(!TupIsNull(s2));

	for (i=0;i<nkey;++i)
	{
		SortSupport key = &state->sub_sort_support[i];
		AttrNumber index = key->ssup_attno-1;
		int compare = ApplySortComparator(s1->tts_values[index], s1->tts_isnull[index],
										  s2->tts_values[index], s2->tts_isnull[index],
										  key);
		if (compare != 0)
			return compare;
	}
	return 0;
}

static void SortHashSaveLevelRow(ConnectByState *cbstate, TupleTableSlot *slot, uint32 hashval, int batch_no, bool qual)
{
	SortHashConnectByState *state = cbstate->private_state;
	HashSortRowData		   *row;
	TupleTableSlot		   *sort_slot;

	if (batch_no == InvalidBatchNumber &&
		qual == false)
		return;

	if (state->sub_sort_row_size == state->sub_sort_row_space)
	{
		uint32 i;
		uint32 new_size = state->sub_sort_row_space + HASH_SORT_ROW_STEP_SIZE;
		MemoryContext oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(cbstate));
		if (state->sub_sort_rows == NULL)
			state->sub_sort_rows = palloc(sizeof(state->sub_sort_rows[0]) * new_size);
		else
			state->sub_sort_rows = repalloc(state->sub_sort_rows,
											sizeof(state->sub_sort_rows[0]) * new_size);
		state->sub_sort_row_space = new_size;
		for (i=state->sub_sort_row_size;i<new_size;++i)
		{
			row = &state->sub_sort_rows[i];
			MemSet(row, 0, sizeof(*row));
			row->slot = MakeTupleTableSlot(state->sub_sort_desc);
		}
		MemoryContextSwitchTo(oldcontext);
	}

	Assert(state->sub_sort_row_size < state->sub_sort_row_space);
	row = &state->sub_sort_rows[state->sub_sort_row_size];
	++(state->sub_sort_row_size);

	row->qual = qual;
	row->hashval = hashval;
	row->batch_no = batch_no;

	/*
	 * slot data should from Hash's memory,
	 * so we can keep it until end of this bucket
	 */
	Assert(slot->tts_mintuple != NULL);
	row->mtup = slot->tts_mintuple;
	sort_slot = ExecProject(state->sub_sort_proj);
	Assert(sort_slot->tts_tupleDescriptor == row->slot->tts_tupleDescriptor);
	memcpy(row->slot->tts_values,
		   sort_slot->tts_values,
		   sizeof(sort_slot->tts_values[0]) * sort_slot->tts_tupleDescriptor->natts);
	memcpy(row->slot->tts_isnull,
		   sort_slot->tts_isnull,
		   sizeof(sort_slot->tts_isnull[0]) * sort_slot->tts_tupleDescriptor->natts);
	ExecStoreVirtualTuple(row->slot);
}

static void SortHashEndLevelRow(ConnectByState *cbstate)
{
	int						i;
	TupleTableSlot		   *inner_slot;
	TupleTableSlot		   *final_slot;
	TupleTableSlot		   *save_slot;
	SortHashConnectByState *state = cbstate->private_state;
	ExprContext			   *econtext;
	int64				   *cur_num;
	Datum					cur_arr;
	if (state->sub_sort_row_size == 0)
		return;

	/* sort rows */
	if (state->sub_sort_row_size > 1)
	{
		qsort_arg(state->sub_sort_rows,
				  state->sub_sort_row_size,
				  sizeof(state->sub_sort_rows[0]),
				  cmp_sort_hash_rows,
				  cbstate);
	}

	/* make current siblings sort values */
	econtext = cbstate->ps.ps_ExprContext;
	cur_arr = MakeSortHashDatum(econtext->ecxt_outertuple, state->outer_sort_index+1, &cur_num);

	/* save rows */
	inner_slot = econtext->ecxt_innertuple;
	for (i=0;i<state->sub_sort_row_size;++i)
	{
		HashSortRowData *row = &state->sub_sort_rows[i];
		ExecStoreMinimalTuple(row->mtup, inner_slot, false);

		/* update sort column */
		(*cur_num) = (int64)i;

		if (row->batch_no != InvalidBatchNumber)
		{
			save_slot = ExecProject(cbstate->pj_save_targetlist);
			Assert(save_slot->tts_tupleDescriptor->natts == state->outer_sort_index+1 &&
				   TupleDescAttr(save_slot->tts_tupleDescriptor, state->outer_sort_index)->atttypid == INT8ARRAYOID);
			Assert(!TupIsNull(save_slot) &&
				   save_slot->tts_mintuple == NULL);
			save_slot->tts_values[state->outer_sort_index] = cur_arr;
			save_slot->tts_isnull[state->outer_sort_index] = false;
			ExecHashJoinSaveTuple(ExecFetchSlotMinimalTuple(save_slot),
								  row->hashval,
								  &state->base.base.outer_save[row->batch_no]);
			ExecClearTuple(save_slot);
		}

		if (row->qual)
		{
			final_slot = ExecProject(state->base.material_proj);
			Assert(final_slot->tts_tupleDescriptor->natts == state->final_sort_index+1 &&
				   TupleDescAttr(final_slot->tts_tupleDescriptor, state->final_sort_index)->atttypid == INT8ARRAYOID);
			Assert(!TupIsNull(final_slot) &&
				   final_slot->tts_mintuple == NULL);
			final_slot->tts_values[state->final_sort_index] = cur_arr;
			final_slot->tts_isnull[state->final_sort_index] = false;
			tuplesort_puttupleslot(state->base.material_state, final_slot);
			ExecClearTuple(final_slot);
		}
		ExecClearTuple(row->slot);
		row->mtup = NULL;
	}

	/* clean */
	state->sub_sort_row_size = 0;
	pfree(DatumGetPointer(cur_arr));
}

static TupleTableSlot *ExecFirstSortHashConnectBy(PlanState *pstate)
{
	ConnectByState		   *cbstate = castNode(ConnectByState, pstate);
	ConnectByPlan		   *node = castNode(ConnectByPlan, pstate->plan);
	SortHashConnectByState *state = cbstate->private_state;
	MemoryContext			oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(pstate));

	Assert(state->root_sort == NULL);
	state->base.save_root = SortHashSaveRootRow;
	state->base.end_root = SortHashEndRootRow;
	state->base.save_level = SortHashSaveLevelRow;
	state->base.end_level = SortHashEndLevelRow;
	state->root_sort = tuplesort_begin_heap(state->sub_sort_desc,
											node->numCols,
											node->sortColIdx,
											node->sortOperators,
											node->collations,
											node->nullsFirst,
											work_mem,
											NULL,
											false);

	if (state->base.material_state == NULL)
	{
		Oid sortOperator;
		Oid collation = InvalidOid;
		AttrNumber final_sort_attr = state->final_sort_index+1;
		bool null_first = true;

		get_sort_group_operators(INT8ARRAYOID, true, false, false, &sortOperator, NULL, NULL, NULL);
		state->base.material_state = tuplesort_begin_heap(state->base.material_desc,
														  1,
														  &final_sort_attr,
														  &sortOperator,
														  &collation,
														  &null_first,
														  work_mem,
														  NULL,
														  false);
	}
	MemoryContextSwitchTo(oldcontext);

	FirstMaterialHashConnectBy(cbstate, &state->base);
	tuplesort_performsort(state->base.material_state);
	ExecSetExecProcNode(pstate, ExecSortHashConnectBy);

	return ExecSortHashConnectBy(pstate);
}

static void ProcessTuplesortRoot(ConnectByState *cbstate, TuplestoreConnectByLeaf *leaf)
{
	TupleTableSlot *inner_slot;
	ExprContext *econtext = cbstate->ps.ps_ExprContext;
	TuplesortConnectByState *state = cbstate->private_state;
	Assert(state->method == CB_TUPLESORT);

	for(;;)
	{
		CHECK_FOR_INTERRUPTS();
		inner_slot = ExecNestConnectByStartWith(cbstate);
		if (TupIsNull(inner_slot))
			break;

		econtext->ecxt_innertuple = inner_slot;
		econtext->ecxt_outertuple = NULL;
		tuplesort_puttupleslot(leaf->scan_ts,
							   ExecProject(state->sort_project));
	}
}

void ExecEndConnectBy(ConnectByState *node)
{
	ExecEndNode(outerPlanState(node));

	if (node->ts)
	{
		tuplestore_end(node->ts);
		node->ts = NULL;
	}

	switch(CONNECT_BY_METHOD(node))
	{
	case CB_NEST:
		{
			NestConnectByState *state = node->private_state;
			tuplestore_end(state->scan_ts);
			tuplestore_end(state->save_ts);
		}
		break;
	case CB_HASH:
	case CB_SORTHASH:
		{
			HashConnectByState *state = node->private_state;
			if (state->outer_save)
			{
				int i = node->hjt->nbatch;
				while (--i>=0)
				{
					if (state->outer_save[i])
						BufFileClose(state->outer_save[i]);
				}
				pfree(state->outer_save);
				state->outer_save = NULL;
			}
			if (node->hjt)
			{
				HashJoinTable hjt = node->hjt;

				/* ExecHashTableDestroy not close batch 0 file */
				if (hjt->innerBatchFile &&
					hjt->innerBatchFile[0])
					BufFileClose(hjt->innerBatchFile[0]);
				if (hjt->outerBatchFile &&
					hjt->outerBatchFile[0])
					BufFileClose(hjt->outerBatchFile[0]);

				ExecHashTableDestroy(hjt);
				node->hjt = NULL;
			}
			if (state->method == CB_SORTHASH)
			{
				SortHashConnectByState *sstate = (SortHashConnectByState*)state;
				int i;
				if (sstate->root_sort)
					tuplesort_end(sstate->root_sort);
				for (i=0;i<sstate->sub_sort_row_space;++i)
					ExecDropSingleTupleTableSlot(sstate->sub_sort_rows[i].slot);
				if (sstate->base.material_state)
					tuplesort_end(sstate->base.material_state);
			}
		}
		break;
	case CB_TUPLESORT:
		{
			TuplesortConnectByState *state = node->private_state;
			TuplestoreConnectByLeaf *leaf;
			slist_node *node;
			while (slist_is_empty(&state->slist_idle) == false)
			{
				node = slist_pop_head_node(&state->slist_idle);
				leaf = slist_container(TuplestoreConnectByLeaf, snode, node);
				Assert (leaf->scan_ts == NULL);
				pfree(leaf);
			}
			while (slist_is_empty(&state->slist_level) == false)
			{
				node = slist_pop_head_node(&state->slist_level);
				leaf = slist_container(TuplestoreConnectByLeaf, snode, node);
				tuplesort_end(leaf->scan_ts);
				if (leaf->outer_tup)
					pfree(leaf->outer_tup);
				pfree(leaf);
			}
		}
		if (node->hjt)
		{
			HashJoinTable hjt = node->hjt;

			/* ExecHashTableDestroy not close batch 0 file */
			if (hjt->outerBatchFile &&
				hjt->outerBatchFile[0])
				BufFileClose(hjt->outerBatchFile[0]);
			if (hjt->innerBatchFile &&
				hjt->innerBatchFile[0])
				BufFileClose(hjt->innerBatchFile[0]);

			ExecHashTableDestroy(hjt);
			node->hjt = NULL;
		}
		break;
	default:
		ereport(ERROR,
				(errmsg("unknown connect by method %u", CONNECT_BY_METHOD(node))));
	}
	ExecFreeExprContext(&node->ps);
}

static void ExecReScanNestConnectBy(ConnectByState *cbstate, NestConnectByState *state)
{
	tuplestore_clear(state->save_ts);
	tuplestore_clear(state->scan_ts);
	if (outerPlanState(cbstate)->chgParam != NULL)
	{
		if (cbstate->ts)
			tuplestore_clear(cbstate->ts);
		cbstate->is_rescan = false;
		cbstate->eof_underlying = false;
	}else
	{
		cbstate->is_rescan = true;
	}
}

static void ExecReScanHashConnectBy(ConnectByState *cbstate, HashConnectByState *state)
{
	HashJoinTable hjt = cbstate->hjt;
	int i;

	if (hjt == NULL)
		return;	/* not initialized */

	/* clear saved */
	for (i=hjt->nbatch;--i>=0;)
	{
		if (state->outer_save[i])
		{
			BufFileClose(state->outer_save[i]);
			state->outer_save[i] = NULL;
		}
	}

	if (outerPlanState(cbstate)->chgParam == NULL)
	{
		if (cbstate->processing_root == false)
		{
			/* clear outer */
			for (i=hjt->nbatch;--i>=0;)
			{
				if (hjt->outerBatchFile[i])
				{
					BufFileClose(hjt->outerBatchFile[i]);
					hjt->outerBatchFile[i] = NULL;
				}
			}
		}
		cbstate->is_rescan = true;
	}else
	{
		if (hjt->outerBatchFile &&
			hjt->outerBatchFile[0])
			BufFileClose(hjt->outerBatchFile[0]);
		ExecHashTableDestroy(hjt);
		cbstate->hjt = NULL;
		/* state->outer_save pfreed by ExecHashTableDestroy() */
		state->outer_save = NULL;
		castNode(HashState, outerPlanState(cbstate))->hashtable = NULL;
	}
}

static void ExecReScanTuplesortConnectBy(ConnectByState *cbstate, TuplesortConnectByState *state)
{
	slist_node *slistnode;
	TuplestoreConnectByLeaf *leaf;

	while (slist_is_empty(&state->slist_level) == false)
	{
		slistnode = slist_pop_head_node(&state->slist_level);
		slist_push_head(&state->slist_idle, slistnode);
		leaf = slist_container(TuplestoreConnectByLeaf, snode, slistnode);
		tuplesort_end(leaf->scan_ts);
		leaf->scan_ts = NULL;
		if (leaf->outer_tup)
		{
			pfree(leaf->outer_tup);
			leaf->outer_tup = NULL;
		}
	}

	Assert(CONNECT_BY_METHOD(cbstate) == CB_TUPLESORT);
	if (outerPlanState(cbstate)->chgParam != NULL)
	{
		if (cbstate->ts != NULL)
			tuplestore_clear(cbstate->ts);
		cbstate->is_rescan = false;
		cbstate->eof_underlying = false;
	}else
	{
		if (cbstate->ts)
			tuplestore_rescan(cbstate->ts);
		cbstate->is_rescan = true;
	}
	leaf = GetConnectBySortLeaf(cbstate);
	slist_push_head(&state->slist_level, &leaf->snode);
}

static void ExecReScanSortHashConnectBy(ConnectByState *cbstate, SortHashConnectByState *state)
{
	if (state->base.material_state)
	{
		if (cbstate->ps.chgParam != NULL)
		{
			tuplesort_end(state->base.material_state);
			state->base.material_state = NULL;
			if (state->root_sort)
			{
				tuplesort_end(state->root_sort);
				state->root_sort = NULL;
			}
			ExecSetExecProcNode(&cbstate->ps, ExecFirstSortHashConnectBy);
			ExecReScan(outerPlanState(cbstate));
		}else
		{
			tuplesort_rescan(state->base.material_state);
		}
	}
}

void ExecReScanConnectBy(ConnectByState *node)
{
	switch(CONNECT_BY_METHOD(node))
	{
	case CB_NEST:
		ExecReScanNestConnectBy(node, node->private_state);
		break;
	case CB_HASH:
		ExecReScanHashConnectBy(node, node->private_state);
		break;
	case CB_TUPLESORT:
		ExecReScanTuplesortConnectBy(node, node->private_state);
		break;
	case CB_SORTHASH:
		ExecReScanSortHashConnectBy(node, node->private_state);
		break;
	default:
		ereport(ERROR,
				(errmsg("unknown connect by method %u", CONNECT_BY_METHOD(node))));
	}

	if (outerPlanState(node)->chgParam != NULL)
		ExecReScan(outerPlanState(node));

	ExecClearTuple(node->outer_slot);
	ExecClearTuple(node->inner_slot);
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	node->processing_root = true;
	node->level = 1L;
	node->check_start_state = START_WITH_UNCHECK;
}

static TupleTableSlot* ExecNestConnectByStartWith(ConnectByState *ps)
{
	Tuplestorestate *outer_ts = ps->ts;
	PlanState	   *outer_ps = outerPlanState(ps);
	ExprContext	   *econtext = ps->ps.ps_ExprContext;
	TupleTableSlot *slot;
	uint64			removed = 0;

#ifdef USE_ASSERT_CHECKING
	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_outertuple = NULL;
	econtext->ecxt_innertuple = NULL;
#endif
	for(;;)
	{
		if (ps->is_rescan)
		{
			slot = ps->inner_slot;
			tuplestore_gettupleslot(outer_ts, true, false, slot);
			if (TupIsNull(slot))
			{
				ps->is_rescan = false;
				continue;	/* try is is eof underlying? */
			}
		}else if (ps->eof_underlying == false)
		{
			ResetExprContext(econtext);
			slot = ExecProcNode(outer_ps);
			if (TupIsNull(slot))
			{
				ps->eof_underlying = true;
				break;
			}

			tuplestore_puttupleslot(outer_ts, slot);
		}else
		{
			/* not in rescan and eof underlying */
			break;
		}

		econtext->ecxt_outertuple = slot;
		if (ps->start_with == NULL ||
			ExecQual(ps->start_with, econtext))
		{
			InstrCountFiltered2(ps, removed);
			return slot;
		}
		++removed;
		CHECK_START_WITH(ps);
		if (ps->check_start_state == START_WITH_HAS_EMPTY)
		{
			ExecClearTuple(slot);
			break;
		}
	}

	InstrCountFiltered2(ps, removed);
	return NULL;
}

static TuplestoreConnectByLeaf* GetConnectBySortLeaf(ConnectByState *ps)
{
	TuplestoreConnectByLeaf *leaf;
	MemoryContext oldcontext;
	TuplesortConnectByState *state = ps->private_state;
	ConnectByPlan *node = castNode(ConnectByPlan, ps->ps.plan);

	if (slist_is_empty(&state->slist_idle))
	{
		leaf = MemoryContextAllocZero(GetMemoryChunkContext(ps), sizeof(*leaf));
	}else
	{
		slist_node *node = slist_pop_head_node(&state->slist_idle);
		leaf = slist_container(TuplestoreConnectByLeaf, snode, node);
	}

	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(ps));
	leaf->scan_ts = tuplesort_begin_heap(state->sort_slot->tts_tupleDescriptor,
										 node->numCols,
										 node->sortColIdx,
										 node->sortOperators,
										 node->collations,
										 node->nullsFirst,
										 0,
										 NULL,
										 false);
	MemoryContextSwitchTo(oldcontext);

	return leaf;
}

static TuplestoreConnectByLeaf *GetNextTuplesortLeaf(ConnectByState *cbstate, TupleTableSlot *outer_slot)
{
	TuplesortConnectByState *state = cbstate->private_state;
	ExprContext *econtext = cbstate->ps.ps_ExprContext;
	TupleTableSlot *inner_slot = cbstate->inner_slot;
	TuplestoreConnectByLeaf *leaf;
	Assert(!TupIsNull(outer_slot));

	ExecMaterializeSlot(outer_slot);
	econtext->ecxt_outertuple = outer_slot;
	econtext->ecxt_innertuple = NULL;
	tuplestore_rescan(cbstate->ts);

	leaf = NULL;
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		tuplestore_gettupleslot(cbstate->ts, true, false, inner_slot);
		if (TupIsNull(inner_slot))
			break;
		econtext->ecxt_innertuple = inner_slot;
		if (ExecQualAndReset(cbstate->joinclause, econtext))
		{
			if (leaf == NULL)
				leaf = GetConnectBySortLeaf(cbstate);
			tuplesort_puttupleslot(leaf->scan_ts,
								   ExecProject(state->sort_project));
		}
	}

	return leaf;
}

static TupleTableSlot *InsertRootHashValue(ConnectByState *cbstate, TupleTableSlot *slot)
{
	ExprContext	   *econtext;
	HashJoinTable	hjt;
	uint32			hashvalue;
	int				bucket_no;
	int				batch_no;

	if (TupIsNull(slot))
		return slot;
	if (cbstate->check_start_state == START_WITH_HAS_EMPTY)
		return ExecClearTuple(slot);

	econtext = cbstate->ps.ps_ExprContext;
	ResetExprContext(econtext);
	econtext->ecxt_outertuple = slot;
	if (cbstate->start_with == NULL ||
		ExecQual(cbstate->start_with, econtext))
	{
		HashConnectByState *state = cbstate->private_state;
		hjt = cbstate->hjt;
		ExecHashGetHashValue(hjt,
							 econtext,
							 state->outer_HashKeys,
							 true,
							 true,	/* we need return this tuple, when is null we also need a hashvalue */
							 &hashvalue);
		econtext->ecxt_innertuple = slot;
		econtext->ecxt_outertuple = ExecClearTuple(cbstate->outer_slot);
		ExecHashGetBucketAndBatch(hjt, hashvalue, &bucket_no, &batch_no);
		ExecHashJoinSaveTuple(ExecFetchSlotMinimalTuple(ExecProject(cbstate->pj_save_targetlist)),
							  hashvalue,
							  &hjt->outerBatchFile[batch_no]);
	}else
	{
		CHECK_START_WITH(cbstate);
		InstrCountFiltered2(cbstate, 1);
	}

	return slot;
}

static TupleTableSlot *InsertMaterialHashValue(ConnectByState *cbstate, TupleTableSlot *slot)
{
	MaterialHashConnectByState *state = cbstate->private_state;
	ExprContext	   *econtext;

	if (TupIsNull(slot))
		return slot;
	if (cbstate->check_start_state == START_WITH_HAS_EMPTY)
		return ExecClearTuple(slot);

	econtext = cbstate->ps.ps_ExprContext;
	ResetExprContext(econtext);
	econtext->ecxt_outertuple = slot;
	if (cbstate->start_with == NULL ||
		ExecQual(cbstate->start_with, econtext))
	{
		state->save_root(cbstate, slot);
	}else
	{
		CHECK_START_WITH(cbstate);
		InstrCountFiltered2(cbstate, 1);
	}

	return slot;
}

/* like ExecHashJoinNewBatch */
static bool ExecHashNewBatch(HashJoinTable hashtable, HashConnectByState *state, TupleTableSlot *slot)
{
	BufFile	   *innerFile;
	int			nbatch = hashtable->nbatch;
	int			curbatch = hashtable->curbatch;
	uint32		hashvalue;

	if (curbatch >= 0)
	{
		/*
		 * We no longer need the previous outer batch file; close it right
		 * away to free disk space.
		 */
		if (hashtable->outerBatchFile[curbatch])
		{
			BufFileClose(hashtable->outerBatchFile[curbatch]);
			hashtable->outerBatchFile[curbatch] = NULL;
		}
	}

	++curbatch;
	while (curbatch < nbatch &&
		   (hashtable->outerBatchFile[curbatch] == NULL ||
			hashtable->innerBatchFile[curbatch] == NULL))
	{
		/* Release associated temp files right away. */
		if (hashtable->outerBatchFile[curbatch])
		{
			BufFileClose(hashtable->outerBatchFile[curbatch]);
			hashtable->outerBatchFile[curbatch] = NULL;
		}
		++curbatch;
	}

	if (curbatch >= nbatch)
		return false;			/* no more batches */

	hashtable->curbatch = curbatch;

	/*
	 * Reload the hash table with the new inner batch (which could be empty)
	 */
	ExecHashTableReset(hashtable);

	innerFile = hashtable->innerBatchFile[curbatch];

	if (innerFile != NULL)
	{
		RestartBufFile(innerFile);

		while ((slot = ExecHashJoinReadTuple(innerFile, &hashvalue, slot)))
		{
			/*
			 * NOTE: some tuples may be sent to future batches.  Also, it is
			 * possible for hashtable->nbatch to be increased here!
			 */
			ExecHashTableInsert(hashtable, slot, hashvalue);
		}
	}

	RestartBufFile(hashtable->outerBatchFile[curbatch]);

	return true;
}

static void RestartBufFile(BufFile *file)
{
	if (file && BufFileSeek(file, 0, 0L, SEEK_SET))
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind connect-by temporary file: %m")));
	}
}

void ExecEvalSysConnectByPathExpr(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ConnectByState *cbstate = castNode(ConnectByState, state->parent);
	StringInfoData buf;
	short narg;
	bool isnull = true;

	initStringInfo(&buf);
	if (cbstate->level != 1L)
	{
		TupleTableSlot *slot = econtext->ecxt_outertuple;
		char *prior_str;
		AttrNumber attnum = op->d.scbp.attnum;

		slot_getsomeattrs(slot, attnum+1);
		if (slot->tts_isnull[attnum] == false)
		{
			prior_str = TextDatumGetCString(slot->tts_values[attnum]);
			appendStringInfoString(&buf, prior_str);
			pfree(prior_str);
			isnull = false;
		}
	}

	narg = op->d.scbp.narg;
	while (narg > 0)
	{
		--narg;
		if (op->d.scbp.argnull[narg] == false)
		{
			appendStringInfoString(&buf, DatumGetCString(op->d.scbp.arg[narg]));
			isnull = false;
		}
	}

	*op->resnull = isnull;
	if (isnull == false)
	{
		text *result = cstring_to_text_with_len(buf.data, buf.len);
		*op->resvalue = PointerGetDatum(result);
	}
}
