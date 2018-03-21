
#ifndef MONITOR_DISK_H
#define MONITOR_DISK_H

#ifdef BUILD_BKI
#include "catalog/buildbki.h"
#else /* BUILD_BKI */
#include "catalog/genbki.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "utils/portal.h"
#include "utils/timestamp.h"
#define timestamptz int
#endif /* BUILD_BKI */

#define MonitorDiskRelationId 4809

CATALOG(monitor_disk,4809)
{
    NameData    hostname;           /* host name */
    timestamptz md_timestamptz;     /* monitor disk timestamp */
    int64       md_total;           /* monitor disk total size */
    int64       md_used;       /* monitor disk available size */
    int64       md_io_read_bytes;   /* monitor disk i/o read bytes */
    int64       md_io_read_time;    /* monitor disk i/o read time */
    int64       md_io_write_bytes;  /* monitor disk i/o write bytes */
    int64       md_io_write_time;   /* monitor disk i/o wirte time */
} FormData_monitor_disk;

#ifndef BUILD_BKI
#undef timestamptz
#endif

/* ----------------
 *      Form_monitor_disk corresponds to a pointer to a tuple with
 *      the format of moniotr_disk relation.
 * ----------------
 */
typedef FormData_monitor_disk *Form_monitor_disk;

/* ----------------
 *      compiler constants for monitor_disk
 * ----------------
 */
#define Natts_monitor_disk                          8
#define Anum_monitor_disk_host_name                 1
#define Anum_monitor_disk_md_timestamptz            2
#define Anum_monitor_disk_md_total                  3
#define Anum_monitor_disk_md_used                   4
#define Anum_monitor_disk_md_io_read_bytes          5
#define Anum_monitor_disk_md_io_reat_time           6
#define Anum_monitor_disk_md_io_write_bytes         7
#define Anum_monitor_disk_md_io_write_time          8

#endif /* MONITOR_DISK_H */
