/*-------------------------------------------------------------------------
 *
 * makefuncs.c
 *	  creator functions for primitive nodes. The functions here are for
 *	  the most frequently created nodes.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/makefuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#ifdef ADB
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "parser/parser.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "utils/typcache.h"
#endif /* ADB */

/*
 * makeA_Expr -
 *		makes an A_Expr node
 */
A_Expr *
makeA_Expr(A_Expr_Kind kind, List *name,
		   Node *lexpr, Node *rexpr, int location)
{
	A_Expr	   *a = makeNode(A_Expr);

	a->kind = kind;
	a->name = name;
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	a->location = location;
	return a;
}

/*
 * makeSimpleA_Expr -
 *		As above, given a simple (unqualified) operator name
 */
A_Expr *
makeSimpleA_Expr(A_Expr_Kind kind, char *name,
				 Node *lexpr, Node *rexpr, int location)
{
	A_Expr	   *a = makeNode(A_Expr);

	a->kind = kind;
	a->name = list_make1(makeString((char *) name));
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	a->location = location;
	return a;
}

/*
 * makeVar -
 *	  creates a Var node
 */
Var *
makeVar(Index varno,
		AttrNumber varattno,
		Oid vartype,
		int32 vartypmod,
		Oid varcollid,
		Index varlevelsup)
{
	Var		   *var = makeNode(Var);

	var->varno = varno;
	var->varattno = varattno;
	var->vartype = vartype;
	var->vartypmod = vartypmod;
	var->varcollid = varcollid;
	var->varlevelsup = varlevelsup;

	/*
	 * Since few if any routines ever create Var nodes with varnoold/varoattno
	 * different from varno/varattno, we don't provide separate arguments for
	 * them, but just initialize them to the given varno/varattno. This
	 * reduces code clutter and chance of error for most callers.
	 */
	var->varnoold = varno;
	var->varoattno = varattno;

	/* Likewise, we just set location to "unknown" here */
	var->location = -1;

	return var;
}

/*
 * makeVarFromTargetEntry -
 *		convenience function to create a same-level Var node from a
 *		TargetEntry
 */
Var *
makeVarFromTargetEntry(Index varno,
					   TargetEntry *tle)
{
	return makeVar(varno,
				   tle->resno,
				   exprType((Node *) tle->expr),
				   exprTypmod((Node *) tle->expr),
				   exprCollation((Node *) tle->expr),
				   0);
}

/*
 * makeWholeRowVar -
 *	  creates a Var node representing a whole row of the specified RTE
 *
 * A whole-row reference is a Var with varno set to the correct range
 * table entry, and varattno == 0 to signal that it references the whole
 * tuple.  (Use of zero here is unclean, since it could easily be confused
 * with error cases, but it's not worth changing now.)  The vartype indicates
 * a rowtype; either a named composite type, or a domain over a named
 * composite type (only possible if the RTE is a function returning that),
 * or RECORD.  This function encapsulates the logic for determining the
 * correct rowtype OID to use.
 *
 * If allowScalar is true, then for the case where the RTE is a single function
 * returning a non-composite result type, we produce a normal Var referencing
 * the function's result directly, instead of the single-column composite
 * value that the whole-row notation might otherwise suggest.
 */
Var *
makeWholeRowVar(RangeTblEntry *rte,
				Index varno,
				Index varlevelsup,
				bool allowScalar)
{
	Var		   *result;
	Oid			toid;
	Node	   *fexpr;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* relation: the rowtype is a named composite type */
			toid = get_rel_type_id(rte->relid);
			if (!OidIsValid(toid))
				elog(ERROR, "could not find type OID for relation %u",
					 rte->relid);
			result = makeVar(varno,
							 InvalidAttrNumber,
							 toid,
							 -1,
							 InvalidOid,
							 varlevelsup);
			break;

		case RTE_FUNCTION:

			/*
			 * If there's more than one function, or ordinality is requested,
			 * force a RECORD result, since there's certainly more than one
			 * column involved and it can't be a known named type.
			 */
			if (rte->funcordinality || list_length(rte->functions) != 1)
			{
				/* always produces an anonymous RECORD result */
				result = makeVar(varno,
								 InvalidAttrNumber,
								 RECORDOID,
								 -1,
								 InvalidOid,
								 varlevelsup);
				break;
			}

			fexpr = ((RangeTblFunction *) linitial(rte->functions))->funcexpr;
			toid = exprType(fexpr);
			if (type_is_rowtype(toid))
			{
				/* func returns composite; same as relation case */
				result = makeVar(varno,
								 InvalidAttrNumber,
								 toid,
								 -1,
								 InvalidOid,
								 varlevelsup);
			}
			else if (allowScalar)
			{
				/* func returns scalar; just return its output as-is */
				result = makeVar(varno,
								 1,
								 toid,
								 -1,
								 exprCollation(fexpr),
								 varlevelsup);
			}
			else
			{
				/* func returns scalar, but we want a composite result */
				result = makeVar(varno,
								 InvalidAttrNumber,
								 RECORDOID,
								 -1,
								 InvalidOid,
								 varlevelsup);
			}
			break;
#ifdef ADB
		case RTE_REMOTE_DUMMY:
			result = NULL;
			elog(ERROR, "Invalid RTE found");
			break;
#endif /* ADB */
		default:

			/*
			 * RTE is a join, subselect, tablefunc, or VALUES.  We represent
			 * this as a whole-row Var of RECORD type. (Note that in most
			 * cases the Var will be expanded to a RowExpr during planning,
			 * but that is not our concern here.)
			 */
			result = makeVar(varno,
							 InvalidAttrNumber,
							 RECORDOID,
							 -1,
							 InvalidOid,
							 varlevelsup);
			break;
	}

	return result;
}

/*
 * makeTargetEntry -
 *	  creates a TargetEntry node
 */
TargetEntry *
makeTargetEntry(Expr *expr,
				AttrNumber resno,
				char *resname,
				bool resjunk)
{
	TargetEntry *tle = makeNode(TargetEntry);

	tle->expr = expr;
	tle->resno = resno;
	tle->resname = resname;

	/*
	 * We always set these fields to 0. If the caller wants to change them he
	 * must do so explicitly.  Few callers do that, so omitting these
	 * arguments reduces the chance of error.
	 */
	tle->ressortgroupref = 0;
	tle->resorigtbl = InvalidOid;
	tle->resorigcol = 0;

	tle->resjunk = resjunk;

	return tle;
}

/*
 * flatCopyTargetEntry -
 *	  duplicate a TargetEntry, but don't copy substructure
 *
 * This is commonly used when we just want to modify the resno or substitute
 * a new expression.
 */
TargetEntry *
flatCopyTargetEntry(TargetEntry *src_tle)
{
	TargetEntry *tle = makeNode(TargetEntry);

	Assert(IsA(src_tle, TargetEntry));
	memcpy(tle, src_tle, sizeof(TargetEntry));
	return tle;
}

/*
 * makeFromExpr -
 *	  creates a FromExpr node
 */
FromExpr *
makeFromExpr(List *fromlist, Node *quals)
{
	FromExpr   *f = makeNode(FromExpr);

	f->fromlist = fromlist;
	f->quals = quals;
	return f;
}

/*
 * makeConst -
 *	  creates a Const node
 */
Const *
makeConst(Oid consttype,
		  int32 consttypmod,
		  Oid constcollid,
		  int constlen,
		  Datum constvalue,
		  bool constisnull,
		  bool constbyval)
{
	Const	   *cnst = makeNode(Const);

	/*
	 * If it's a varlena value, force it to be in non-expanded (non-toasted)
	 * format; this avoids any possible dependency on external values and
	 * improves consistency of representation, which is important for equal().
	 */
	if (!constisnull && constlen == -1)
		constvalue = PointerGetDatum(PG_DETOAST_DATUM(constvalue));

	cnst->consttype = consttype;
	cnst->consttypmod = consttypmod;
	cnst->constcollid = constcollid;
	cnst->constlen = constlen;
	cnst->constvalue = constvalue;
	cnst->constisnull = constisnull;
	cnst->constbyval = constbyval;
	cnst->location = -1;		/* "unknown" */

	return cnst;
}

/*
 * makeNullConst -
 *	  creates a Const node representing a NULL of the specified type/typmod
 *
 * This is a convenience routine that just saves a lookup of the type's
 * storage properties.
 */
Const *
makeNullConst(Oid consttype, int32 consttypmod, Oid constcollid)
{
	int16		typLen;
	bool		typByVal;

	get_typlenbyval(consttype, &typLen, &typByVal);
	return makeConst(consttype,
					 consttypmod,
					 constcollid,
					 (int) typLen,
					 (Datum) 0,
					 true,
					 typByVal);
}

/*
 * makeBoolConst -
 *	  creates a Const node representing a boolean value (can be NULL too)
 */
Node *
makeBoolConst(bool value, bool isnull)
{
	/* note that pg_type.h hardwires size of bool as 1 ... duplicate it */
	return (Node *) makeConst(BOOLOID, -1, InvalidOid, 1,
							  BoolGetDatum(value), isnull, true);
}

/*
 * makeBoolExpr -
 *	  creates a BoolExpr node
 */
Expr *
makeBoolExpr(BoolExprType boolop, List *args, int location)
{
	BoolExpr   *b = makeNode(BoolExpr);

	b->boolop = boolop;
	b->args = args;
	b->location = location;

	return (Expr *) b;
}

/*
 * makeAlias -
 *	  creates an Alias node
 *
 * NOTE: the given name is copied, but the colnames list (if any) isn't.
 */
Alias *
makeAlias(const char *aliasname, List *colnames)
{
	Alias	   *a = makeNode(Alias);

	a->aliasname = pstrdup(aliasname);
	a->colnames = colnames;

	return a;
}

/*
 * makeRelabelType -
 *	  creates a RelabelType node
 */
RelabelType *
makeRelabelType(Expr *arg, Oid rtype, int32 rtypmod, Oid rcollid,
				CoercionForm rformat)
{
	RelabelType *r = makeNode(RelabelType);

	r->arg = arg;
	r->resulttype = rtype;
	r->resulttypmod = rtypmod;
	r->resultcollid = rcollid;
	r->relabelformat = rformat;
	r->location = -1;

	return r;
}

/*
 * makeRangeVar -
 *	  creates a RangeVar node (rather oversimplified case)
 */
RangeVar *
makeRangeVar(char *schemaname, char *relname, int location)
{
	RangeVar   *r = makeNode(RangeVar);

	r->catalogname = NULL;
	r->schemaname = schemaname;
	r->relname = relname;
	r->inh = true;
	r->relpersistence = RELPERSISTENCE_PERMANENT;
	r->alias = NULL;
	r->location = location;

	return r;
}

/*
 * makeTypeName -
 *	build a TypeName node for an unqualified name.
 *
 * typmod is defaulted, but can be changed later by caller.
 */
TypeName *
makeTypeName(char *typnam)
{
	return makeTypeNameFromNameList(list_make1(makeString(typnam)));
}

/*
 * makeTypeNameFromNameList -
 *	build a TypeName node for a String list representing a qualified name.
 *
 * typmod is defaulted, but can be changed later by caller.
 */
TypeName *
makeTypeNameFromNameList(List *names)
{
	TypeName   *n = makeNode(TypeName);

	n->names = names;
	n->typmods = NIL;
	n->typemod = -1;
	n->location = -1;
	return n;
}

/*
 * makeTypeNameFromOid -
 *	build a TypeName node to represent a type already known by OID/typmod.
 */
TypeName *
makeTypeNameFromOid(Oid typeOid, int32 typmod)
{
	TypeName   *n = makeNode(TypeName);

	n->typeOid = typeOid;
	n->typemod = typmod;
	n->location = -1;
	return n;
}

/*
 * makeColumnDef -
 *	build a ColumnDef node to represent a simple column definition.
 *
 * Type and collation are specified by OID.
 * Other properties are all basic to start with.
 */
ColumnDef *
makeColumnDef(const char *colname, Oid typeOid, int32 typmod, Oid collOid)
{
	ColumnDef  *n = makeNode(ColumnDef);

	n->colname = pstrdup(colname);
	n->typeName = makeTypeNameFromOid(typeOid, typmod);
	n->inhcount = 0;
	n->is_local = true;
	n->is_not_null = false;
	n->is_from_type = false;
	n->is_from_parent = false;
	n->storage = 0;
	n->raw_default = NULL;
	n->cooked_default = NULL;
	n->collClause = NULL;
	n->collOid = collOid;
	n->constraints = NIL;
	n->fdwoptions = NIL;
	n->location = -1;

	return n;
}

/*
 * makeFuncExpr -
 *	build an expression tree representing a function call.
 *
 * The argument expressions must have been transformed already.
 */
FuncExpr *
makeFuncExpr(Oid funcid, Oid rettype, List *args,
			 Oid funccollid, Oid inputcollid, CoercionForm fformat)
{
	FuncExpr   *funcexpr;

	funcexpr = makeNode(FuncExpr);
	funcexpr->funcid = funcid;
	funcexpr->funcresulttype = rettype;
	funcexpr->funcretset = false;	/* only allowed case here */
	funcexpr->funcvariadic = false; /* only allowed case here */
	funcexpr->funcformat = fformat;
	funcexpr->funccollid = funccollid;
	funcexpr->inputcollid = inputcollid;
	funcexpr->args = args;
	funcexpr->location = -1;

	return funcexpr;
}

/*
 * makeDefElem -
 *	build a DefElem node
 *
 * This is sufficient for the "typical" case with an unqualified option name
 * and no special action.
 */
DefElem *
makeDefElem(char *name, Node *arg, int location)
{
	DefElem    *res = makeNode(DefElem);

	res->defnamespace = NULL;
	res->defname = name;
	res->arg = arg;
	res->defaction = DEFELEM_UNSPEC;
	res->location = location;

	return res;
}

/*
 * makeDefElemExtended -
 *	build a DefElem node with all fields available to be specified
 */
DefElem *
makeDefElemExtended(char *nameSpace, char *name, Node *arg,
					DefElemAction defaction, int location)
{
	DefElem    *res = makeNode(DefElem);

	res->defnamespace = nameSpace;
	res->defname = name;
	res->arg = arg;
	res->defaction = defaction;
	res->location = location;

	return res;
}

/*
 * makeFuncCall -
 *
 * Initialize a FuncCall struct with the information every caller must
 * supply.  Any non-default parameters have to be inserted by the caller.
 */
FuncCall *
makeFuncCall(List *name, List *args, int location)
{
	FuncCall   *n = makeNode(FuncCall);

	n->funcname = name;
	n->args = args;
	n->agg_order = NIL;
	n->agg_filter = NULL;
	n->agg_within_group = false;
	n->agg_star = false;
	n->agg_distinct = false;
	n->func_variadic = false;
	n->over = NULL;
	n->location = location;
	return n;
}

/*
 * makeGroupingSet
 *
 */
GroupingSet *
makeGroupingSet(GroupingSetKind kind, List *content, int location)
{
	GroupingSet *n = makeNode(GroupingSet);

	n->kind = kind;
	n->content = content;
	n->location = location;
	return n;
}

/*
 * makeVacuumRelation -
 *	  create a VacuumRelation node
 */
VacuumRelation *
makeVacuumRelation(RangeVar *relation, Oid oid, List *va_cols)
{
	VacuumRelation *v = makeNode(VacuumRelation);

	v->relation = relation;
	v->oid = oid;
	v->va_cols = va_cols;
	return v;
}

#ifdef ADB

Expr *makeHashExpr(Expr *expr)
{
	TypeCacheEntry *typeCache;
	Oid typoid = exprType((Node*)expr);
	Oid collid = exprCollation((Node*)expr);
	if(type_is_enum(typoid))
	{
		/* convert to Name */
		expr = (Expr*)coerce_to_target_type(NULL,
											(Node*)expr,
											typoid,
											NAMEOID,
											-1,
											COERCION_EXPLICIT,
											COERCE_IMPLICIT_CAST,
											-1);
		typoid = NAMEOID;
	}
	typeCache = lookup_type_cache(typoid, TYPECACHE_HASH_PROC);

	if(!OidIsValid(typeCache->hash_proc))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify a hash function for type %s",
						format_type_be(typoid))));
	}
	return (Expr*)makeFuncExpr(typeCache->hash_proc,
							   INT4OID,
							   list_make1(expr),
							   collid, collid,
							   COERCE_EXPLICIT_CALL);
}

int32 execHashValue(Datum datum, Oid typid, Oid collid)
{
	TypeCacheEntry *typeCache;
	Datum result;
	if(type_is_enum(typid))
	{
		/* convert to CString, it like Name */
		datum = DirectFunctionCall1(enum_out, datum);
		typid = NAMEOID;
	}
	typeCache = lookup_type_cache(typid, TYPECACHE_HASH_PROC_FINFO);
	if(!OidIsValid(typeCache->hash_proc_finfo.fn_oid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify a hash function for type %s",
						format_type_be(typid))));
	}
	Assert(get_func_rettype(typeCache->hash_proc_finfo.fn_oid) == INT4OID);

	result = FunctionCall1Coll(&typeCache->hash_proc_finfo,
							  collid,
							  datum);
	return DatumGetInt32(result);
}

/* (expr % right) */
Expr *makeModuloExpr(Expr *expr, int right)
{
	Const *r = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
						 Int32GetDatum(right), false, true);
	return make_op(NULL, SystemFuncName("%"), (Node*)expr, (Node*)r, NULL, -1);
}

NullTest *makeNullTest(Expr *arg, NullTestType type, bool isrow, int location)
{
	NullTest *node = makeNode(NullTest);

	node->arg = arg;
	node->nulltesttype = type;
	node->argisrow = isrow;
	node->location = location;

	return node;
}

int32 execModuloValue(Datum datum, Oid typid, int right)
{
	MemoryContext old_context;
	EState *estate;
	Const *left;
	Expr *expr;
	ExprState *exprState;
	Datum result;
	int32 i32;
	int16 typlen;
	bool boolValue;

	estate = CreateExecutorState();
	old_context = MemoryContextSwitchTo(estate->es_query_cxt);

	if(typid==BOOLOID)
	{
		datum = DirectFunctionCall1(bool_int4, datum);
		typid = INT4OID;
	}

	get_typlenbyval(typid, &typlen, &boolValue);
	left = makeConst(typid, -1, InvalidOid, typlen, datum, false, boolValue);
	expr = makeModuloExpr((Expr*)left, right);
	expr = (Expr*)coerce_to_target_type(NULL, (Node*)expr,
										exprType((Node*)expr),
										INT4OID,
										-1,
										COERCION_EXPLICIT,
										COERCE_IMPLICIT_CAST,
										-1);

	exprState = ExecInitExpr(expr, NULL);
	result = ExecEvalExpr(exprState, GetPerTupleExprContext(estate), &boolValue);
	Assert(boolValue == false);

	MemoryContextSwitchTo(old_context);
	FreeExecutorState(estate);

	i32 = DatumGetInt32(result);
	if(i32 < 0)
		i32 = -i32;
	return i32;
}

#endif /* ADB */
