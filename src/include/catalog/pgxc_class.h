/*-----------------------------------------------------------
 *
 * Copyright (c) 2010-2013 Postgres-XC Development Group
 * Portions Copyright (c) 2014-2017, ADB Development Group
 *
 *-----------------------------------------------------------
 */
#ifndef PGXC_CLASS_H
#define PGXC_CLASS_H

#ifdef BUILD_BKI
#include "catalog/buildbki.h"
#else /* BUILD_BKI */
#include "catalog/genbki.h"
#include "nodes/parsenodes.h"
#endif /* BUILD_BKI */

#define PgxcClassRelationId  9001

CATALOG(pgxc_class,9001) BKI_WITHOUT_OIDS
{
	Oid			pcrelid;			/* Table Oid */
	char		pclocatortype;		/* Type of distribution */
	int16		pcattnum;			/* Column number of distribution */
	int16		pchashalgorithm;	/* Hashing algorithm */
	int16		pchashbuckets;		/* Number of buckets */

	/* VARIABLE LENGTH FIELDS: */
	oidvector	nodeoids;			/* List of nodes used by table */
	Oid			pcfuncid;			/* User-defined distribution function oid */
	int2vector	pcfuncattnums;		/* List of column number of distribution */
} FormData_pgxc_class;

typedef FormData_pgxc_class *Form_pgxc_class;

#define Natts_pgxc_class					8

#define Anum_pgxc_class_pcrelid				1
#define Anum_pgxc_class_pclocatortype		2
#define Anum_pgxc_class_pcattnum			3
#define Anum_pgxc_class_pchashalgorithm		4
#define Anum_pgxc_class_pchashbuckets		5
#define Anum_pgxc_class_nodes				6
#define Anum_pgxc_class_pcfuncid			7
#define Anum_pgxc_class_pcfuncattnums		8

typedef enum PgxcClassAlterType
{
	PGXC_CLASS_ALTER_DISTRIBUTION,
	PGXC_CLASS_ALTER_NODES,
	PGXC_CLASS_ALTER_ALL
} PgxcClassAlterType;

extern void PgxcClassCreate(Oid pcrelid,
							char pclocatortype,
							int pcattnum,
							int pchashalgorithm,
							int pchashbuckets,
							int numnodes,
							Oid *nodes,
							Oid pcfuncid,
							int numatts,
							int16 *pcfuncattnums);
extern void PgxcClassAlter(Oid pcrelid,
						   char pclocatortype,
						   int pcattnum,
						   int pchashalgorithm,
						   int pchashbuckets,
						   int numnodes,
						   Oid *nodes,
						   PgxcClassAlterType type,
						   Oid pcfuncid,
						   int numatts,
						   int16 *pcfuncattnums);
extern void RemovePgxcClass(Oid pcrelid);

extern void CreatePgxcClassFuncDepend(char locatortype, Oid relid, Oid funcid);

#endif   /* PGXC_CLASS_H */

