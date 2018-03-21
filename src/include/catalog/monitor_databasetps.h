
#ifndef MONITOR_DATABASETPS_H
#define MONITOR_DATABASETPS_H

#ifdef BUILD_BKI
#include "catalog/buildbki.h"
#else /* BUILD_BKI */
#include "catalog/genbki.h"
#include "utils/timestamp.h"
#define timestamptz int
#endif /* BUILD_BKI */

#define MdatabasetpsRelationId 4814


CATALOG(monitor_databasetps,4814) BKI_WITHOUT_OIDS
{
	timestamptz		monitor_databasetps_time;		/* monitor tps timestamp */
	NameData		monitor_databasetps_dbname;
	int64			monitor_databasetps_tps;
	int64			monitor_databasetps_qps;
	int64			monitor_databasetps_runtime;
	
} FormData_monitor_databasetps;

/* ----------------
 *		Form_monitor_databasetps corresponds to a pointer to a tuple with
 *		the format of monitor_databasetps relation.
 * ----------------
 */
typedef FormData_monitor_databasetps *Form_monitor_databasetps;

#ifndef BUILD_BKI
#undef timestamptz
#endif

/* ----------------
 *		compiler constants for monitor_databasetps
 * ----------------
 */
#define Natts_monitor_databasetps								5
#define Anum_monitor_databasetps_time							1
#define Anum_monitor_databasetps_dbname							2
#define Anum_monitor_databasetps_tps							3
#define Anum_monitor_databasetps_qps							4
#define Anum_monitor_databasetps_runtime						5

#endif /* MONITOR_DATABASETPS_H */
