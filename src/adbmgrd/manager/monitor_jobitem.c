﻿/*
 * monitor_jobitem.c
 *
 * ADB Integrated Monitor Daemon
 *
 * The ADB monitor dynamic item, uses two catalog table to record the job content:
 * job table and jobitem table. Jobitem table used to record monitor item name,
 * batch absoulte path with filename and its description. The jobitem is used for
 * job table.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2017 ADB Development Group
 *
 * IDENTIFICATION
 *	  src/adbmgrd/manager/monitor_jobitem.c
 */

#include "postgres.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "access/skey.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "lib/ilist.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/adbmonitor.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "tcop/tcopprot.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/tqual.h"
#include "access/heapam.h"
#include "catalog/monitor_job.h"
#include "catalog/monitor_jobitem.h"
#include "access/htup_details.h"
#include "catalog/indexing.h"
#include "parser/mgr_node.h"
#include "mgr/mgr_cmds.h"
#include "utils/builtins.h"
#include "commands/defrem.h"
#include "common/fe_memutils.h"

static HeapTuple montiot_jobitem_get_item_tuple(Relation rel_jobitem, Name itemname);
static void monitor_get_absolute_filepath(StringInfo strinfo, char *str);

/*
* ADD ITEM itemname(itemname, filepath, desc)
*/
void monitor_jobitem_add(MonitorJobitemAdd *node, ParamListInfo params, DestReceiver *dest)
{
	if (mgr_has_priv_add())
	{
		DirectFunctionCall3(monitor_jobitem_add_func,
									BoolGetDatum(node->if_not_exists),
									CStringGetDatum(node->name),
									PointerGetDatum(node->options));
		return;
	}
	else
	{
		ereport(ERROR, (errmsg("permission denied")));
		return ;
	}
}

Datum monitor_jobitem_add_func(PG_FUNCTION_ARGS)
{
	Relation rel;
	HeapTuple newtuple;
	HeapTuple checktuple;
	ListCell *lc;
	DefElem *def;
	NameData itemnamedata;
	Datum datum[Natts_monitor_jobitem];
	bool isnull[Natts_monitor_jobitem];
	bool got[Natts_monitor_jobitem];
	bool if_not_exists = false;
	char *str;
	char *itemname;
	char space = ' ';
	StringInfoData filepathstrdata;
	List *options;

	if_not_exists = PG_GETARG_BOOL(0);
	itemname = PG_GETARG_CSTRING(1);
	options = (List *)PG_GETARG_POINTER(2);

	Assert(itemname);
	namestrcpy(&itemnamedata, itemname);
	rel = heap_open(MjobitemRelationId, AccessShareLock);
	/* check exists */
	checktuple = montiot_jobitem_get_item_tuple(rel, &itemnamedata);
	if (HeapTupleIsValid(checktuple))
	{
		heap_freetuple(checktuple);
		if(if_not_exists)
		{
			ereport(NOTICE, (errcode(ERRCODE_DUPLICATE_OBJECT),
				errmsg("\"%s\" already exists, skipping", itemname)));
			PG_RETURN_BOOL(false);
		}
		heap_close(rel, AccessShareLock);
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT)
				, errmsg("\"%s\" already exists", itemname)));
	}
	heap_close(rel, AccessShareLock);
	memset(datum, 0, sizeof(datum));
	memset(isnull, 0, sizeof(isnull));
	memset(got, 0, sizeof(got));

	/* name */
	datum[Anum_monitor_jobitem_jobitem_itemname-1] = NameGetDatum(&itemnamedata);
	foreach(lc, options)
	{
		def = lfirst(lc);
		Assert(def && IsA(def, DefElem));

		if (strcmp(def->defname, "path") == 0)
		{
			if(got[Anum_monitor_jobitem_jobitem_path-1])
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
					,errmsg("conflicting or redundant options")));
			str = defGetString(def);
			while (space == *str)
				str++;
			if(str[0] != '/' || str[0] == '\0')
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
					,errmsg("invalid absoulte path: \"%s\"", str)));
			/*check whether space in str*/
			if (strchr(str, space) == NULL)
				datum[Anum_monitor_jobitem_jobitem_path-1] = PointerGetDatum(cstring_to_text(str));
			else
			{
				/*add single quota*/
				initStringInfo(&filepathstrdata);
				monitor_get_absolute_filepath(&filepathstrdata, str);
				datum[Anum_monitor_jobitem_jobitem_path-1] = PointerGetDatum(cstring_to_text(filepathstrdata.data));
				pfree(filepathstrdata.data);
			}
			got[Anum_monitor_jobitem_jobitem_path-1] = true;
		}
		else if (strcmp(def->defname, "desc") == 0)
		{
			if(got[Anum_monitor_jobitem_jobitem_desc-1])
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
					,errmsg("conflicting or redundant options")));
			str = defGetString(def);
			datum[Anum_monitor_jobitem_jobitem_desc-1] = PointerGetDatum(cstring_to_text(str));
			got[Anum_monitor_jobitem_jobitem_desc-1] = true;
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
				,errmsg("option \"%s\" is not recognized", def->defname)
				,errhint("option is path, desc")));
		}
	}
	/* if not give, set to default */
	if (false == datum[Anum_monitor_jobitem_jobitem_path-1])
	{
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
			, errmsg("option \"path\" must be given")));
	}
	if (false == got[Anum_monitor_jobitem_jobitem_desc-1])
	{
		datum[Anum_monitor_jobitem_jobitem_desc-1] = PointerGetDatum(cstring_to_text(""));
	}
	/* now, we can insert record */
	rel = heap_open(MjobitemRelationId, RowExclusiveLock);
	newtuple = heap_form_tuple(RelationGetDescr(rel), datum, isnull);
	CatalogTupleInsert(rel, newtuple);
	heap_freetuple(newtuple);
	/*close relation */
	heap_close(rel, RowExclusiveLock);

	PG_RETURN_BOOL(true);
}


void monitor_jobitem_alter(MonitorJobitemAlter *node, ParamListInfo params, DestReceiver *dest)
{
	if (mgr_has_priv_add())
	{
		DirectFunctionCall2(monitor_jobitem_alter_func,
									CStringGetDatum(node->name),
									PointerGetDatum(node->options));
		return;
	}
	else
	{
		ereport(ERROR, (errmsg("permission denied")));
		return ;
	}
}

Datum monitor_jobitem_alter_func(PG_FUNCTION_ARGS)
{
	Relation rel;
	HeapTuple newtuple;
	HeapTuple checktuple;
	ListCell *lc;
	DefElem *def;
	NameData itemnamedata;
	Datum datum[Natts_monitor_jobitem];
	bool isnull[Natts_monitor_jobitem];
	bool got[Natts_monitor_jobitem];
	char *str;
	char *itemname;
	List *options;
	TupleDesc jobitem_dsc;
	StringInfoData filepathstrdata;

	itemname = PG_GETARG_CSTRING(0);
	options = (List *)PG_GETARG_POINTER(1);

	Assert(itemname);
	namestrcpy(&itemnamedata, itemname);
	rel = heap_open(MjobitemRelationId, RowExclusiveLock);
	/* check exists */
	checktuple = montiot_jobitem_get_item_tuple(rel, &itemnamedata);
	if (!HeapTupleIsValid(checktuple))
	{
		heap_close(rel, RowExclusiveLock);
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
				,errmsg("\"%s\" does not exist", itemname)));
	}
	memset(datum, 0, sizeof(datum));
	memset(isnull, 0, sizeof(isnull));
	memset(got, 0, sizeof(got));

	/* name */
	datum[Anum_monitor_jobitem_jobitem_itemname-1] = NameGetDatum(&itemnamedata);
	foreach(lc, options)
	{
		def = lfirst(lc);
		Assert(def && IsA(def, DefElem));

		if (strcmp(def->defname, "path") == 0)
		{
			if(got[Anum_monitor_jobitem_jobitem_path-1])
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
					,errmsg("conflicting or redundant options")));
			str = defGetString(def);
			if (str[0] != '/' || str[0] == '\0')
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
					,errmsg("invalid absoulte path: \"%s\"", str)));
			if (strchr(str, ' ') == NULL)
				datum[Anum_monitor_jobitem_jobitem_path-1] = PointerGetDatum(cstring_to_text(str));
			else
			{
				/*add single quota*/
				initStringInfo(&filepathstrdata);
				monitor_get_absolute_filepath(&filepathstrdata, str);
				datum[Anum_monitor_jobitem_jobitem_path-1] = PointerGetDatum(cstring_to_text(filepathstrdata.data));
				pfree(filepathstrdata.data);
			}
			got[Anum_monitor_jobitem_jobitem_path-1] = true;
		}
		else if (strcmp(def->defname, "desc") == 0)
		{
			if(got[Anum_monitor_jobitem_jobitem_desc-1])
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
					,errmsg("conflicting or redundant options")));
			str = defGetString(def);
			datum[Anum_monitor_jobitem_jobitem_desc-1] = PointerGetDatum(cstring_to_text(str));
			got[Anum_monitor_jobitem_jobitem_desc-1] = true;
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR)
				,errmsg("option \"%s\" is not recognized", def->defname)
				,errhint("option is path, desc")));
		}
	}
	jobitem_dsc = RelationGetDescr(rel);
	newtuple = heap_modify_tuple(checktuple, jobitem_dsc, datum,isnull, got);
	CatalogTupleUpdate(rel, &checktuple->t_self, newtuple);

	heap_freetuple(checktuple);
	/* at end, close relation */
	heap_close(rel, RowExclusiveLock);

	PG_RETURN_BOOL(true);
}


void monitor_jobitem_drop(MonitorJobitemDrop *node, ParamListInfo params, DestReceiver *dest)
{
	if (mgr_has_priv_add())
	{
		DirectFunctionCall2(monitor_jobitem_drop_func,
									BoolGetDatum(node->if_exists),
									PointerGetDatum(node->namelist));
		return;
	}
	else
	{
		ereport(ERROR, (errmsg("permission denied")));
		return ;
	}
}

Datum monitor_jobitem_drop_func(PG_FUNCTION_ARGS)
{
	Relation rel;
	HeapTuple tuple;
	ListCell *lc;
	Value *val;
	NameData name;
	Datum datum[Natts_monitor_jobitem];
	bool isnull[Natts_monitor_jobitem];
	bool got[Natts_monitor_jobitem];
	bool if_exists = false;
	MemoryContext context, old_context;
	List *name_list;

	if_exists = PG_GETARG_BOOL(0);
	name_list = (List *)PG_GETARG_POINTER(1);
	Assert(name_list);
	context = AllocSetContextCreate(CurrentMemoryContext,
									"DROP ITEM",
									ALLOCSET_DEFAULT_SIZES);
	rel = heap_open(MjobitemRelationId, RowExclusiveLock);
	old_context = MemoryContextSwitchTo(context);

	/* first we need check is it all exists and used by other */
	foreach(lc, name_list)
	{
		val = lfirst(lc);
		Assert(val && IsA(val,String));
		MemoryContextReset(context);
		namestrcpy(&name, strVal(val));
		tuple = montiot_jobitem_get_item_tuple(rel, &name);
		if(!HeapTupleIsValid(tuple))
		{
			if(if_exists)
			{
				ereport(NOTICE,  (errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("\"%s\" does not exist, skipping", NameStr(name))));
				continue;
			}
			else
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT)
					,errmsg("\"%s\" does not exist", NameStr(name))));
		}
		heap_freetuple(tuple);
	}

	memset(datum, 0, sizeof(datum));
	memset(isnull, 0, sizeof(isnull));
	memset(got, 0, sizeof(got));

	/* name */
	foreach(lc, name_list)
	{
		val = lfirst(lc);
		Assert(val && IsA(val,String));
		MemoryContextReset(context);
		namestrcpy(&name, strVal(val));
		tuple = montiot_jobitem_get_item_tuple(rel, &name);
		if(HeapTupleIsValid(tuple))
		{
			CatalogTupleDelete(rel, &(tuple->t_self));
			heap_freetuple(tuple);
		}
	}
	/* at end, close relation */
	heap_close(rel, RowExclusiveLock);
	(void)MemoryContextSwitchTo(old_context);
	MemoryContextDelete(context);
	PG_RETURN_BOOL(true);
}


static HeapTuple montiot_jobitem_get_item_tuple(Relation rel_jobitem, Name itemname)
{
	ScanKeyData key[1];
	HeapTuple tupleret = NULL;
	HeapTuple tuple = NULL;
	HeapScanDesc rel_scan;

	ScanKeyInit(&key[0]
				,Anum_monitor_jobitem_jobitem_itemname
				,BTEqualStrategyNumber
				,F_NAMEEQ
				,NameGetDatum(itemname));
	rel_scan = heap_beginscan_catalog(rel_jobitem, 1, key);
	while((tuple = heap_getnext(rel_scan, ForwardScanDirection)) != NULL)
	{
		tupleret = heap_copytuple(tuple);
		break;
	}
	heap_endscan(rel_scan);

	return tupleret;
}

/*
* if the file absoulte path includes space, add single quota where the folder or file name contains the space
*/
static void monitor_get_absolute_filepath(StringInfo strinfo, char *str)
{
	char *point[MAXPGPATH/2];
	char *savestr=NULL;
	char *filestr;
	int in = 0;
	int nsize = 0;
	int j = 0;
	char space = ' ';

	Assert(str != NULL);
	filestr = pstrdup(str);
	savestr = str;
	while(*(savestr++))
	{
		if ( '/' == *savestr)
			nsize++;
	}
	savestr = NULL;
	while((point[in] = strtok_r(filestr, "/", &savestr))!=NULL)
	{
		in++;
		filestr = NULL;
	}
	/*discard the space in the end of point[in-1]*/
	nsize = strlen(point[in-1]);
	while(nsize >= 1)
	{
		if (space == point[in-1][nsize-1])
			point[in-1][nsize-1] = '\0';
		else
			break;
		nsize--;
	}
	for (j=0; j<in; j++)
	{
		if (space == point[j][0] || ((point[j][0] != '\'' && point[j][0] != '\"') && strchr(point[j], space) != NULL))
			appendStringInfo(strinfo, "/'%s'", point[j]);
		else
			appendStringInfo(strinfo, "/%s", point[j]);
	}
}
