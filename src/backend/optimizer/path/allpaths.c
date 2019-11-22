/*-------------------------------------------------------------------------
 *
 * allpaths.c
 *	  Routines to find possible search paths for processing a query
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/allpaths.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/sysattr.h"
#include "access/tsmapi.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#ifdef OPTIMIZER_DEBUG
#include "nodes/print.h"
#endif
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "partitioning/partprune.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#ifdef ADB
#include "catalog/pgxc_node.h"
#include "optimizer/planmain.h"
#include "optimizer/reduceinfo.h"
#include "pgxc/pgxcnode.h"
#endif /* ADB */

/* results of subquery_is_pushdown_safe */
typedef struct pushdown_safety_info
{
	bool	   *unsafeColumns;	/* which output columns are unsafe to use */
	bool		unsafeVolatile; /* don't push down volatile quals */
	bool		unsafeLeaky;	/* don't push down leaky quals */
} pushdown_safety_info;

/* These parameters are set by GUC */
bool		enable_geqo = false;	/* just in case GUC doesn't set it */
int			geqo_threshold;
int			min_parallel_table_scan_size;
int			min_parallel_index_scan_size;

/* Hook for plugins to get control in set_rel_pathlist() */
set_rel_pathlist_hook_type set_rel_pathlist_hook = NULL;

/* Hook for plugins to replace standard_join_search() */
join_search_hook_type join_search_hook = NULL;


static void set_base_rel_consider_startup(PlannerInfo *root);
static void set_base_rel_sizes(PlannerInfo *root);
static void set_base_rel_pathlists(PlannerInfo *root);
static void set_rel_size(PlannerInfo *root, RelOptInfo *rel,
			 Index rti, RangeTblEntry *rte);
static void set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
				 Index rti, RangeTblEntry *rte);
static void set_plain_rel_size(PlannerInfo *root, RelOptInfo *rel,
				   RangeTblEntry *rte);
static void create_plain_partial_paths(PlannerInfo *root, RelOptInfo *rel);
static void set_rel_consider_parallel(PlannerInfo *root, RelOptInfo *rel,
						  RangeTblEntry *rte);
static void set_plain_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
					   RangeTblEntry *rte);
static void set_tablesample_rel_size(PlannerInfo *root, RelOptInfo *rel,
						 RangeTblEntry *rte);
static void set_tablesample_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte);
static void set_foreign_size(PlannerInfo *root, RelOptInfo *rel,
				 RangeTblEntry *rte);
static void set_foreign_pathlist(PlannerInfo *root, RelOptInfo *rel,
					 RangeTblEntry *rte);
static void set_append_rel_size(PlannerInfo *root, RelOptInfo *rel,
					Index rti, RangeTblEntry *rte);
static void set_append_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
						Index rti, RangeTblEntry *rte);
static void generate_mergeappend_paths(PlannerInfo *root, RelOptInfo *rel,
						   List *live_childrels,
						   List *all_child_pathkeys,
						   List *partitioned_rels);
static Path *get_cheapest_parameterized_child_path(PlannerInfo *root,
									  RelOptInfo *rel,
									  Relids required_outer);
static void accumulate_append_subpath(Path *path,
						  List **subpaths, List **special_subpaths);
static void set_subquery_pathlist(PlannerInfo *root, RelOptInfo *rel,
					  Index rti, RangeTblEntry *rte);
static void set_function_pathlist(PlannerInfo *root, RelOptInfo *rel,
					  RangeTblEntry *rte);
static void set_values_pathlist(PlannerInfo *root, RelOptInfo *rel,
					RangeTblEntry *rte);
static void set_tablefunc_pathlist(PlannerInfo *root, RelOptInfo *rel,
					   RangeTblEntry *rte);
static void set_cte_pathlist(PlannerInfo *root, RelOptInfo *rel,
				 RangeTblEntry *rte);
static void set_namedtuplestore_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte);
#ifdef ADB
static void set_paramtuplestore_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte);
#endif /* ADB */
static void set_worktable_pathlist(PlannerInfo *root, RelOptInfo *rel,
					   RangeTblEntry *rte);
static RelOptInfo *make_rel_from_joinlist(PlannerInfo *root, List *joinlist);
static bool subquery_is_pushdown_safe(Query *subquery, Query *topquery,
						  pushdown_safety_info *safetyInfo);
static bool recurse_pushdown_safe(Node *setOp, Query *topquery,
					  pushdown_safety_info *safetyInfo);
static void check_output_expressions(Query *subquery,
						 pushdown_safety_info *safetyInfo);
static void compare_tlist_datatypes(List *tlist, List *colTypes,
						pushdown_safety_info *safetyInfo);
static bool targetIsInAllPartitionLists(TargetEntry *tle, Query *query);
static bool qual_is_pushdown_safe(Query *subquery, Index rti, Node *qual,
					  pushdown_safety_info *safetyInfo);
static void subquery_push_qual(Query *subquery,
				   RangeTblEntry *rte, Index rti, Node *qual);
static void recurse_push_qual(Node *setOp, Query *topquery,
				  RangeTblEntry *rte, Index rti, Node *qual);
static void remove_unused_subquery_outputs(Query *subquery, RelOptInfo *rel);

#ifdef ADB
static bool set_path_reduce_info_worker(Path *path, List *reduce_info_list);
static bool get_subplan_ref_walker(Expr *expr, int *subplan_ref);
static int get_max_parallel_workers(List *pathlist);
static RelOptInfo* make_no_execparam_base_rel(PlannerInfo *root, RelOptInfo *baserel, List *exec_param_clauses);
#endif /* ADB */

/*
 * make_one_rel
 *	  Finds all possible access paths for executing a query, returning a
 *	  single rel that represents the join of all base rels in the query.
 */
RelOptInfo *
make_one_rel(PlannerInfo *root, List *joinlist)
{
	RelOptInfo *rel;
	Index		rti;

	/*
	 * Construct the all_baserels Relids set.
	 */
	root->all_baserels = NULL;
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (brel == NULL)
			continue;

		Assert(brel->relid == rti); /* sanity check on array */

		/* ignore RTEs that are "other rels" */
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		root->all_baserels = bms_add_member(root->all_baserels, brel->relid);
	}

	/* Mark base rels as to whether we care about fast-start plans */
	set_base_rel_consider_startup(root);

	/*
	 * Compute size estimates and consider_parallel flags for each base rel,
	 * then generate access paths.
	 */
	set_base_rel_sizes(root);
	set_base_rel_pathlists(root);

	/*
	 * Generate access paths for the entire join tree.
	 */
	rel = make_rel_from_joinlist(root, joinlist);

	/*
	 * The result should join all and only the query's base rels.
	 */
	Assert(bms_equal(rel->relids, root->all_baserels));

	return rel;
}

/*
 * set_base_rel_consider_startup
 *	  Set the consider_[param_]startup flags for each base-relation entry.
 *
 * For the moment, we only deal with consider_param_startup here; because the
 * logic for consider_startup is pretty trivial and is the same for every base
 * relation, we just let build_simple_rel() initialize that flag correctly to
 * start with.  If that logic ever gets more complicated it would probably
 * be better to move it here.
 */
static void
set_base_rel_consider_startup(PlannerInfo *root)
{
	/*
	 * Since parameterized paths can only be used on the inside of a nestloop
	 * join plan, there is usually little value in considering fast-start
	 * plans for them.  However, for relations that are on the RHS of a SEMI
	 * or ANTI join, a fast-start plan can be useful because we're only going
	 * to care about fetching one tuple anyway.
	 *
	 * To minimize growth of planning time, we currently restrict this to
	 * cases where the RHS is a single base relation, not a join; there is no
	 * provision for consider_param_startup to get set at all on joinrels.
	 * Also we don't worry about appendrels.  costsize.c's costing rules for
	 * nestloop semi/antijoins don't consider such cases either.
	 */
	ListCell   *lc;

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			varno;

		if ((sjinfo->jointype == JOIN_SEMI || sjinfo->jointype == JOIN_ANTI) &&
			bms_get_singleton_member(sjinfo->syn_righthand, &varno))
		{
			RelOptInfo *rel = find_base_rel(root, varno);

			rel->consider_param_startup = true;
		}
	}
}

/*
 * set_base_rel_sizes
 *	  Set the size estimates (rows and widths) for each base-relation entry.
 *	  Also determine whether to consider parallel paths for base relations.
 *
 * We do this in a separate pass over the base rels so that rowcount
 * estimates are available for parameterized path generation, and also so
 * that each rel's consider_parallel flag is set correctly before we begin to
 * generate paths.
 */
static void
set_base_rel_sizes(PlannerInfo *root)
{
	Index		rti;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		RangeTblEntry *rte;

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (rel == NULL)
			continue;

		Assert(rel->relid == rti);	/* sanity check on array */

		/* ignore RTEs that are "other rels" */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		rte = root->simple_rte_array[rti];

		/*
		 * If parallelism is allowable for this query in general, see whether
		 * it's allowable for this rel in particular.  We have to do this
		 * before set_rel_size(), because (a) if this rel is an inheritance
		 * parent, set_append_rel_size() will use and perhaps change the rel's
		 * consider_parallel flag, and (b) for some RTE types, set_rel_size()
		 * goes ahead and makes paths immediately.
		 */
		if (root->glob->parallelModeOK)
			set_rel_consider_parallel(root, rel, rte);

		set_rel_size(root, rel, rti, rte);
	}
}

/*
 * set_base_rel_pathlists
 *	  Finds all paths available for scanning each base-relation entry.
 *	  Sequential scan and any available indices are considered.
 *	  Each useful path is attached to its relation's 'pathlist' field.
 */
static void
set_base_rel_pathlists(PlannerInfo *root)
{
	Index		rti;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (rel == NULL)
			continue;

		Assert(rel->relid == rti);	/* sanity check on array */

		/* ignore RTEs that are "other rels" */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		set_rel_pathlist(root, rel, rti, root->simple_rte_array[rti]);
	}
}

/*
 * set_rel_size
 *	  Set size estimates for a base relation
 */
static void
set_rel_size(PlannerInfo *root, RelOptInfo *rel,
			 Index rti, RangeTblEntry *rte)
{
	if (rel->reloptkind == RELOPT_BASEREL &&
		relation_excluded_by_constraints(root, rel, rte))
	{
		/*
		 * We proved we don't need to scan the rel via constraint exclusion,
		 * so set up a single dummy path for it.  Here we only check this for
		 * regular baserels; if it's an otherrel, CE was already checked in
		 * set_append_rel_size().
		 *
		 * In this case, we go ahead and set up the relation's path right away
		 * instead of leaving it for set_rel_pathlist to do.  This is because
		 * we don't have a convention for marking a rel as dummy except by
		 * assigning a dummy path to it.
		 */
		set_dummy_rel_pathlist(rel);
	}
	else if (rte->inh)
	{
		/* It's an "append relation", process accordingly */
		set_append_rel_size(root, rel, rti, rte);
	}
	else
	{
		switch (rel->rtekind)
		{
			case RTE_RELATION:
				if (rte->relkind == RELKIND_FOREIGN_TABLE)
				{
					/* Foreign table */
					set_foreign_size(root, rel, rte);
				}
				else if (rte->relkind == RELKIND_PARTITIONED_TABLE)
				{
					/*
					 * A partitioned table without any partitions is marked as
					 * a dummy rel.
					 */
					set_dummy_rel_pathlist(rel);
				}
				else if (rte->tablesample != NULL)
				{
					/* Sampled relation */
					set_tablesample_rel_size(root, rel, rte);
				}
				else
				{
					/* Plain relation */
					set_plain_rel_size(root, rel, rte);
				}
				break;
			case RTE_SUBQUERY:

				/*
				 * Subqueries don't support making a choice between
				 * parameterized and unparameterized paths, so just go ahead
				 * and build their paths immediately.
				 */
				set_subquery_pathlist(root, rel, rti, rte);
				break;
			case RTE_FUNCTION:
				set_function_size_estimates(root, rel);
				break;
			case RTE_TABLEFUNC:
				set_tablefunc_size_estimates(root, rel);
				break;
			case RTE_VALUES:
				set_values_size_estimates(root, rel);
				break;
			case RTE_CTE:

				/*
				 * CTEs don't support making a choice between parameterized
				 * and unparameterized paths, so just go ahead and build their
				 * paths immediately.
				 */
				if (rte->self_reference)
					set_worktable_pathlist(root, rel, rte);
				else
					set_cte_pathlist(root, rel, rte);
				break;
			case RTE_NAMEDTUPLESTORE:
				set_namedtuplestore_pathlist(root, rel, rte);
				break;
#ifdef ADB
			case RTE_PARAMTS:
				set_paramtuplestore_pathlist(root, rel, rte);
				break;
#endif /* ADB */
			default:
				elog(ERROR, "unexpected rtekind: %d", (int) rel->rtekind);
				break;
		}
	}

	/*
	 * We insist that all non-dummy rels have a nonzero rowcount estimate.
	 */
	Assert(rel->rows > 0 || IS_DUMMY_REL(rel));
}

/*
 * set_rel_pathlist
 *	  Build access paths for a base relation
 */
static void
set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
				 Index rti, RangeTblEntry *rte)
{
	if (IS_DUMMY_REL(rel))
	{
		/* We already proved the relation empty, so nothing more to do */
	}
	else if (rte->inh)
	{
		/* It's an "append relation", process accordingly */
		set_append_rel_pathlist(root, rel, rti, rte);
	}
	else
	{
		switch (rel->rtekind)
		{
			case RTE_RELATION:
				if (rte->relkind == RELKIND_FOREIGN_TABLE)
				{
					/* Foreign table */
					set_foreign_pathlist(root, rel, rte);
				}
				else if (rte->tablesample != NULL)
				{
					/* Sampled relation */
					set_tablesample_rel_pathlist(root, rel, rte);
				}
				else
				{
					/* Plain relation */
					set_plain_rel_pathlist(root, rel, rte);
				}
				break;
			case RTE_SUBQUERY:
				/* Subquery --- fully handled during set_rel_size */
				break;
			case RTE_FUNCTION:
				/* RangeFunction */
				set_function_pathlist(root, rel, rte);
				break;
			case RTE_TABLEFUNC:
				/* Table Function */
				set_tablefunc_pathlist(root, rel, rte);
				break;
			case RTE_VALUES:
				/* Values list */
				set_values_pathlist(root, rel, rte);
				break;
			case RTE_CTE:
				/* CTE reference --- fully handled during set_rel_size */
				break;
			case RTE_NAMEDTUPLESTORE:
				/* tuplestore reference --- fully handled during set_rel_size */
				break;
#ifdef ADB
			case RTE_PARAMTS:
				/* tuplestore reference --- fully handled during set_rel_size */
				break;
#endif /* ADB */
			default:
				elog(ERROR, "unexpected rtekind: %d", (int) rel->rtekind);
				break;
		}
	}

	/*
	 * If this is a baserel, we should normally consider gathering any partial
	 * paths we may have created for it.
	 *
	 * However, if this is an inheritance child, skip it.  Otherwise, we could
	 * end up with a very large number of gather nodes, each trying to grab
	 * its own pool of workers.  Instead, we'll consider gathering partial
	 * paths for the parent appendrel.
	 *
	 * Also, if this is the topmost scan/join rel (that is, the only baserel),
	 * we postpone this until the final scan/join targelist is available (see
	 * grouping_planner).
	 */
	if (rel->reloptkind == RELOPT_BASEREL &&
		bms_membership(root->all_baserels) != BMS_SINGLETON)
		generate_gather_paths(root, rel, false);

	/*
	 * Allow a plugin to editorialize on the set of Paths for this base
	 * relation.  It could add new paths (such as CustomPaths) by calling
	 * add_path(), or delete or modify paths added by the core code.
	 */
	if (set_rel_pathlist_hook)
		(*set_rel_pathlist_hook) (root, rel, rti, rte);

	/* Now find the cheapest of the paths for this rel */
	set_cheapest(rel);

#ifdef OPTIMIZER_DEBUG
	debug_print_rel(root, rel);
#endif
}

/*
 * set_plain_rel_size
 *	  Set size estimates for a plain relation (no subquery, no inheritance)
 */
static void
set_plain_rel_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/*
	 * Test any partial indexes of rel for applicability.  We must do this
	 * first since partial unique indexes can affect size estimates.
	 */
	check_index_predicates(root, rel);

#ifdef ADB
	if (rel->loc_info)
		rel->remote_oids = relation_remote_by_constraints(root, rel, use_aux_type >= USE_AUX_CTID);
#endif /* ADB */

	/* Mark rel with estimated output rows, width, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * If this relation could possibly be scanned from within a worker, then set
 * its consider_parallel flag.
 */
static void
set_rel_consider_parallel(PlannerInfo *root, RelOptInfo *rel,
						  RangeTblEntry *rte)
{
	/*
	 * The flag has previously been initialized to false, so we can just
	 * return if it becomes clear that we can't safely set it.
	 */
	Assert(!rel->consider_parallel);

	/* Don't call this if parallelism is disallowed for the entire query. */
	Assert(root->glob->parallelModeOK);

	/* This should only be called for baserels and appendrel children. */
	Assert(IS_SIMPLE_REL(rel));

	/* Assorted checks based on rtekind. */
	switch (rte->rtekind)
	{
		case RTE_RELATION:

			/*
			 * Currently, parallel workers can't access the leader's temporary
			 * tables.  We could possibly relax this if the wrote all of its
			 * local buffers at the start of the query and made no changes
			 * thereafter (maybe we could allow hint bit changes), and if we
			 * taught the workers to read them.  Writing a large number of
			 * temporary buffers could be expensive, though, and we don't have
			 * the rest of the necessary infrastructure right now anyway.  So
			 * for now, bail out if we see a temporary table.
			 */
			if (get_rel_persistence(rte->relid) == RELPERSISTENCE_TEMP)
				return;

			/*
			 * Table sampling can be pushed down to workers if the sample
			 * function and its arguments are safe.
			 */
			if (rte->tablesample != NULL)
			{
				char		proparallel = func_parallel(rte->tablesample->tsmhandler);

				if (proparallel != PROPARALLEL_SAFE)
					return;
				if (!is_parallel_safe(root, (Node *) rte->tablesample->args))
					return;
			}

			/*
			 * Ask FDWs whether they can support performing a ForeignScan
			 * within a worker.  Most often, the answer will be no.  For
			 * example, if the nature of the FDW is such that it opens a TCP
			 * connection with a remote server, each parallel worker would end
			 * up with a separate connection, and these connections might not
			 * be appropriately coordinated between workers and the leader.
			 */
			if (rte->relkind == RELKIND_FOREIGN_TABLE)
			{
				Assert(rel->fdwroutine);
				if (!rel->fdwroutine->IsForeignScanParallelSafe)
					return;
				if (!rel->fdwroutine->IsForeignScanParallelSafe(root, rel, rte))
					return;
			}

			/*
			 * There are additional considerations for appendrels, which we'll
			 * deal with in set_append_rel_size and set_append_rel_pathlist.
			 * For now, just set consider_parallel based on the rel's own
			 * quals and targetlist.
			 */
			break;

		case RTE_SUBQUERY:

			/*
			 * There's no intrinsic problem with scanning a subquery-in-FROM
			 * (as distinct from a SubPlan or InitPlan) in a parallel worker.
			 * If the subquery doesn't happen to have any parallel-safe paths,
			 * then flagging it as consider_parallel won't change anything,
			 * but that's true for plain tables, too.  We must set
			 * consider_parallel based on the rel's own quals and targetlist,
			 * so that if a subquery path is parallel-safe but the quals and
			 * projection we're sticking onto it are not, we correctly mark
			 * the SubqueryScanPath as not parallel-safe.  (Note that
			 * set_subquery_pathlist() might push some of these quals down
			 * into the subquery itself, but that doesn't change anything.)
			 */
			break;

		case RTE_JOIN:
			/* Shouldn't happen; we're only considering baserels here. */
			Assert(false);
			return;

		case RTE_FUNCTION:
			/* Check for parallel-restricted functions. */
			if (!is_parallel_safe(root, (Node *) rte->functions))
				return;
			break;

		case RTE_TABLEFUNC:
			/* not parallel safe */
			return;

		case RTE_VALUES:
			/* Check for parallel-restricted functions. */
			if (!is_parallel_safe(root, (Node *) rte->values_lists))
				return;
			break;

		case RTE_CTE:
#ifdef ADB
		case RTE_PARAMTS:
		/*
		 * ADBQ: Can not decide remote dummy work with parallel now.
		 */
		case RTE_REMOTE_DUMMY:
#endif

			/*
			 * CTE tuplestores aren't shared among parallel workers, so we
			 * force all CTE scans to happen in the leader.  Also, populating
			 * the CTE would require executing a subplan that's not available
			 * in the worker, might be parallel-restricted, and must get
			 * executed only once.
			 */
			return;

		case RTE_NAMEDTUPLESTORE:

			/*
			 * tuplestore cannot be shared, at least without more
			 * infrastructure to support that.
			 */
			return;
	}

	/*
	 * If there's anything in baserestrictinfo that's parallel-restricted, we
	 * give up on parallelizing access to this relation.  We could consider
	 * instead postponing application of the restricted quals until we're
	 * above all the parallelism in the plan tree, but it's not clear that
	 * that would be a win in very many cases, and it might be tricky to make
	 * outer join clauses work correctly.  It would likely break equivalence
	 * classes, too.
	 */
	if (!is_parallel_safe(root, (Node *) rel->baserestrictinfo))
		return;

	/*
	 * Likewise, if the relation's outputs are not parallel-safe, give up.
	 * (Usually, they're just Vars, but sometimes they're not.)
	 */
	if (!is_parallel_safe(root, (Node *) rel->reltarget->exprs))
		return;

	/* We have a winner. */
	rel->consider_parallel = true;
}

#ifdef ADB
static void recost_plain_path(Path *path, ReduceInfo *rinfo)
{
	if (rinfo->exclude_exec == NIL &&	/* not hin distribute key */
		path->pathtype == T_SeqScan)	/* not IndexScan,TidScan... */
	{
		Cost run_cost = path->total_cost - path->startup_cost;
		path->total_cost = path->startup_cost + run_cost / list_length(rinfo->storage_nodes);

		if (path->rows >= 1.0)			/* avoid final rows is zero */
		{
			path->rows /= list_length(rinfo->storage_nodes);
			if (path->rows < 1.0)
				path->rows = 1.0;
		}
	}
}
#endif /* ADB */

/*
 * set_plain_rel_pathlist
 *	  Build access paths for a plain relation (no subquery, no inheritance)
 */
static void
set_plain_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;

	/*
	 * We don't support pushing join clauses into the quals of a seqscan, but
	 * it could still have required parameterization due to LATERAL refs in
	 * its tlist.
	 */
	required_outer = rel->lateral_relids;
#ifdef ADB
	if (rel->loc_info &&
		rel->remote_oids == NIL)
	{
		set_dummy_rel_pathlist(rel);
		return;
	}

	if(root->glob->clusterPlanOK && rel->loc_info)
	{
		Path *path;
		ListCell *lc;
		List *reduce_info_list;
		ReduceInfo *rinfo;
		RelOptInfo *current_rel;
		List *exec_param_clauses;
		List *exclude = NIL;
		RelationLocInfo *loc_info = rel->loc_info;
		if (IsLocatorDistributedByValue(loc_info->locatorType) ||
			loc_info->locatorType == LOCATOR_TYPE_USER_DEFINED)
		{
			exclude = list_difference_oid(rel->loc_info->nodeids, rel->remote_oids);
		}
		rinfo = MakeReduceInfoFromLocInfo(loc_info, exclude, rte->relid, rel->relid);
		list_free(exclude);

		if (!IsReduceInfoInOneNode(rinfo))
		{
			/*
			 * volatile exprs can not run in one more node,
			 * because they return different results on different nodes
			 */
			bool has_volatile_expr = false;
			foreach(lc, rel->baserestrictinfo)
			{
				RestrictInfo *ri = lfirst_node(RestrictInfo, lc);
				if (contain_volatile_functions((Node*)(ri->clause)))
				{
					has_volatile_expr = true;
					break;
				}
			}

			/* target list usually not real for now, so skip test PathTarget */
#if 0
			if (has_volatile_expr == false &&
				rel->reltarget != NULL && /* Is it can be null? Add test in case of safety */
				contain_volatile_functions((Node*)(rel->reltarget->exprs)))
				has_volatile_expr = true;
#endif

			if (has_volatile_expr)
			{
				List *exec_on;
				Oid preferred;
				if (!IsReduceInfoReplicated(rinfo))
				{
					/*
					 * when not replicate relation,
					 * we have no idea to make cluster plan only run in one node
					 */
					root->glob->clusterPlanOK = false;
					FreeReduceInfo(rinfo);
					goto no_cluster_paths_;
				}

				/* let plan run in preferred node */
				if (rinfo->exclude_exec == NIL)
					exec_on = rinfo->storage_nodes;
				else
					exec_on = list_difference_oid(rinfo->storage_nodes, rinfo->exclude_exec);

				preferred = get_preferred_nodeoid(exec_on);
				if (exec_on != rinfo->storage_nodes)
					list_free(exec_on);
				FreeReduceInfo(rinfo);

				/*
				 * make a random ReduceInfo only run in preferred node,
				 * when other node received this plan will replace it to empty plan
				 */
				exec_on = list_make1_oid(preferred);
				rinfo = MakeRandomReduceInfo(exec_on);
				list_free(exec_on);
			}
		}

		exec_param_clauses = NIL;
		foreach(lc, rel->baserestrictinfo)
		{
			RestrictInfo *ri = lfirst(lc);
			if(expr_have_upper_reference(ri->clause, root))
			{
				exec_param_clauses = lappend(exec_param_clauses, ri);
			}
		}

		if(exec_param_clauses)
		{
			/* make a no EXEC_PARAM qual list RelOptInfo */
			current_rel = make_no_execparam_base_rel(root, rel, exec_param_clauses);
		}else
		{
			current_rel = rel;
		}

		add_path(current_rel, create_seqscan_path(root, current_rel, required_outer, 0));

		/* If appropriate, consider parallel sequential scan */
		if (current_rel->consider_parallel && required_outer == NULL)
			create_plain_partial_paths(root, current_rel);

		/* Consider index scans */
		create_index_paths(root, current_rel);

		/* Consider TID scans */
		create_tidscan_paths(root, current_rel);

		reduce_info_list = list_make1(rinfo);

		/* recost pathlist */
		foreach(lc, current_rel->pathlist)
		{
			path = lfirst(lc);

			set_path_reduce_info_worker(path, reduce_info_list);

			recost_plain_path(path, rinfo);
		}

		/* same idea to recost partial pathlist */
		foreach(lc, current_rel->partial_pathlist)
		{
			path = lfirst(lc);

			set_path_reduce_info_worker(path, reduce_info_list);

			recost_plain_path(path, rinfo);
		}

		if (exec_param_clauses)
		{
			List *replicate = list_make1(MakeFinalReplicateReduceInfo());

			generate_gather_paths(root, current_rel, false);

			foreach(lc, current_rel->pathlist)
			{
				path = lfirst(lc);
				if (PATH_REQ_OUTER(path))
					continue;
				path = (Path*)create_reducescan_path(root,
													 rel,
													 rel->reltarget,
													 path,
													 replicate,
													 path->pathkeys,
													 exec_param_clauses);
				/* don't need add_cluster_path */
				rel->cluster_pathlist = lappend(rel->cluster_pathlist, path);
			}
		}else
		{
			Assert(rel == current_rel);
			/* move pathlist to cluster_pathlist */
			rel->cluster_pathlist = rel->pathlist;
			rel->pathlist = NIL;
			rel->cluster_partial_pathlist = rel->partial_pathlist;
			rel->partial_pathlist = NIL;
		}
	}

no_cluster_paths_:
	Assert(rel->pathlist == NIL);
	Assert(rel->partial_pathlist == NIL);
	if (!create_plainrel_rqpath(root, rel, rte, required_outer))
	{
#endif
	/* Consider sequential scan */
	add_path(rel, create_seqscan_path(root, rel, required_outer, 0));

	/* If appropriate, consider parallel sequential scan */
	if (rel->consider_parallel && required_outer == NULL)
		create_plain_partial_paths(root, rel);

	/* Consider index scans */
	create_index_paths(root, rel);

	/* Consider TID scans */
	create_tidscan_paths(root, rel);
#ifdef ADB
	}
#endif
}

/*
 * create_plain_partial_paths
 *	  Build partial access paths for parallel scan of a plain relation
 */
static void
create_plain_partial_paths(PlannerInfo *root, RelOptInfo *rel)
{
	int			parallel_workers;

	parallel_workers = compute_parallel_worker(rel, rel->pages, -1,
											   max_parallel_workers_per_gather);

	/* If any limit was set to zero, the user doesn't want a parallel scan. */
	if (parallel_workers <= 0)
		return;

	/* Add an unordered partial path based on a parallel sequential scan. */
	add_partial_path(rel, create_seqscan_path(root, rel, NULL, parallel_workers));
}

/*
 * set_tablesample_rel_size
 *	  Set size estimates for a sampled relation
 */
static void
set_tablesample_rel_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	TableSampleClause *tsc = rte->tablesample;
	TsmRoutine *tsm;
	BlockNumber pages;
	double		tuples;

	/*
	 * Test any partial indexes of rel for applicability.  We must do this
	 * first since partial unique indexes can affect size estimates.
	 */
	check_index_predicates(root, rel);

	/*
	 * Call the sampling method's estimation function to estimate the number
	 * of pages it will read and the number of tuples it will return.  (Note:
	 * we assume the function returns sane values.)
	 */
	tsm = GetTsmRoutine(tsc->tsmhandler);
	tsm->SampleScanGetSampleSize(root, rel, tsc->args,
								 &pages, &tuples);

	/*
	 * For the moment, because we will only consider a SampleScan path for the
	 * rel, it's okay to just overwrite the pages and tuples estimates for the
	 * whole relation.  If we ever consider multiple path types for sampled
	 * rels, we'll need more complication.
	 */
	rel->pages = pages;
	rel->tuples = tuples;

	/* Mark rel with estimated output rows, width, etc */
	set_baserel_size_estimates(root, rel);
}

/*
 * set_tablesample_rel_pathlist
 *	  Build access paths for a sampled relation
 */
static void
set_tablesample_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;
	Path	   *path;

	/*
	 * We don't support pushing join clauses into the quals of a samplescan,
	 * but it could still have required parameterization due to LATERAL refs
	 * in its tlist or TABLESAMPLE arguments.
	 */
	required_outer = rel->lateral_relids;

	/* Consider sampled scan */
	path = create_samplescan_path(root, rel, required_outer);

	/*
	 * If the sampling method does not support repeatable scans, we must avoid
	 * plans that would scan the rel multiple times.  Ideally, we'd simply
	 * avoid putting the rel on the inside of a nestloop join; but adding such
	 * a consideration to the planner seems like a great deal of complication
	 * to support an uncommon usage of second-rate sampling methods.  Instead,
	 * if there is a risk that the query might perform an unsafe join, just
	 * wrap the SampleScan in a Materialize node.  We can check for joins by
	 * counting the membership of all_baserels (note that this correctly
	 * counts inheritance trees as single rels).  If we're inside a subquery,
	 * we can't easily check whether a join might occur in the outer query, so
	 * just assume one is possible.
	 *
	 * GetTsmRoutine is relatively expensive compared to the other tests here,
	 * so check repeatable_across_scans last, even though that's a bit odd.
	 */
	if ((root->query_level > 1 ||
		 bms_membership(root->all_baserels) != BMS_SINGLETON) &&
		!(GetTsmRoutine(rte->tablesample->tsmhandler)->repeatable_across_scans))
	{
		path = (Path *) create_material_path(rel, path);
	}

	add_path(rel, path);

	/* For the moment, at least, there are no other paths to consider */
}

/*
 * set_foreign_size
 *		Set size estimates for a foreign table RTE
 */
static void
set_foreign_size(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* Mark rel with estimated output rows, width, etc */
	set_foreign_size_estimates(root, rel);

	/* Let FDW adjust the size estimates, if it can */
	rel->fdwroutine->GetForeignRelSize(root, rel, rte->relid);

	/* ... but do not let it set the rows estimate to zero */
	rel->rows = clamp_row_est(rel->rows);
}

/*
 * set_foreign_pathlist
 *		Build access paths for a foreign table RTE
 */
static void
set_foreign_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	/* Call the FDW's GetForeignPaths function to generate path(s) */
	rel->fdwroutine->GetForeignPaths(root, rel, rte->relid);
}

/*
 * set_append_rel_size
 *	  Set size estimates for a simple "append relation"
 *
 * The passed-in rel and RTE represent the entire append relation.  The
 * relation's contents are computed by appending together the output of the
 * individual member relations.  Note that in the non-partitioned inheritance
 * case, the first member relation is actually the same table as is mentioned
 * in the parent RTE ... but it has a different RTE and RelOptInfo.  This is
 * a good thing because their outputs are not the same size.
 */
static void
set_append_rel_size(PlannerInfo *root, RelOptInfo *rel,
					Index rti, RangeTblEntry *rte)
{
	int			parentRTindex = rti;
	bool		has_live_children;
	double		parent_rows;
	double		parent_size;
	double	   *parent_attrsizes;
	int			nattrs;
	ListCell   *l;
	Relids		live_children = NULL;
	bool		did_pruning = false;
#ifdef ADB
	int		   *subplan_ref;
	if (root->glob->subplans == NIL ||
		root->glob->clusterPlanOK == false)
		subplan_ref = NULL;
	else
		subplan_ref = palloc0(list_length(root->glob->subplans) * sizeof(subplan_ref[0]));
#endif /* ADB */

	/* Guard against stack overflow due to overly deep inheritance tree. */
	check_stack_depth();

	Assert(IS_SIMPLE_REL(rel));

	/*
	 * Initialize partitioned_child_rels to contain this RT index.
	 *
	 * Note that during the set_append_rel_pathlist() phase, we will bubble up
	 * the indexes of partitioned relations that appear down in the tree, so
	 * that when we've created Paths for all the children, the root
	 * partitioned table's list will contain all such indexes.
	 */
	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
		rel->partitioned_child_rels = list_make1_int(rti);

	/*
	 * If the partitioned relation has any baserestrictinfo quals then we
	 * attempt to use these quals to prune away partitions that cannot
	 * possibly contain any tuples matching these quals.  In this case we'll
	 * store the relids of all partitions which could possibly contain a
	 * matching tuple, and skip anything else in the loop below.
	 */
	if (enable_partition_pruning &&
		rte->relkind == RELKIND_PARTITIONED_TABLE &&
		rel->baserestrictinfo != NIL)
	{
		live_children = prune_append_rel_partitions(rel);
		did_pruning = true;
	}

	/*
	 * Initialize to compute size estimates for whole append relation.
	 *
	 * We handle width estimates by weighting the widths of different child
	 * rels proportionally to their number of rows.  This is sensible because
	 * the use of width estimates is mainly to compute the total relation
	 * "footprint" if we have to sort or hash it.  To do this, we sum the
	 * total equivalent size (in "double" arithmetic) and then divide by the
	 * total rowcount estimate.  This is done separately for the total rel
	 * width and each attribute.
	 *
	 * Note: if you consider changing this logic, beware that child rels could
	 * have zero rows and/or width, if they were excluded by constraints.
	 */
	has_live_children = false;
	parent_rows = 0;
	parent_size = 0;
	nattrs = rel->max_attr - rel->min_attr + 1;
	parent_attrsizes = (double *) palloc0(nattrs * sizeof(double));

	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		int			childRTindex;
		RangeTblEntry *childRTE;
		RelOptInfo *childrel;
		List	   *childquals;
		Index		cq_min_security;
		bool		have_const_false_cq;
		ListCell   *parentvars;
		ListCell   *childvars;
		ListCell   *lc;

		/* append_rel_list contains all append rels; ignore others */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		childRTindex = appinfo->child_relid;
		childRTE = root->simple_rte_array[childRTindex];

		/*
		 * The child rel's RelOptInfo was already created during
		 * add_base_rels_to_query.
		 */
		childrel = find_base_rel(root, childRTindex);
		Assert(childrel->reloptkind == RELOPT_OTHER_MEMBER_REL);

		if (rel->part_scheme)
		{
			AttrNumber	attno;

			/*
			 * We need attr_needed data for building targetlist of a join
			 * relation representing join between matching partitions for
			 * partitionwise join. A given attribute of a child will be needed
			 * in the same highest joinrel where the corresponding attribute
			 * of parent is needed. Hence it suffices to use the same Relids
			 * set for parent and child.
			 */
			for (attno = rel->min_attr; attno <= rel->max_attr; attno++)
			{
				int			index = attno - rel->min_attr;
				Relids		attr_needed = rel->attr_needed[index];

				/* System attributes do not need translation. */
				if (attno <= 0)
				{
					Assert(rel->min_attr == childrel->min_attr);
					childrel->attr_needed[index] = attr_needed;
				}
				else
				{
					Var		   *var = list_nth_node(Var,
													appinfo->translated_vars,
													attno - 1);
					int			child_index;

					/*
					 * Ignore any column dropped from the parent.
					 * Corresponding Var won't have any translation. It won't
					 * have attr_needed information, since it can not be
					 * referenced in the query.
					 */
					if (var == NULL)
					{
						Assert(attr_needed == NULL);
						continue;
					}

					child_index = var->varattno - childrel->min_attr;
					childrel->attr_needed[child_index] = attr_needed;
				}
			}
		}

		/*
		 * Copy/Modify targetlist. Even if this child is deemed empty, we need
		 * its targetlist in case it falls on nullable side in a child-join
		 * because of partitionwise join.
		 *
		 * NB: the resulting childrel->reltarget->exprs may contain arbitrary
		 * expressions, which otherwise would not occur in a rel's targetlist.
		 * Code that might be looking at an appendrel child must cope with
		 * such.  (Normally, a rel's targetlist would only include Vars and
		 * PlaceHolderVars.)  XXX we do not bother to update the cost or width
		 * fields of childrel->reltarget; not clear if that would be useful.
		 */
		childrel->reltarget->exprs = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) rel->reltarget->exprs,
								   1, &appinfo);

		/*
		 * We have to make child entries in the EquivalenceClass data
		 * structures as well.  This is needed either if the parent
		 * participates in some eclass joins (because we will want to consider
		 * inner-indexscan joins on the individual children) or if the parent
		 * has useful pathkeys (because we should try to build MergeAppend
		 * paths that produce those sort orderings). Even if this child is
		 * deemed dummy, it may fall on nullable side in a child-join, which
		 * in turn may participate in a MergeAppend, where we will need the
		 * EquivalenceClass data structures.
		 */
		if (rel->has_eclass_joins || has_useful_pathkeys(root, rel))
			add_child_rel_equivalences(root, appinfo, rel, childrel);
		childrel->has_eclass_joins = rel->has_eclass_joins;

		/*
		 * We have to copy the parent's quals to the child, with appropriate
		 * substitution of variables.  However, only the baserestrictinfo
		 * quals are needed before we can check for constraint exclusion; so
		 * do that first and then check to see if we can disregard this child.
		 *
		 * The child rel's targetlist might contain non-Var expressions, which
		 * means that substitution into the quals could produce opportunities
		 * for const-simplification, and perhaps even pseudoconstant quals.
		 * Therefore, transform each RestrictInfo separately to see if it
		 * reduces to a constant or pseudoconstant.  (We must process them
		 * separately to keep track of the security level of each qual.)
		 */
		childquals = NIL;
		cq_min_security = UINT_MAX;
		have_const_false_cq = false;
		foreach(lc, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
			Node	   *childqual;
			ListCell   *lc2;

			Assert(IsA(rinfo, RestrictInfo));
			childqual = adjust_appendrel_attrs(root,
											   (Node *) rinfo->clause,
											   1, &appinfo);
			childqual = eval_const_expressions(root, childqual);
			/* check for flat-out constant */
			if (childqual && IsA(childqual, Const))
			{
				if (((Const *) childqual)->constisnull ||
					!DatumGetBool(((Const *) childqual)->constvalue))
				{
					/* Restriction reduces to constant FALSE or NULL */
					have_const_false_cq = true;
					break;
				}
				/* Restriction reduces to constant TRUE, so drop it */
				continue;
			}
			/* might have gotten an AND clause, if so flatten it */
			foreach(lc2, make_ands_implicit((Expr *) childqual))
			{
				Node	   *onecq = (Node *) lfirst(lc2);
				bool		pseudoconstant;

				/* check for pseudoconstant (no Vars or volatile functions) */
				pseudoconstant =
					!contain_vars_of_level(onecq, 0) &&
					!contain_volatile_functions(onecq);
				if (pseudoconstant)
				{
					/* tell createplan.c to check for gating quals */
					root->hasPseudoConstantQuals = true;
				}
				/* reconstitute RestrictInfo with appropriate properties */
				childquals = lappend(childquals,
									 make_restrictinfo((Expr *) onecq,
													   rinfo->is_pushed_down,
													   rinfo->outerjoin_delayed,
													   pseudoconstant,
													   rinfo->security_level,
													   NULL, NULL, NULL));
				/* track minimum security level among child quals */
				cq_min_security = Min(cq_min_security, rinfo->security_level);
			}
		}

		/*
		 * In addition to the quals inherited from the parent, we might have
		 * securityQuals associated with this particular child node.
		 * (Currently this can only happen in appendrels originating from
		 * UNION ALL; inheritance child tables don't have their own
		 * securityQuals, see expand_inherited_rtentry().)	Pull any such
		 * securityQuals up into the baserestrictinfo for the child.  This is
		 * similar to process_security_barrier_quals() for the parent rel,
		 * except that we can't make any general deductions from such quals,
		 * since they don't hold for the whole appendrel.
		 */
		if (childRTE->securityQuals)
		{
			Index		security_level = 0;

			foreach(lc, childRTE->securityQuals)
			{
				List	   *qualset = (List *) lfirst(lc);
				ListCell   *lc2;

				foreach(lc2, qualset)
				{
					Expr	   *qual = (Expr *) lfirst(lc2);

					/* not likely that we'd see constants here, so no check */
					childquals = lappend(childquals,
										 make_restrictinfo(qual,
														   true, false, false,
														   security_level,
														   NULL, NULL, NULL));
					cq_min_security = Min(cq_min_security, security_level);
				}
				security_level++;
			}
			Assert(security_level <= root->qual_security_level);
		}

		/*
		 * OK, we've got all the baserestrictinfo quals for this child.
		 */
		childrel->baserestrictinfo = childquals;
		childrel->baserestrict_min_security = cq_min_security;

		if (have_const_false_cq)
		{
			/*
			 * Some restriction clause reduced to constant FALSE or NULL after
			 * substitution, so this child need not be scanned.
			 */
			set_dummy_rel_pathlist(childrel);
			continue;
		}

		if (did_pruning && !bms_is_member(appinfo->child_relid, live_children))
		{
			/* This partition was pruned; skip it. */
			set_dummy_rel_pathlist(childrel);
			continue;
		}

		if (relation_excluded_by_constraints(root, childrel, childRTE))
		{
			/*
			 * This child need not be scanned, so we can omit it from the
			 * appendrel.
			 */
			set_dummy_rel_pathlist(childrel);
			continue;
		}

		/* CE failed, so finish copying/modifying join quals. */
		childrel->joininfo = (List *)
			adjust_appendrel_attrs(root,
								   (Node *) rel->joininfo,
								   1, &appinfo);

		/*
		 * If parallelism is allowable for this query in general, see whether
		 * it's allowable for this childrel in particular.  But if we've
		 * already decided the appendrel is not parallel-safe as a whole,
		 * there's no point in considering parallelism for this child.  For
		 * consistency, do this before calling set_rel_size() for the child.
		 */
		if (root->glob->parallelModeOK && rel->consider_parallel)
			set_rel_consider_parallel(root, childrel, childRTE);

		/*
		 * Compute the child's size.
		 */
		set_rel_size(root, childrel, childRTindex, childRTE);

		/*
		 * It is possible that constraint exclusion detected a contradiction
		 * within a child subquery, even though we didn't prove one above. If
		 * so, we can skip this child.
		 */
		if (IS_DUMMY_REL(childrel))
			continue;

#ifdef ADB
		if (subplan_ref)
			get_subplan_ref_walker((Expr*)childquals, subplan_ref);
#endif /* ADB */

		/* We have at least one live child. */
		has_live_children = true;

		/*
		 * If any live child is not parallel-safe, treat the whole appendrel
		 * as not parallel-safe.  In future we might be able to generate plans
		 * in which some children are farmed out to workers while others are
		 * not; but we don't have that today, so it's a waste to consider
		 * partial paths anywhere in the appendrel unless it's all safe.
		 * (Child rels visited before this one will be unmarked in
		 * set_append_rel_pathlist().)
		 */
		if (!childrel->consider_parallel)
			rel->consider_parallel = false;

		/*
		 * Accumulate size information from each live child.
		 */
		Assert(childrel->rows > 0);

		parent_rows += childrel->rows;
		parent_size += childrel->reltarget->width * childrel->rows;

		/*
		 * Accumulate per-column estimates too.  We need not do anything for
		 * PlaceHolderVars in the parent list.  If child expression isn't a
		 * Var, or we didn't record a width estimate for it, we have to fall
		 * back on a datatype-based estimate.
		 *
		 * By construction, child's targetlist is 1-to-1 with parent's.
		 */
		forboth(parentvars, rel->reltarget->exprs,
				childvars, childrel->reltarget->exprs)
		{
			Var		   *parentvar = (Var *) lfirst(parentvars);
			Node	   *childvar = (Node *) lfirst(childvars);

			if (IsA(parentvar, Var))
			{
				int			pndx = parentvar->varattno - rel->min_attr;
				int32		child_width = 0;

				if (IsA(childvar, Var) &&
					((Var *) childvar)->varno == childrel->relid)
				{
					int			cndx = ((Var *) childvar)->varattno - childrel->min_attr;

					child_width = childrel->attr_widths[cndx];
				}
				if (child_width <= 0)
					child_width = get_typavgwidth(exprType(childvar),
												  exprTypmod(childvar));
				Assert(child_width > 0);
				parent_attrsizes[pndx] += child_width * childrel->rows;
			}
		}
	}

	if (has_live_children)
	{
		/*
		 * Save the finished size estimates.
		 */
		int			i;

		Assert(parent_rows > 0);
		rel->rows = parent_rows;
		rel->reltarget->width = rint(parent_size / parent_rows);
		for (i = 0; i < nattrs; i++)
			rel->attr_widths[i] = rint(parent_attrsizes[i] / parent_rows);

		/*
		 * Set "raw tuples" count equal to "rows" for the appendrel; needed
		 * because some places assume rel->tuples is valid for any baserel.
		 */
		rel->tuples = parent_rows;

#ifdef ADB
		if (subplan_ref)
		{
			/*
			 * when ClusterReduce use more then one times, it not work(not support ReScan)
			 */
			ListCell *lc;
			int i = 0;
			foreach(lc, root->glob->subroots)
			{
				if (subplan_ref[i] > 1)
				{
					/* SubPlan's cluster plan is not created, just mark it need rewind */
					root->glob->rewindPlanIDs = bms_add_member(root->glob->rewindPlanIDs, i+1);
				}
				++i;
			}
		}
#endif /* ADB */
	}
	else
	{
		/*
		 * All children were excluded by constraints, so mark the whole
		 * appendrel dummy.  We must do this in this phase so that the rel's
		 * dummy-ness is visible when we generate paths for other rels.
		 */
		set_dummy_rel_pathlist(rel);
	}

	pfree(parent_attrsizes);
#ifdef ADB
	if (subplan_ref)
		pfree(subplan_ref);
#endif /* ADB */
}

/*
 * set_append_rel_pathlist
 *	  Build access paths for an "append relation"
 */
static void
set_append_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
						Index rti, RangeTblEntry *rte)
{
	int			parentRTindex = rti;
	List	   *live_childrels = NIL;
	ListCell   *l;

	/*
	 * Generate access paths for each member relation, and remember the
	 * non-dummy children.
	 */
	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		int			childRTindex;
		RangeTblEntry *childRTE;
		RelOptInfo *childrel;

		/* append_rel_list contains all append rels; ignore others */
		if (appinfo->parent_relid != parentRTindex)
			continue;

		/* Re-locate the child RTE and RelOptInfo */
		childRTindex = appinfo->child_relid;
		childRTE = root->simple_rte_array[childRTindex];
		childrel = root->simple_rel_array[childRTindex];

		/*
		 * If set_append_rel_size() decided the parent appendrel was
		 * parallel-unsafe at some point after visiting this child rel, we
		 * need to propagate the unsafety marking down to the child, so that
		 * we don't generate useless partial paths for it.
		 */
		if (!rel->consider_parallel)
			childrel->consider_parallel = false;

		/*
		 * Compute the child's access paths.
		 */
		set_rel_pathlist(root, childrel, childRTindex, childRTE);

		/*
		 * If child is dummy, ignore it.
		 */
		if (IS_DUMMY_REL(childrel))
			continue;

		/* Bubble up childrel's partitioned children. */
		if (rel->part_scheme)
			rel->partitioned_child_rels =
				list_concat(rel->partitioned_child_rels,
							list_copy(childrel->partitioned_child_rels));

		/*
		 * Child is live, so add it to the live_childrels list for use below.
		 */
		live_childrels = lappend(live_childrels, childrel);
	}

	/* Add paths to the append relation. */
	add_paths_to_append_rel(root, rel, live_childrels);
}


/*
 * add_paths_to_append_rel
 *		Generate paths for the given append relation given the set of non-dummy
 *		child rels.
 *
 * The function collects all parameterizations and orderings supported by the
 * non-dummy children. For every such parameterization or ordering, it creates
 * an append path collecting one path from each non-dummy child with given
 * parameterization or ordering. Similarly it collects partial paths from
 * non-dummy children to create partial append paths.
 */
void
add_paths_to_append_rel(PlannerInfo *root, RelOptInfo *rel,
						List *live_childrels)
{
	List	   *subpaths = NIL;
	bool		subpaths_valid = true;
	List	   *partial_subpaths = NIL;
	List	   *pa_partial_subpaths = NIL;
	List	   *pa_nonpartial_subpaths = NIL;
	bool		partial_subpaths_valid = true;
	bool		pa_subpaths_valid;
	List	   *all_child_pathkeys = NIL;
	List	   *all_child_outers = NIL;
	ListCell   *l;
	List	   *partitioned_rels = NIL;
	RelOptInfo *childrel;
	Path	   *path;
	bool		build_partitioned_rels = false;
	double		partial_rows = -1;

	/* If appropriate, consider parallel append */
	pa_subpaths_valid = enable_parallel_append && rel->consider_parallel;

	/*
	 * AppendPath generated for partitioned tables must record the RT indexes
	 * of partitioned tables that are direct or indirect children of this
	 * Append rel.
	 *
	 * AppendPath may be for a sub-query RTE (UNION ALL), in which case, 'rel'
	 * itself does not represent a partitioned relation, but the child sub-
	 * queries may contain references to partitioned relations.  The loop
	 * below will look for such children and collect them in a list to be
	 * passed to the path creation function.  (This assumes that we don't need
	 * to look through multiple levels of subquery RTEs; if we ever do, we
	 * could consider stuffing the list we generate here into sub-query RTE's
	 * RelOptInfo, just like we do for partitioned rels, which would be used
	 * when populating our parent rel with paths.  For the present, that
	 * appears to be unnecessary.)
	 */
	if (rel->part_scheme != NULL)
	{
		if (IS_SIMPLE_REL(rel))
			partitioned_rels = rel->partitioned_child_rels;
		else if (IS_JOIN_REL(rel))
		{
			int			relid = -1;

			/*
			 * For a partitioned joinrel, concatenate the component rels'
			 * partitioned_child_rels lists.
			 */
			while ((relid = bms_next_member(rel->relids, relid)) >= 0)
			{
				RelOptInfo *component;

				Assert(relid >= 1 && relid < root->simple_rel_array_size);
				component = root->simple_rel_array[relid];
				Assert(component->part_scheme != NULL);
				Assert(list_length(component->partitioned_child_rels) >= 1);
				partitioned_rels =
					list_concat(partitioned_rels,
								list_copy(component->partitioned_child_rels));
			}
		}

		Assert(list_length(partitioned_rels) >= 1);
	}
	else if (rel->rtekind == RTE_SUBQUERY)
		build_partitioned_rels = true;

	/*
	 * For every non-dummy child, remember the cheapest path.  Also, identify
	 * all pathkeys (orderings) and parameterizations (required_outer sets)
	 * available for the non-dummy member relations.
	 */
	foreach(l, live_childrels)
	{
		ListCell   *lcp;
		Path	   *cheapest_partial_path = NULL;
		childrel = lfirst(l);

		/*
		 * If we need to build partitioned_rels, accumulate the partitioned
		 * rels for this child.  We must ensure that parents are always listed
		 * before their child partitioned tables.
		 */
		if (build_partitioned_rels)
		{
			List	   *cprels = childrel->partitioned_child_rels;

			partitioned_rels = list_concat(partitioned_rels,
										   list_copy(cprels));
		}

		/*
		 * If child has an unparameterized cheapest-total path, add that to
		 * the unparameterized Append path we are constructing for the parent.
		 * If not, there's no workable unparameterized path.
		 *
		 * With partitionwise aggregates, the child rel's pathlist may be
		 * empty, so don't assume that a path exists here.
		 */
		if (childrel->pathlist != NIL &&
			childrel->cheapest_total_path->param_info == NULL)
			accumulate_append_subpath(childrel->cheapest_total_path,
									  &subpaths, NULL);
		else
			subpaths_valid = false;

		/* Same idea, but for a partial plan. */
		if (childrel->partial_pathlist != NIL)
		{
			cheapest_partial_path = linitial(childrel->partial_pathlist);
			accumulate_append_subpath(cheapest_partial_path,
									  &partial_subpaths, NULL);
		}
		else
			partial_subpaths_valid = false;

		/*
		 * Same idea, but for a parallel append mixing partial and non-partial
		 * paths.
		 */
		if (pa_subpaths_valid)
		{
			Path	   *nppath = NULL;

			nppath =
				get_cheapest_parallel_safe_total_inner(childrel->pathlist);

			if (cheapest_partial_path == NULL && nppath == NULL)
			{
				/* Neither a partial nor a parallel-safe path?  Forget it. */
				pa_subpaths_valid = false;
			}
			else if (nppath == NULL ||
					 (cheapest_partial_path != NULL &&
					  cheapest_partial_path->total_cost < nppath->total_cost))
			{
				/* Partial path is cheaper or the only option. */
				Assert(cheapest_partial_path != NULL);
				accumulate_append_subpath(cheapest_partial_path,
										  &pa_partial_subpaths,
										  &pa_nonpartial_subpaths);

			}
			else
			{
				/*
				 * Either we've got only a non-partial path, or we think that
				 * a single backend can execute the best non-partial path
				 * faster than all the parallel backends working together can
				 * execute the best partial path.
				 *
				 * It might make sense to be more aggressive here.  Even if
				 * the best non-partial path is more expensive than the best
				 * partial path, it could still be better to choose the
				 * non-partial path if there are several such paths that can
				 * be given to different workers.  For now, we don't try to
				 * figure that out.
				 */
				accumulate_append_subpath(nppath,
										  &pa_nonpartial_subpaths,
										  NULL);
			}
		}

		/*
		 * Collect lists of all the available path orderings and
		 * parameterizations for all the children.  We use these as a
		 * heuristic to indicate which sort orderings and parameterizations we
		 * should build Append and MergeAppend paths for.
		 */
		foreach(lcp, childrel->pathlist)
		{
			Path	   *childpath = (Path *) lfirst(lcp);
			List	   *childkeys = childpath->pathkeys;
			Relids		childouter = PATH_REQ_OUTER(childpath);

			/* Unsorted paths don't contribute to pathkey list */
			if (childkeys != NIL)
			{
				ListCell   *lpk;
				bool		found = false;

				/* Have we already seen this ordering? */
				foreach(lpk, all_child_pathkeys)
				{
					List	   *existing_pathkeys = (List *) lfirst(lpk);

					if (compare_pathkeys(existing_pathkeys,
										 childkeys) == PATHKEYS_EQUAL)
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					/* No, so add it to all_child_pathkeys */
					all_child_pathkeys = lappend(all_child_pathkeys,
												 childkeys);
				}
			}

			/* Unparameterized paths don't contribute to param-set list */
			if (childouter)
			{
				ListCell   *lco;
				bool		found = false;

				/* Have we already seen this param set? */
				foreach(lco, all_child_outers)
				{
					Relids		existing_outers = (Relids) lfirst(lco);

					if (bms_equal(existing_outers, childouter))
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					/* No, so add it to all_child_outers */
					all_child_outers = lappend(all_child_outers,
											   childouter);
				}
			}
		}
	}

	/*
	 * If we found unparameterized paths for all children, build an unordered,
	 * unparameterized Append path for the rel.  (Note: this is correct even
	 * if we have zero or one live subpath due to constraint exclusion.)
	 */
	if (subpaths_valid)
		add_path(rel, (Path *) create_append_path(root, rel, subpaths, NIL,
												  NULL, 0, false,
												  partitioned_rels, -1));

	/*
	 * Consider an append of unordered, unparameterized partial paths.  Make
	 * it parallel-aware if possible.
	 */
	if (partial_subpaths_valid)
	{
		AppendPath *appendpath;
		ListCell   *lc;
		int			parallel_workers = 0;

		/* Find the highest number of workers requested for any subpath. */
		foreach(lc, partial_subpaths)
		{
			path = lfirst(lc);

			parallel_workers = Max(parallel_workers, path->parallel_workers);
		}
		Assert(parallel_workers > 0);

		/*
		 * If the use of parallel append is permitted, always request at least
		 * log2(# of children) workers.  We assume it can be useful to have
		 * extra workers in this case because they will be spread out across
		 * the children.  The precise formula is just a guess, but we don't
		 * want to end up with a radically different answer for a table with N
		 * partitions vs. an unpartitioned table with the same data, so the
		 * use of some kind of log-scaling here seems to make some sense.
		 */
		if (enable_parallel_append)
		{
			parallel_workers = Max(parallel_workers,
								   fls(list_length(live_childrels)));
			parallel_workers = Min(parallel_workers,
								   max_parallel_workers_per_gather);
		}
		Assert(parallel_workers > 0);

		/* Generate a partial append path. */
		appendpath = create_append_path(root, rel, NIL, partial_subpaths,
										NULL, parallel_workers,
										enable_parallel_append,
										partitioned_rels, -1);

		/*
		 * Make sure any subsequent partial paths use the same row count
		 * estimate.
		 */
		partial_rows = appendpath->path.rows;

		/* Add the path. */
		add_partial_path(rel, (Path *) appendpath);
	}

	/*
	 * Consider a parallel-aware append using a mix of partial and non-partial
	 * paths.  (This only makes sense if there's at least one child which has
	 * a non-partial path that is substantially cheaper than any partial path;
	 * otherwise, we should use the append path added in the previous step.)
	 */
	if (pa_subpaths_valid && pa_nonpartial_subpaths != NIL)
	{
		AppendPath *appendpath;
		ListCell   *lc;
		int			parallel_workers = 0;

		/*
		 * Find the highest number of workers requested for any partial
		 * subpath.
		 */
		foreach(lc, pa_partial_subpaths)
		{
			path = lfirst(lc);

			parallel_workers = Max(parallel_workers, path->parallel_workers);
		}

		/*
		 * Same formula here as above.  It's even more important in this
		 * instance because the non-partial paths won't contribute anything to
		 * the planned number of parallel workers.
		 */
		parallel_workers = Max(parallel_workers,
							   fls(list_length(live_childrels)));
		parallel_workers = Min(parallel_workers,
							   max_parallel_workers_per_gather);
		Assert(parallel_workers > 0);

		appendpath = create_append_path(root, rel, pa_nonpartial_subpaths,
										pa_partial_subpaths,
										NULL, parallel_workers, true,
										partitioned_rels, partial_rows);
		add_partial_path(rel, (Path *) appendpath);
	}

	/*
	 * Also build unparameterized MergeAppend paths based on the collected
	 * list of child pathkeys.
	 */
	if (subpaths_valid)
		generate_mergeappend_paths(root, rel, live_childrels,
								   all_child_pathkeys,
								   partitioned_rels);

	/*
	 * Build Append paths for each parameterization seen among the child rels.
	 * (This may look pretty expensive, but in most cases of practical
	 * interest, the child rels will expose mostly the same parameterizations,
	 * so that not that many cases actually get considered here.)
	 *
	 * The Append node itself cannot enforce quals, so all qual checking must
	 * be done in the child paths.  This means that to have a parameterized
	 * Append path, we must have the exact same parameterization for each
	 * child path; otherwise some children might be failing to check the
	 * moved-down quals.  To make them match up, we can try to increase the
	 * parameterization of lesser-parameterized paths.
	 */
	foreach(l, all_child_outers)
	{
		Relids		required_outer = (Relids) lfirst(l);
		ListCell   *lcr;

		/* Select the child paths for an Append with this parameterization */
		subpaths = NIL;
		subpaths_valid = true;
		foreach(lcr, live_childrels)
		{
			childrel = (RelOptInfo *) lfirst(lcr);
			Path	   *subpath;

			if (childrel->pathlist == NIL)
			{
				/* failed to make a suitable path for this child */
				subpaths_valid = false;
				break;
			}

			subpath = get_cheapest_parameterized_child_path(root,
															childrel,
															required_outer);
			if (subpath == NULL)
			{
				/* failed to make a suitable path for this child */
				subpaths_valid = false;
				break;
			}
			accumulate_append_subpath(subpath, &subpaths, NULL);
		}

		if (subpaths_valid)
			add_path(rel, (Path *)
					 create_append_path(root, rel, subpaths, NIL,
										required_outer, 0, false,
										partitioned_rels, -1));
	}
#ifdef ADB
	add_cluster_paths_to_append_rel(root, rel, live_childrels, partitioned_rels);
#endif /* ADB */
}

#ifdef ADB
void add_cluster_paths_to_append_rel(PlannerInfo *root, RelOptInfo *rel,
									 List *childrels, List *partitioned_rels)
{
	List	   *reduce_list;
	List	   *reduce_var_map;
	List	   *all_reduce_by_val_list;
	List	   *all_replicate_oid;
	List	   *all_child_pathkeys;
	List	   *all_child_outers;
	List	   *subpaths;
	ListCell   *lc_pk;
	ListCell   *l;
	Path	   *path;
	RelOptInfo *childrel;
	ReduceInfo *reduce_info;
	bool		have_reduce_coord;
	bool		have_final_replicate;
	bool		generate_partial;
	bool		subpaths_valid;

	generate_partial = false;
re_generate_append_:
	have_reduce_coord = false;
	have_final_replicate = false;
	reduce_var_map = NIL;
	all_reduce_by_val_list = NIL;
	all_replicate_oid = NIL;
	all_child_pathkeys = NIL;
	all_child_outers = NIL;
	foreach(l, childrels)
	{
		ListCell *lc_path;
		childrel = lfirst(l);

		foreach(lc_path, generate_partial ? childrel->cluster_partial_pathlist : childrel->cluster_pathlist)
		{
			Relids		childouter;
			path = lfirst(lc_path);

			/* find all param paths */
			childouter = PATH_REQ_OUTER(path);
			if(childouter)
			{
				ListCell   *lco;
				bool		found = false;

				foreach(lco, all_child_outers)
				{
					if (bms_equal(lfirst(lco), childouter))
					{
						found = true;
						break;
					}
				}
				if (!found)
					all_child_outers = lappend(all_child_outers, childouter);
			}

			/* find all reduce info */
			reduce_list = get_reduce_info_list(path);
			if(IsReduceInfoListByValue(reduce_list))
			{
				ListCell *lc_path_reduce;
				ListCell *lc_saved_reduce;
				ReduceInfo *save_reduce;

				foreach(lc_path_reduce, reduce_list)
				{
					List *new_attno;
					reduce_info = lfirst(lc_path_reduce);
					Assert(IsReduceInfoByValue(reduce_info));

					/* find reduce column(s) target */
					new_attno = ReduceInfoFindPathTarget(reduce_info, path->pathtarget);
					if(new_attno == NIL)
					{
						/* lost target */
						continue;
					}

					foreach(lc_saved_reduce, all_reduce_by_val_list)
					{
						save_reduce = lfirst(lc_saved_reduce);
						if (IsReduceInfoSame(reduce_info, save_reduce))
							break;
					}
					if(lc_saved_reduce == NULL)
					{
						/* no saved */
						all_reduce_by_val_list = lappend(all_reduce_by_val_list, reduce_info);
						reduce_var_map = lappend(reduce_var_map, new_attno);
					}else
					{
						list_free(new_attno);
					}
				}
			}else if(IsReduceInfoListCoordinator(reduce_list))
			{
				have_reduce_coord = true;
			}else if(IsReduceInfoListFinalReplicated(reduce_list))
			{
				have_final_replicate = true;
			}else if(IsReduceInfoListReplicated(reduce_list))
			{
				/* remember all replicated nodes oid */
				Assert(list_length(reduce_list) == 1);
				reduce_info = linitial(reduce_list);
				all_replicate_oid = list_concat_unique_oid(all_replicate_oid, reduce_info->storage_nodes);
			}

			/* remember all pathkeys */
			if (path->pathkeys != NIL)
			{
				ListCell   *lpk;
				bool		found = false;

				foreach(lpk, all_child_pathkeys)
				{
					if (compare_pathkeys(lfirst(lpk), path->pathkeys) == PATHKEYS_EQUAL)
					{
						found = true;
						break;
					}
				}
				if (!found)
					all_child_pathkeys = lappend(all_child_pathkeys, path->pathkeys);
			}
		}
	}

	all_child_pathkeys = lcons(NIL, all_child_pathkeys);
	all_child_outers = lcons(NULL, all_child_outers);
	foreach(lc_pk, all_child_pathkeys)
	{
		ListCell	   *lc_reduce_attno;
		ReduceInfo	   *sub_reduce;
		ListCell	   *lc_rel;
		ListCell	   *lc_path;
		ListCell	   *lc_reduce;
		ListCell	   *lc_outer;
		Relids			req_outer;
		Path		   *test_path;
		Path		   *pathkey_path;
		List		   *reduce_attno;
		List		   *pathkeys = lfirst(lc_pk);
		int				parallel_workers;
		bool			have_pathkeys_path;

		foreach(lc_outer, all_child_outers)
		{
			req_outer = lfirst(lc_outer);

			/* make redue by value AppendPath */
			forboth(l, all_reduce_by_val_list, lc_reduce_attno, reduce_var_map)
			{
				have_pathkeys_path = false;
				reduce_info = lfirst(l);
				reduce_attno = lfirst(lc_reduce_attno);
				subpaths = NIL;
				subpaths_valid = true;
				foreach(lc_rel, childrels)
				{
					childrel = lfirst(lc_rel);
					path = pathkey_path = NULL;
					/* find same reduce */
					foreach(lc_path, generate_partial ? childrel->cluster_partial_pathlist : childrel->cluster_pathlist)
					{
						test_path = lfirst(lc_path);
						if (bms_equal(PATH_REQ_OUTER(test_path), req_outer) == false)
							continue;
						reduce_list = get_reduce_info_list(test_path);
						if (!IsReduceInfoListByValue(reduce_list))
							continue;

						foreach(lc_reduce, reduce_list)
						{
							List *new_attno;
							sub_reduce = lfirst(lc_reduce);
							new_attno = ReduceInfoFindPathTarget(sub_reduce, test_path->pathtarget);
							if (equal(new_attno, reduce_attno) &&
								IsReduceInfoSame(sub_reduce, reduce_info))
							{
								/* found match reduce */
								if (path == NULL ||
									compare_path_costs(path, test_path, TOTAL_COST) > 0)
								{
									path = test_path;
								}

								if (pathkeys &&
									pathkeys_contained_in(pathkeys, test_path->pathkeys) &&
									(pathkey_path == NULL ||
									 compare_path_costs(pathkey_path, test_path, TOTAL_COST) > 0))
								{
									pathkey_path = test_path;
								}
							}
							list_free(new_attno);
						}
					}

					if (pathkeys && pathkey_path)
					{
						path = pathkey_path;
						have_pathkeys_path = true;
					}

					if(path)
					{
						accumulate_append_subpath(path, &subpaths, NULL);
					}else
					{
						subpaths_valid = false;
						break;
					}
				}

				if (pathkeys && have_pathkeys_path == false)
					subpaths_valid = false;

				if(subpaths_valid)
				{
					/* make new reduce for AppendPath */
					List *params;
					if (rel->rtekind == RTE_SUBQUERY)
						params = MakeVarList(reduce_attno, rel->relid, rel->reltarget);
					else
						params = ExtractExprList(reduce_attno, rel->reltarget->exprs);
					Assert(params != NIL);
					parallel_workers = 0;

					sub_reduce = CopyReduceInfoExtend(reduce_info,
													  REDUCE_MARK_ALL & ~(REDUCE_MARK_PARAMS|REDUCE_MARK_EXCLUDE));
					sub_reduce->params = params;

					/* combination exclude node oids */
					if (reduce_info->exclude_exec == NIL)
					{
						/* quick combination */
						sub_reduce->exclude_exec = NIL;
					}else
					{
						List *exec_on = PathListGetReduceInfoListExecuteOn(subpaths);
						sub_reduce->exclude_exec = list_difference_oid(sub_reduce->storage_nodes,
																	   exec_on);
						list_free(exec_on);
					}

					if (generate_partial)
						parallel_workers = get_max_parallel_workers(subpaths);

					if (pathkeys)
					{
						Assert(have_pathkeys_path);
						path = (Path*)create_merge_append_path(root,
															   rel,
															   subpaths,
															   pathkeys,
															   req_outer,
															   partitioned_rels);
						path->parallel_workers = parallel_workers;
					}else
					{
						path = (Path*)create_append_path(root,
														 rel,
														 generate_partial ? NIL:subpaths,
														 generate_partial ? subpaths:NIL,
														 req_outer,
														 parallel_workers,
														 generate_partial ? enable_parallel_append:false,
														 partitioned_rels,
														 -1);
					}
					path->reduce_info_list = list_make1(sub_reduce);
					path->reduce_is_valid = true;
					if (generate_partial)
						add_cluster_partial_path(rel, path);
					else
						add_cluster_path(rel, path);
				}else
				{
					list_free(subpaths);
				}
			} /* foreach all by value reduce info */

			if (pathkeys == NIL && req_outer == NULL && /* first loop */
				(generate_partial ? rel->cluster_partial_pathlist : rel->cluster_pathlist) == NIL)
			{
				/* make a none reduce path */
				List *storage = NIL;
				List *exclude = NIL;
				subpaths = NIL;
				subpaths_valid = true;
				foreach(l, childrels)
				{
					/* find cheapest path */
					childrel = lfirst(l);
					path = NULL;
					foreach(lc_path, generate_partial ? childrel->cluster_partial_pathlist : childrel->cluster_pathlist)
					{
						test_path = lfirst(lc_path);
						if(PATH_REQ_OUTER(test_path))
							continue;
						reduce_list = get_reduce_info_list(test_path);
						if (IsReduceInfoListReplicated(reduce_list))
							continue;

						if (path == NULL ||
							compare_path_costs(path, test_path, TOTAL_COST) > 0)
						{
							path = test_path;
							foreach(lc_reduce, reduce_list)
							{
								reduce_info = lfirst(lc_reduce);
								storage = list_concat_unique_oid(storage, reduce_info->storage_nodes);
								exclude = list_concat_unique_oid(exclude, reduce_info->exclude_exec);
							}
						}
					}
					if(path == NULL)
					{
						subpaths_valid = false;
						break;
					}else
					{
						accumulate_append_subpath(path, &subpaths, NULL);
					}
				}
				if(subpaths_valid)
				{
					List *new_exclude = NIL;
					parallel_workers = 0;
					foreach(l, exclude)
					{
						Oid exclude_node = lfirst_oid(l);
						foreach (lc_path, subpaths)
						{
							if (IsPathExcludeNodeOid(lfirst(lc_path), exclude_node) == false)
								break;
						}
						/* must all paths exclude */
						if (lc_path == NULL)
							new_exclude = lappend_oid(new_exclude, exclude_node);
					}
					reduce_info = MakeRandomReduceInfo(storage);
					reduce_info->exclude_exec = new_exclude;
					if (generate_partial)
						parallel_workers = get_max_parallel_workers(subpaths);
					path = (Path*)create_append_path(root,
													 rel,
													 generate_partial ? NIL:subpaths,
													 generate_partial ? subpaths:NIL,
													 NULL,
													 parallel_workers,
													 generate_partial ? enable_parallel_append:false,
													 partitioned_rels,
													 -1);
					path->reduce_info_list = list_make1(reduce_info);
					path->reduce_is_valid = true;
					if (generate_partial)
						add_cluster_partial_path(rel, path);
					else
						add_cluster_path(rel, path);
				}else
				{
					list_free(subpaths);
				}
				list_free(storage);
				list_free(exclude);
			}

#define MY_MAKE_PATH(reduce_list_test_fun_, new_reduce_info_)							\
	do																					\
	{																					\
		have_pathkeys_path = false;														\
		subpaths_valid = true;															\
		subpaths = NIL;																	\
		foreach(l, childrels)															\
		{																				\
			childrel = lfirst(l);														\
			path = pathkey_path = NULL;													\
			foreach(lc_path, generate_partial ? childrel->cluster_partial_pathlist : childrel->cluster_pathlist)	\
			{																			\
				test_path = lfirst(lc_path);											\
				if (bms_equal(PATH_REQ_OUTER(test_path), req_outer) == false ||			\
					!reduce_list_test_fun_(get_reduce_info_list(test_path)))			\
					continue;															\
																						\
				if (path == NULL ||														\
					compare_path_costs(path, test_path, TOTAL_COST) > 0)				\
				{																		\
					path = test_path;													\
				}																		\
																						\
				if (pathkeys &&															\
					pathkeys_contained_in(pathkeys, test_path->pathkeys) &&				\
					(pathkey_path == NULL ||											\
					 compare_path_costs(pathkey_path, test_path, TOTAL_COST) > 0))		\
				{																		\
					pathkey_path = test_path;											\
				}																		\
			}																			\
																						\
			if (pathkeys && pathkey_path)												\
			{																			\
				path = pathkey_path;													\
				have_pathkeys_path = true;												\
			}																			\
			if (path)																	\
			{																			\
				accumulate_append_subpath(path, &subpaths, NULL);						\
			}else																		\
			{																			\
				subpaths_valid = false;													\
				break;																	\
			}																			\
		}																				\
																						\
		if (pathkeys && have_pathkeys_path == false)									\
			subpaths_valid = false;														\
		if (subpaths_valid)																\
		{																				\
			parallel_workers = 0;														\
			if (generate_partial)														\
				parallel_workers = get_max_parallel_workers(subpaths);					\
			if (pathkeys)																\
			{																			\
				Assert(have_pathkeys_path);												\
				path = (Path*)create_merge_append_path(root,							\
													   rel,								\
													   subpaths,						\
													   pathkeys,						\
													   req_outer,						\
													   partitioned_rels);				\
				path->parallel_workers = parallel_workers;								\
			}else																		\
			{																			\
				path = (Path*)create_append_path(root,									\
												 rel,									\
												 generate_partial ? NIL:subpaths,		\
												 generate_partial ? subpaths:NIL,		\
												 req_outer,								\
												 parallel_workers,						\
												 generate_partial ? enable_parallel_append:false, \
												 partitioned_rels,						\
												 -1);									\
			}																			\
			path->reduce_info_list = list_make1(new_reduce_info_);						\
			path->reduce_is_valid = true;												\
			if (generate_partial)														\
				add_cluster_partial_path(rel, path);									\
			else																		\
				add_cluster_path(rel, path);											\
		}																				\
	}while(0)

			if (have_reduce_coord)
			{
				/* make reduce to coordinator append path */
				MY_MAKE_PATH(IsReduceInfoListCoordinator,
							 MakeCoordinatorReduceInfo());
			}

			if (have_final_replicate)
			{
				MY_MAKE_PATH(IsReduceInfoListFinalReplicated,
							 MakeFinalReplicateReduceInfo());
			}

			if (all_replicate_oid)
			{
				#define MY_TEST_FUN(list_) IsReduceInfoListReplicatedOids((list_), all_replicate_oid, NIL)
				MY_MAKE_PATH(MY_TEST_FUN,
							 MakeReplicateReduceInfo(all_replicate_oid));
				#undef MY_TEST_FUN
			}
#undef MY_MAKE_PATH
		} /* foreach all_child_outers */
	} /* foreach all_child_pathkeys */

	if (rel->consider_parallel &&
		generate_partial == false)
	{
		generate_partial = true;
		goto re_generate_append_;
	}
}
#endif /* ADB */

/*
 * generate_mergeappend_paths
 *		Generate MergeAppend paths for an append relation
 *
 * Generate a path for each ordering (pathkey list) appearing in
 * all_child_pathkeys.
 *
 * We consider both cheapest-startup and cheapest-total cases, ie, for each
 * interesting ordering, collect all the cheapest startup subpaths and all the
 * cheapest total paths, and build a MergeAppend path for each case.
 *
 * We don't currently generate any parameterized MergeAppend paths.  While
 * it would not take much more code here to do so, it's very unclear that it
 * is worth the planning cycles to investigate such paths: there's little
 * use for an ordered path on the inside of a nestloop.  In fact, it's likely
 * that the current coding of add_path would reject such paths out of hand,
 * because add_path gives no credit for sort ordering of parameterized paths,
 * and a parameterized MergeAppend is going to be more expensive than the
 * corresponding parameterized Append path.  If we ever try harder to support
 * parameterized mergejoin plans, it might be worth adding support for
 * parameterized MergeAppends to feed such joins.  (See notes in
 * optimizer/README for why that might not ever happen, though.)
 */
static void
generate_mergeappend_paths(PlannerInfo *root, RelOptInfo *rel,
						   List *live_childrels,
						   List *all_child_pathkeys,
						   List *partitioned_rels)
{
	ListCell   *lcp;

	foreach(lcp, all_child_pathkeys)
	{
		List	   *pathkeys = (List *) lfirst(lcp);
		List	   *startup_subpaths = NIL;
		List	   *total_subpaths = NIL;
		bool		startup_neq_total = false;
		ListCell   *lcr;

		/* Select the child paths for this ordering... */
		foreach(lcr, live_childrels)
		{
			RelOptInfo *childrel = (RelOptInfo *) lfirst(lcr);
			Path	   *cheapest_startup,
					   *cheapest_total;

			/* Locate the right paths, if they are available. */
			cheapest_startup =
				get_cheapest_path_for_pathkeys(childrel->pathlist,
											   pathkeys,
											   NULL,
											   STARTUP_COST,
											   false);
			cheapest_total =
				get_cheapest_path_for_pathkeys(childrel->pathlist,
											   pathkeys,
											   NULL,
											   TOTAL_COST,
											   false);

			/*
			 * If we can't find any paths with the right order just use the
			 * cheapest-total path; we'll have to sort it later.
			 */
			if (cheapest_startup == NULL || cheapest_total == NULL)
			{
				cheapest_startup = cheapest_total =
					childrel->cheapest_total_path;
				/* Assert we do have an unparameterized path for this child */
				Assert(cheapest_total->param_info == NULL);
			}

			/*
			 * Notice whether we actually have different paths for the
			 * "cheapest" and "total" cases; frequently there will be no point
			 * in two create_merge_append_path() calls.
			 */
			if (cheapest_startup != cheapest_total)
				startup_neq_total = true;

			accumulate_append_subpath(cheapest_startup,
									  &startup_subpaths, NULL);
			accumulate_append_subpath(cheapest_total,
									  &total_subpaths, NULL);
		}

		/* ... and build the MergeAppend paths */
		add_path(rel, (Path *) create_merge_append_path(root,
														rel,
														startup_subpaths,
														pathkeys,
														NULL,
														partitioned_rels));
		if (startup_neq_total)
			add_path(rel, (Path *) create_merge_append_path(root,
															rel,
															total_subpaths,
															pathkeys,
															NULL,
															partitioned_rels));
	}
}

/*
 * get_cheapest_parameterized_child_path
 *		Get cheapest path for this relation that has exactly the requested
 *		parameterization.
 *
 * Returns NULL if unable to create such a path.
 */
static Path *
get_cheapest_parameterized_child_path(PlannerInfo *root, RelOptInfo *rel,
									  Relids required_outer)
{
	Path	   *cheapest;
	ListCell   *lc;

	/*
	 * Look up the cheapest existing path with no more than the needed
	 * parameterization.  If it has exactly the needed parameterization, we're
	 * done.
	 */
	cheapest = get_cheapest_path_for_pathkeys(rel->pathlist,
											  NIL,
											  required_outer,
											  TOTAL_COST,
											  false);
	Assert(cheapest != NULL);
	if (bms_equal(PATH_REQ_OUTER(cheapest), required_outer))
		return cheapest;

	/*
	 * Otherwise, we can "reparameterize" an existing path to match the given
	 * parameterization, which effectively means pushing down additional
	 * joinquals to be checked within the path's scan.  However, some existing
	 * paths might check the available joinquals already while others don't;
	 * therefore, it's not clear which existing path will be cheapest after
	 * reparameterization.  We have to go through them all and find out.
	 */
	cheapest = NULL;
	foreach(lc, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		/* Can't use it if it needs more than requested parameterization */
		if (!bms_is_subset(PATH_REQ_OUTER(path), required_outer))
			continue;

		/*
		 * Reparameterization can only increase the path's cost, so if it's
		 * already more expensive than the current cheapest, forget it.
		 */
		if (cheapest != NULL &&
			compare_path_costs(cheapest, path, TOTAL_COST) <= 0)
			continue;

		/* Reparameterize if needed, then recheck cost */
		if (!bms_equal(PATH_REQ_OUTER(path), required_outer))
		{
			path = reparameterize_path(root, path, required_outer, 1.0);
			if (path == NULL)
				continue;		/* failed to reparameterize this one */
			Assert(bms_equal(PATH_REQ_OUTER(path), required_outer));

			if (cheapest != NULL &&
				compare_path_costs(cheapest, path, TOTAL_COST) <= 0)
				continue;
		}

		/* We have a new best path */
		cheapest = path;
	}

	/* Return the best path, or NULL if we found no suitable candidate */
	return cheapest;
}

/*
 * accumulate_append_subpath
 *		Add a subpath to the list being built for an Append or MergeAppend.
 *
 * It's possible that the child is itself an Append or MergeAppend path, in
 * which case we can "cut out the middleman" and just add its child paths to
 * our own list.  (We don't try to do this earlier because we need to apply
 * both levels of transformation to the quals.)
 *
 * Note that if we omit a child MergeAppend in this way, we are effectively
 * omitting a sort step, which seems fine: if the parent is to be an Append,
 * its result would be unsorted anyway, while if the parent is to be a
 * MergeAppend, there's no point in a separate sort on a child.
 * its result would be unsorted anyway.
 *
 * Normally, either path is a partial path and subpaths is a list of partial
 * paths, or else path is a non-partial plan and subpaths is a list of those.
 * However, if path is a parallel-aware Append, then we add its partial path
 * children to subpaths and the rest to special_subpaths.  If the latter is
 * NULL, we don't flatten the path at all (unless it contains only partial
 * paths).
 */
static void
accumulate_append_subpath(Path *path, List **subpaths, List **special_subpaths)
{
	if (IsA(path, AppendPath))
	{
		AppendPath *apath = (AppendPath *) path;

		if (!apath->path.parallel_aware || apath->first_partial_path == 0)
		{
			/* list_copy is important here to avoid sharing list substructure */
			*subpaths = list_concat(*subpaths, list_copy(apath->subpaths));
			return;
		}
		else if (special_subpaths != NULL)
		{
			List	   *new_special_subpaths;

			/* Split Parallel Append into partial and non-partial subpaths */
			*subpaths = list_concat(*subpaths,
									list_copy_tail(apath->subpaths,
												   apath->first_partial_path));
			new_special_subpaths =
				list_truncate(list_copy(apath->subpaths),
							  apath->first_partial_path);
			*special_subpaths = list_concat(*special_subpaths,
											new_special_subpaths);
			return;
		}
	}
	else if (IsA(path, MergeAppendPath))
	{
		MergeAppendPath *mpath = (MergeAppendPath *) path;

		/* list_copy is important here to avoid sharing list substructure */
		*subpaths = list_concat(*subpaths, list_copy(mpath->subpaths));
		return;
	}

	*subpaths = lappend(*subpaths, path);
}

/*
 * set_dummy_rel_pathlist
 *	  Build a dummy path for a relation that's been excluded by constraints
 *
 * Rather than inventing a special "dummy" path type, we represent this as an
 * AppendPath with no members (see also IS_DUMMY_PATH/IS_DUMMY_REL macros).
 *
 * This is exported because inheritance_planner() has need for it.
 */
void
set_dummy_rel_pathlist(RelOptInfo *rel)
{
	/* Set dummy size estimates --- we leave attr_widths[] as zeroes */
	rel->rows = 0;
	rel->reltarget->width = 0;

	/* Discard any pre-existing paths; no further need for them */
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	add_path(rel, (Path *) create_append_path(NULL, rel, NIL, NIL, NULL,
											  0, false, NIL, -1));

	/*
	 * We set the cheapest path immediately, to ensure that IS_DUMMY_REL()
	 * will recognize the relation as dummy if anyone asks.  This is redundant
	 * when we're called from set_rel_size(), but not when called from
	 * elsewhere, and doing it twice is harmless anyway.
	 */
	set_cheapest(rel);
}

/* quick-and-dirty test to see if any joining is needed */
static bool
has_multiple_baserels(PlannerInfo *root)
{
	int			num_base_rels = 0;
	Index		rti;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		if (brel == NULL)
			continue;

		/* ignore RTEs that are "other rels" */
		if (brel->reloptkind == RELOPT_BASEREL)
			if (++num_base_rels > 1)
				return true;
	}
	return false;
}

/*
 * set_subquery_pathlist
 *		Generate SubqueryScan access paths for a subquery RTE
 *
 * We don't currently support generating parameterized paths for subqueries
 * by pushing join clauses down into them; it seems too expensive to re-plan
 * the subquery multiple times to consider different alternatives.
 * (XXX that could stand to be reconsidered, now that we use Paths.)
 * So the paths made here will be parameterized if the subquery contains
 * LATERAL references, otherwise not.  As long as that's true, there's no need
 * for a separate set_subquery_size phase: just make the paths right away.
 */
static void
set_subquery_pathlist(PlannerInfo *root, RelOptInfo *rel,
					  Index rti, RangeTblEntry *rte)
{
	Query	   *parse = root->parse;
	Query	   *subquery = rte->subquery;
	Relids		required_outer;
	pushdown_safety_info safetyInfo;
	double		tuple_fraction;
	RelOptInfo *sub_final_rel;
	ListCell   *lc;
#ifdef ADB
	bool need_limit;
#endif

	/*
	 * Must copy the Query so that planning doesn't mess up the RTE contents
	 * (really really need to fix the planner to not scribble on its input,
	 * someday ... but see remove_unused_subquery_outputs to start with).
	 */
	subquery = copyObject(subquery);

	/*
	 * If it's a LATERAL subquery, it might contain some Vars of the current
	 * query level, requiring it to be treated as parameterized, even though
	 * we don't support pushing down join quals into subqueries.
	 */
	required_outer = rel->lateral_relids;

	/*
	 * Zero out result area for subquery_is_pushdown_safe, so that it can set
	 * flags as needed while recursing.  In particular, we need a workspace
	 * for keeping track of unsafe-to-reference columns.  unsafeColumns[i]
	 * will be set true if we find that output column i of the subquery is
	 * unsafe to use in a pushed-down qual.
	 */
	memset(&safetyInfo, 0, sizeof(safetyInfo));
	safetyInfo.unsafeColumns = (bool *)
		palloc0((list_length(subquery->targetList) + 1) * sizeof(bool));

	/*
	 * If the subquery has the "security_barrier" flag, it means the subquery
	 * originated from a view that must enforce row level security.  Then we
	 * must not push down quals that contain leaky functions.  (Ideally this
	 * would be checked inside subquery_is_pushdown_safe, but since we don't
	 * currently pass the RTE to that function, we must do it here.)
	 */
	safetyInfo.unsafeLeaky = rte->security_barrier;

	/*
	 * If there are any restriction clauses that have been attached to the
	 * subquery relation, consider pushing them down to become WHERE or HAVING
	 * quals of the subquery itself.  This transformation is useful because it
	 * may allow us to generate a better plan for the subquery than evaluating
	 * all the subquery output rows and then filtering them.
	 *
	 * There are several cases where we cannot push down clauses. Restrictions
	 * involving the subquery are checked by subquery_is_pushdown_safe().
	 * Restrictions on individual clauses are checked by
	 * qual_is_pushdown_safe().  Also, we don't want to push down
	 * pseudoconstant clauses; better to have the gating node above the
	 * subquery.
	 *
	 * Non-pushed-down clauses will get evaluated as qpquals of the
	 * SubqueryScan node.
	 *
	 * XXX Are there any cases where we want to make a policy decision not to
	 * push down a pushable qual, because it'd result in a worse plan?
	 */
	if (rel->baserestrictinfo != NIL &&
		subquery_is_pushdown_safe(subquery, subquery, &safetyInfo))
	{
		/* OK to consider pushing down individual quals */
		List	   *upperrestrictlist = NIL;
		ListCell   *l;

		foreach(l, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
			Node	   *clause = (Node *) rinfo->clause;

			if (!rinfo->pseudoconstant &&
				qual_is_pushdown_safe(subquery, rti, clause, &safetyInfo))
			{
				/* Push it down */
				subquery_push_qual(subquery, rte, rti, clause);
			}
			else
			{
				/* Keep it in the upper query */
				upperrestrictlist = lappend(upperrestrictlist, rinfo);
			}
		}
		rel->baserestrictinfo = upperrestrictlist;
		/* We don't bother recomputing baserestrict_min_security */
	}

	pfree(safetyInfo.unsafeColumns);

	/*
	 * The upper query might not use all the subquery's output columns; if
	 * not, we can simplify.
	 */
	remove_unused_subquery_outputs(subquery, rel);

	/*
	 * We can safely pass the outer tuple_fraction down to the subquery if the
	 * outer level has no joining, aggregation, or sorting to do. Otherwise
	 * we'd better tell the subquery to plan for full retrieval. (XXX This
	 * could probably be made more intelligent ...)
	 */
	if (parse->hasAggs ||
		parse->groupClause ||
		parse->groupingSets ||
		parse->havingQual ||
		parse->distinctClause ||
		parse->sortClause ||
		has_multiple_baserels(root))
		tuple_fraction = 0.0;	/* default case */
	else
		tuple_fraction = root->tuple_fraction;

	/* plan_params should not be in use in current query level */
	Assert(root->plan_params == NIL);

	/* Generate a subroot and Paths for the subquery */
	rel->subroot = subquery_planner(root->glob, subquery,
									root,
									false, tuple_fraction);

	/* Isolate the params needed by this specific subplan */
	rel->subplan_params = root->plan_params;
	root->plan_params = NIL;

	/*
	 * It's possible that constraint exclusion proved the subquery empty. If
	 * so, it's desirable to produce an unadorned dummy path so that we will
	 * recognize appropriate optimizations at this query level.
	 */
	sub_final_rel = fetch_upper_rel(rel->subroot, UPPERREL_FINAL, NULL);

	if (IS_DUMMY_REL(sub_final_rel))
	{
		set_dummy_rel_pathlist(rel);
		return;
	}

	/*
	 * Mark rel with estimated output rows, width, etc.  Note that we have to
	 * do this before generating outer-query paths, else cost_subqueryscan is
	 * not happy.
	 */
	set_subquery_size_estimates(root, rel);

	/*
	 * For each Path that subquery_planner produced, make a SubqueryScanPath
	 * in the outer query.
	 */
	foreach(lc, sub_final_rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		List	   *pathkeys;

		/* Convert subpath's pathkeys to outer representation */
		pathkeys = convert_subquery_pathkeys(root,
											 rel,
											 subpath->pathkeys,
											 make_tlist_from_pathtarget(subpath->pathtarget));

		/* Generate outer path using this subpath */
		add_path(rel, (Path *)
				 create_subqueryscan_path(root, rel, subpath,
										  pathkeys, required_outer));
	}

	/* If outer rel allows parallelism, do same for partial paths. */
	if (rel->consider_parallel && bms_is_empty(required_outer))
	{
		/* If consider_parallel is false, there should be no partial paths. */
		Assert(sub_final_rel->consider_parallel ||
			   sub_final_rel->partial_pathlist == NIL);

		/* Same for partial paths. */
		foreach(lc, sub_final_rel->partial_pathlist)
		{
			Path	   *subpath = (Path *) lfirst(lc);
			List	   *pathkeys;

			/* Convert subpath's pathkeys to outer representation */
			pathkeys = convert_subquery_pathkeys(root,
												 rel,
												 subpath->pathkeys,
												 make_tlist_from_pathtarget(subpath->pathtarget));

			/* Generate outer path using this subpath */
			add_partial_path(rel, (Path *)
							 create_subqueryscan_path(root, rel, subpath,
													  pathkeys,
													  required_outer));
		}

#ifdef ADB
		/* If consider_parallel is false, there should be no partial paths. */
		Assert(sub_final_rel->consider_parallel ||
			   sub_final_rel->cluster_partial_pathlist == NIL);

		/* Same for partial paths. */
		foreach(lc, sub_final_rel->cluster_partial_pathlist)
		{
			Path	   *subpath = (Path *) lfirst(lc);
			List	   *pathkeys;

			/* Convert subpath's pathkeys to outer representation */
			pathkeys = convert_subquery_pathkeys(root,
												 rel,
												 subpath->pathkeys,
												 make_tlist_from_pathtarget(subpath->pathtarget));

			/* Generate outer path using this subpath */
			add_cluster_partial_path(rel, (Path *)
									 create_subqueryscan_path(root, rel, subpath,
															  pathkeys,
															  required_outer));
		}
#endif /* ADB */
	}

#ifdef ADB
	need_limit = limit_needed(subquery);
	foreach(lc, sub_final_rel->cluster_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		List	   *pathkeys;

		/* Convert subpath's pathkeys to outer representation */
		pathkeys = convert_subquery_pathkeys(root,
											 rel,
											 subpath->pathkeys,
											 make_tlist_from_pathtarget(subpath->pathtarget));

		if (need_limit &&
			!IsReduceInfoListReplicated(get_reduce_info_list(subpath)) &&
			!IsReduceInfoListInOneNode(get_reduce_info_list(subpath)))
		{
			subpath = create_cluster_reduce_path(root,
												 subpath,
												 list_make1(MakeCoordinatorReduceInfo()),
												 sub_final_rel,
												 subquery->sortClause);
				/* we need limit again */
			subpath = (Path*) create_limit_path(root,
												rel,
												subpath,
												subquery->limitOffset,
												subquery->limitCount,
												0,0);
		}

		/* Generate outer path using this subpath */
		add_cluster_path(rel, (Path *)
						 create_subqueryscan_path(root, rel, subpath,
												  pathkeys, required_outer));
	}
#endif /* ADB */
}

/*
 * set_function_pathlist
 *		Build the (single) access path for a function RTE
 */
static void
set_function_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;
	List	   *pathkeys = NIL;

	/*
	 * We don't support pushing join clauses into the quals of a function
	 * scan, but it could still have required parameterization due to LATERAL
	 * refs in the function expression.
	 */
	required_outer = rel->lateral_relids;

	/*
	 * The result is considered unordered unless ORDINALITY was used, in which
	 * case it is ordered by the ordinal column (the last one).  See if we
	 * care, by checking for uses of that Var in equivalence classes.
	 */
	if (rte->funcordinality)
	{
		AttrNumber	ordattno = rel->max_attr;
		Var		   *var = NULL;
		ListCell   *lc;

		/*
		 * Is there a Var for it in rel's targetlist?  If not, the query did
		 * not reference the ordinality column, or at least not in any way
		 * that would be interesting for sorting.
		 */
		foreach(lc, rel->reltarget->exprs)
		{
			Var		   *node = (Var *) lfirst(lc);

			/* checking varno/varlevelsup is just paranoia */
			if (IsA(node, Var) &&
				node->varattno == ordattno &&
				node->varno == rel->relid &&
				node->varlevelsup == 0)
			{
				var = node;
				break;
			}
		}

		/*
		 * Try to build pathkeys for this Var with int8 sorting.  We tell
		 * build_expression_pathkey not to build any new equivalence class; if
		 * the Var isn't already mentioned in some EC, it means that nothing
		 * cares about the ordering.
		 */
		if (var)
			pathkeys = build_expression_pathkey(root,
												(Expr *) var,
												NULL,	/* below outer joins */
												Int8LessOperator,
												rel->relids,
												false);
	}

	/* Generate appropriate path */
	add_path(rel, create_functionscan_path(root, rel,
										   pathkeys, required_outer));
}

/*
 * set_values_pathlist
 *		Build the (single) access path for a VALUES RTE
 */
static void
set_values_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;

	/*
	 * We don't support pushing join clauses into the quals of a values scan,
	 * but it could still have required parameterization due to LATERAL refs
	 * in the values expressions.
	 */
	required_outer = rel->lateral_relids;

	/* Generate appropriate path */
	add_path(rel, create_valuesscan_path(root, rel, required_outer));
}

/*
 * set_tablefunc_pathlist
 *		Build the (single) access path for a table func RTE
 */
static void
set_tablefunc_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Relids		required_outer;

	/*
	 * We don't support pushing join clauses into the quals of a tablefunc
	 * scan, but it could still have required parameterization due to LATERAL
	 * refs in the function expression.
	 */
	required_outer = rel->lateral_relids;

	/* Generate appropriate path */
	add_path(rel, create_tablefuncscan_path(root, rel,
											required_outer));
}

/*
 * set_cte_pathlist
 *		Build the (single) access path for a non-self-reference CTE RTE
 *
 * There's no need for a separate set_cte_size phase, since we don't
 * support join-qual-parameterized paths for CTEs.
 */
static void
set_cte_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Plan	   *cteplan;
	PlannerInfo *cteroot;
	Index		levelsup;
	int			ndx;
	ListCell   *lc;
	int			plan_id;
	Relids		required_outer;

	/*
	 * Find the referenced CTE, and locate the plan previously made for it.
	 */
	levelsup = rte->ctelevelsup;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}

	/*
	 * Note: cte_plan_ids can be shorter than cteList, if we are still working
	 * on planning the CTEs (ie, this is a side-reference from another CTE).
	 * So we mustn't use forboth here.
	 */
	ndx = 0;
	foreach(lc, cteroot->parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			break;
		ndx++;
	}
	if (lc == NULL)				/* shouldn't happen */
		elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
	if (ndx >= list_length(cteroot->cte_plan_ids))
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);
	plan_id = list_nth_int(cteroot->cte_plan_ids, ndx);
	Assert(plan_id > 0);
	cteplan = (Plan *) list_nth(root->glob->subplans, plan_id - 1);

	/* Mark rel with estimated output rows, width, etc */
	set_cte_size_estimates(root, rel, cteplan->plan_rows);

	/*
	 * We don't support pushing join clauses into the quals of a CTE scan, but
	 * it could still have required parameterization due to LATERAL refs in
	 * its tlist.
	 */
	required_outer = rel->lateral_relids;

	/* Generate appropriate path */
	add_path(rel, create_ctescan_path(root, rel, required_outer));
#ifdef ADB
	if(root->glob->clusterPlanOK && bms_is_empty(required_outer))
	{
		PlannerInfo *subroot = list_nth(root->glob->subroots, plan_id - 1);
		RelOptInfo *final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
		if(final_rel->cheapest_cluster_total_path)
		{
			Path *cheapest_path = final_rel->cheapest_cluster_total_path;
			Path *cluster_path;
			Path *normal_path = linitial(rel->pathlist);
			List *reduce_info_list = get_reduce_info_list(cheapest_path);
			List *exec_param_clauses = NIL;
			RelOptInfo *current_rel;
			ListCell *lc;

			foreach(lc, rel->baserestrictinfo)
			{
				RestrictInfo *ri = lfirst_node(RestrictInfo, lc);
				if (expr_have_upper_reference(ri->clause, subroot))
					exec_param_clauses = lappend(exec_param_clauses, ri);
			}

			if (exec_param_clauses)
			{
				current_rel = make_no_execparam_base_rel(root, rel, exec_param_clauses);
			}else
			{
				current_rel = rel;
			}

			cluster_path = create_ctescan_path(root, current_rel, NULL);
			cluster_path->reduce_info_list = ConvertReduceInfoList(reduce_info_list,
																   subroot->upper_targets[UPPERREL_FINAL],
																   rel->relid);
			cluster_path->reduce_is_valid = true;

			if(exec_param_clauses)
			{
				cluster_path = (Path*)create_reducescan_path(root,
															 rel,
															 rel->reltarget,
															 cluster_path,
															 list_make1(MakeFinalReplicateReduceInfo()),
															 NIL,
															 exec_param_clauses);
			}

			/* we need have diffent cost for cluster path and not cluster path */
			normal_path->startup_cost = cteplan->startup_cost;
			normal_path->total_cost = cteplan->total_cost;
			cluster_path->startup_cost = cheapest_path->startup_cost;
			cluster_path->total_cost = cheapest_path->total_cost;
			cluster_path->rows = cheapest_path->rows;

			add_cluster_path(rel, cluster_path);
		}
	}
#endif /* ADB */
}

#ifdef ADB
static void set_paramtuplestore_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte)
{
	Relids required_outer;
	Path *path;
	rel->tuples = rte->rows;
	set_baserel_size_estimates(root, rel);

	/*
	 * We don't support pushing join clauses into the quals of a tuplestore
	 * scan, but it could still have required parameterization due to LATERAL
	 * refs in its tlist.
	 */
	required_outer = rel->lateral_relids;

	/* Generate path */
	path = create_paramtuplestorescan_path(root, rel, required_outer);
	path->reduce_info_list = list_make1(MakeCoordinatorReduceInfo());
	path->reduce_is_valid = true;
	add_path(rel, path);

	/* generate cluster path */
	if (root->glob->clusterPlanOK)
	{
		path = create_paramtuplestorescan_path(root, rel, required_outer);
		path->reduce_info_list = list_make1(MakeRandomReduceInfo(rte->execNodes));
		path->reduce_is_valid = true;
		add_cluster_path(rel, path);
	}

	/* Select cheapest path (pretty easy in this case...) */
	set_cheapest(rel);

}

#endif /* ADB */

/*
 * set_namedtuplestore_pathlist
 *		Build the (single) access path for a named tuplestore RTE
 *
 * There's no need for a separate set_namedtuplestore_size phase, since we
 * don't support join-qual-parameterized paths for tuplestores.
 */
static void
set_namedtuplestore_pathlist(PlannerInfo *root, RelOptInfo *rel,
							 RangeTblEntry *rte)
{
	Relids		required_outer;

	/* Mark rel with estimated output rows, width, etc */
	set_namedtuplestore_size_estimates(root, rel);

	/*
	 * We don't support pushing join clauses into the quals of a tuplestore
	 * scan, but it could still have required parameterization due to LATERAL
	 * refs in its tlist.
	 */
	required_outer = rel->lateral_relids;

	/* Generate appropriate path */
	add_path(rel, create_namedtuplestorescan_path(root, rel, required_outer));

	/* Select cheapest path (pretty easy in this case...) */
	set_cheapest(rel);
}

/*
 * set_worktable_pathlist
 *		Build the (single) access path for a self-reference CTE RTE
 *
 * There's no need for a separate set_worktable_size phase, since we don't
 * support join-qual-parameterized paths for CTEs.
 */
static void
set_worktable_pathlist(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	Path	   *ctepath;
	PlannerInfo *cteroot;
	Index		levelsup;
	Relids		required_outer;

	/*
	 * We need to find the non-recursive term's path, which is in the plan
	 * level that's processing the recursive UNION, which is one level *below*
	 * where the CTE comes from.
	 */
	levelsup = rte->ctelevelsup;
	if (levelsup == 0)			/* shouldn't happen */
		elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	levelsup--;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	ctepath = cteroot->non_recursive_path;
	if (!ctepath)				/* shouldn't happen */
		elog(ERROR, "could not find path for CTE \"%s\"", rte->ctename);

	/* Mark rel with estimated output rows, width, etc */
	set_cte_size_estimates(root, rel, ctepath->rows);

	/*
	 * We don't support pushing join clauses into the quals of a worktable
	 * scan, but it could still have required parameterization due to LATERAL
	 * refs in its tlist.  (I'm not sure this is actually possible given the
	 * restrictions on recursive references, but it's easy enough to support.)
	 */
	required_outer = rel->lateral_relids;

	/* Generate appropriate path */
	add_path(rel, create_worktablescan_path(root, rel, required_outer));
}

/*
 * generate_gather_paths
 *		Generate parallel access paths for a relation by pushing a Gather or
 *		Gather Merge on top of a partial path.
 *
 * This must not be called until after we're done creating all partial paths
 * for the specified relation.  (Otherwise, add_partial_path might delete a
 * path that some GatherPath or GatherMergePath has a reference to.)
 *
 * If we're generating paths for a scan or join relation, override_rows will
 * be false, and we'll just use the relation's size estimate.  When we're
 * being called for a partially-grouped path, though, we need to override
 * the rowcount estimate.  (It's not clear that the particular value we're
 * using here is actually best, but the underlying rel has no estimate so
 * we must do something.)
 */
void
generate_gather_paths(PlannerInfo *root, RelOptInfo *rel, bool override_rows)
{
	Path	   *cheapest_partial_path;
	Path	   *simple_gather_path;
	ListCell   *lc;
	double		rows;
	double	   *rowsp = NULL;

	/* If there are no partial paths, there's nothing to do here. */
	if (rel->partial_pathlist == NIL)
#ifdef ADB
		goto generate_cluster_gather_;
#else
		return;
#endif /* ADB */

	/* Should we override the rel's rowcount estimate? */
	if (override_rows)
		rowsp = &rows;

	/*
	 * The output of Gather is always unsorted, so there's only one partial
	 * path of interest: the cheapest one.  That will be the one at the front
	 * of partial_pathlist because of the way add_partial_path works.
	 */
	cheapest_partial_path = linitial(rel->partial_pathlist);
	rows =
		cheapest_partial_path->rows * cheapest_partial_path->parallel_workers;
	simple_gather_path = (Path *)
		create_gather_path(root, rel, cheapest_partial_path, rel->reltarget,
						   NULL, rowsp);
	add_path(rel, simple_gather_path);

	/*
	 * For each useful ordering, we can consider an order-preserving Gather
	 * Merge.
	 */
	foreach(lc, rel->partial_pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		GatherMergePath *path;

		if (subpath->pathkeys == NIL)
			continue;

		rows = subpath->rows * subpath->parallel_workers;
		path = create_gather_merge_path(root, rel, subpath, rel->reltarget,
										subpath->pathkeys, NULL, rowsp);
		add_path(rel, &path->path);
	}
#ifdef ADB
generate_cluster_gather_:
	foreach(lc, rel->cluster_partial_pathlist)
	{
		Path   *subpath = lfirst(lc);
		double rows = subpath->rows * subpath->parallel_workers;
		Path   *path = (Path*)create_gather_path(root,
												 rel,
												 subpath,
												 rel->reltarget,
												 NULL, &rows);
		path->reduce_info_list = CopyReduceInfoList(get_reduce_info_list(subpath));
		path->reduce_is_valid = true;
		add_cluster_path(rel, path);

		if(subpath->pathkeys)
		{
			path = (Path*)create_gather_merge_path(root, rel, subpath, rel->reltarget,
												   subpath->pathkeys, NULL, &rows);
			path->reduce_info_list = CopyReduceInfoList(get_reduce_info_list(subpath));
			path->reduce_is_valid = true;
			add_cluster_path(rel, path);
		}
	}
#endif /* ADB */
}

/*
 * make_rel_from_joinlist
 *	  Build access paths using a "joinlist" to guide the join path search.
 *
 * See comments for deconstruct_jointree() for definition of the joinlist
 * data structure.
 */
static RelOptInfo *
make_rel_from_joinlist(PlannerInfo *root, List *joinlist)
{
	int			levels_needed;
	List	   *initial_rels;
	ListCell   *jl;

	/*
	 * Count the number of child joinlist nodes.  This is the depth of the
	 * dynamic-programming algorithm we must employ to consider all ways of
	 * joining the child nodes.
	 */
	levels_needed = list_length(joinlist);

	if (levels_needed <= 0)
		return NULL;			/* nothing to do? */

	/*
	 * Construct a list of rels corresponding to the child joinlist nodes.
	 * This may contain both base rels and rels constructed according to
	 * sub-joinlists.
	 */
	initial_rels = NIL;
	foreach(jl, joinlist)
	{
		Node	   *jlnode = (Node *) lfirst(jl);
		RelOptInfo *thisrel;

		if (IsA(jlnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jlnode)->rtindex;

			thisrel = find_base_rel(root, varno);
		}
		else if (IsA(jlnode, List))
		{
			/* Recurse to handle subproblem */
			thisrel = make_rel_from_joinlist(root, (List *) jlnode);
		}
		else
		{
			elog(ERROR, "unrecognized joinlist node type: %d",
				 (int) nodeTag(jlnode));
			thisrel = NULL;		/* keep compiler quiet */
		}

		initial_rels = lappend(initial_rels, thisrel);
	}

	if (levels_needed == 1)
	{
		/*
		 * Single joinlist node, so we're done.
		 */
		return (RelOptInfo *) linitial(initial_rels);
	}
	else
	{
		/*
		 * Consider the different orders in which we could join the rels,
		 * using a plugin, GEQO, or the regular join search code.
		 *
		 * We put the initial_rels list into a PlannerInfo field because
		 * has_legal_joinclause() needs to look at it (ugly :-().
		 */
		root->initial_rels = initial_rels;

		if (join_search_hook)
			return (*join_search_hook) (root, levels_needed, initial_rels);
		else if (enable_geqo && levels_needed >= geqo_threshold)
			return geqo(root, levels_needed, initial_rels);
		else
			return standard_join_search(root, levels_needed, initial_rels);
	}
}

/*
 * standard_join_search
 *	  Find possible joinpaths for a query by successively finding ways
 *	  to join component relations into join relations.
 *
 * 'levels_needed' is the number of iterations needed, ie, the number of
 *		independent jointree items in the query.  This is > 1.
 *
 * 'initial_rels' is a list of RelOptInfo nodes for each independent
 *		jointree item.  These are the components to be joined together.
 *		Note that levels_needed == list_length(initial_rels).
 *
 * Returns the final level of join relations, i.e., the relation that is
 * the result of joining all the original relations together.
 * At least one implementation path must be provided for this relation and
 * all required sub-relations.
 *
 * To support loadable plugins that modify planner behavior by changing the
 * join searching algorithm, we provide a hook variable that lets a plugin
 * replace or supplement this function.  Any such hook must return the same
 * final join relation as the standard code would, but it might have a
 * different set of implementation paths attached, and only the sub-joinrels
 * needed for these paths need have been instantiated.
 *
 * Note to plugin authors: the functions invoked during standard_join_search()
 * modify root->join_rel_list and root->join_rel_hash.  If you want to do more
 * than one join-order search, you'll probably need to save and restore the
 * original states of those data structures.  See geqo_eval() for an example.
 */
RelOptInfo *
standard_join_search(PlannerInfo *root, int levels_needed, List *initial_rels)
{
	int			lev;
	RelOptInfo *rel;

	/*
	 * This function cannot be invoked recursively within any one planning
	 * problem, so join_rel_level[] can't be in use already.
	 */
	Assert(root->join_rel_level == NULL);

	/*
	 * We employ a simple "dynamic programming" algorithm: we first find all
	 * ways to build joins of two jointree items, then all ways to build joins
	 * of three items (from two-item joins and single items), then four-item
	 * joins, and so on until we have considered all ways to join all the
	 * items into one rel.
	 *
	 * root->join_rel_level[j] is a list of all the j-item rels.  Initially we
	 * set root->join_rel_level[1] to represent all the single-jointree-item
	 * relations.
	 */
	root->join_rel_level = (List **) palloc0((levels_needed + 1) * sizeof(List *));

	root->join_rel_level[1] = initial_rels;

	for (lev = 2; lev <= levels_needed; lev++)
	{
		ListCell   *lc;

		/*
		 * Determine all possible pairs of relations to be joined at this
		 * level, and build paths for making each one from every available
		 * pair of lower-level relations.
		 */
		join_search_one_level(root, lev);

		/*
		 * Run generate_partitionwise_join_paths() and generate_gather_paths()
		 * for each just-processed joinrel.  We could not do this earlier
		 * because both regular and partial paths can get added to a
		 * particular joinrel at multiple times within join_search_one_level.
		 *
		 * After that, we're done creating paths for the joinrel, so run
		 * set_cheapest().
		 */
		foreach(lc, root->join_rel_level[lev])
		{
			rel = (RelOptInfo *) lfirst(lc);

			/* Create paths for partitionwise joins. */
			generate_partitionwise_join_paths(root, rel);

			/*
			 * Except for the topmost scan/join rel, consider gathering
			 * partial paths.  We'll do the same for the topmost scan/join rel
			 * once we know the final targetlist (see grouping_planner).
			 */
			if (lev < levels_needed)
				generate_gather_paths(root, rel, false);

			/* Find and save the cheapest paths for this rel */
			set_cheapest(rel);

#ifdef OPTIMIZER_DEBUG
			debug_print_rel(root, rel);
#endif
		}
	}

	/*
	 * We should have a single rel at the final level.
	 */
	if (root->join_rel_level[levels_needed] == NIL)
		elog(ERROR, "failed to build any %d-way joins", levels_needed);
	Assert(list_length(root->join_rel_level[levels_needed]) == 1);

	rel = (RelOptInfo *) linitial(root->join_rel_level[levels_needed]);

	root->join_rel_level = NULL;

	return rel;
}

/*****************************************************************************
 *			PUSHING QUALS DOWN INTO SUBQUERIES
 *****************************************************************************/

/*
 * subquery_is_pushdown_safe - is a subquery safe for pushing down quals?
 *
 * subquery is the particular component query being checked.  topquery
 * is the top component of a set-operations tree (the same Query if no
 * set-op is involved).
 *
 * Conditions checked here:
 *
 * 1. If the subquery has a LIMIT clause, we must not push down any quals,
 * since that could change the set of rows returned.
 *
 * 2. If the subquery contains EXCEPT or EXCEPT ALL set ops we cannot push
 * quals into it, because that could change the results.
 *
 * 3. If the subquery uses DISTINCT, we cannot push volatile quals into it.
 * This is because upper-level quals should semantically be evaluated only
 * once per distinct row, not once per original row, and if the qual is
 * volatile then extra evaluations could change the results.  (This issue
 * does not apply to other forms of aggregation such as GROUP BY, because
 * when those are present we push into HAVING not WHERE, so that the quals
 * are still applied after aggregation.)
 *
 * 4. If the subquery contains window functions, we cannot push volatile quals
 * into it.  The issue here is a bit different from DISTINCT: a volatile qual
 * might succeed for some rows of a window partition and fail for others,
 * thereby changing the partition contents and thus the window functions'
 * results for rows that remain.
 *
 * 5. If the subquery contains any set-returning functions in its targetlist,
 * we cannot push volatile quals into it.  That would push them below the SRFs
 * and thereby change the number of times they are evaluated.  Also, a
 * volatile qual could succeed for some SRF output rows and fail for others,
 * a behavior that cannot occur if it's evaluated before SRF expansion.
 *
 * In addition, we make several checks on the subquery's output columns to see
 * if it is safe to reference them in pushed-down quals.  If output column k
 * is found to be unsafe to reference, we set safetyInfo->unsafeColumns[k]
 * to true, but we don't reject the subquery overall since column k might not
 * be referenced by some/all quals.  The unsafeColumns[] array will be
 * consulted later by qual_is_pushdown_safe().  It's better to do it this way
 * than to make the checks directly in qual_is_pushdown_safe(), because when
 * the subquery involves set operations we have to check the output
 * expressions in each arm of the set op.
 *
 * Note: pushing quals into a DISTINCT subquery is theoretically dubious:
 * we're effectively assuming that the quals cannot distinguish values that
 * the DISTINCT's equality operator sees as equal, yet there are many
 * counterexamples to that assumption.  However use of such a qual with a
 * DISTINCT subquery would be unsafe anyway, since there's no guarantee which
 * "equal" value will be chosen as the output value by the DISTINCT operation.
 * So we don't worry too much about that.  Another objection is that if the
 * qual is expensive to evaluate, running it for each original row might cost
 * more than we save by eliminating rows before the DISTINCT step.  But it
 * would be very hard to estimate that at this stage, and in practice pushdown
 * seldom seems to make things worse, so we ignore that problem too.
 *
 * Note: likewise, pushing quals into a subquery with window functions is a
 * bit dubious: the quals might remove some rows of a window partition while
 * leaving others, causing changes in the window functions' results for the
 * surviving rows.  We insist that such a qual reference only partitioning
 * columns, but again that only protects us if the qual does not distinguish
 * values that the partitioning equality operator sees as equal.  The risks
 * here are perhaps larger than for DISTINCT, since no de-duplication of rows
 * occurs and thus there is no theoretical problem with such a qual.  But
 * we'll do this anyway because the potential performance benefits are very
 * large, and we've seen no field complaints about the longstanding comparable
 * behavior with DISTINCT.
 */
static bool
subquery_is_pushdown_safe(Query *subquery, Query *topquery,
						  pushdown_safety_info *safetyInfo)
{
	SetOperationStmt *topop;

	/* Check point 1 */
	if (subquery->limitOffset != NULL || subquery->limitCount != NULL)
		return false;

	/* Check points 3, 4, and 5 */
	if (subquery->distinctClause ||
		subquery->hasWindowFuncs ||
		subquery->hasTargetSRFs)
		safetyInfo->unsafeVolatile = true;

	/*
	 * If we're at a leaf query, check for unsafe expressions in its target
	 * list, and mark any unsafe ones in unsafeColumns[].  (Non-leaf nodes in
	 * setop trees have only simple Vars in their tlists, so no need to check
	 * them.)
	 */
	if (subquery->setOperations == NULL)
		check_output_expressions(subquery, safetyInfo);

	/* Are we at top level, or looking at a setop component? */
	if (subquery == topquery)
	{
		/* Top level, so check any component queries */
		if (subquery->setOperations != NULL)
			if (!recurse_pushdown_safe(subquery->setOperations, topquery,
									   safetyInfo))
				return false;
	}
	else
	{
		/* Setop component must not have more components (too weird) */
		if (subquery->setOperations != NULL)
			return false;
		/* Check whether setop component output types match top level */
		topop = castNode(SetOperationStmt, topquery->setOperations);
		Assert(topop);
		compare_tlist_datatypes(subquery->targetList,
								topop->colTypes,
								safetyInfo);
	}
	return true;
}

/*
 * Helper routine to recurse through setOperations tree
 */
static bool
recurse_pushdown_safe(Node *setOp, Query *topquery,
					  pushdown_safety_info *safetyInfo)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, topquery->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);
		return subquery_is_pushdown_safe(subquery, topquery, safetyInfo);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* EXCEPT is no good (point 2 for subquery_is_pushdown_safe) */
		if (op->op == SETOP_EXCEPT)
			return false;
		/* Else recurse */
		if (!recurse_pushdown_safe(op->larg, topquery, safetyInfo))
			return false;
		if (!recurse_pushdown_safe(op->rarg, topquery, safetyInfo))
			return false;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
	return true;
}

/*
 * check_output_expressions - check subquery's output expressions for safety
 *
 * There are several cases in which it's unsafe to push down an upper-level
 * qual if it references a particular output column of a subquery.  We check
 * each output column of the subquery and set unsafeColumns[k] to true if
 * that column is unsafe for a pushed-down qual to reference.  The conditions
 * checked here are:
 *
 * 1. We must not push down any quals that refer to subselect outputs that
 * return sets, else we'd introduce functions-returning-sets into the
 * subquery's WHERE/HAVING quals.
 *
 * 2. We must not push down any quals that refer to subselect outputs that
 * contain volatile functions, for fear of introducing strange results due
 * to multiple evaluation of a volatile function.
 *
 * 3. If the subquery uses DISTINCT ON, we must not push down any quals that
 * refer to non-DISTINCT output columns, because that could change the set
 * of rows returned.  (This condition is vacuous for DISTINCT, because then
 * there are no non-DISTINCT output columns, so we needn't check.  Note that
 * subquery_is_pushdown_safe already reported that we can't use volatile
 * quals if there's DISTINCT or DISTINCT ON.)
 *
 * 4. If the subquery has any window functions, we must not push down quals
 * that reference any output columns that are not listed in all the subquery's
 * window PARTITION BY clauses.  We can push down quals that use only
 * partitioning columns because they should succeed or fail identically for
 * every row of any one window partition, and totally excluding some
 * partitions will not change a window function's results for remaining
 * partitions.  (Again, this also requires nonvolatile quals, but
 * subquery_is_pushdown_safe handles that.)
 */
static void
check_output_expressions(Query *subquery, pushdown_safety_info *safetyInfo)
{
	ListCell   *lc;

	foreach(lc, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->resjunk)
			continue;			/* ignore resjunk columns */

		/* We need not check further if output col is already known unsafe */
		if (safetyInfo->unsafeColumns[tle->resno])
			continue;

		/* Functions returning sets are unsafe (point 1) */
		if (subquery->hasTargetSRFs &&
			expression_returns_set((Node *) tle->expr))
		{
			safetyInfo->unsafeColumns[tle->resno] = true;
			continue;
		}

		/* Volatile functions are unsafe (point 2) */
		if (contain_volatile_functions((Node *) tle->expr))
		{
			safetyInfo->unsafeColumns[tle->resno] = true;
			continue;
		}

		/* If subquery uses DISTINCT ON, check point 3 */
		if (subquery->hasDistinctOn &&
			!targetIsInSortList(tle, InvalidOid, subquery->distinctClause))
		{
			/* non-DISTINCT column, so mark it unsafe */
			safetyInfo->unsafeColumns[tle->resno] = true;
			continue;
		}

		/* If subquery uses window functions, check point 4 */
		if (subquery->hasWindowFuncs &&
			!targetIsInAllPartitionLists(tle, subquery))
		{
			/* not present in all PARTITION BY clauses, so mark it unsafe */
			safetyInfo->unsafeColumns[tle->resno] = true;
			continue;
		}
	}
}

/*
 * For subqueries using UNION/UNION ALL/INTERSECT/INTERSECT ALL, we can
 * push quals into each component query, but the quals can only reference
 * subquery columns that suffer no type coercions in the set operation.
 * Otherwise there are possible semantic gotchas.  So, we check the
 * component queries to see if any of them have output types different from
 * the top-level setop outputs.  unsafeColumns[k] is set true if column k
 * has different type in any component.
 *
 * We don't have to care about typmods here: the only allowed difference
 * between set-op input and output typmods is input is a specific typmod
 * and output is -1, and that does not require a coercion.
 *
 * tlist is a subquery tlist.
 * colTypes is an OID list of the top-level setop's output column types.
 * safetyInfo->unsafeColumns[] is the result array.
 */
static void
compare_tlist_datatypes(List *tlist, List *colTypes,
						pushdown_safety_info *safetyInfo)
{
	ListCell   *l;
	ListCell   *colType = list_head(colTypes);

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
			continue;			/* ignore resjunk columns */
		if (colType == NULL)
			elog(ERROR, "wrong number of tlist entries");
		if (exprType((Node *) tle->expr) != lfirst_oid(colType))
			safetyInfo->unsafeColumns[tle->resno] = true;
		colType = lnext(colType);
	}
	if (colType != NULL)
		elog(ERROR, "wrong number of tlist entries");
}

/*
 * targetIsInAllPartitionLists
 *		True if the TargetEntry is listed in the PARTITION BY clause
 *		of every window defined in the query.
 *
 * It would be safe to ignore windows not actually used by any window
 * function, but it's not easy to get that info at this stage; and it's
 * unlikely to be useful to spend any extra cycles getting it, since
 * unreferenced window definitions are probably infrequent in practice.
 */
static bool
targetIsInAllPartitionLists(TargetEntry *tle, Query *query)
{
	ListCell   *lc;

	foreach(lc, query->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(lc);

		if (!targetIsInSortList(tle, InvalidOid, wc->partitionClause))
			return false;
	}
	return true;
}

/*
 * qual_is_pushdown_safe - is a particular qual safe to push down?
 *
 * qual is a restriction clause applying to the given subquery (whose RTE
 * has index rti in the parent query).
 *
 * Conditions checked here:
 *
 * 1. The qual must not contain any SubPlans (mainly because I'm not sure
 * it will work correctly: SubLinks will already have been transformed into
 * SubPlans in the qual, but not in the subquery).  Note that SubLinks that
 * transform to initplans are safe, and will be accepted here because what
 * we'll see in the qual is just a Param referencing the initplan output.
 *
 * 2. If unsafeVolatile is set, the qual must not contain any volatile
 * functions.
 *
 * 3. If unsafeLeaky is set, the qual must not contain any leaky functions
 * that are passed Var nodes, and therefore might reveal values from the
 * subquery as side effects.
 *
 * 4. The qual must not refer to the whole-row output of the subquery
 * (since there is no easy way to name that within the subquery itself).
 *
 * 5. The qual must not refer to any subquery output columns that were
 * found to be unsafe to reference by subquery_is_pushdown_safe().
 */
static bool
qual_is_pushdown_safe(Query *subquery, Index rti, Node *qual,
					  pushdown_safety_info *safetyInfo)
{
	bool		safe = true;
	List	   *vars;
	ListCell   *vl;

	/* Refuse subselects (point 1) */
	if (contain_subplans(qual))
		return false;

	/* Refuse volatile quals if we found they'd be unsafe (point 2) */
	if (safetyInfo->unsafeVolatile &&
		contain_volatile_functions(qual))
		return false;

	/* Refuse leaky quals if told to (point 3) */
	if (safetyInfo->unsafeLeaky &&
		contain_leaked_vars(qual))
		return false;

	/*
	 * It would be unsafe to push down window function calls, but at least for
	 * the moment we could never see any in a qual anyhow.  (The same applies
	 * to aggregates, which we check for in pull_var_clause below.)
	 */
	Assert(!contain_window_function(qual));

	/*
	 * Examine all Vars used in clause; since it's a restriction clause, all
	 * such Vars must refer to subselect output columns.
	 */
	vars = pull_var_clause(qual, PVC_INCLUDE_PLACEHOLDERS);
	foreach(vl, vars)
	{
		Var		   *var = (Var *) lfirst(vl);

		/*
		 * XXX Punt if we find any PlaceHolderVars in the restriction clause.
		 * It's not clear whether a PHV could safely be pushed down, and even
		 * less clear whether such a situation could arise in any cases of
		 * practical interest anyway.  So for the moment, just refuse to push
		 * down.
		 */
		if (!IsA(var, Var))
		{
			safe = false;
			break;
		}

		Assert(var->varno == rti);
		Assert(var->varattno >= 0);

		/* Check point 4 */
		if (var->varattno == 0)
		{
			safe = false;
			break;
		}

		/* Check point 5 */
		if (safetyInfo->unsafeColumns[var->varattno])
		{
			safe = false;
			break;
		}
	}

	list_free(vars);

	return safe;
}

/*
 * subquery_push_qual - push down a qual that we have determined is safe
 */
static void
subquery_push_qual(Query *subquery, RangeTblEntry *rte, Index rti, Node *qual)
{
	if (subquery->setOperations != NULL)
	{
		/* Recurse to push it separately to each component query */
		recurse_push_qual(subquery->setOperations, subquery,
						  rte, rti, qual);
	}
	else
	{
		/*
		 * We need to replace Vars in the qual (which must refer to outputs of
		 * the subquery) with copies of the subquery's targetlist expressions.
		 * Note that at this point, any uplevel Vars in the qual should have
		 * been replaced with Params, so they need no work.
		 *
		 * This step also ensures that when we are pushing into a setop tree,
		 * each component query gets its own copy of the qual.
		 */
		qual = ReplaceVarsFromTargetList(qual, rti, 0, rte,
										 subquery->targetList,
										 REPLACEVARS_REPORT_ERROR, 0,
										 &subquery->hasSubLinks);

		/*
		 * Now attach the qual to the proper place: normally WHERE, but if the
		 * subquery uses grouping or aggregation, put it in HAVING (since the
		 * qual really refers to the group-result rows).
		 */
		if (subquery->hasAggs || subquery->groupClause || subquery->groupingSets || subquery->havingQual)
			subquery->havingQual = make_and_qual(subquery->havingQual, qual);
		else
			subquery->jointree->quals =
				make_and_qual(subquery->jointree->quals, qual);

		/*
		 * We need not change the subquery's hasAggs or hasSubLinks flags,
		 * since we can't be pushing down any aggregates that weren't there
		 * before, and we don't push down subselects at all.
		 */
	}
}

/*
 * Helper routine to recurse through setOperations tree
 */
static void
recurse_push_qual(Node *setOp, Query *topquery,
				  RangeTblEntry *rte, Index rti, Node *qual)
{
	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *subrte = rt_fetch(rtr->rtindex, topquery->rtable);
		Query	   *subquery = subrte->subquery;

		Assert(subquery != NULL);
		subquery_push_qual(subquery, rte, rti, qual);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		recurse_push_qual(op->larg, topquery, rte, rti, qual);
		recurse_push_qual(op->rarg, topquery, rte, rti, qual);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*****************************************************************************
 *			SIMPLIFYING SUBQUERY TARGETLISTS
 *****************************************************************************/

/*
 * remove_unused_subquery_outputs
 *		Remove subquery targetlist items we don't need
 *
 * It's possible, even likely, that the upper query does not read all the
 * output columns of the subquery.  We can remove any such outputs that are
 * not needed by the subquery itself (e.g., as sort/group columns) and do not
 * affect semantics otherwise (e.g., volatile functions can't be removed).
 * This is useful not only because we might be able to remove expensive-to-
 * compute expressions, but because deletion of output columns might allow
 * optimizations such as join removal to occur within the subquery.
 *
 * To avoid affecting column numbering in the targetlist, we don't physically
 * remove unused tlist entries, but rather replace their expressions with NULL
 * constants.  This is implemented by modifying subquery->targetList.
 */
static void
remove_unused_subquery_outputs(Query *subquery, RelOptInfo *rel)
{
	Bitmapset  *attrs_used = NULL;
	ListCell   *lc;

	/*
	 * Do nothing if subquery has UNION/INTERSECT/EXCEPT: in principle we
	 * could update all the child SELECTs' tlists, but it seems not worth the
	 * trouble presently.
	 */
	if (subquery->setOperations)
		return;

	/*
	 * If subquery has regular DISTINCT (not DISTINCT ON), we're wasting our
	 * time: all its output columns must be used in the distinctClause.
	 */
	if (subquery->distinctClause && !subquery->hasDistinctOn)
		return;

	/*
	 * Collect a bitmap of all the output column numbers used by the upper
	 * query.
	 *
	 * Add all the attributes needed for joins or final output.  Note: we must
	 * look at rel's targetlist, not the attr_needed data, because attr_needed
	 * isn't computed for inheritance child rels, cf set_append_rel_size().
	 * (XXX might be worth changing that sometime.)
	 */
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs_used);

	/* Add all the attributes used by un-pushed-down restriction clauses. */
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, rel->relid, &attrs_used);
	}

	/*
	 * If there's a whole-row reference to the subquery, we can't remove
	 * anything.
	 */
	if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs_used))
		return;

	/*
	 * Run through the tlist and zap entries we don't need.  It's okay to
	 * modify the tlist items in-place because set_subquery_pathlist made a
	 * copy of the subquery.
	 */
	foreach(lc, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Node	   *texpr = (Node *) tle->expr;

		/*
		 * If it has a sortgroupref number, it's used in some sort/group
		 * clause so we'd better not remove it.  Also, don't remove any
		 * resjunk columns, since their reason for being has nothing to do
		 * with anybody reading the subquery's output.  (It's likely that
		 * resjunk columns in a sub-SELECT would always have ressortgroupref
		 * set, but even if they don't, it seems imprudent to remove them.)
		 */
		if (tle->ressortgroupref || tle->resjunk)
			continue;

		/*
		 * If it's used by the upper query, we can't remove it.
		 */
		if (bms_is_member(tle->resno - FirstLowInvalidHeapAttributeNumber,
						  attrs_used))
			continue;

		/*
		 * If it contains a set-returning function, we can't remove it since
		 * that could change the number of rows returned by the subquery.
		 */
		if (subquery->hasTargetSRFs &&
			expression_returns_set(texpr))
			continue;

		/*
		 * If it contains volatile functions, we daren't remove it for fear
		 * that the user is expecting their side-effects to happen.
		 */
		if (contain_volatile_functions(texpr))
			continue;

		/*
		 * OK, we don't need it.  Replace the expression with a NULL constant.
		 * Preserve the exposed type of the expression, in case something
		 * looks at the rowtype of the subquery's result.
		 */
		tle->expr = (Expr *) makeNullConst(exprType(texpr),
										   exprTypmod(texpr),
										   exprCollation(texpr));
	}
}

/*
 * create_partial_bitmap_paths
 *	  Build partial bitmap heap path for the relation
 */
void
create_partial_bitmap_paths(PlannerInfo *root, RelOptInfo *rel,
							Path *bitmapqual)
{
	int			parallel_workers;
	double		pages_fetched;

	/* Compute heap pages for bitmap heap scan */
	pages_fetched = compute_bitmap_pages(root, rel, bitmapqual, 1.0,
										 NULL, NULL);

	parallel_workers = compute_parallel_worker(rel, pages_fetched, -1,
											   max_parallel_workers_per_gather);

	if (parallel_workers <= 0)
		return;

	add_partial_path(rel, (Path *) create_bitmap_heap_path(root, rel,
														   bitmapqual, rel->lateral_relids, 1.0, parallel_workers));
}

/*
 * Compute the number of parallel workers that should be used to scan a
 * relation.  We compute the parallel workers based on the size of the heap to
 * be scanned and the size of the index to be scanned, then choose a minimum
 * of those.
 *
 * "heap_pages" is the number of pages from the table that we expect to scan, or
 * -1 if we don't expect to scan any.
 *
 * "index_pages" is the number of pages from the index that we expect to scan, or
 * -1 if we don't expect to scan any.
 *
 * "max_workers" is caller's limit on the number of workers.  This typically
 * comes from a GUC.
 */
int
compute_parallel_worker(RelOptInfo *rel, double heap_pages, double index_pages,
						int max_workers)
{
	int			parallel_workers = 0;

	/*
	 * If the user has set the parallel_workers reloption, use that; otherwise
	 * select a default number of workers.
	 */
	if (rel->rel_parallel_workers != -1)
		parallel_workers = rel->rel_parallel_workers;
	else
	{
		/*
		 * If the number of pages being scanned is insufficient to justify a
		 * parallel scan, just return zero ... unless it's an inheritance
		 * child. In that case, we want to generate a parallel path here
		 * anyway.  It might not be worthwhile just for this relation, but
		 * when combined with all of its inheritance siblings it may well pay
		 * off.
		 */
		if (rel->reloptkind == RELOPT_BASEREL &&
			((heap_pages >= 0 && heap_pages < min_parallel_table_scan_size) ||
			 (index_pages >= 0 && index_pages < min_parallel_index_scan_size)))
			return 0;

		if (heap_pages >= 0)
		{
			int			heap_parallel_threshold;
			int			heap_parallel_workers = 1;

			/*
			 * Select the number of workers based on the log of the size of
			 * the relation.  This probably needs to be a good deal more
			 * sophisticated, but we need something here for now.  Note that
			 * the upper limit of the min_parallel_table_scan_size GUC is
			 * chosen to prevent overflow here.
			 */
			heap_parallel_threshold = Max(min_parallel_table_scan_size, 1);
			while (heap_pages >= (BlockNumber) (heap_parallel_threshold * 3))
			{
				heap_parallel_workers++;
				heap_parallel_threshold *= 3;
				if (heap_parallel_threshold > INT_MAX / 3)
					break;		/* avoid overflow */
			}

			parallel_workers = heap_parallel_workers;
		}

		if (index_pages >= 0)
		{
			int			index_parallel_workers = 1;
			int			index_parallel_threshold;

			/* same calculation as for heap_pages above */
			index_parallel_threshold = Max(min_parallel_index_scan_size, 1);
			while (index_pages >= (BlockNumber) (index_parallel_threshold * 3))
			{
				index_parallel_workers++;
				index_parallel_threshold *= 3;
				if (index_parallel_threshold > INT_MAX / 3)
					break;		/* avoid overflow */
			}

			if (parallel_workers > 0)
				parallel_workers = Min(parallel_workers, index_parallel_workers);
			else
				parallel_workers = index_parallel_workers;
		}
	}

	/* In no case use more than caller supplied maximum number of workers */
	parallel_workers = Min(parallel_workers, max_workers);

	return parallel_workers;
}

/*
 * generate_partitionwise_join_paths
 * 		Create paths representing partitionwise join for given partitioned
 * 		join relation.
 *
 * This must not be called until after we are done adding paths for all
 * child-joins. Otherwise, add_path might delete a path to which some path
 * generated here has a reference.
 */
void
generate_partitionwise_join_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *live_children = NIL;
	int			cnt_parts;
	int			num_parts;
	RelOptInfo **part_rels;

	/* Handle only join relations here. */
	if (!IS_JOIN_REL(rel))
		return;

	/* We've nothing to do if the relation is not partitioned. */
	if (!IS_PARTITIONED_REL(rel))
		return;

	/* Guard against stack overflow due to overly deep partition hierarchy. */
	check_stack_depth();

	num_parts = rel->nparts;
	part_rels = rel->part_rels;

	/* Collect non-dummy child-joins. */
	for (cnt_parts = 0; cnt_parts < num_parts; cnt_parts++)
	{
		RelOptInfo *child_rel = part_rels[cnt_parts];

		Assert(child_rel != NULL);

		/* Add partitionwise join paths for partitioned child-joins. */
		generate_partitionwise_join_paths(root, child_rel);

		/* Dummy children will not be scanned, so ignore those. */
		if (IS_DUMMY_REL(child_rel))
			continue;

		set_cheapest(child_rel);

#ifdef OPTIMIZER_DEBUG
		debug_print_rel(root, child_rel);
#endif

		live_children = lappend(live_children, child_rel);
	}

	/* If all child-joins are dummy, parent join is also dummy. */
	if (!live_children)
	{
		mark_dummy_rel(rel);
		return;
	}

	/* Build additional paths for this rel from child-join paths. */
	add_paths_to_append_rel(root, rel, live_children);
	list_free(live_children);
}

#ifdef ADB
static bool set_path_reduce_info_worker(Path *path, List *reduce_info_list)
{
	if (path != NULL)
	{
		if(path->reduce_is_valid == false)
		{
			path->reduce_info_list = reduce_info_list;
			path->reduce_is_valid = true;
		}
		return path_tree_walker(path, set_path_reduce_info_worker, reduce_info_list);
	}
	return false;
}

static bool get_subplan_ref_walker(Expr *expr, int *subplan_ref)
{
	if (expr == NULL)
		return false;
	if (IsA(expr, SubPlan))
	{
		SubPlan *sub = (SubPlan*)expr;
		subplan_ref[sub->plan_id-1]++;
		return false;
	}else if (IsA(expr, RestrictInfo))
	{
		return expression_tree_walker((Node*)((RestrictInfo*)expr)->clause,
									  get_subplan_ref_walker,
									  subplan_ref);
	}
	return expression_tree_walker((Node*)expr, get_subplan_ref_walker, subplan_ref);
}

static int get_max_parallel_workers(List *pathlist)
{
	ListCell *lc;
	Path *path;
	int max_parallel_workers = 0;

	foreach(lc, pathlist)
	{
		path = lfirst(lc);
		if (max_parallel_workers < path->parallel_workers)
			max_parallel_workers = path->parallel_workers;
	}

	return max_parallel_workers;
}

static RelOptInfo* make_no_execparam_base_rel(PlannerInfo *root, RelOptInfo *baserel, List *exec_param_clauses)
{
	RelOptInfo *no_param_rel;
	List	   *tlist_vars;
	ListCell   *lc;
	Relids		relids;

	Assert(baserel->rtekind == RTE_CTE || baserel->rtekind == RTE_RELATION);

	no_param_rel = palloc(sizeof(RelOptInfo));
	memcpy(no_param_rel, baserel, sizeof(RelOptInfo));
	baserel->no_param_rel = no_param_rel;
	no_param_rel->consider_parallel = false;
	no_param_rel->baserestrictinfo = list_difference_ptr(baserel->baserestrictinfo, exec_param_clauses);

	/* don't need index paths for join */
	no_param_rel->joininfo = NIL;
	no_param_rel->has_eclass_joins = false;

	if (no_param_rel->rtekind == RTE_RELATION)
	{
		RangeTblEntry *rte = root->simple_rte_array[no_param_rel->relid];
		no_param_rel->rows = 0.0;
		get_relation_info(root, rte->relid, rte->inh, no_param_rel);
	}else
	{
		Size attr_count = no_param_rel->max_attr - no_param_rel->min_attr + 1;
		no_param_rel->attr_needed = palloc0(sizeof(Relids)*attr_count);
		no_param_rel->attr_widths = palloc0(sizeof(int32)*attr_count);
	}

	/* rebuild path target */
	relids = bms_make_singleton(0);
	no_param_rel->reltarget = create_empty_pathtarget();

	/* replace baserel, function add_vars_to_targetlist need it */
	Assert(root->simple_rel_array[baserel->relid] == baserel);
	root->simple_rel_array[baserel->relid] = no_param_rel;
	/* add needed vars */
	tlist_vars = pull_var_clause((Node*)baserel->reltarget->exprs, PVC_INCLUDE_PLACEHOLDERS);
	if (tlist_vars)
	{
		add_vars_to_targetlist(root, tlist_vars, relids, true);
		list_free(tlist_vars);
	}
	foreach (lc, exec_param_clauses)
	{
		tlist_vars = pull_var_clause((Node*)(lfirst_node(RestrictInfo, lc)->clause),
									 PVC_INCLUDE_PLACEHOLDERS);
		if (tlist_vars)
		{
			add_vars_to_targetlist(root, tlist_vars, relids, true);
			list_free(tlist_vars);
		}
	}
	/* resotre baserel */
	root->simple_rel_array[baserel->relid] = baserel;
	bms_free(relids);

	if (baserel->rtekind != RTE_CTE)
	{
		check_index_predicates(root, no_param_rel);

		no_param_rel->rows = 0.0;
		if(root->glob->parallelModeOK)
			set_rel_consider_parallel(root, no_param_rel, root->simple_rte_array[baserel->relid]);
		set_baserel_size_estimates(root, no_param_rel);
	}

	no_param_rel->pathlist = NIL;
	no_param_rel->partial_pathlist = NIL;
	no_param_rel->cluster_pathlist = NIL;
	no_param_rel->cluster_partial_pathlist = NIL;

	return no_param_rel;
}

#endif /* ADB */

#ifdef ADB_GRAM_ORA
void add_paths_to_connect_by_rel(PlannerInfo *root,
								 RelOptInfo *connect_rel,
								 RelOptInfo *input_rel)
{
	RestrictInfo   *ri;
	ListCell	   *lc;
	Path		   *subpath;
	List		   *hash_quals = NIL;
	JoinCostWorkspace workspace;

	foreach (lc, connect_rel->joininfo)
	{
		ri = lfirst_node(RestrictInfo, lc);
		if (ri->can_join &&
			OidIsValid(ri->hashjoinoperator))
			hash_quals = lappend(hash_quals, ri);
	}

	JoinPathExtraData extra;
	MemSet(&extra, 0, sizeof(extra));
	extra.restrictlist = connect_rel->joininfo;

	foreach(lc, input_rel->pathlist)
	{
		ConnectByPath *path = makeNode(ConnectByPath);
		path->path.pathtype = T_ConnectByPlan;
		path->path.parent = connect_rel;
		path->subpath = subpath = lfirst(lc);
		path->path.pathtarget = connect_rel->reltarget;
		path->path.rows = connect_rel->rows;
		if (hash_quals)
		{
			initial_cost_hashjoin(root, &workspace, JOIN_INNER, hash_quals, subpath, subpath, &extra, false);
		}else
		{
			initial_cost_nestloop(root, &workspace, JOIN_INNER, subpath, subpath, &extra);
		}
		path->path.startup_cost = workspace.startup_cost;
		path->path.total_cost = workspace.total_cost;

		add_path(connect_rel, (Path*)path);
	}

	list_free(hash_quals);
}
#endif /* ADB_GRAM_ORA */

/*****************************************************************************
 *			DEBUG SUPPORT
 *****************************************************************************/

#ifdef OPTIMIZER_DEBUG

static void
print_relids(PlannerInfo *root, Relids relids)
{
	int			x;
	bool		first = true;

	x = -1;
	while ((x = bms_next_member(relids, x)) >= 0)
	{
		if (!first)
			printf(" ");
		if (x < root->simple_rel_array_size &&
			root->simple_rte_array[x])
			printf("%s", root->simple_rte_array[x]->eref->aliasname);
		else
			printf("%d", x);
		first = false;
	}
}

static void
print_restrictclauses(PlannerInfo *root, List *clauses)
{
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *c = lfirst(l);

		print_expr((Node *) c->clause, root->parse->rtable);
		if (lnext(l))
			printf(", ");
	}
}

static void
print_path(PlannerInfo *root, Path *path, int indent)
{
	const char *ptype;
	bool		join = false;
	Path	   *subpath = NULL;
	int			i;

	switch (nodeTag(path))
	{
		case T_Path:
			switch (path->pathtype)
			{
				case T_SeqScan:
					ptype = "SeqScan";
					break;
				case T_SampleScan:
					ptype = "SampleScan";
					break;
				case T_SubqueryScan:
					ptype = "SubqueryScan";
					break;
				case T_FunctionScan:
					ptype = "FunctionScan";
					break;
				case T_TableFuncScan:
					ptype = "TableFuncScan";
					break;
				case T_ValuesScan:
					ptype = "ValuesScan";
					break;
				case T_CteScan:
					ptype = "CteScan";
					break;
				case T_WorkTableScan:
					ptype = "WorkTableScan";
					break;
				default:
					ptype = "???Path";
					break;
			}
			break;
		case T_IndexPath:
			ptype = "IdxScan";
			break;
		case T_BitmapHeapPath:
			ptype = "BitmapHeapScan";
			break;
		case T_BitmapAndPath:
			ptype = "BitmapAndPath";
			break;
		case T_BitmapOrPath:
			ptype = "BitmapOrPath";
			break;
		case T_TidPath:
			ptype = "TidScan";
			break;
		case T_SubqueryScanPath:
			ptype = "SubqueryScanScan";
			break;
		case T_ForeignPath:
			ptype = "ForeignScan";
			break;
		case T_AppendPath:
			ptype = "Append";
			break;
		case T_MergeAppendPath:
			ptype = "MergeAppend";
			break;
		case T_ResultPath:
			ptype = "Result";
			break;
		case T_MaterialPath:
			ptype = "Material";
			subpath = ((MaterialPath *) path)->subpath;
			break;
		case T_UniquePath:
			ptype = "Unique";
			subpath = ((UniquePath *) path)->subpath;
			break;
		case T_GatherPath:
			ptype = "Gather";
			subpath = ((GatherPath *) path)->subpath;
			break;
		case T_ProjectionPath:
			ptype = "Projection";
			subpath = ((ProjectionPath *) path)->subpath;
			break;
		case T_ProjectSetPath:
			ptype = "ProjectSet";
			subpath = ((ProjectSetPath *) path)->subpath;
			break;
		case T_SortPath:
			ptype = "Sort";
			subpath = ((SortPath *) path)->subpath;
			break;
		case T_GroupPath:
			ptype = "Group";
			subpath = ((GroupPath *) path)->subpath;
			break;
		case T_UpperUniquePath:
			ptype = "UpperUnique";
			subpath = ((UpperUniquePath *) path)->subpath;
			break;
		case T_AggPath:
			ptype = "Agg";
			subpath = ((AggPath *) path)->subpath;
			break;
		case T_GroupingSetsPath:
			ptype = "GroupingSets";
			subpath = ((GroupingSetsPath *) path)->subpath;
			break;
		case T_MinMaxAggPath:
			ptype = "MinMaxAgg";
			break;
		case T_WindowAggPath:
			ptype = "WindowAgg";
			subpath = ((WindowAggPath *) path)->subpath;
			break;
		case T_SetOpPath:
			ptype = "SetOp";
			subpath = ((SetOpPath *) path)->subpath;
			break;
		case T_RecursiveUnionPath:
			ptype = "RecursiveUnion";
			break;
		case T_LockRowsPath:
			ptype = "LockRows";
			subpath = ((LockRowsPath *) path)->subpath;
			break;
		case T_ModifyTablePath:
			ptype = "ModifyTable";
			break;
		case T_LimitPath:
			ptype = "Limit";
			subpath = ((LimitPath *) path)->subpath;
			break;
		case T_NestPath:
			ptype = "NestLoop";
			join = true;
			break;
		case T_MergePath:
			ptype = "MergeJoin";
			join = true;
			break;
		case T_HashPath:
			ptype = "HashJoin";
			join = true;
			break;
#ifdef ADB
		case T_RemoteQueryPath:
			ptype = "RemoteQuery";
			break;
		case T_ClusterScanPath:
			ptype = "ClusterScan";
			subpath = ((ClusterScanPath*) path)->cluster_path.subpath;
			break;
		case T_ClusterGatherPath:
			ptype = "ClusterGather";
			subpath = ((ClusterGatherPath*) path)->subpath;
			break;
		case T_ClusterMergeGatherPath:
			ptype = "ClusterMergeGather";
			subpath = ((ClusterMergeGatherPath*) path)->subpath;
			break;
#endif /* ADB */
		default:
			ptype = "???Path";
			break;
	}

	for (i = 0; i < indent; i++)
		printf("\t");
	printf("%s", ptype);

	if (path->parent)
	{
		printf("(");
		print_relids(root, path->parent->relids);
		printf(")");
	}
	if (path->param_info)
	{
		printf(" required_outer (");
		print_relids(root, path->param_info->ppi_req_outer);
		printf(")");
	}
	printf(" rows=%.0f cost=%.2f..%.2f\n",
		   path->rows, path->startup_cost, path->total_cost);

	if (path->pathkeys)
	{
		for (i = 0; i < indent; i++)
			printf("\t");
		printf("  pathkeys: ");
		print_pathkeys(path->pathkeys, root->parse->rtable);
	}

	if (join)
	{
		JoinPath   *jp = (JoinPath *) path;

		for (i = 0; i < indent; i++)
			printf("\t");
		printf("  clauses: ");
		print_restrictclauses(root, jp->joinrestrictinfo);
		printf("\n");

		if (IsA(path, MergePath))
		{
			MergePath  *mp = (MergePath *) path;

			for (i = 0; i < indent; i++)
				printf("\t");
			printf("  sortouter=%d sortinner=%d materializeinner=%d\n",
				   ((mp->outersortkeys) ? 1 : 0),
				   ((mp->innersortkeys) ? 1 : 0),
				   ((mp->materialize_inner) ? 1 : 0));
		}

		print_path(root, jp->outerjoinpath, indent + 1);
		print_path(root, jp->innerjoinpath, indent + 1);
	}

	if (subpath)
		print_path(root, subpath, indent + 1);
}

void
debug_print_rel(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *l;

	printf("RELOPTINFO (");
	print_relids(root, rel->relids);
	printf("): rows=%.0f width=%d\n", rel->rows, rel->reltarget->width);

	if (rel->baserestrictinfo)
	{
		printf("\tbaserestrictinfo: ");
		print_restrictclauses(root, rel->baserestrictinfo);
		printf("\n");
	}

	if (rel->joininfo)
	{
		printf("\tjoininfo: ");
		print_restrictclauses(root, rel->joininfo);
		printf("\n");
	}

	printf("\tpath list:\n");
	foreach(l, rel->pathlist)
		print_path(root, lfirst(l), 1);
	if (rel->cheapest_parameterized_paths)
	{
		printf("\n\tcheapest parameterized paths:\n");
		foreach(l, rel->cheapest_parameterized_paths)
			print_path(root, lfirst(l), 1);
	}
	if (rel->cheapest_startup_path)
	{
		printf("\n\tcheapest startup path:\n");
		print_path(root, rel->cheapest_startup_path, 1);
	}
	if (rel->cheapest_total_path)
	{
		printf("\n\tcheapest total path:\n");
		print_path(root, rel->cheapest_total_path, 1);
	}
	printf("\n");
#ifdef ADB
	printf("\tcluster path list:\n");
	foreach(l, rel->cluster_pathlist)
		print_path(root, lfirst(l), 1);
	printf("\n");
#endif /* ADB */
	fflush(stdout);
}

#endif							/* OPTIMIZER_DEBUG */
