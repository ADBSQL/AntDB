#include "agent.h"

#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/*#include "plpython.h"*/

#include "agt_msg.h"
#include "agt_utility.h"
#include "mgr/mgr_msg_type.h"
#include "conf_scan.h"
#include "get_uptime.h"
#include "hba_scan.h"
#include "utils/memutils.h"

#undef _POSIX_C_SOURCE
#undef _XOPEN_SOURCE
#undef HAVE_STRERROR
#undef HAVE_TZNAME

#include <Python.h>

#ifdef USE_REPL_SNPRINTF
#undef snprintf
#undef vsnprintf
#endif

bool get_cpu_info(StringInfo hostinfostring);
bool get_mem_info(StringInfo hostinfostring);
bool get_disk_info(StringInfo hostinfostring);
bool get_net_info(StringInfo hostinfostring);
bool get_host_info(StringInfo hostinfostring);
bool get_disk_iops_info(StringInfo hostinfostring);
bool get_system_info(StringInfo hostinfostring);
bool get_platform_type_info(StringInfo hostinfostring);
bool get_cpu_freq(StringInfo hostinfostring);

static void monitor_append_str(StringInfo hostinfostring, char *str);
static void monitor_append_int64(StringInfo hostinfostring, int64 i);
static void monitor_append_float(StringInfo hostinfostring, float f);


/*
 * get cpu info: timestamp and cpu usage.
 * timestamp: string type, for example:201606130951
 * cpu usage: the current system-wide CPU utilization as a percentage
 *            float type, for example 3.24 it means 3.24%
 */
bool get_cpu_info(StringInfo hostinfostring)
{
    PyObject *pModule,*pDict,*pFunc,*pRetValue,*sysPath,*path;
    float cpu_Usage;
    int result;
    char *time_Stamp = NULL;
    char my_exec_path[MAXPGPATH];
    char pghome[MAXPGPATH];

    memset(pghome, 0, MAXPGPATH);

    if (find_my_exec(agent_argv0, my_exec_path) < 0)
        ereport(ERROR, (errmsg("%s: could not locate my own executable path", agent_argv0)));
    get_share_path(my_exec_path, pghome);

    Py_Initialize();
    if (!Py_IsInitialized())
        return false;

    PyRun_SimpleString("import sys");
    PyRun_SimpleString("import psutil");
    PyRun_SimpleString("import time");
    PyRun_SimpleString("ISOTIMEFORMAT = '%Y-%m-%d %H:%M:%S %Z'");

    sysPath = PySys_GetObject("path");
    path = PyString_FromString(pghome);
    if ((result = PyList_Insert(sysPath, 0, path)) != 0)
        ereport(ERROR, (errmsg("can't insert path %s to sysPath", pghome)));

    pModule = PyImport_ImportModule("host_info");
    if (!pModule)
        ereport(ERROR, (errmsg("can't find file host_info.py in path:%s", pghome)));

    pDict = PyModule_GetDict(pModule);
    if (!pDict)
        ereport(ERROR, (errmsg("can't get path for host_info.py")));

    pFunc = PyDict_GetItemString(pDict, "get_cpu_info");
    if (!pFunc || !PyCallable_Check(pFunc))
        ereport(ERROR, (errmsg("can't find function get_cpu_info in file host_info.py")));

    pRetValue = PyObject_CallObject(pFunc, NULL);
    PyArg_ParseTuple(pRetValue, "sf", &time_Stamp,&cpu_Usage);
    monitor_append_str(hostinfostring, time_Stamp);
    monitor_append_float(hostinfostring, cpu_Usage);
    get_cpu_freq(hostinfostring);

    Py_DECREF(pModule);
    Py_DECREF(pRetValue);
    Py_DECREF(pFunc);
    Py_Finalize();

    return true;
}

/*
 * get memory info:timestamp, memory Total, memory Used and memory Usage.
 * timestamp:string type, for example:201606130951
 * memory Total: total physical memory available (in Bytes).
 * memory Used: memory used (in Bytes).
 * memory Usage: the percentage usage calculated as (total - available) / total * 100
 *               float type, for example 3.24 it means 3.24%
 */
bool get_mem_info(StringInfo hostinfostring)
{
    PyObject *pModule,*pDict,*pFunc,*pRetValue,*sysPath,*path;
    char *time_Stamp = NULL;
    float mem_Usage;
    int64 mem_Total, mem_Used;
    int result;
    char my_exec_path[MAXPGPATH];
    char pghome[MAXPGPATH];

    memset(pghome, 0, MAXPGPATH);

    if (find_my_exec(agent_argv0, my_exec_path) < 0)
        ereport(ERROR, (errmsg("%s: could not locate my own executable path", agent_argv0)));
    get_share_path(my_exec_path, pghome);

    Py_Initialize();
    if (!Py_IsInitialized())
        return false;

    PyRun_SimpleString("import sys");
    PyRun_SimpleString("import psutil");
    PyRun_SimpleString("import time");
    PyRun_SimpleString("ISOTIMEFORMAT = '%Y-%m-%d %H:%M:%S %Z'");

    sysPath = PySys_GetObject("path");
    path = PyString_FromString(pghome);
    if ((result = PyList_Insert(sysPath, 0, path)) != 0)
        ereport(ERROR, (errmsg("can't insert path %s to sysPath", pghome)));

    pModule = PyImport_ImportModule("host_info");
    if (!pModule)
        ereport(ERROR, (errmsg("can't find file host_info.py in path:%s", pghome)));

    pDict = PyModule_GetDict(pModule);
    if (!pDict)
        ereport(ERROR, (errmsg("can't get path for host_info.py")));

    pFunc = PyDict_GetItemString(pDict, "get_mem_info");
    if (!pFunc || !PyCallable_Check(pFunc))
        ereport(ERROR, (errmsg("can't find function get_mem_info in file host_info.py")));

    pRetValue = PyObject_CallObject(pFunc, NULL);
    PyArg_ParseTuple(pRetValue, "sllf", &time_Stamp,&mem_Total,&mem_Used,&mem_Usage);
    monitor_append_str(hostinfostring, time_Stamp);
    monitor_append_int64(hostinfostring, mem_Total);
    monitor_append_int64(hostinfostring, mem_Used);
    monitor_append_float(hostinfostring, mem_Usage);

    Py_DECREF(pModule);
    Py_DECREF(pRetValue);
    Py_DECREF(pFunc);
    Py_Finalize();

    return true;
}

/*
 * get disk info:timestamp, disk_Read_Bytes, disk_Read_Time,
 *               disk_Write_Bytes, disk_Write_Time,disk_Total,disk_Used.
 * timestamp:string type, for example:201606130951.
 * disk_Read_Bytes: number of reads (in Bytes).
 * disk_Read_Time: time spent reading from disk (in milliseconds).
 * disk_Write_Bytes: number of writes (in Bytes).
 * disk_Write_Time: time spent writing to disk (in milliseconds).
 * disk_Total: total physical disk available (in Bytes).
 * disk_Used: disk used (in Bytes).
 */
bool get_disk_info(StringInfo hostinfostring)
{
    PyObject *pModule,*pDict,*pFunc,*pRetValue,*sysPath,*path;
    char *time_Stamp = NULL;
    int64 disk_Read_Bytes, disk_Read_Time,
          disk_Write_Bytes, disk_Write_Time,
          disk_Total, disk_Used;
    int result;
    char my_exec_path[MAXPGPATH];
    char pghome[MAXPGPATH];

    memset(pghome, 0, MAXPGPATH);

    if (find_my_exec(agent_argv0, my_exec_path) < 0)
        ereport(ERROR, (errmsg("%s: could not locate my own executable path", agent_argv0)));
    get_share_path(my_exec_path, pghome);

    Py_Initialize();
    if (!Py_IsInitialized())
        return false;

    PyRun_SimpleString("import sys");
    PyRun_SimpleString("import psutil");
    PyRun_SimpleString("import time");
    PyRun_SimpleString("ISOTIMEFORMAT = '%Y-%m-%d %H:%M:%S %Z'");

    sysPath = PySys_GetObject("path");
    path = PyString_FromString(pghome);
    if ((result = PyList_Insert(sysPath, 0, path)) != 0)
        ereport(ERROR, (errmsg("can't insert path %s to sysPath", pghome)));

    pModule = PyImport_ImportModule("host_info");
    if (!pModule)
        ereport(ERROR, (errmsg("can't find file host_info.py in path:%s", pghome)));

    pDict = PyModule_GetDict(pModule);
    if (!pDict)
        ereport(ERROR, (errmsg("can't get path for host_info.py")));

    pFunc = PyDict_GetItemString(pDict, "get_disk_info");
    if (!pFunc || !PyCallable_Check(pFunc))
        ereport(ERROR, (errmsg("can't find function get_disk_info in file host_info.py.")));

    pRetValue = PyObject_CallObject(pFunc, NULL);
    PyArg_ParseTuple(pRetValue, "sllllll", &time_Stamp, &disk_Read_Bytes, &disk_Read_Time,
                                           &disk_Write_Bytes, &disk_Write_Time,
                                           &disk_Total, &disk_Used);

    monitor_append_str(hostinfostring, time_Stamp);
    monitor_append_int64(hostinfostring, disk_Read_Bytes);
    monitor_append_int64(hostinfostring, disk_Read_Time);
    monitor_append_int64(hostinfostring, disk_Write_Bytes);
    monitor_append_int64(hostinfostring, disk_Write_Time);
    monitor_append_int64(hostinfostring, disk_Total);
    monitor_append_int64(hostinfostring, disk_Used);

    Py_DECREF(pModule);
    Py_DECREF(pRetValue);
    Py_DECREF(pFunc);
    Py_Finalize();

    return true;
}

/*
 * get network info: system-wide network I/O statistics
 *                   timestamp, sent_speed and recv_speed.
 * timestamp: string type, for example:201606130951
 * sent_Speed: the network to sent data rate (in bytes/s).
 * recv_Speed: the network to recv data rate (in bytes/s).
 */
bool get_net_info(StringInfo hostinfostring)
{
    PyObject *pModule,*pDict,*pFunc,*pRetValue,*sysPath,*path;
    int64 sent_Speed,recv_Speed;
    int result;
    char *time_Stamp = NULL;
    char my_exec_path[MAXPGPATH];
    char pghome[MAXPGPATH];
    
    memset(pghome, 0, MAXPGPATH);
    
    if (find_my_exec(agent_argv0, my_exec_path) < 0)
        ereport(ERROR, (errmsg("%s: could not locate my own executable path.", agent_argv0)));
    get_share_path(my_exec_path, pghome);

    Py_Initialize();
    if (!Py_IsInitialized())
        return false;

    PyRun_SimpleString("import sys");
    PyRun_SimpleString("import psutil");
    PyRun_SimpleString("import time");
    PyRun_SimpleString("ISOTIMEFORMAT = '%Y-%m-%d %H:%M:%S %Z'");

    sysPath = PySys_GetObject("path");
    path = PyString_FromString(pghome);
    if ((result = PyList_Insert(sysPath, 0, path)) != 0)
        ereport(ERROR, (errmsg("can't insert path %s to sysPath.", pghome)));

    pModule = PyImport_ImportModule("host_info");
    if (!pModule)
        ereport(ERROR, (errmsg("can't find file host_info.py in path:%s.", pghome)));

    pDict = PyModule_GetDict(pModule);
    if ( !pDict )
        ereport(ERROR, (errmsg("can't get path for host_info.py")));

    pFunc = PyDict_GetItemString(pDict, "get_net_info");
    if (!pFunc || !PyCallable_Check(pFunc))
        ereport(ERROR, (errmsg("can't find function get_net_info in file host_info.py.")));

    pRetValue = PyObject_CallObject(pFunc, NULL);
    PyArg_ParseTuple(pRetValue, "sll", &time_Stamp,&sent_Speed,&recv_Speed);
    monitor_append_str(hostinfostring, time_Stamp);
    monitor_append_int64(hostinfostring,sent_Speed);
    monitor_append_int64(hostinfostring,recv_Speed);

    Py_DECREF(pModule);
    Py_DECREF(pRetValue);
    Py_DECREF(pFunc);
    Py_Finalize();

    return true;
}

bool get_host_info(StringInfo hostinfostring)
{
    time_t seconds_since_boot;
    int cpu_cores_total, cpu_cores_available;

    cpu_cores_total = sysconf(_SC_NPROCESSORS_CONF);
    cpu_cores_available = sysconf(_SC_NPROCESSORS_ONLN);
    monitor_append_int64(hostinfostring, cpu_cores_total);
    monitor_append_int64(hostinfostring, cpu_cores_available);

    seconds_since_boot = get_uptime();
    monitor_append_int64(hostinfostring, seconds_since_boot);

    return true;
}

bool get_disk_iops_info(StringInfo hostinfostring)
{
    FILE *fstream=NULL;
    char cmd[MAXPGPATH],
         cmd_output[MAXPGPATH];

    memset(cmd,0,sizeof(cmd));
    snprintf(cmd,sizeof(cmd),"iostat  -x -d | grep -v -i -E \"linux|device|^$\"|awk '{sum += $4+$5 } END {print sum}'");
    if(NULL == (fstream=popen(cmd,"r")))
    {
        ereport(ERROR, (errmsg("execute command failed: %s", strerror(errno))));
        return false;
    }
    if(NULL != fgets(cmd_output, sizeof(cmd_output), fstream))
    {
         monitor_append_float(hostinfostring,(float)atof(cmd_output));
    }
    else
    {
	monitor_append_float(hostinfostring, 0);
        pclose(fstream);
        return false;
    }
    pclose(fstream);
    return true;
}

bool get_system_info(StringInfo hostinfostring)
{
    FILE *fstream=NULL;
    char cmd[MAXPGPATH],
         cmd_output[MAXPGPATH];

    memset(cmd, 0, sizeof(cmd));
    snprintf(cmd, sizeof(cmd), "lsb_release -d | awk 'BEGIN { FS=\":\"} { print $2}' | sed 's/^[ \t]*//g'");
    if(NULL == (fstream=popen(cmd, "r")))
    {
        ereport(ERROR, (errmsg("execute command failed: %s", strerror(errno))));
        return false;
    }
    if(NULL != fgets(cmd_output, sizeof(cmd_output), fstream))
    {
        cmd_output[strlen(cmd_output) - 1] = 0;
        monitor_append_str(hostinfostring, cmd_output);
    }
    else
    {
	monitor_append_str(hostinfostring, "unknown operating system version");
        pclose(fstream);
        return false;
    }
    pclose(fstream);
    return true;
}

bool get_platform_type_info(StringInfo hostinfostring)
{
    FILE *fstream=NULL;
    char cmd[MAXPGPATH],
         cmd_output[MAXPGPATH];

    memset(cmd, 0, sizeof(cmd));
    snprintf(cmd, sizeof(cmd), "uname -m");
    if(NULL == (fstream=popen(cmd, "r")))
    {
        ereport(ERROR, (errmsg("execute command failed: %s", strerror(errno))));
        return false;
    }
    if(NULL != fgets(cmd_output, sizeof(cmd_output), fstream))
    {
        cmd_output[strlen(cmd_output) - 1] = 0;
        monitor_append_str(hostinfostring, cmd_output);
    }
    else
    {
        monitor_append_str(hostinfostring, "nnknown operating system architecture");
        pclose(fstream);
        return false;
    }
    pclose(fstream);
    return true;
}

bool get_cpu_freq(StringInfo hostinfostring)
{
    FILE *fstream=NULL;
    char cmd[MAXPGPATH],
         cmd_output[MAXPGPATH];

    memset(cmd, 0, sizeof(cmd));
    snprintf(cmd, sizeof(cmd), "cat /proc/cpuinfo | grep GHz | uniq -c | awk ' BEGIN { FS=\"@\"} { print $2}' | sed 's/^[ \t]*//g'");
    if(NULL == (fstream=popen(cmd, "r")))
    {
        ereport(ERROR, (errmsg("execute command failed: %s", strerror(errno))));
        return false;
    }
    if(NULL != fgets(cmd_output, sizeof(cmd_output), fstream))
    {
        cmd_output[strlen(cmd_output) - 1] = 0;
        monitor_append_str(hostinfostring, cmd_output);
    }
    else
    {
        pclose(fstream);
        return false;
    }
    pclose(fstream);
    return true;
}

static void monitor_append_str(StringInfo hostinfostring, char *str)
{
    Assert(str != NULL && &(hostinfostring->data) != NULL);
    appendStringInfoString(hostinfostring, str);
    appendStringInfoCharMacro(hostinfostring, '\0');
}

static void monitor_append_int64(StringInfo hostinfostring, int64 i)
{
    appendStringInfo(hostinfostring, INT64_FORMAT, i);
    appendStringInfoCharMacro(hostinfostring, '\0');
}

static void monitor_append_float(StringInfo hostinfostring, float f)
{
    appendStringInfo(hostinfostring, "%0.2f", f);
    appendStringInfoCharMacro(hostinfostring, '\0');
}
