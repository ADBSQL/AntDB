/*-------------------------------------------------------------------------
 *
 * parser.c
 *		Main entry point/driver for PostgreSQL grammar
 *
 * Note that the grammar is not allowed to perform any table access
 * (since we need to be able to do basic parsing even while inside an
 * aborted transaction).  Therefore, the data structures returned by
 * the grammar are "raw" parsetrees that still need to be analyzed by
 * analyze.c and related files.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/parser/parser.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/gramparse.h"
#include "parser/parser.h"

#ifdef ADB
#include "catalog/heap.h" /* SystemAttributeByName */
#include "lib/stringinfo.h"
#include "miscadmin.h" /* check_stack_depth */
#include "parser/ora_gramparse.h"
#include "parser/parse_target.h"
#endif

#ifdef ADB

typedef enum MutatorConnectByExprKind
{
	 MCBEK_LEFT = 1
	,MCBEK_RIGHT
	,MCBEK_RIGHT_TL
	,MCBEK_TOP
	,MCBEK_CLAUSE
}MutatorConnectByExprKind;

typedef struct ConnectByParseState
{
	core_yyscan_t yyscanner;
	List *column_list;
	List *scbp_list;		/* sys_connect_by_path list */
	List *scbp_as_list;		/* sys_connect_by_path as name list */
	const char *base_rel_name;
	const char *schema_name;
	const char *database_name;
	const char *cte_name;
	char *column_level;
	MutatorConnectByExprKind mutator_kind;
	bool have_star;
	bool have_level;
}ConnectByParseState;

static bool search_columnref(Node *node, ConnectByParseState *context);
static void make_scbp_as_list(ConnectByParseState *context);
static List* make_target_list_base(ConnectByParseState *context);
static List* make_target_list_left(ConnectByParseState *context);
static List* make_target_list_right(ConnectByParseState *context);
static List* make_target_list_top(ConnectByParseState *context, List *old_tl);
static Node* mutator_connect_by_expr(Node *node, ConnectByParseState *context);
static bool is_level_expr(Node *node);
static bool is_sys_connect_by_path_expr(Node *node);
static void change_expr_to_target(List *list);
static char* get_unique_as_name(const char *prefix, uint32 *start, List *list);
static char* get_expr_name(Node *expr, List *expr_list, List *name_list);
static bool have_prior_expr(Node *node, void *context);
static Node* make_concat_expr(Node *larg, Node *rarg, int location);

#endif /* ADB */
/*
 * raw_parser
 *		Given a query in string form, do lexical and grammatical analysis.
 *
 * Returns a list of raw (un-analyzed) parse trees.
 */
List *
raw_parser(const char *str)
{
	core_yyscan_t yyscanner;
	base_yy_extra_type yyextra;
	int			yyresult;

	/* initialize the flex scanner */
	yyscanner = scanner_init(str, &yyextra.core_yy_extra,
							 ScanKeywords, NumScanKeywords);

	/* base_yylex() only needs this much initialization */
	yyextra.have_lookahead = false;

	/* initialize the bison parser */
	parser_init(&yyextra);

	/* Parse! */
	yyresult = base_yyparse(yyscanner);

	/* Clean up (release memory) */
	scanner_finish(yyscanner);

	if (yyresult)				/* error */
		return NIL;

	return yyextra.parsetree;
}

#ifdef ADB
List* ora_raw_parser(const char *str)
{
	core_yyscan_t yyscanner;
	ora_yy_extra_type yyextra;
	int yyresult;

	/* initialize the flex scanner */
	yyscanner = scanner_init(str, &yyextra.core_yy_extra,
							 OraScanKeywords, OraNumScanKeywords);

	/* initialize the bison parser */
	ora_parser_init(&yyextra);

	/* Parse! */
	yyresult = ora_yyparse(yyscanner);

	/* Clean up (release memory) */
	scanner_finish(yyscanner);

	if (yyresult)				/* error */
		return NIL;

	return yyextra.parsetree;
}
#endif

/*
 * Intermediate filter between parser and core lexer (core_yylex in scan.l).
 *
 * This filter is needed because in some cases the standard SQL grammar
 * requires more than one token lookahead.  We reduce these cases to one-token
 * lookahead by replacing tokens here, in order to keep the grammar LALR(1).
 *
 * Using a filter is simpler than trying to recognize multiword tokens
 * directly in scan.l, because we'd have to allow for comments between the
 * words.  Furthermore it's not clear how to do that without re-introducing
 * scanner backtrack, which would cost more performance than this filter
 * layer does.
 *
 * The filter also provides a convenient place to translate between
 * the core_YYSTYPE and YYSTYPE representations (which are really the
 * same thing anyway, but notationally they're different).
 */
int
base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, core_yyscan_t yyscanner)
{
	base_yy_extra_type *yyextra = pg_yyget_extra(yyscanner);
	int			cur_token;
	int			next_token;
	int			cur_token_length;
	YYLTYPE		cur_yylloc;

	/* Get next token --- we might already have it */
	if (yyextra->have_lookahead)
	{
		cur_token = yyextra->lookahead_token;
		lvalp->core_yystype = yyextra->lookahead_yylval;
		*llocp = yyextra->lookahead_yylloc;
		*(yyextra->lookahead_end) = yyextra->lookahead_hold_char;
		yyextra->have_lookahead = false;
	}
	else
		cur_token = core_yylex(&(lvalp->core_yystype), llocp, yyscanner);

	/*
	 * If this token isn't one that requires lookahead, just return it.  If it
	 * does, determine the token length.  (We could get that via strlen(), but
	 * since we have such a small set of possibilities, hardwiring seems
	 * feasible and more efficient.)
	 */
	switch (cur_token)
	{
		case NOT:
			cur_token_length = 3;
			break;
		case NULLS_P:
			cur_token_length = 5;
			break;
		case WITH:
			cur_token_length = 4;
			break;
		default:
			return cur_token;
	}

	/*
	 * Identify end+1 of current token.  core_yylex() has temporarily stored a
	 * '\0' here, and will undo that when we call it again.  We need to redo
	 * it to fully revert the lookahead call for error reporting purposes.
	 */
	yyextra->lookahead_end = yyextra->core_yy_extra.scanbuf +
		*llocp + cur_token_length;
	Assert(*(yyextra->lookahead_end) == '\0');

	/*
	 * Save and restore *llocp around the call.  It might look like we could
	 * avoid this by just passing &lookahead_yylloc to core_yylex(), but that
	 * does not work because flex actually holds onto the last-passed pointer
	 * internally, and will use that for error reporting.  We need any error
	 * reports to point to the current token, not the next one.
	 */
	cur_yylloc = *llocp;

	/* Get next token, saving outputs into lookahead variables */
	next_token = core_yylex(&(yyextra->lookahead_yylval), llocp, yyscanner);
	yyextra->lookahead_token = next_token;
	yyextra->lookahead_yylloc = *llocp;

	*llocp = cur_yylloc;

	/* Now revert the un-truncation of the current token */
	yyextra->lookahead_hold_char = *(yyextra->lookahead_end);
	*(yyextra->lookahead_end) = '\0';

	yyextra->have_lookahead = true;

	/* Replace cur_token if needed, based on lookahead */
	switch (cur_token)
	{
		case NOT:
			/* Replace NOT by NOT_LA if it's followed by BETWEEN, IN, etc */
			switch (next_token)
			{
				case BETWEEN:
				case IN_P:
				case LIKE:
				case ILIKE:
				case SIMILAR:
					cur_token = NOT_LA;
					break;
			}
			break;

		case NULLS_P:
			/* Replace NULLS_P by NULLS_LA if it's followed by FIRST or LAST */
			switch (next_token)
			{
				case FIRST_P:
				case LAST_P:
					cur_token = NULLS_LA;
					break;
			}
			break;

		case WITH:
			/* Replace WITH by WITH_LA if it's followed by TIME or ORDINALITY */
			switch (next_token)
			{
				case TIME:
				case ORDINALITY:
					cur_token = WITH_LA;
					break;
			}
			break;
	}

	return cur_token;
}

/* ADB move from gram.y */
#define parser_yyerror(msg)  scanner_yyerror(msg, yyscanner)
#define parser_errposition(pos)  scanner_errposition(pos, yyscanner)

Node *
makeColumnRef(char *colname, List *indirection,
			  int location, core_yyscan_t yyscanner)
{
	/*
	 * Generate a ColumnRef node, with an A_Indirection node added if there
	 * is any subscripting in the specified indirection list.  However,
	 * any field selection at the start of the indirection list must be
	 * transposed into the "fields" part of the ColumnRef node.
	 */
	ColumnRef  *c = makeNode(ColumnRef);
	int		nfields = 0;
	ListCell *l;

	c->location = location;
	foreach(l, indirection)
	{
		if (IsA(lfirst(l), A_Indices))
		{
			A_Indirection *i = makeNode(A_Indirection);

			if (nfields == 0)
			{
				/* easy case - all indirection goes to A_Indirection */
				c->fields = list_make1(makeString(colname));
				i->indirection = check_indirection(indirection, yyscanner);
			}
			else
			{
				/* got to split the list in two */
				i->indirection = check_indirection(list_copy_tail(indirection,
																  nfields),
												   yyscanner);
				indirection = list_truncate(indirection, nfields);
				c->fields = lcons(makeString(colname), indirection);
			}
			i->arg = (Node *) c;
			return (Node *) i;
		}
		else if (IsA(lfirst(l), A_Star))
		{
			/* We only allow '*' at the end of a ColumnRef */
			if (lnext(l) != NULL)
				parser_yyerror("improper use of \"*\"");
		}
		nfields++;
	}
	/* No subscripting, so all indirection gets added to field list */
	c->fields = lcons(makeString(colname), indirection);
	return (Node *) c;
}

Node *
makeTypeCast(Node *arg, TypeName *typename, int location)
{
	TypeCast *n = makeNode(TypeCast);
	n->arg = arg;
	n->typeName = typename;
	n->location = location;
	return (Node *) n;
}

Node *
makeStringConst(char *str, int location)
{
	A_Const *n = makeNode(A_Const);

	n->val.type = T_String;
	n->val.val.str = str;
	n->location = location;

	return (Node *)n;
}

Node *
makeStringConstCast(char *str, int location, TypeName *typename)
{
	Node *s = makeStringConst(str, location);

	return makeTypeCast(s, typename, -1);
}

Node *
makeIntConst(int val, int location)
{
	A_Const *n = makeNode(A_Const);

	n->val.type = T_Integer;
	n->val.val.ival = val;
	n->location = location;

	return (Node *)n;
}

Node *
makeFloatConst(char *str, int location)
{
	A_Const *n = makeNode(A_Const);

	n->val.type = T_Float;
	n->val.val.str = str;
	n->location = location;

	return (Node *)n;
}

Node *
makeBitStringConst(char *str, int location)
{
	A_Const *n = makeNode(A_Const);

	n->val.type = T_BitString;
	n->val.val.str = str;
	n->location = location;

	return (Node *)n;
}

Node *
makeNullAConst(int location)
{
	A_Const *n = makeNode(A_Const);

	n->val.type = T_Null;
	n->location = location;

	return (Node *)n;
}

Node *
makeAConst(Value *v, int location)
{
	Node *n;

	switch (v->type)
	{
		case T_Float:
			n = makeFloatConst(v->val.str, location);
			break;

		case T_Integer:
			n = makeIntConst(v->val.ival, location);
			break;

		case T_String:
		default:
			n = makeStringConst(v->val.str, location);
			break;
	}

	return n;
}

/* makeBoolAConst()
 * Create an A_Const string node and put it inside a boolean cast.
 */
Node *
makeBoolAConst(bool state, int location)
{
	A_Const *n = makeNode(A_Const);

	n->val.type = T_String;
	n->val.val.str = (state ? "t" : "f");
	n->location = location;

	return makeTypeCast((Node *)n, SystemTypeName("bool"), -1);
}

#ifdef ADB
List *
check_sequence_name(List *names, core_yyscan_t yyscanner, int location)
{
	ListCell   *i;
	StringInfoData buf;

	initStringInfo(&buf);
	foreach(i, names)
	{
		if (!IsA(lfirst(i), String))
			parser_yyerror("syntax error");

		appendStringInfo(&buf, "%s.", strVal((Value*)lfirst(i)));
	}
	buf.data[buf.len - 1] = '\0';

	return list_make1(makeStringConst(buf.data, location));
}

Node *makeConnectByStmt(SelectStmt *stmt, Node *start, Node *connect_by,
								core_yyscan_t yyscanner)
{
	SelectStmt *new_select,
			   *union_all_left,
			   *union_all_right;
	CommonTableExpr *common_table;
	RangeVar *range;
	ConnectByParseState pstate;
	JoinExpr *join;
	AssertArg(stmt && connect_by && yyscanner);
	memset(&pstate, 0, sizeof(pstate));

	/* have PriorExpr? */
	if(have_prior_expr(connect_by, NULL) == false)
	{
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("have no prior expression")));
	}

	if(stmt->distinctClause || stmt->groupClause || stmt->havingClause || stmt->windowClause)
	{
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("connect by not support distinct group window yet!")));
	}

	/* make new select and have recursive cte */
	new_select = makeNode(SelectStmt);
	new_select->withClause = makeNode(WithClause);
	new_select->withClause->recursive = true;
	new_select->withClause->location = -1;

	/* get base rel name */
	if(list_length(stmt->fromClause) != 1
		|| !IsA(linitial(stmt->fromClause), RangeVar))
	{
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("connect by support one table yet")));
		pstate.base_rel_name = NULL;	/* never run, keep analyze quiet */
	}else
	{
		range = linitial(stmt->fromClause);
		pstate.base_rel_name = range->relname;
		if(range->alias)
			pstate.base_rel_name = range->alias->aliasname;
	}

	/* get using target list */
	pstate.yyscanner = yyscanner;
	search_columnref((Node*)stmt->targetList, &pstate);
	search_columnref((Node*)stmt->distinctClause, &pstate);
	search_columnref(stmt->whereClause, &pstate);
	search_columnref((Node*)stmt->groupClause, &pstate);
	search_columnref(stmt->havingClause, &pstate);
	search_columnref(start, &pstate);
	search_columnref(connect_by, &pstate);
	if(pstate.have_level)
		pstate.column_level = get_unique_as_name("_level_", NULL, pstate.column_list);
	make_scbp_as_list(&pstate);

	/* when with a group select we let it as a CTE */
	if(stmt->distinctClause || stmt->groupClause)
	{
		common_table = makeNode(CommonTableExpr);
		common_table->ctename = pstrdup(pstate.base_rel_name);
		common_table->ctequery = (Node*)stmt;
		common_table->location = -1;
		new_select->withClause->ctes = list_make1(common_table);
	}

	/* make select union all left as "select * from base_rel where where_clause*/
	union_all_left = makeNode(SelectStmt);
	union_all_left->targetList = make_target_list_left(&pstate);
	union_all_left->fromClause = stmt->fromClause;

	/* make CTE rel */
	common_table = makeNode(CommonTableExpr);
	if(strcmp(pstate.base_rel_name, "_CTE1") == 0)
		common_table->ctename = pstrdup("_CTE2");
	else
		common_table->ctename = pstrdup("_CTE1");
	common_table->location = -1;
	pstate.cte_name = common_table->ctename;

	/* add "start with clause" */
	pstate.mutator_kind = MCBEK_LEFT;
	union_all_left->whereClause = mutator_connect_by_expr(start, &pstate);

	/* make union all right as
	 *   select base_rel.* from base_rel iner join "_CTEn" on "connect by"
	 */
	union_all_right = makeNode(SelectStmt);

	union_all_right->targetList = make_target_list_right(&pstate);

	/* make join */
	join = makeNode(JoinExpr);
	join->jointype = JOIN_INNER;
	join->larg = linitial(stmt->fromClause);
	join->rarg = (Node*)makeRangeVar(NULL, pstrdup(common_table->ctename), -1);

	/* mutator qual */
	pstate.mutator_kind = MCBEK_CLAUSE;
	join->quals = mutator_connect_by_expr(connect_by, &pstate);

	union_all_right->fromClause = list_make1(join);

	/* make union all select */
	common_table->ctequery = makeSetOp(SETOP_UNION, true, (Node*)union_all_left, (Node*)union_all_right);
	/* and append it to CTEs */
	new_select->withClause->ctes = lappend(new_select->withClause->ctes, common_table);

	/* make new target list */
	new_select->targetList = make_target_list_top(&pstate, stmt->targetList);

	new_select->fromClause = list_make1(makeRangeVar(NULL, common_table->ctename, -1));

	pstate.mutator_kind = MCBEK_TOP;
	new_select->whereClause = mutator_connect_by_expr(stmt->whereClause, &pstate);

	return (Node*)new_select;
}

static bool have_prior_expr(Node *node, void *context)
{
	if(node == NULL)
		return false;
	if(IsA(node, PriorExpr))
		return true;
	/*ADBQ, undefine function node_tree_walker*/
	//return node_tree_walker(node, have_prior_expr, context);
	return false;
}

static Node* make_concat_expr(Node *larg, Node *rarg, int location)
{
	FuncCall *func = makeNode(FuncCall);
	func->location = -1;
	func->funcname = SystemFuncName("concat");
	func->args = list_make2(larg, rarg);
	return (Node*)func;
}

static bool search_columnref(Node *node, ConnectByParseState *context)
{
	if(node == NULL)
		return false;

	check_stack_depth();

	if(is_sys_connect_by_path_expr(node))
	{
		ListCell *lc;
		/* have same sys_connect_by_path ? */
		foreach(lc, context->scbp_list)
		{
			if(equal(lfirst(lc), node))
				break;
		}
		if(lc == NULL)
			context->scbp_list = lappend(context->scbp_list, node);
	}else if(is_level_expr(node))
	{
		context->have_level = true;
		return false;
	}else if(IsA(node, ColumnRef))
	{
		ListCell *lc;
		ColumnRef *c2,*c;
		c = (ColumnRef*)node;

		lc = list_head(c->fields);
		Assert(list_length(c->fields) <= 4);
		if(list_length(c->fields) > 3)
		{
			Assert(IsA(lfirst(lc), String));
			if(context->database_name == NULL)
			{
				context->database_name = strVal(lfirst(lc));
			}else if(strcmp(context->database_name, strVal(lfirst(lc))) != 0)
			{
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("cross-database references are not implemented"),
					scanner_errposition(c->location, context->yyscanner)));
			}
			lc = lnext(lc);
		}
		if(list_length(c->fields) > 2)
		{
			Assert(IsA(lfirst(lc), String));
			if(context->schema_name == NULL)
			{
				context->schema_name = strVal(lfirst(lc));
			}else if(strcmp(context->schema_name, strVal(lfirst(lc))) != 0)
			{
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("different schema name"),
					err_generic_string(PG_DIAG_SCHEMA_NAME, strVal(lfirst(lc))),
					scanner_errposition(c->location, context->yyscanner)));
			}
			lc = lnext(lc);
		}
		if(list_length(c->fields) > 1)
		{
			Assert(IsA(lfirst(lc), String));
			if(strcmp(context->base_rel_name, strVal(lfirst(lc))) != 0)
			{
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("different table name"),
					err_generic_string(PG_DIAG_TABLE_NAME, strVal(lfirst(lc))),
					scanner_errposition(c->location, context->yyscanner)));
			}
			lc = lnext(lc);
		}
		Assert(lnext(lc) == NULL);
		if(IsA(lfirst(lc), A_Star))
		{
			context->have_star = true;
		}else
		{
			const char *name;
			Assert(IsA(lfirst(lc), String));
			name = strVal(lfirst(lc));
			foreach(lc, context->column_list)
			{
				c2 = lfirst(lc);
				if(strcmp(strVal(llast(c2->fields)), name) == 0)
					break;
			}
			if(lc == NULL)
				context->column_list = lappend(context->column_list, copyObject(c));
		}
		return false;
	}
	/*ADBQ, undefine function node_tree_walker*/
	//return node_tree_walker((Node*)node, search_columnref, context);
	return false;
}

static void make_scbp_as_list(ConnectByParseState *context)
{
	char *name;
	ListCell *lc;
	unsigned int index;

	index = 1;
	foreach(lc, context->scbp_list)
	{
		name = get_unique_as_name("_scbp", &index, context->column_list);
		context->scbp_as_list = lappend(context->scbp_as_list, name);
	}
}

static List* make_target_list_base(ConnectByParseState *context)
{
	List *new_list;
	ListCell *lc;

	new_list = NIL;
	if(context->have_star == false)
	{
		/*ADBQ, undefine the function node_copy*/
		/* have no "*" just use all column */
		//new_list = (List*)node_copy((Node*)context->column_list);
	}else
	{
		/* have "*", we select "*" and all system column */
		ColumnRef *cr;
		new_list = list_make1(make_star_target(-1));
		foreach(lc, context->column_list)
		{
			cr = lfirst(lc);
			Assert(cr);
			if(SystemAttributeByName(strVal(llast(cr->fields)), true) != NULL)
			{
				new_list = lappend(new_list, cr);
			}
		}
	}

	return new_list;
}

static List* make_target_list_left(ConnectByParseState *context)
{
	List *new_list;
	ListCell *lc,*lc2;
	ResTarget *rt;
	int save_kind = context->mutator_kind;
	context->mutator_kind = MCBEK_LEFT;

	new_list = make_target_list_base(context);

	if(context->have_level)
	{
		rt = makeNode(ResTarget);
		rt->name = context->column_level;
		rt->location = -1;
		rt->val = makeIntConst(1, -1);
		new_list = lappend(new_list, rt);
	}

	/* make sys_connect_by_path target */
	forboth(lc, context->scbp_list, lc2, context->scbp_as_list)
	{
		Assert(is_sys_connect_by_path_expr(lfirst(lc)));

		rt = makeNode(ResTarget);
		rt->location = -1;
		rt->name = lfirst(lc2);
		rt->val = mutator_connect_by_expr(lfirst(lc), context);
		new_list = lappend(new_list, rt);
	}

	if(new_list == NIL)
		new_list = list_make1(makeNullAConst(-1));

	change_expr_to_target(new_list);

	context->mutator_kind = save_kind;
	return new_list;
}

static List* make_target_list_right(ConnectByParseState *context)
{
	ListCell *lc;
	List *tl;
	int save_kind = context->mutator_kind;
	context->mutator_kind = MCBEK_RIGHT_TL;
	Assert(context->cte_name != NULL);

	tl = make_target_list_base(context);
	tl = (List*)mutator_connect_by_expr((Node*)tl, context);

	if(context->have_level)
	{
		ResTarget *rt = makeNode(ResTarget);
		LevelExpr level = {T_LevelExpr, -1};
		rt->location = -1;
		rt->val = mutator_connect_by_expr((Node*)&level, context);
		tl = lappend(tl, rt);
	}

	foreach(lc, context->scbp_list)
		tl = lappend(tl, mutator_connect_by_expr(lfirst(lc), context));

	if(tl == NIL)
		tl = list_make1(makeNullAConst(-1));

	context->mutator_kind = save_kind;
	change_expr_to_target(tl);
	return tl;
}

static List* make_target_list_top(ConnectByParseState *context, List *old_tl)
{
	List *tl;
	ListCell *lc;
	ResTarget *rt;
	int save_kind = context->mutator_kind;
	context->mutator_kind = MCBEK_TOP;

	tl = NIL;
	foreach(lc, old_tl)
	{
		rt = (ResTarget*)mutator_connect_by_expr(lfirst(lc), context);
		Assert(IsA(rt, ResTarget));
		if(rt->name == NULL)
			rt->name = FigureColname(((ResTarget*)lfirst(lc))->val);
		tl = lappend(tl, rt);
	}

	context->mutator_kind = save_kind;
	return tl;
}

static Node* mutator_connect_by_expr(Node *node, ConnectByParseState *context)
{
	ColumnRef *cr;
	if(node == NULL)
		return NULL;
	check_stack_depth();

	if(is_level_expr(node))
	{
		switch(context->mutator_kind)
		{
		case MCBEK_LEFT:
			return makeIntConst(1, -1);
		case MCBEK_RIGHT:
		case MCBEK_RIGHT_TL:
		case MCBEK_CLAUSE:
			/* cte.level + 1 */
			cr = makeNode(ColumnRef);
			cr->location = -1;
			cr->fields = list_make2(makeString(pstrdup(context->cte_name)),
						makeString(context->column_level));
			return (Node*)makeSimpleA_Expr(AEXPR_OP, "+", (Node*)cr, makeIntConst(1, -1), -1);
		case MCBEK_TOP:
			cr = makeNode(ColumnRef);
			cr->location = -1;
			cr->fields = list_make2(makeString(pstrdup(context->cte_name))
				, makeString(pstrdup(context->column_level)));
			return (Node*)cr;
		default:
			Assert(false);
		}
	}else if(is_sys_connect_by_path_expr(node))
	{
		FuncCall *func = (FuncCall*)node;
		Assert(IsA(func, FuncCall));

		switch(context->mutator_kind)
		{
		case MCBEK_LEFT:
			return make_concat_expr(mutator_connect_by_expr(llast(func->args), context),
				mutator_connect_by_expr(linitial(func->args), context), -1);
		case MCBEK_RIGHT:
		case MCBEK_RIGHT_TL:
		case MCBEK_CLAUSE:
			/* make cte.scbp */
			cr = makeNode(ColumnRef);
			cr->location = -1;
			cr->fields = list_make2(makeString(pstrdup(context->cte_name)),
							makeString(get_expr_name(node, context->scbp_list, context->scbp_as_list)));
			/* (cte.scbp || rarg) || larg */
			return make_concat_expr(
					/* (cte.scbp || rarg) */
					 make_concat_expr((Node*)cr, mutator_connect_by_expr(llast(func->args), context), -1)
					 /* larg */
					,mutator_connect_by_expr(linitial(func->args), context)
					, -1 /* location */);
		case MCBEK_TOP:
			cr = makeNode(ColumnRef);
			cr->location = -1;
			cr->fields = list_make1(makeString(get_expr_name(node, context->scbp_list, context->scbp_as_list)));
			return (Node*)cr;
		default:
			Assert(false);
		}
	}else if(IsA(node, ColumnRef))
	{
		char *table_name = NULL;
		cr = palloc(sizeof(ColumnRef));
		memcpy(cr, node, sizeof(ColumnRef));

		switch(context->mutator_kind)
		{
		case MCBEK_LEFT:
		case MCBEK_RIGHT_TL:
		case MCBEK_CLAUSE:
			table_name = pstrdup(context->base_rel_name);
			break;
		case MCBEK_TOP:
		case MCBEK_RIGHT:
			table_name = pstrdup(context->cte_name);
			break;
		default:
			Assert(false);
			table_name = NULL;	/* keep compiler quiet */
		}

		cr->fields = list_make2(makeString(table_name), copyObject(llast(cr->fields)));
		return (Node*)cr;
	}else if(IsA(node, PriorExpr))
	{
		if(context->mutator_kind != MCBEK_CLAUSE)
			goto mutator_connect_by_expr_error_;
		context->mutator_kind = MCBEK_TOP;
		node = mutator_connect_by_expr(((PriorExpr*)node)->expr, context);
		context->mutator_kind = MCBEK_CLAUSE;
		return node;
	}
	return node_tree_mutator(node, mutator_connect_by_expr, context);

mutator_connect_by_expr_error_:
	ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
		errmsg("syntax error"),
		scanner_errposition(exprLocation(node), context->yyscanner)));
	return NULL;	/* keep compiler quiet */
}

static bool is_level_expr(Node *node)
{
	if(node != NULL && IsA(node, LevelExpr))
		return true;
	return false;
}

static bool is_sys_connect_by_path_expr(Node *node)
{
	if(node != NULL && IsA(node, FuncCall))
	{
		FuncCall *func = (FuncCall*)node;
		if(list_length(func->funcname) == 1
			&& list_length(func->args) == 2
			&& func->agg_order == NIL
			&& func->agg_star == false
			&& func->agg_distinct == false
			&& func->func_variadic == false
			&& func->over == NULL
			&& IsA(linitial(func->funcname), String)
			&& strcmp(strVal(linitial(func->funcname)), "sys_connect_by_path") == 0)
		{
			return true;
		}
	}
	return false;
}

static void change_expr_to_target(List *list)
{
	ResTarget *rt;
	ListCell *lc;
	foreach(lc, list)
	{
		if(!IsA(lfirst(lc), ResTarget))
		{
			rt = makeNode(ResTarget);
			rt->location = -1;
			rt->val = lfirst(lc);
			lfirst(lc) = rt;
		}
	}
}

static char* get_unique_as_name(const char *prefix, uint32 *start, List *list)
{
	ListCell *lc;
	ColumnRef *cr;
	Value *value;
	char *name;
	uint32 i;
	Assert(prefix);

	i = (start == NULL ? 1 : *start);
	do
	{
		name = psprintf("%s%u", prefix, i);
		foreach(lc, list)
		{
			cr = lfirst(lc);
			Assert(IsA(cr, ColumnRef));
			value = llast(cr->fields);
			Assert(IsA(value, String));
			if(strcmp(strVal(value), name) == 0)
			{
				pfree(name);
				name = NULL;
				++i;
				break;
			}
		}
	}while(name == NULL);

	if(start)
		*start = i;
	return name;
}

static char* get_expr_name(Node *expr, List *expr_list, List *name_list)
{
	ListCell *lc;
	ListCell *lc2;
	forboth(lc, expr_list, lc2, name_list)
	{
		if(equal(expr, lfirst(lc)))
			return lfirst(lc2);
	}
	return NULL;
}

#endif

/* makeRoleSpec
 * Create a RoleSpec with the given type
 */
Node *
makeRoleSpec(RoleSpecType type, int location)
{
	RoleSpec *spec = makeNode(RoleSpec);

	spec->roletype = type;
	spec->location = location;

	return (Node *) spec;
}

/* check_qualified_name --- check the result of qualified_name production
 *
 * It's easiest to let the grammar production for qualified_name allow
 * subscripts and '*', which we then must reject here.
 */
void
check_qualified_name(List *names, core_yyscan_t yyscanner)
{
	ListCell   *i;

	foreach(i, names)
	{
		if (!IsA(lfirst(i), String))
			parser_yyerror("syntax error");
	}
}

/* check_func_name --- check the result of func_name production
 *
 * It's easiest to let the grammar production for func_name allow subscripts
 * and '*', which we then must reject here.
 */
List *
check_func_name(List *names, core_yyscan_t yyscanner)
{
	ListCell   *i;

	foreach(i, names)
	{
		if (!IsA(lfirst(i), String))
			parser_yyerror("syntax error");
	}
	return names;
}

/* check_indirection --- check the result of indirection production
 *
 * We only allow '*' at the end of the list, but it's hard to enforce that
 * in the grammar, so do it here.
 */
List *
check_indirection(List *indirection, core_yyscan_t yyscanner)
{
	ListCell *l;

	foreach(l, indirection)
	{
		if (IsA(lfirst(l), A_Star))
		{
			if (lnext(l) != NULL)
				parser_yyerror("improper use of \"*\"");
		}
	}
	return indirection;
}

/* extractArgTypes()
 * Given a list of FunctionParameter nodes, extract a list of just the
 * argument types (TypeNames) for input parameters only.  This is what
 * is needed to look up an existing function, which is what is wanted by
 * the productions that use this call.
 */
List *
extractArgTypes(List *parameters)
{
	List	   *result = NIL;
	ListCell   *i;

	foreach(i, parameters)
	{
		FunctionParameter *p = (FunctionParameter *) lfirst(i);

		if (p->mode != FUNC_PARAM_OUT && p->mode != FUNC_PARAM_TABLE)
			result = lappend(result, p->argType);
	}
	return result;
}

/* extractAggrArgTypes()
 * As above, but work from the output of the aggr_args production.
 */
List *
extractAggrArgTypes(List *aggrargs)
{
	Assert(list_length(aggrargs) == 2);
	return extractArgTypes((List *) linitial(aggrargs));
}

/* makeOrderedSetArgs()
 * Build the result of the aggr_args production (which see the comments for).
 * This handles only the case where both given lists are nonempty, so that
 * we have to deal with multiple VARIADIC arguments.
 */
List *
makeOrderedSetArgs(List *directargs, List *orderedargs,
				   core_yyscan_t yyscanner)
{
	FunctionParameter *lastd = (FunctionParameter *) llast(directargs);
	int			ndirectargs;

	/* No restriction unless last direct arg is VARIADIC */
	if (lastd->mode == FUNC_PARAM_VARIADIC)
	{
		FunctionParameter *firsto = (FunctionParameter *) linitial(orderedargs);

		/*
		 * We ignore the names, though the aggr_arg production allows them;
		 * it doesn't allow default values, so those need not be checked.
		 */
		if (list_length(orderedargs) != 1 ||
			firsto->mode != FUNC_PARAM_VARIADIC ||
			!equal(lastd->argType, firsto->argType))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("an ordered-set aggregate with a VARIADIC direct argument must have one VARIADIC aggregated argument of the same data type"),
					 parser_errposition(exprLocation((Node *) firsto))));

		/* OK, drop the duplicate VARIADIC argument from the internal form */
		orderedargs = NIL;
	}

	/* don't merge into the next line, as list_concat changes directargs */
	ndirectargs = list_length(directargs);

	return list_make2(list_concat(directargs, orderedargs),
					  makeInteger(ndirectargs));
}

/* insertSelectOptions()
 * Insert ORDER BY, etc into an already-constructed SelectStmt.
 *
 * This routine is just to avoid duplicating code in SelectStmt productions.
 */
void
insertSelectOptions(SelectStmt *stmt,
					List *sortClause, List *lockingClause,
					Node *limitOffset, Node *limitCount,
					WithClause *withClause,
					core_yyscan_t yyscanner)
{
	Assert(IsA(stmt, SelectStmt));

	/*
	 * Tests here are to reject constructs like
	 *	(SELECT foo ORDER BY bar) ORDER BY baz
	 */
	if (sortClause)
	{
		if (stmt->sortClause)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple ORDER BY clauses not allowed"),
					 parser_errposition(exprLocation((Node *) sortClause))));
		stmt->sortClause = sortClause;
	}
	/* We can handle multiple locking clauses, though */
	stmt->lockingClause = list_concat(stmt->lockingClause, lockingClause);
	if (limitOffset)
	{
		if (stmt->limitOffset)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple OFFSET clauses not allowed"),
					 parser_errposition(exprLocation(limitOffset))));
		stmt->limitOffset = limitOffset;
	}
	if (limitCount)
	{
		if (stmt->limitCount)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple LIMIT clauses not allowed"),
					 parser_errposition(exprLocation(limitCount))));
		stmt->limitCount = limitCount;
	}
	if (withClause)
	{
		if (stmt->withClause)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple WITH clauses not allowed"),
					 parser_errposition(exprLocation((Node *) withClause))));
		stmt->withClause = withClause;
	}
}

Node *
makeSetOp(SetOperation op, bool all, Node *larg, Node *rarg)
{
	SelectStmt *n = makeNode(SelectStmt);

	n->op = op;
	n->all = all;
	n->larg = (SelectStmt *) larg;
	n->rarg = (SelectStmt *) rarg;
	return (Node *) n;
}

/* SystemFuncName()
 * Build a properly-qualified reference to a built-in function.
 */
List *
SystemFuncName(char *name)
{
	return list_make2(makeString("pg_catalog"), makeString(name));
}

/* SystemTypeName()
 * Build a properly-qualified reference to a built-in type.
 *
 * typmod is defaulted, but may be changed afterwards by caller.
 * Likewise for the location.
 */
TypeName *
SystemTypeName(char *name)
{
	return makeTypeNameFromNameList(list_make2(makeString("pg_catalog"),
											   makeString(name)));
}

#ifdef ADB
TypeName *SystemTypeNameLocation(char *name, int location)
{
	TypeName *typ = makeTypeNameFromNameList(list_make2(makeString("pg_catalog"),
											   makeString(name)));
	typ->location = location;
	return typ;
}
#endif /* ADB */

/* doNegate()
 * Handle negation of a numeric constant.
 *
 * Formerly, we did this here because the optimizer couldn't cope with
 * indexquals that looked like "var = -4" --- it wants "var = const"
 * and a unary minus operator applied to a constant didn't qualify.
 * As of Postgres 7.0, that problem doesn't exist anymore because there
 * is a constant-subexpression simplifier in the optimizer.  However,
 * there's still a good reason for doing this here, which is that we can
 * postpone committing to a particular internal representation for simple
 * negative constants.	It's better to leave "-123.456" in string form
 * until we know what the desired type is.
 */
Node *
doNegate(Node *n, int location)
{
	if (IsA(n, A_Const))
	{
		A_Const *con = (A_Const *)n;

		/* report the constant's location as that of the '-' sign */
		con->location = location;

		if (con->val.type == T_Integer)
		{
			con->val.val.ival = -con->val.val.ival;
			return n;
		}
		if (con->val.type == T_Float)
		{
			doNegateFloat(&con->val);
			return n;
		}
	}

	return (Node *) makeSimpleA_Expr(AEXPR_OP, "-", NULL, n, location);
}

void
doNegateFloat(Value *v)
{
	char   *oldval = v->val.str;

	Assert(IsA(v, Float));
	if (*oldval == '+')
		oldval++;
	if (*oldval == '-')
		v->val.str = oldval+1;	/* just strip the '-' */
	else
		v->val.str = psprintf("-%s", oldval);
}

Node *
makeAndExpr(Node *lexpr, Node *rexpr, int location)
{
	Node	   *lexp = lexpr;

	/* Look through AEXPR_PAREN nodes so they don't affect flattening */
	while (IsA(lexp, A_Expr) &&
		   ((A_Expr *) lexp)->kind == AEXPR_PAREN)
		lexp = ((A_Expr *) lexp)->lexpr;
	/* Flatten "a AND b AND c ..." to a single BoolExpr on sight */
	if (IsA(lexp, BoolExpr))
	{
		BoolExpr *blexpr = (BoolExpr *) lexp;

		if (blexpr->boolop == AND_EXPR)
		{
			blexpr->args = lappend(blexpr->args, rexpr);
			return (Node *) blexpr;
		}
	}
	return (Node *) makeBoolExpr(AND_EXPR, list_make2(lexpr, rexpr), location);
}

Node *
makeOrExpr(Node *lexpr, Node *rexpr, int location)
{
	Node	   *lexp = lexpr;

	/* Look through AEXPR_PAREN nodes so they don't affect flattening */
	while (IsA(lexp, A_Expr) &&
		   ((A_Expr *) lexp)->kind == AEXPR_PAREN)
		lexp = ((A_Expr *) lexp)->lexpr;
	/* Flatten "a OR b OR c ..." to a single BoolExpr on sight */
	if (IsA(lexp, BoolExpr))
	{
		BoolExpr *blexpr = (BoolExpr *) lexp;

		if (blexpr->boolop == OR_EXPR)
		{
			blexpr->args = lappend(blexpr->args, rexpr);
			return (Node *) blexpr;
		}
	}
	return (Node *) makeBoolExpr(OR_EXPR, list_make2(lexpr, rexpr), location);
}

Node *
makeNotExpr(Node *expr, int location)
{
	return (Node *) makeBoolExpr(NOT_EXPR, list_make1(expr), location);
}

Node *
makeAArrayExpr(List *elements, int location)
{
	A_ArrayExpr *n = makeNode(A_ArrayExpr);

	n->elements = elements;
	n->location = location;
	return (Node *) n;
}

Node *
makeXmlExpr(XmlExprOp op, char *name, List *named_args, List *args,
			int location)
{
	XmlExpr		*x = makeNode(XmlExpr);

	x->op = op;
	x->name = name;
	/*
	 * named_args is a list of ResTarget; it'll be split apart into separate
	 * expression and name lists in transformXmlExpr().
	 */
	x->named_args = named_args;
	x->arg_names = NIL;
	x->args = args;
	/* xmloption, if relevant, must be filled in by caller */
	/* type and typmod will be filled in during parse analysis */
	x->type = InvalidOid;			/* marks the node as not analyzed */
	x->location = location;
	return (Node *) x;
}

/*
 * Merge the input and output parameters of a table function.
 */
List *
mergeTableFuncParameters(List *func_args, List *columns)
{
	ListCell   *lc;

	/* Explicit OUT and INOUT parameters shouldn't be used in this syntax */
	foreach(lc, func_args)
	{
		FunctionParameter *p = (FunctionParameter *) lfirst(lc);

		if (p->mode != FUNC_PARAM_IN && p->mode != FUNC_PARAM_VARIADIC)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("OUT and INOUT arguments aren't allowed in TABLE functions")));
	}

	return list_concat(func_args, columns);
}

/*
 * Determine return type of a TABLE function.  A single result column
 * returns setof that column's type; otherwise return setof record.
 */
TypeName *
TableFuncTypeName(List *columns)
{
	TypeName *result;

	if (list_length(columns) == 1)
	{
		FunctionParameter *p = (FunctionParameter *) linitial(columns);

		result = (TypeName *) copyObject(p->argType);
	}
	else
		result = SystemTypeName("record");

	result->setof = true;

	return result;
}

/*
 * Convert a list of (dotted) names to a RangeVar (like
 * makeRangeVarFromNameList, but with position support).  The
 * "AnyName" refers to the any_name production in the grammar.
 */
RangeVar *
makeRangeVarFromAnyName(List *names, int position, core_yyscan_t yyscanner)
{
	RangeVar *r = makeNode(RangeVar);

	switch (list_length(names))
	{
		case 1:
			r->catalogname = NULL;
			r->schemaname = NULL;
			r->relname = strVal(linitial(names));
			break;
		case 2:
			r->catalogname = NULL;
			r->schemaname = strVal(linitial(names));
			r->relname = strVal(lsecond(names));
			break;
		case 3:
			r->catalogname = strVal(linitial(names));
			r->schemaname = strVal(lsecond(names));
			r->relname = strVal(lthird(names));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("improper qualified name (too many dotted names): %s",
							NameListToString(names)),
					 parser_errposition(position)));
			break;
	}

	r->relpersistence = RELPERSISTENCE_PERMANENT;
	r->location = position;

	return r;
}

/*----------
 * Recursive view transformation
 *
 * Convert
 *
 *     CREATE RECURSIVE VIEW relname (aliases) AS query
 *
 * to
 *
 *     CREATE VIEW relname (aliases) AS
 *         WITH RECURSIVE relname (aliases) AS (query)
 *         SELECT aliases FROM relname
 *
 * Actually, just the WITH ... part, which is then inserted into the original
 * view definition as the query.
 * ----------
 */
Node *
makeRecursiveViewSelect(char *relname, List *aliases, Node *query)
{
	SelectStmt *s = makeNode(SelectStmt);
	WithClause *w = makeNode(WithClause);
	CommonTableExpr *cte = makeNode(CommonTableExpr);
	List	   *tl = NIL;
	ListCell   *lc;

	/* create common table expression */
	cte->ctename = relname;
	cte->aliascolnames = aliases;
	cte->ctequery = query;
	cte->location = -1;

	/* create WITH clause and attach CTE */
	w->recursive = true;
	w->ctes = list_make1(cte);
	w->location = -1;

	/* create target list for the new SELECT from the alias list of the
	 * recursive view specification */
	foreach (lc, aliases)
	{
		ResTarget *rt = makeNode(ResTarget);

		rt->name = NULL;
		rt->indirection = NIL;
		rt->val = makeColumnRef(strVal(lfirst(lc)), NIL, -1, 0);
		rt->location = -1;

		tl = lappend(tl, rt);
	}

	/* create new SELECT combining WITH clause, target list, and fake FROM
	 * clause */
	s->withClause = w;
	s->targetList = tl;
	s->fromClause = list_make1(makeRangeVar(NULL, relname, -1));

	return (Node *) s;
}

ResTarget* make_star_target(int location)
{
	ResTarget *target;
	ColumnRef *n = makeNode(ColumnRef);
	n->fields = list_make1(makeNode(A_Star));
	n->location = -1;

	target = makeNode(ResTarget);
	target->name = NULL;
	target->indirection = NIL;
	target->val = (Node *)n;
	target->location = -1;

	return target;
}

/* Separate Constraint nodes from COLLATE clauses in a ColQualList */
void
SplitColQualList(List *qualList,
				 List **constraintList, CollateClause **collClause,
				 core_yyscan_t yyscanner)
{
	ListCell   *cell;
	ListCell   *prev;
	ListCell   *next;

	*collClause = NULL;
	prev = NULL;
	for (cell = list_head(qualList); cell; cell = next)
	{
		Node   *n = (Node *) lfirst(cell);

		next = lnext(cell);
		if (IsA(n, Constraint))
		{
			/* keep it in list */
			prev = cell;
			continue;
		}
		if (IsA(n, CollateClause))
		{
			CollateClause *c = (CollateClause *) n;

			if (*collClause)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("multiple COLLATE clauses not allowed"),
						 parser_errposition(c->location)));
			*collClause = c;
		}
		else
			elog(ERROR, "unexpected node type %d", (int) n->type);
		/* remove non-Constraint nodes from qualList */
		qualList = list_delete_cell(qualList, cell, prev);
	}
	*constraintList = qualList;
}

/* ADB end from gram.y */

#ifdef ADB
List *OracleFuncName(char *name)
{
	return list_make2(makeString("oracle"), makeString(name));
}

TypeName *OracleTypeName(char *name)
{
	return makeTypeNameFromNameList(list_make2(makeString("oracle"),
											   makeString(name)));
}

TypeName *OracleTypeNameLocation(char *name, int location)
{
	TypeName *typ = makeTypeNameFromNameList(list_make2(makeString("oracle"),
												makeString(name)));
	typ->location = location;
	return typ;
}

void transformDistributeBy(DistributeBy *dbstmt)
{
	List *funcname = NIL;
	List *funcargs = NIL;

	if (dbstmt == NULL ||
		/* must be replication or roundrobin */
		dbstmt->disttype != DISTTYPE_USER_DEFINED)
		return ;

	funcname = dbstmt->funcname;
	funcargs = dbstmt->funcargs;

	Assert(funcname && funcargs);

	/*
	 * try to judge distribution type
	 * HASH or MODULE or USER-DEFINED.
	 */
	if (list_length(funcname) == 1)
	{
		Node *argnode = linitial(funcargs);
		char *fname = strVal(linitial(funcname));
		if (strcasecmp(fname, "HASH") == 0)
		{
			if (list_length(funcargs) != 1 ||
				IsA(argnode, ColumnRef) == false ||
				list_length(((ColumnRef *)argnode)->fields) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("Invalid distribution column specified for \"HASH\""),
						 errhint("Valid syntax input: HASH(column)")));

			dbstmt->disttype = DISTTYPE_HASH;
			dbstmt->colname = strVal(linitial(((ColumnRef *)argnode)->fields));
		} else if (strcasecmp(fname, "MODULO") == 0)
		{
			if (list_length(funcargs) != 1 ||
				IsA(argnode, ColumnRef) == false ||
				list_length(((ColumnRef *)argnode)->fields) != 1)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("Invalid distribution column specified for \"MODULO\""),
					errhint("Valid syntax input: MODULO(column)")));

			dbstmt->disttype = DISTTYPE_MODULO;
			dbstmt->colname = strVal(linitial(((ColumnRef *)argnode)->fields));
		} else
		{
			/*
			 * Nothing changed.
			 * Just keep compiler quiet.
			 */
		}
	} else
	{
		/*
		 * Nothing changed.
		 * Just keep compiler quiet.
		 */
	}
}
#endif
