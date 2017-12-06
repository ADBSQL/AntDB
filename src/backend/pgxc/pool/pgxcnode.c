/*-------------------------------------------------------------------------
 *
 * pgxcnode.c
 *
 *	  Functions for the Coordinator communicating with the PGXC nodes:
 *	  Datanodes and Coordinators
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 * Portions Copyright (c) 2014-2017, ADB Development Group
 *
 * IDENTIFICATION
 *	  src/backend/pgxc/pool/pgxcnode.c
 *
 *
 *-------------------------------------------------------------------------
 */
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "postgres.h"
#include "miscadmin.h" /* fro CHECK_FOR_INTERRUPTS */

#include "access/transam.h"
#include "access/xact.h"
#include "agtm/agtm.h"
#include "catalog/pg_type.h"
#include "catalog/pg_collation.h"
#include "catalog/pgxc_node.h"
#include "commands/prepare.h"
#include "libpq/libpq-fe.h"
#include "libpq/libpq-int.h"
#include "nodes/nodes.h"
#include "pgxc/pause.h"
#include "pgxc/pgxcnode.h"
#include "pgxc/execRemote.h"
#include "pgxc/locator.h"
#include "pgxc/nodemgr.h"
#include "pgxc/pgxc.h"
#include "pgxc/poolmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "tcop/dest.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/fmgroids.h"
#include "utils/formatting.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#ifdef ADB
#include "pgxc/pause.h"
#endif

#define CMD_ID_MSG_LEN 8
#define PGXC_CANCEL_DELAY 15

/* Number of connections held */
static int	datanode_count = 0;
static int	coord_count = 0;

/*
 * Datanode handles saved in Transaction memory context
 * when PostgresMain is launched.
 * Those handles are used inside a transaction by Coordinator to Datanodes.
 */
static PGXCNodeHandle *dn_handles = NULL;

/*
 * Coordinator handles saved in Transaction memory context
 * when PostgresMain is launched.
 * Those handles are used inside a transaction by Coordinator to Coordinators
 */
static PGXCNodeHandle *co_handles = NULL;

/* Current size of dn_handles and co_handles */
volatile int NumDataNodes;
volatile int NumCoords;

/* Cancel Delay Duration -> set by GUC */
int			pgxcnode_cancel_delay = 10;

bool		enable_node_tcp_log;
extern int	MyProcPid;

static void pgxc_node_init(PGXCNodeHandle *handle, int sock);
static void pgxc_node_free(PGXCNodeHandle *handle, bool freebuf);
static void pgxc_node_all_free(void);

static int	get_int(PGXCNodeHandle * conn, size_t len, int *out);
static int	get_char(PGXCNodeHandle * conn, char *out);
/*
 * Initialize PGXCNodeHandle struct
 */
static void
init_pgxc_handle(PGXCNodeHandle *pgxc_handle)
{
	Assert(pgxc_handle);
	MemSet(pgxc_handle->name.data, 0, NAMEDATALEN);

#ifdef DEBUG_ADB
	MemSet(pgxc_handle->last_query, 0, DEBUG_BUF_SIZE);
#endif

	/*
	 * Socket descriptor is small non-negative integer,
	 * Indicate the handle is not initialized yet
	 */
	pgxc_handle->sock = NO_SOCKET;

	/* Initialise buffers */
	pgxc_handle->error = NULL;
	pgxc_handle->outSize = 16 * 1024;
	pgxc_handle->outBuffer = (char *) MemoryContextAllocZero(TopMemoryContext, pgxc_handle->outSize);
	pgxc_handle->inSize = 16 * 1024;
	pgxc_handle->inBuffer = (char *) MemoryContextAllocZero(TopMemoryContext, pgxc_handle->inSize);
	pgxc_handle->combiner = NULL;
	pgxc_handle->inStart = 0;
	pgxc_handle->inEnd = 0;
	pgxc_handle->inCursor = 0;
	pgxc_handle->outEnd = 0;
	pgxc_handle->file_data = NULL;

	if (pgxc_handle->outBuffer == NULL || pgxc_handle->inBuffer == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
}

/*
 * Allocate and initialize memory to store Datanode and Coordinator handles.
 */
void
InitMultinodeExecutor(bool is_force)
{
	int				count;
	Oid				*coOids = NULL;
	Oid				*dnOids = NULL;
	char			*nodeName = NULL;

	/* Free all the existing information first */
	if (is_force)
		pgxc_node_all_free();

	/* This function could get called multiple times because of sigjmp */
	if (dn_handles != NULL &&
		co_handles != NULL)
		return;

	/* Update node table in the shared memory */
	PgxcNodeListAndCount();

	/* Get classified list of node Oids */
	PgxcNodeGetOids(&coOids, &dnOids, (int*)&NumCoords, (int*)&NumDataNodes, true);

	/* Do proper initialization of handles */
	if (NumDataNodes > 0)
		dn_handles = (PGXCNodeHandle *) MemoryContextAllocZero(TopMemoryContext,
			NumDataNodes * sizeof(PGXCNodeHandle));
	if (NumCoords > 0)
		co_handles = (PGXCNodeHandle *) MemoryContextAllocZero(TopMemoryContext,
			NumCoords * sizeof(PGXCNodeHandle));

	if ((!dn_handles && NumDataNodes > 0) ||
		(!co_handles && NumCoords > 0))
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory for node handles")));

	/* Initialize new empty slots */
	for (count = 0; count < NumDataNodes; count++)
	{
		init_pgxc_handle(&dn_handles[count]);
		dn_handles[count].nodeoid = dnOids[count];
		dn_handles[count].type = PGXC_NODE_DATANODE;
		nodeName = get_pgxc_nodename(dn_handles[count].nodeoid);
		namestrcpy(&(dn_handles[count].name), nodeName);
		pfree(nodeName);
	}
	for (count = 0; count < NumCoords; count++)
	{
		init_pgxc_handle(&co_handles[count]);
		co_handles[count].nodeoid = coOids[count];
		co_handles[count].type = PGXC_NODE_COORDINATOR;
		nodeName = get_pgxc_nodename(co_handles[count].nodeoid);
		namestrcpy(&(co_handles[count].name), nodeName);
		pfree(nodeName);
	}

	safe_pfree(coOids);
	safe_pfree(dnOids);

	datanode_count = 0;
	coord_count = 0;
	PGXCNodeId = 0;

	/* Finally determine which is the node-self */
	for (count = 0; count < NumCoords; count++)
	{
		if (pg_strcasecmp(PGXCNodeName, NameStr(co_handles[count].name)) == 0)
		{
			PGXCNodeId = count + 1;
			PGXCNodeOid = co_handles[count].nodeoid;
			break;
		}
	}

	/*
	 * No node-self?
	 * PGXCTODO: Change error code
	 */
	if (PGXCNodeId == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("Coordinator cannot identify itself")));
}


/*
 * Builds up a connection string
 */
char *
PGXCNodeConnStr(char *host, int port, char *dbname,
				char *user, char *pgoptions, char *remote_type)
{
	return psprintf("host=%s port=%d dbname=%s user=%s application_name=pgxc"
				   " options='-c grammar=postgres -c remotetype=%s %s'",
				   host, port, dbname, user, remote_type, pgoptions);
}


/*
 * Connect to a Datanode using a connection string
 */
NODE_CONNECTION *
PGXCNodeConnect(char *connstr)
{
	PGconn	   *conn;

	/* Delegate call to the pglib */
	conn = PQconnectdb(connstr);
	return (NODE_CONNECTION *) conn;
}


/*
 * Close specified connection
 */
void
PGXCNodeClose(NODE_CONNECTION *conn)
{
	/* Delegate call to the pglib */
	PQfinish((PGconn *) conn);
}

/*
 * Send SET query to given connection.
 * Query is sent asynchronously and results are consumed
 */
int
PGXCNodeSendSetQuery(NODE_CONNECTION *conn, const char *sql_command)
{
	PGresult	*result;
	int res;

	if (!PQsendQuery((PGconn *) conn, sql_command))
		return -1;

	/* Consume results from SET commands */
	res = 0;
	while ((result = PQgetResult((PGconn *) conn)) != NULL)
	{
		if(PQresultStatus(result) == PGRES_FATAL_ERROR)
			res = 1;
		PQclear(result);
	}

	return res;
}

/*
 * Checks if connection active
 */
int
PGXCNodeConnected(NODE_CONNECTION *conn)
{
	/* Delegate call to the pglib */
	PGconn	   *pgconn = (PGconn *) conn;

	/*
	 * Simple check, want to do more comprehencive -
	 * check if it is ready for guery
	 */
	return pgconn && PQstatus(pgconn) == CONNECTION_OK;
}



/* Close the socket handle (this process' copy) and free occupied memory
 *
 * Note that we do not free the handle and its members. This will be
 * taken care of when the transaction ends, when TopTransactionContext
 * is destroyed in xact.c.
 */
static void
pgxc_node_free(PGXCNodeHandle *handle, bool freebuf)
{
	close(handle->sock);
	handle->sock = NO_SOCKET;
	handle->state = DN_CONNECTION_STATE_IDLE;
	handle->combiner = NULL;
	FreeHandleError(handle);
	if (freebuf)
	{
		safe_pfree(handle->outBuffer);
		safe_pfree(handle->inBuffer);
	}
	if(handle->file_data)
	{
		char file_name[20];
		sprintf(file_name, "%06d-%06d.bin", MyProcPid, handle->sock);
		FreeFile(handle->file_data);
		handle->file_data = NULL;
		unlink(file_name);
	}
}

/*
 * Free all the node handles cached
 */
static void
pgxc_node_all_free(void)
{
	if(dn_handles)
	{
		Assert(NumDataNodes > 0);
		while(NumDataNodes > 0)
		{
			--NumDataNodes;
			pgxc_node_free(&dn_handles[NumDataNodes], true);
		}
		pfree(dn_handles);
		dn_handles = NULL;
	}
	if(co_handles)
	{
		Assert(NumCoords > 0);
		while(NumCoords > 0)
		{
			--NumCoords;
			pgxc_node_free(&co_handles[NumCoords], true);
		}
		pfree(co_handles);
		co_handles = NULL;
	}
}

/*
 * Create and initialise internal structure to communicate to
 * Datanode via supplied socket descriptor.
 * Structure stores state info and I/O buffers
 */
static void
pgxc_node_init(PGXCNodeHandle *handle, int sock)
{
	handle->sock = sock;
	handle->transaction_status = 'I';
	handle->state = DN_CONNECTION_STATE_IDLE;
	handle->combiner = NULL;
#ifdef DN_CONNECTION_DEBUG
	handle->have_row_desc = false;
#endif
	handle->error = NULL;
	handle->outEnd = 0;
	handle->inStart = 0;
	handle->inEnd = 0;
	handle->inCursor = 0;
	if(enable_node_tcp_log)
	{
		char file_name[20];
		sprintf(file_name, "%06d-%06d.bin", MyProcPid, sock);
		handle->file_data = AllocateFile(file_name, "wb");
	}
}


/*
 * Wait while at least one of specified connections has data available and read
 * the data into the buffer
 */
bool
pgxc_node_receive(const int conn_count,
				  PGXCNodeHandle ** connections, struct timeval * timeout)
{
#define ERROR_OCCURED		true
#define NO_ERROR_OCCURED	false
	int			i,
				res_select,
				nfds = 0;
	fd_set			readfds;
	bool			is_msg_buffered;

	FD_ZERO(&readfds);

	is_msg_buffered = false;
	for (i = 0; i < conn_count; i++)
	{
		/* If connection has a buffered message */
		if (HAS_MESSAGE_BUFFERED(connections[i]))
		{
			is_msg_buffered = true;
			break;
		}
	}

	for (i = 0; i < conn_count; i++)
	{
		/* If connection finished sending do not wait input from it */
		if (connections[i]->state == DN_CONNECTION_STATE_IDLE || HAS_MESSAGE_BUFFERED(connections[i]))
			continue;

		/* prepare select params */
		if (connections[i]->sock > 0)
		{
			FD_SET(connections[i]->sock, &readfds);
			nfds = connections[i]->sock;
		}
		else
		{
			/* flag as bad, it will be removed from the list */
			connections[i]->state = DN_CONNECTION_STATE_ERROR_FATAL;
		}
	}

	/*
	 * Return if we do not have connections to receive input
	 */
	if (nfds == 0)
	{
		if (is_msg_buffered)
			return NO_ERROR_OCCURED;
		return ERROR_OCCURED;
	}

retry:
	res_select = select(nfds + 1, &readfds, NULL, NULL, timeout);
	if (res_select < 0)
	{
		/* error - retry if EINTR or EAGAIN */
		if (errno == EINTR || errno == EAGAIN)
			goto retry;

		if (errno == EBADF)
		{
			elog(WARNING, "select() bad file descriptor set");
		}
		elog(WARNING, "select() error: %d", errno);
		if (errno)
			return ERROR_OCCURED;
		return NO_ERROR_OCCURED;
	}

	if (res_select == 0)
	{
		/* Handle timeout */
		elog(WARNING, "timeout while waiting for response");
		return ERROR_OCCURED;
	}

	/* read data */
	for (i = 0; i < conn_count; i++)
	{
		PGXCNodeHandle *conn = connections[i];

		if (FD_ISSET(conn->sock, &readfds))
		{
			int	read_status = pgxc_node_read_data(conn, true);

			if (read_status == EOF || read_status < 0)
			{
				/* Can not read - no more actions, just discard connection */
				conn->state = DN_CONNECTION_STATE_ERROR_FATAL;
				add_error_message(conn,
					"unexpected EOF on datanode %s's connection", NameStr(conn->name));
				elog(WARNING,
					"unexpected EOF on datanode %s's connection", NameStr(conn->name));
				/* Should we read from the other connections before returning? */
				return ERROR_OCCURED;
			}
		}
	}
	return NO_ERROR_OCCURED;
}

/*
 * Is there any data enqueued in the TCP input buffer waiting
 * to be read sent by the PGXC node connection
 */
int
pgxc_node_is_data_enqueued(PGXCNodeHandle *conn)
{
	int ret;
	int enqueued;

	if (conn->sock < 0)
		return 0;
	ret = ioctl(conn->sock, FIONREAD, &enqueued);
	if (ret != 0)
		return 0;

	return enqueued;
}

/*
 * Read up incoming messages from the PGXC node connection
 */
int
pgxc_node_read_data(PGXCNodeHandle *conn, bool close_if_error)
{
	int			someread = 0;
	int			nread;

	if (conn->sock < 0)
	{
		if (close_if_error)
			add_error_message(conn, "bad socket");
		return EOF;
	}

	/* Left-justify any data in the buffer to make room */
	if (conn->inStart < conn->inEnd)
	{
		if (conn->inStart > 0)
		{
			memmove(conn->inBuffer, conn->inBuffer + conn->inStart,
					conn->inEnd - conn->inStart);
			conn->inEnd -= conn->inStart;
			conn->inCursor -= conn->inStart;
			conn->inStart = 0;
		}
	}
	else
	{
		/* buffer is logically empty, reset it */
		conn->inStart = conn->inCursor = conn->inEnd = 0;
	}

	/*
	 * If the buffer is fairly full, enlarge it. We need to be able to enlarge
	 * the buffer in case a single message exceeds the initial buffer size. We
	 * enlarge before filling the buffer entirely so as to avoid asking the
	 * kernel for a partial packet. The magic constant here should be large
	 * enough for a TCP packet or Unix pipe bufferload.  8K is the usual pipe
	 * buffer size, so...
	 */
	if (conn->inSize - conn->inEnd < 8192)
	{
		if (ensure_in_buffer_capacity(conn->inEnd + (size_t) 8192, conn) != 0)
		{
			/*
			 * We don't insist that the enlarge worked, but we need some room
			 */
			if (conn->inSize - conn->inEnd < 100)
			{
				if (close_if_error)
					add_error_message(conn, "can not allocate buffer");
				return -1;
			}
		}
	}

retry:
	nread = recv(conn->sock, conn->inBuffer + conn->inEnd,
				 conn->inSize - conn->inEnd, 0);

	if (nread < 0)
	{
		if (close_if_error)
			elog(DEBUG1, "dnrd errno = %d", errno);
		if (errno == EINTR)
			goto retry;
		/* Some systems return EAGAIN/EWOULDBLOCK for no data */
#ifdef EAGAIN
		if (errno == EAGAIN)
			return someread;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
		if (errno == EWOULDBLOCK)
			return someread;
#endif
		/* We might get ECONNRESET here if using TCP and backend died */
#ifdef ECONNRESET
		if (errno == ECONNRESET)
		{
			/*
			 * OK, we are getting a zero read even though select() says ready. This
			 * means the connection has been closed.  Cope.
			 */
			if (close_if_error)
			{
				add_error_message(conn,
					"Datanode %s closed the connection unexpectedly\n"
					"\tThis probably means the Datanode terminated abnormally\n"
					"\tbefore or while processing the request.",
					NameStr(conn->name));
				conn->state = DN_CONNECTION_STATE_ERROR_FATAL;	/* No more connection to
															* backend */
				closesocket(conn->sock);
				conn->sock = NO_SOCKET;
			}
			return -1;
		}
#endif
		if (close_if_error)
		{
			add_error_message(conn,
				"Could not receive data from server %s", NameStr(conn->name));

			conn->state = DN_CONNECTION_STATE_ERROR_FATAL;	/* No more connection to
															* backend */
			closesocket(conn->sock);
			conn->sock = NO_SOCKET;
		}
		return -1;

	}

	if (nread > 0)
	{
		if(conn->file_data)
		{
			bool bwrite = false;
			fwrite(&bwrite, sizeof(bwrite), 1, conn->file_data); 
			fwrite(&nread, sizeof(nread), 1, conn->file_data);
			fwrite(conn->inBuffer+conn->inEnd, 1, nread, conn->file_data);
		}
		conn->inEnd += nread;

		/*
		 * Hack to deal with the fact that some kernels will only give us back
		 * 1 packet per recv() call, even if we asked for more and there is
		 * more available.	If it looks like we are reading a long message,
		 * loop back to recv() again immediately, until we run out of data or
		 * buffer space.  Without this, the block-and-restart behavior of
		 * libpq's higher levels leads to O(N^2) performance on long messages.
		 *
		 * Since we left-justified the data above, conn->inEnd gives the
		 * amount of data already read in the current message.	We consider
		 * the message "long" once we have acquired 32k ...
		 */
		if (conn->inEnd > 32768 &&
			(conn->inSize - conn->inEnd) >= 8192)
		{
			someread = 1;
			goto retry;
		}
		return 1;
	}

	if (nread == 0)
	{
		if (close_if_error)
		{
			elog(DEBUG1, "nread returned 0");
			conn->state = DN_CONNECTION_STATE_ERROR_FATAL;	/* No more connection to
															* backend */
			closesocket(conn->sock);
			conn->sock = NO_SOCKET;
		}
		return -1;
	}

	if (someread)
		return 1;				/* got a zero read after successful tries */

	return 0;
}


/*
 * Get one character from the connection buffer and advance cursor
 */
static int
get_char(PGXCNodeHandle * conn, char *out)
{
	if (conn->inCursor < conn->inEnd)
	{
		*out = conn->inBuffer[conn->inCursor++];
		return 0;
	}
	return EOF;
}

/*
 * Read an integer from the connection buffer and advance cursor
 */
static int
get_int(PGXCNodeHandle *conn, size_t len, int *out)
{
	unsigned short tmp2;
	unsigned int tmp4;

	if (conn->inCursor + len > conn->inEnd)
		return EOF;

	switch (len)
	{
		case 2:
			memcpy(&tmp2, conn->inBuffer + conn->inCursor, 2);
			conn->inCursor += 2;
			*out = (int) ntohs(tmp2);
			break;
		case 4:
			memcpy(&tmp4, conn->inBuffer + conn->inCursor, 4);
			conn->inCursor += 4;
			*out = (int) ntohl(tmp4);
			break;
		default:
			add_error_message(conn, "not supported int size");
			return EOF;
	}

	return 0;
}


/*
 * get_message
 * If connection has enough data read entire message from the connection buffer
 * and returns message type. Message data and data length are returned as
 * var parameters.
 * If buffer does not have enough data leaves cursor unchanged, changes
 * connection status to DN_CONNECTION_STATE_QUERY indicating it needs to
 * receive more and returns \0
 * conn - connection to read from
 * len - returned length of the data where msg is pointing to
 * msg - returns pointer to memory in the incoming buffer. The buffer probably
 * will be overwritten upon next receive, so if caller wants to refer it later
 * it should make a copy.
 */
char
get_message(PGXCNodeHandle *conn, int *len, char **msg)
{
	char 		msgtype;

	if (get_char(conn, &msgtype) || get_int(conn, 4, len))
	{
		/* Successful get_char would move cursor, restore position */
		conn->inCursor = conn->inStart;
		return '\0';
	}

	*len -= 4;

	if (conn->inCursor + *len > conn->inEnd)
	{
		/*
		 * Not enough data in the buffer, we should read more.
		 * Reading function will discard already consumed data in the buffer
		 * till conn->inBegin. Then we want the message that is partly in the
		 * buffer now has been read completely, to avoid extra read/handle
		 * cycles. The space needed is 1 byte for message type, 4 bytes for
		 * message length and message itself which size is currently in *len.
		 * The buffer may already be large enough, in this case the function
		 * ensure_in_buffer_capacity() will immediately return
		 */
		ensure_in_buffer_capacity(5 + (size_t) *len, conn);
		conn->inCursor = conn->inStart;
		return '\0';
	}

	*msg = conn->inBuffer + conn->inCursor;
	conn->inCursor += *len;
	conn->inStart = conn->inCursor;
	return msgtype;
}

/*
 * Release all Datanode and Coordinator connections
 * back to pool and release occupied memory
 */
void
release_handles(void)
{
	return release_handles2(false);
}

void release_handles2(bool force_close)
{
	int			i;
	bool has_error = false;

#ifdef ADB
	/* don't free connection if holding a cluster lock */
	if (cluster_ex_lock_held)
	{
		return;
	}
#endif

	if (datanode_count == 0 && coord_count == 0)
		return;

	/* Do not release connections if we have prepared statements on nodes */
	if (HaveActiveDatanodeStatements())
		return;

	/* Free Datanodes handles */
	for (i = 0; i < NumDataNodes; i++)
	{
		PGXCNodeHandle *handle = &dn_handles[i];

		if (handle->sock != NO_SOCKET)
		{
			if (handle->state != DN_CONNECTION_STATE_IDLE)
			{
				has_error = true;
				elog(DEBUG1,
					"Connection to Datanode %s has unexpected state %d and will be dropped",
					 NameStr(handle->name), handle->state);
			}
			pgxc_node_free(handle, false);
		}
	}

	/* Collect Coordinator handles */
	for (i = 0; i < NumCoords; i++)
	{
		PGXCNodeHandle *handle = &co_handles[i];

		if (handle->sock != NO_SOCKET)
		{
			if (handle->state != DN_CONNECTION_STATE_IDLE)
			{
				has_error = true;
				elog(DEBUG1,
					"Connection to Coordinator %s has unexpected state %d and will be dropped",
					 NameStr(handle->name), handle->state);
			}
			pgxc_node_free(handle, false);
		}
	}

	/* And finally release all the connections on pooler */
	PoolManagerReleaseConnections(force_close || has_error);

	datanode_count = 0;
	coord_count = 0;
}

/*
 * cancel a running query due to error while processing rows
 */
void
cancel_query(void)
{
	int			i;
	int 		dn_cancel[NumDataNodes];
	int			co_cancel[NumCoords];
	int			dn_count = 0;
	int			co_count = 0;

	if (datanode_count == 0 && coord_count == 0)
		return;

	/* Collect Datanodes handles */
	for (i = 0; i < NumDataNodes; i++)
	{
		PGXCNodeHandle *handle = &dn_handles[i];

		if (handle->sock != NO_SOCKET)
		{
			if (handle->state == DN_CONNECTION_STATE_COPY_IN ||
				handle->state == DN_CONNECTION_STATE_COPY_OUT)
			{
				DataNodeCopyEnd(handle, true);
			}
			else
			{
				if (handle->state != DN_CONNECTION_STATE_IDLE)
				{
					dn_cancel[dn_count++] = PGXCNodeGetNodeId(handle->nodeoid,
															  PGXC_NODE_DATANODE);
				}
			}
		}
	}

	/* Collect Coordinator handles */
	for (i = 0; i < NumCoords; i++)
	{
		PGXCNodeHandle *handle = &co_handles[i];

		if (handle->sock != NO_SOCKET)
		{
			if (handle->state == DN_CONNECTION_STATE_COPY_IN ||
				handle->state == DN_CONNECTION_STATE_COPY_OUT)
			{
				DataNodeCopyEnd(handle, true);
			}
			else
			{
				if (handle->state != DN_CONNECTION_STATE_IDLE)
				{
					co_cancel[co_count++] = PGXCNodeGetNodeId(handle->nodeoid,
															  PGXC_NODE_COORDINATOR);
				}
			}
		}
	}

	PoolManagerCancelQuery(dn_count, dn_cancel, co_count, co_cancel);

	/*
	 * Read responses from the nodes to whom we sent the cancel command. This
	 * ensures that there are no pending messages left on the connection
	 */
	for (i = 0; i < NumDataNodes; i++)
	{
		PGXCNodeHandle *handle = &dn_handles[i];

		if ((handle->sock != NO_SOCKET) && (handle->state != DN_CONNECTION_STATE_IDLE))
		{
			pgxc_node_flush_read(handle);
			handle->state = DN_CONNECTION_STATE_IDLE;
		}
	}

	for (i = 0; i < NumCoords; i++)
	{
		PGXCNodeHandle *handle = &co_handles[i];

		if (handle->sock != NO_SOCKET && handle->state != DN_CONNECTION_STATE_IDLE)
		{
			pgxc_node_flush_read(handle);
			handle->state = DN_CONNECTION_STATE_IDLE;
		}
	}
		/*
		 * Hack to wait a moment to cancel requests are processed in other nodes.
		 * If we send a new query to nodes before cancel requests get to be
		 * processed, the query will get unanticipated failure.
		 * As we have no way to know when to the request processed, and
		 * because this dulation depends upon the platform and the environment,
		 * this value is now moved to GUC (pgxc_calcen_delay) parameter.
		 */
	if (pgxcnode_cancel_delay > 0)
		pg_usleep(pgxcnode_cancel_delay * 1000);
}
/*
 * This method won't return until all network buffers are empty
 * To ensure all data in all network buffers is read and wasted
 */
void
clear_all_data(void)
{
	PGXCNodeHandle *handle;
	int				i;

	if (datanode_count == 0 && coord_count == 0)
		return;

	/* Collect Datanodes handles */
	for (i = 0; i < NumDataNodes; i++)
	{
		handle = &dn_handles[i];

		if (handle->sock != NO_SOCKET && handle->state != DN_CONNECTION_STATE_IDLE)
		{
			pgxc_node_flush_read(handle);
			handle->state = DN_CONNECTION_STATE_IDLE;
		}
		/* Clear any previous error messages */
		FreeHandleError(handle);
	}

	/* Collect Coordinator handles */
	for (i = 0; i < NumCoords; i++)
	{
		handle = &co_handles[i];

		if (handle->sock != NO_SOCKET && handle->state != DN_CONNECTION_STATE_IDLE)
		{
			pgxc_node_flush_read(handle);
			handle->state = DN_CONNECTION_STATE_IDLE;
		}
		/* Clear any previous error messages */
		FreeHandleError(handle);
	}
}

void
cancel_some_handles(int num_dnhandles, PGXCNodeHandle **dnhandles,
					int num_cohandles, PGXCNodeHandle **cohandles)
{
	PGXCNodeHandle	*handle;
	PGXCNodeHandle **new_dnhandles = NULL;
	PGXCNodeHandle **new_cohandles = NULL;
	int				 i;
	int				*dn_cancel = NULL;
	int				*co_cancel = NULL;
	int				 dn_count = 0;
	int				 co_count = 0;

	if (num_dnhandles <= 0 && num_cohandles <= 0)
		return ;

	PG_TRY();
	{
		if (num_dnhandles > 0)
		{
			AssertArg(dnhandles);

			dn_cancel = (int *) palloc0(num_dnhandles * sizeof(int));
			new_dnhandles = (PGXCNodeHandle **)
				palloc0(num_dnhandles * sizeof(PGXCNodeHandle *));

			for (i = 0; i < num_dnhandles; i++)
			{
				handle = dnhandles[i];
				if (!handle || handle->sock == NO_SOCKET)
					continue;

				if (handle->state == DN_CONNECTION_STATE_COPY_IN ||
					handle->state == DN_CONNECTION_STATE_COPY_OUT)
				{
					DataNodeCopyEnd(handle, true);
				} else
				{
					if (handle->state != DN_CONNECTION_STATE_IDLE)
					{
						dn_cancel[dn_count] = PGXCNodeGetNodeId(
							handle->nodeoid, PGXC_NODE_DATANODE);
						new_dnhandles[dn_count] = handle;
						dn_count++;
					}
				}
			}
		}

		if (num_cohandles > 0)
		{
			AssertArg(cohandles);

			co_cancel = (int *) palloc0(num_cohandles * sizeof(int));
			new_cohandles = (PGXCNodeHandle **)
				palloc0(num_cohandles * sizeof(PGXCNodeHandle *));

			for (i = 0; i < num_cohandles; i++)
			{
				handle = cohandles[i];
				if (!handle || handle->sock == NO_SOCKET)
					continue;

				if (handle->state == DN_CONNECTION_STATE_COPY_IN ||
					handle->state == DN_CONNECTION_STATE_COPY_OUT)
				{
					DataNodeCopyEnd(handle, true);
				} else
				{
					if (handle->state != DN_CONNECTION_STATE_IDLE)
					{
						co_cancel[co_count] = PGXCNodeGetNodeId(
							handle->nodeoid, PGXC_NODE_COORDINATOR);
						new_cohandles[co_count] = handle;
						co_count++;
					}
				}
			}
		}

		PoolManagerCancelQuery(dn_count, dn_cancel, co_count, co_cancel);

		/*
		 * Read responses from the nodes to whom we sent the cancel command. This
		 * ensures that there are no pending messages left on the connection
		 */
		for (i = 0; i < dn_count; i++)
		{
			handle = new_dnhandles[i];

			pgxc_node_flush_read(handle);
		}

		for (i = 0; i < co_count; i++)
		{
			handle = new_cohandles[i];

			pgxc_node_flush_read(handle);
		}
	} PG_CATCH();
	{
		if (dn_cancel)
			pfree(dn_cancel);
		if (co_cancel)
			pfree(co_cancel);
		if (new_dnhandles)
			pfree(new_dnhandles);
		if (new_cohandles)
			pfree(new_cohandles);
		PG_RE_THROW();
	} PG_END_TRY();

	if (dn_cancel)
		pfree(dn_cancel);
	if (co_cancel)
		pfree(co_cancel);
	if (new_dnhandles)
		pfree(new_dnhandles);
	if (new_cohandles)
		pfree(new_cohandles);

	/*
	 * Hack to wait a moment to cancel requests are processed in other nodes.
	 * If we send a new query to nodes before cancel requests get to be
	 * processed, the query will get unanticipated failure.
	 * As we have no way to know when to the request processed, and
	 * because this dulation depends upon the platform and the environment,
	 * this value is now moved to GUC (pgxc_calcen_delay) parameter.
	 */
	if (pgxcnode_cancel_delay > 0)
		pg_usleep(pgxcnode_cancel_delay * 1000);
}

void
clear_some_handles(int num_dnhandles, PGXCNodeHandle **dnhandles,
				   int num_cohandles, PGXCNodeHandle **cohandles)
{
	PGXCNodeHandle *handle;
	int				i;

	if (num_dnhandles <= 0 && num_cohandles <= 0)
		return ;

	for (i = 0; i < num_dnhandles; i++)
	{
		handle = dnhandles[i];

		if (!handle)
			continue;

		/* Free any error message */
		FreeHandleError(handle);

		/* Continue if invalid socket or invalid connection state */
		if (handle->sock == NO_SOCKET ||
			handle->state == DN_CONNECTION_STATE_ERROR_FATAL)
		{
			handle->combiner = NULL;
			continue;
		}

		if (handle->state == DN_CONNECTION_STATE_COPY_IN ||
			handle->state == DN_CONNECTION_STATE_COPY_OUT)
		{
			DataNodeCopyEnd(handle, true);
		}

		/* try to flush read any data */
		if (handle->state != DN_CONNECTION_STATE_IDLE)
			pgxc_node_flush_read(handle);

		/* release any combiner */
		handle->combiner = NULL;
	}

	for (i = 0; i < num_cohandles; i++)
	{
		handle = cohandles[i];

		if (!handle)
			continue;

		/* Free any error message */
		FreeHandleError(handle);

		/* Continue if invalid socket or invalid connection state */
		if (handle->sock == NO_SOCKET ||
			handle->state == DN_CONNECTION_STATE_ERROR_FATAL)
		{
			handle->combiner = NULL;
			continue;
		}

		if (handle->state == DN_CONNECTION_STATE_COPY_IN ||
			handle->state == DN_CONNECTION_STATE_COPY_OUT)
		{
			DataNodeCopyEnd(handle, true);
		}

		/* try to flush read any data */
		if (handle->state != DN_CONNECTION_STATE_IDLE)
			pgxc_node_flush_read(handle);

		/* release any combiner */
		handle->combiner = NULL;
	}

	/*
	 * We cannot release handle here, it is decided with the caller. 
	 */
}

void
clear_all_handles(bool error)
{
	PGXCNodeHandle *handle;
	int 			i;

	if (datanode_count <= 0 && coord_count <= 0)
		return ;

	for (i = 0; i < NumDataNodes; i++)
	{
		handle = &dn_handles[i];

		if (!handle)
			continue;

		/* Free any error message */
		FreeHandleError(handle);

		/* Continue if invalid socket or invalid connection state */
		if (handle->sock == NO_SOCKET ||
			handle->state == DN_CONNECTION_STATE_ERROR_FATAL)
		{
			handle->combiner = NULL;
			continue;
		}

		if (handle->state == DN_CONNECTION_STATE_COPY_IN ||
			handle->state == DN_CONNECTION_STATE_COPY_OUT)
		{
			DataNodeCopyEnd(handle, error);
		}

		/* try to flush read any data */
		if (handle->state != DN_CONNECTION_STATE_IDLE)
			pgxc_node_flush_read(handle);

		/* release any combiner */
		handle->combiner = NULL;
	}

	for (i = 0; i < NumCoords; i++)
	{
		handle = &co_handles[i];

		if (!handle)
			continue;

		/* Free any error message */
		FreeHandleError(handle);

		/* Continue if invalid socket or invalid connection state */
		if (handle->sock == NO_SOCKET ||
			handle->state == DN_CONNECTION_STATE_ERROR_FATAL)
		{
			handle->combiner = NULL;
			continue;
		}

		if (handle->state == DN_CONNECTION_STATE_COPY_IN ||
			handle->state == DN_CONNECTION_STATE_COPY_OUT)
		{
			DataNodeCopyEnd(handle, error);
		}

		/* try to flush read any data */
		if (handle->state != DN_CONNECTION_STATE_IDLE)
			pgxc_node_flush_read(handle);

		/* release any combiner */
		handle->combiner = NULL;
	}

	/*
	 * We cannot release handle here, it is decided with the caller. 
	 */
}

/*
 * Ensure specified amount of data can fit to the incoming buffer and
 * increase it if necessary
 */
int
ensure_in_buffer_capacity(size_t bytes_needed, PGXCNodeHandle *handle)
{
	int			newsize = handle->inSize;
	char	   *newbuf;

	if (bytes_needed <= (size_t) newsize)
		return 0;

	do
	{
		newsize *= 2;
	} while (newsize > 0 && bytes_needed > (size_t) newsize);

	if (newsize > 0 && bytes_needed <= (size_t) newsize)
	{
		newbuf = repalloc(handle->inBuffer, newsize);
		if (newbuf)
		{
			/* repalloc succeeded */
			handle->inBuffer = newbuf;
			handle->inSize = newsize;
			return 0;
		}
	}

	newsize = handle->inSize;
	do
	{
		newsize += 8192;
	} while (newsize > 0 && bytes_needed > (size_t) newsize);

	if (newsize > 0 && bytes_needed <= (size_t) newsize)
	{
		newbuf = repalloc(handle->inBuffer, newsize);
		if (newbuf)
		{
			/* repalloc succeeded */
			handle->inBuffer = newbuf;
			handle->inSize = newsize;
			return 0;
		}
	}

	return EOF;
}


/*
 * Ensure specified amount of data can fit to the outgoing buffer and
 * increase it if necessary
 */
int
ensure_out_buffer_capacity(size_t bytes_needed, PGXCNodeHandle *handle)
{
	int			newsize = handle->outSize;
	char	   *newbuf;

	if (bytes_needed <= (size_t) newsize)
		return 0;

	do
	{
		newsize *= 2;
	} while (newsize > 0 && bytes_needed > (size_t) newsize);

	if (newsize > 0 && bytes_needed <= (size_t) newsize)
	{
		newbuf = repalloc(handle->outBuffer, newsize);
		if (newbuf)
		{
			/* repalloc succeeded */
			handle->outBuffer = newbuf;
			handle->outSize = newsize;
			return 0;
		}
	}

	newsize = handle->outSize;
	do
	{
		newsize += 8192;
	} while (newsize > 0 && bytes_needed > (size_t) newsize);

	if (newsize > 0 && bytes_needed <= (size_t) newsize)
	{
		newbuf = repalloc(handle->outBuffer, newsize);
		if (newbuf)
		{
			/* repalloc succeeded */
			handle->outBuffer = newbuf;
			handle->outSize = newsize;
			return 0;
		}
	}

	return EOF;
}

/*
 * Send specified amount of data from the outgoing buffer over the connection
 */
int
send_some(PGXCNodeHandle *handle, int len)
{
	char	   *ptr = handle->outBuffer;
	int			remaining = handle->outEnd;
	int			result = 0;

	/* while there's still data to send */
	while (len > 0)
	{
		int			sent;

#ifndef WIN32
		sent = send(handle->sock, ptr, len, 0);
#else
		/*
		 * Windows can fail on large sends, per KB article Q201213. The failure-point
		 * appears to be different in different versions of Windows, but 64k should
		 * always be safe.
		 */
		sent = send(handle->sock, ptr, Min(len, 65536), 0);
#endif

		if (sent < 0)
		{
			/*
			 * Anything except EAGAIN/EWOULDBLOCK/EINTR is trouble. If it's
			 * EPIPE or ECONNRESET, assume we've lost the backend connection
			 * permanently.
			 */
			switch (errno)
			{
#ifdef EAGAIN
				case EAGAIN:
					break;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
				case EWOULDBLOCK:
					break;
#endif
				case EINTR:
					continue;

				case EPIPE:
#ifdef ECONNRESET
				case ECONNRESET:
#endif
					add_error_message(handle,
						"server %s closed the connection unexpectedly\n"
						"\tThis probably means the server terminated abnormally\n"
						"\tbefore or while processing the request.",
						NameStr(handle->name));

					/*
					 * We used to close the socket here, but that's a bad idea
					 * since there might be unread data waiting (typically, a
					 * NOTICE message from the backend telling us it's
					 * committing hara-kiri...).  Leave the socket open until
					 * pqReadData finds no more data can be read.  But abandon
					 * attempt to send data.
					 */
					handle->outEnd = 0;
					return -1;

				default:
					add_error_message(handle,
						"could not send data to server %s", NameStr(handle->name));
					/* We don't assume it's a fatal error... */
					handle->outEnd = 0;
					return -1;
			}
		}
		else
		{
			if(handle->file_data)
			{
				bool bwrite = true;
				fwrite(&bwrite, sizeof(bwrite), 1, handle->file_data);
				fwrite(&sent, sizeof(sent), 1, handle->file_data);
				fwrite(ptr, 1, sent, handle->file_data);
			}
			ptr += sent;
			len -= sent;
			remaining -= sent;
		}

		if (len > 0)
		{
			/*
			 * We did not send it all
			 * return 1 to indicate that data is still pending.
			 */
			result = 1;
			break;
		}
	}

	/* shift the remaining contents of the buffer */
	if ((remaining > 0) && (handle->outBuffer != ptr))
		memmove(handle->outBuffer, ptr, remaining);
	handle->outEnd = remaining;

	return result;
}

/*
 * Send PARSE message with specified statement down to the Datanode
 */
int
pgxc_node_send_parse(PGXCNodeHandle * handle, const char* statement,
						const char *query, short num_params, Oid *param_types)
{
	/* statement name size (allow NULL) */
	int			stmtLen = statement ? strlen(statement) + 1 : 1;
	/* size of query string */
	int			strLen = strlen(query) + 1;
	char 		**paramTypes = (char **)palloc(sizeof(char *) * num_params);
	/* total size of parameter type names */
	int 		paramTypeLen;
	/* message length */
	int			msgLen;
	int			cnt_params;
#if USE_ASSERT_CHECKING
	size_t		old_outEnd = handle->outEnd;
#endif	

	/* if there are parameters, param_types should exist */
	Assert(num_params <= 0 || param_types);
	/* 2 bytes for number of parameters, preceding the type names */
	paramTypeLen = 2;
	/* find names of the types of parameters */
	for (cnt_params = 0; cnt_params < num_params; cnt_params++)
	{
		Oid typeoid;

		/* Parameters with no types are simply ignored */
		if (OidIsValid(param_types[cnt_params]))
			typeoid = param_types[cnt_params];
		else
			typeoid = INT4OID;

		paramTypes[cnt_params] = format_type_be(typeoid);
		paramTypeLen += strlen(paramTypes[cnt_params]) + 1;
	}

	/* size + stmtLen + strlen + paramTypeLen */
	msgLen = 4 + stmtLen + strLen + paramTypeLen;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msgLen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 'P';
	/* size */
	msgLen = htonl(msgLen);
	memcpy(handle->outBuffer + handle->outEnd, &msgLen, 4);
	handle->outEnd += 4;
	/* statement name */
	if (statement)
	{
		memcpy(handle->outBuffer + handle->outEnd, statement, stmtLen);
		handle->outEnd += stmtLen;
	}
	else
		handle->outBuffer[handle->outEnd++] = '\0';
	/* query */
	memcpy(handle->outBuffer + handle->outEnd, query, strLen);
	handle->outEnd += strLen;
	/* parameter types */
	Assert(sizeof(num_params) == 2);
	*((short *)(handle->outBuffer + handle->outEnd)) = htons(num_params);
	handle->outEnd += sizeof(num_params);
	/*
	 * instead of parameter ids we should send parameter names (qualified by
	 * schema name if required). The OIDs of types can be different on
	 * Datanodes.
	 */
	for (cnt_params = 0; cnt_params < num_params; cnt_params++)
	{
		memcpy(handle->outBuffer + handle->outEnd, paramTypes[cnt_params],
					strlen(paramTypes[cnt_params]) + 1);
		handle->outEnd += strlen(paramTypes[cnt_params]) + 1;
		pfree(paramTypes[cnt_params]);
	}
	pfree(paramTypes);
	Assert(old_outEnd + ntohl(msgLen) + 1 == handle->outEnd);

 	return 0;
}

/*
 * Send BIND message down to the Datanode
 */
int
pgxc_node_send_bind(PGXCNodeHandle * handle, const char *portal,
					const char *statement, int paramlen, char *params)
{
	int			pnameLen;
	int			stmtLen;
	int 		paramCodeLen;
	int 		paramValueLen;
	int 		paramOutLen;
	int			msgLen;

	/* Invalid connection state, return error */
	if (handle->state != DN_CONNECTION_STATE_IDLE)
		return EOF;

	/* portal name size (allow NULL) */
	pnameLen = portal ? strlen(portal) + 1 : 1;
	/* statement name size (allow NULL) */
	stmtLen = statement ? strlen(statement) + 1 : 1;
	/* size of parameter codes array (always empty for now) */
	paramCodeLen = 2;
	/* size of parameter values array, 2 if no params */
	paramValueLen = paramlen ? paramlen : 2;
	/* size of output parameter codes array (always empty for now) */
	paramOutLen = 2;
	/* size + pnameLen + stmtLen + parameters */
	msgLen = 4 + pnameLen + stmtLen + paramCodeLen + paramValueLen + paramOutLen;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msgLen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 'B';
	/* size */
	msgLen = htonl(msgLen);
	memcpy(handle->outBuffer + handle->outEnd, &msgLen, 4);
	handle->outEnd += 4;
	/* portal name */
	if (portal)
	{
		memcpy(handle->outBuffer + handle->outEnd, portal, pnameLen);
		handle->outEnd += pnameLen;
	}
	else
		handle->outBuffer[handle->outEnd++] = '\0';
	/* statement name */
	if (statement)
	{
		memcpy(handle->outBuffer + handle->outEnd, statement, stmtLen);
		handle->outEnd += stmtLen;
	}
	else
		handle->outBuffer[handle->outEnd++] = '\0';
	/* parameter codes (none) */
	handle->outBuffer[handle->outEnd++] = 0;
	handle->outBuffer[handle->outEnd++] = 0;
	/* parameter values */
	if (paramlen)
	{
		memcpy(handle->outBuffer + handle->outEnd, params, paramlen);
		handle->outEnd += paramlen;
	}
	else
	{
		handle->outBuffer[handle->outEnd++] = 0;
		handle->outBuffer[handle->outEnd++] = 0;
	}
	/* output parameter codes (none) */
	handle->outBuffer[handle->outEnd++] = 0;
	handle->outBuffer[handle->outEnd++] = 0;

 	return 0;
}

/*
 * Send DESCRIBE message (portal or statement) down to the Datanode
 */
int
pgxc_node_send_describe(PGXCNodeHandle * handle, bool is_statement,
						const char *name)
{
	int			nameLen;
	int			msgLen;

	/* Invalid connection state, return error */
	if (handle->state != DN_CONNECTION_STATE_IDLE)
		return EOF;

	/* statement or portal name size (allow NULL) */
	nameLen = name ? strlen(name) + 1 : 1;

	/* size + statement/portal + name */
	msgLen = 4 + 1 + nameLen;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msgLen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 'D';
	/* size */
	msgLen = htonl(msgLen);
	memcpy(handle->outBuffer + handle->outEnd, &msgLen, 4);
	handle->outEnd += 4;
	/* statement/portal flag */
	handle->outBuffer[handle->outEnd++] = is_statement ? 'S' : 'P';
	/* object name */
	if (name)
	{
		memcpy(handle->outBuffer + handle->outEnd, name, nameLen);
		handle->outEnd += nameLen;
	}
	else
		handle->outBuffer[handle->outEnd++] = '\0';

 	return 0;
}

/*
 * Send CLOSE message (portal or statement) down to the Datanode
 */
int
pgxc_node_send_close(PGXCNodeHandle * handle, bool is_statement,
					 const char *name)
{
	/* statement or portal name size (allow NULL) */
	int			nameLen = name ? strlen(name) + 1 : 1;

	/* size + statement/portal + name */
	int			msgLen = 4 + 1 + nameLen;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msgLen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 'C';
	/* size */
	msgLen = htonl(msgLen);
	memcpy(handle->outBuffer + handle->outEnd, &msgLen, 4);
	handle->outEnd += 4;
	/* statement/portal flag */
	handle->outBuffer[handle->outEnd++] = is_statement ? 'S' : 'P';
	/* object name */
	if (name)
	{
		memcpy(handle->outBuffer + handle->outEnd, name, nameLen);
		handle->outEnd += nameLen;
	}
	else
		handle->outBuffer[handle->outEnd++] = '\0';

	handle->state = DN_CONNECTION_STATE_QUERY;

 	return 0;
}

/*
 * Send EXECUTE message down to the Datanode
 */
int
pgxc_node_send_execute(PGXCNodeHandle * handle, const char *portal, int fetch)
{
	/* portal name size (allow NULL) */
	int			pnameLen = portal ? strlen(portal) + 1 : 1;

	/* size + pnameLen + fetchLen */
	int			msgLen = 4 + pnameLen + 4;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msgLen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 'E';
	/* size */
	msgLen = htonl(msgLen);
	memcpy(handle->outBuffer + handle->outEnd, &msgLen, 4);
	handle->outEnd += 4;
	/* portal name */
	if (portal)
	{
		memcpy(handle->outBuffer + handle->outEnd, portal, pnameLen);
		handle->outEnd += pnameLen;
	}
	else
		handle->outBuffer[handle->outEnd++] = '\0';

	/* fetch */
	fetch = htonl(fetch);
	memcpy(handle->outBuffer + handle->outEnd, &fetch, 4);
	handle->outEnd += 4;

	handle->state = DN_CONNECTION_STATE_QUERY;

	return 0;
}

/*
 * Send FLUSH message down to the Datanode
 */
int
pgxc_node_send_flush(PGXCNodeHandle * handle)
{
	/* size */
	int			msgLen = 4;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msgLen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 'H';
	/* size */
	msgLen = htonl(msgLen);
	memcpy(handle->outBuffer + handle->outEnd, &msgLen, 4);
	handle->outEnd += 4;

	return pgxc_node_flush(handle);
}

/*
 * Send SYNC message down to the Datanode
 */
int
pgxc_node_send_sync(PGXCNodeHandle * handle)
{
	/* size */
	int			msgLen = 4;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msgLen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 'S';
	/* size */
	msgLen = htonl(msgLen);
	memcpy(handle->outBuffer + handle->outEnd, &msgLen, 4);
	handle->outEnd += 4;

	return pgxc_node_flush(handle);
}

/*
 * Send the GXID down to the Datanode
 */
int
pgxc_node_send_query_extended(PGXCNodeHandle *handle, const char *query,
							  const char *statement, const char *portal,
							  int num_params, Oid *param_types,
							  int paramlen, char *params,
							  bool send_describe, int fetch_size)
{
	/* NULL query indicates already prepared statement */
	if (query)
	{
#ifdef DEBUG_ADB
		if (ADB_DEBUG)
		{
			StringInfoData buf;
			initStringInfo(&buf);
			appendStringInfo(&buf, "%s/*%d*/", query, MyProcPid);

			adb_ereport(LOG,
				(errmsg("[ADB]Send to [node] %s [sock] %d [query] %s",
					NameStr(handle->name), handle->sock, query)));

			MemSet(handle->last_query, 0, DEBUG_BUF_SIZE);
			snprintf(handle->last_query, DEBUG_BUF_SIZE, "%s", query);

			if (pgxc_node_send_parse(handle, statement, buf.data, num_params, param_types))
			{
				pfree(buf.data);
				return EOF;
			}
			pfree(buf.data);
		} else
		{
			if (pgxc_node_send_parse(handle, statement, query, num_params, param_types))
				return EOF;
		}
#else
		if (pgxc_node_send_parse(handle, statement, query, num_params, param_types))
			return EOF;
#endif /* DEBUG_ADB */
	}
	if (pgxc_node_send_bind(handle, portal, statement, paramlen, params))
		return EOF;
	if (send_describe)
		if (pgxc_node_send_describe(handle, false, portal))
			return EOF;
	if (fetch_size >= 0)
		if (pgxc_node_send_execute(handle, portal, fetch_size))
			return EOF;
	if (pgxc_node_send_sync(handle))
		return EOF;

	return 0;
}

/*
 * This method won't return until connection buffer is empty or error occurs
 * To ensure all data are on the wire before waiting for response
 */
int
pgxc_node_flush(PGXCNodeHandle *handle)
{
	while (handle->outEnd)
	{
		if (send_some(handle, handle->outEnd) < 0)
		{
			add_error_message(handle,
				"Fail to send data to datanode %s", NameStr(handle->name));
			return EOF;
		}
	}
	return 0;
}

/*
 * This method won't return until network buffer is empty or error occurs
 * To ensure all data in network buffers is read and wasted
 */
#define FLUSH_READ_TIMEOUT 60	/* 60 seconds */
void pgxc_node_flush_read(PGXCNodeHandle *handle)
{
	int	result;
	time_t last_time;
	fd_set rfd;
	struct timeval tv;

	if(handle == NULL || handle->sock == NO_SOCKET)
		return;

	last_time = time(NULL);
	for(;;)
	{
		if (is_data_node_ready(handle))
			break;
		
		FD_ZERO(&rfd);
		FD_SET(handle->sock, &rfd);
		tv.tv_sec = FLUSH_READ_TIMEOUT - (time(NULL) - last_time);
		tv.tv_usec = 0;
		result = select(handle->sock + 1, &rfd, NULL, NULL, &tv);
		if(result < 0)
		{
			CHECK_FOR_INTERRUPTS();
			if(errno == EINTR)
				continue;
			handle->state = DN_CONNECTION_STATE_ERROR_FATAL;
			break;
		}else if(result == 0)
		{
			/* timeout */
			handle->state = DN_CONNECTION_STATE_ERROR_FATAL;
			break;
		}

		result = pgxc_node_read_data(handle, true);
		if(result > 0)
			last_time = time(NULL);
		if(result < 0)
			break;
	}
}
#undef FLUSH_READ_TIMEOUT

/*
 * Send specified statement down to the PGXC node
 */
int
pgxc_node_send_query(PGXCNodeHandle * handle, const char *query)
{
	return pgxc_node_send_query_tree(handle, query, NULL);
}

int	pgxc_node_send_query_tree(PGXCNodeHandle * handle, const char *query, StringInfo tree_data)
{
	int			tree_len;
	int			strLen;
	int			msgLen;

#ifdef DEBUG_ADB
	StringInfoData buf;
#endif

	/* Invalid connection state, return error */
	if (handle->state != DN_CONNECTION_STATE_IDLE)
		return EOF;

#ifdef DEBUG_ADB
	initStringInfo(&buf);
	if (ADB_DEBUG)
	{
		appendStringInfo(&buf, "%s/*%d*/", query, MyProcPid);
		query = buf.data;

		adb_ereport(LOG,
			(errmsg("[ADB]Send to [node] %s [sock] %d [query] %s",
				NameStr(handle->name), handle->sock, query)));

		MemSet(handle->last_query, 0, DEBUG_BUF_SIZE);
		snprintf(handle->last_query, DEBUG_BUF_SIZE, "%s", query);
	}
#endif

	strLen = strlen(query) + 1;
	/* size + strlen */
	msgLen = 4 + strLen;
	if(tree_data && tree_data->len > 0)
	{
		tree_len = tree_data->len;
		msgLen += tree_len;
	}else
	{
		tree_len = 0;
	}

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msgLen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	if(tree_len)
		handle->outBuffer[handle->outEnd++] = 'q';
	else
		handle->outBuffer[handle->outEnd++] = 'Q';
	msgLen = htonl(msgLen);
	memcpy(handle->outBuffer + handle->outEnd, &msgLen, 4);
	handle->outEnd += 4;
	memcpy(handle->outBuffer + handle->outEnd, query, strLen);
	handle->outEnd += strLen;
	if(tree_len)
	{
		Assert(tree_data && tree_data->data);
		memcpy(handle->outBuffer + handle->outEnd, tree_data->data, tree_len);
		handle->outEnd += tree_len;
	}

	handle->state = DN_CONNECTION_STATE_QUERY;

#ifdef DEBUG_ADB
	pfree(buf.data);
#endif

 	return pgxc_node_flush(handle);
}

/*
 * Send the GXID down to the PGXC node
 */
int
pgxc_node_send_gxid(PGXCNodeHandle *handle, GlobalTransactionId gxid)
{
	int			msglen = 8;
	int			i32;

	/* Invalid connection state, return error */
	if (handle->state != DN_CONNECTION_STATE_IDLE)
		return EOF;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msglen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 'g';
	msglen = htonl(msglen);
	memcpy(handle->outBuffer + handle->outEnd, &msglen, sizeof(msglen));
	handle->outEnd += sizeof(msglen);
	i32 = htonl(gxid);
	memcpy(handle->outBuffer + handle->outEnd, &i32, sizeof(i32));
	handle->outEnd += sizeof(i32);

	return 0;
}

/*
 * Send the Command ID down to the PGXC node
 */
int
pgxc_node_send_cmd_id(PGXCNodeHandle *handle, CommandId cid)
{
	int			msglen = CMD_ID_MSG_LEN;
	int			i32;

	/* No need to send command ID if its sending flag is not enabled */
	if (!IsSendCommandId())
		return 0;

	/* Invalid connection state, return error */
	if (handle->state != DN_CONNECTION_STATE_IDLE)
		return EOF;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msglen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 'M';
	msglen = htonl(msglen);
	memcpy(handle->outBuffer + handle->outEnd, &msglen, 4);
	handle->outEnd += 4;
	i32 = htonl(cid);
	memcpy(handle->outBuffer + handle->outEnd, &i32, 4);
	handle->outEnd += 4;

	return 0;
}

int
pgxc_node_send_snapshot(PGXCNodeHandle *handle, Snapshot snapshot)
{
	StringInfoData	buf;
	uint32			nval;
	int				i;

	/* Invalid connection state, return error */
	if (handle->state != DN_CONNECTION_STATE_IDLE)
		return EOF;

	initStringInfo(&buf);
	/* RecentGlobalXmin */
	nval = htonl(RecentGlobalXmin);
	appendBinaryStringInfo(&buf, (const char *) &nval, sizeof(TransactionId));
	/* xmin */
	nval = htonl(snapshot->xmin);
	appendBinaryStringInfo(&buf, (const char *) &nval, sizeof(TransactionId));
	/* xmax */
	nval = htonl(snapshot->xmax);
	appendBinaryStringInfo(&buf, (const char *) &nval, sizeof(TransactionId));
	/* curcid */
	nval = htonl(snapshot->curcid);
	appendBinaryStringInfo(&buf, (const char *) &nval, sizeof(CommandId));
	/* xcnt */
	nval = htonl(snapshot->xcnt);
	appendBinaryStringInfo(&buf, (const char *) &nval, sizeof(uint32));
	/* xip */
	for (i = 0; i < snapshot->xcnt; i++)
	{
		nval = htonl(snapshot->xip[i]);
		appendBinaryStringInfo(&buf, (const char *) &nval, sizeof(TransactionId));
	}
	/* subxcnt */
	nval = htonl(snapshot->subxcnt);
	appendBinaryStringInfo(&buf, (const char *) &nval, sizeof(int32));
	/* subxip */
	for (i = 0; i < snapshot->subxcnt; i++)
	{
		nval = htonl(snapshot->subxip[i]);
		appendBinaryStringInfo(&buf, (const char *) &nval, sizeof(TransactionId));
	}

	/* message length */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + 4 + buf.len, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}

	handle->outBuffer[handle->outEnd++] = 's';
	nval = htonl(4 + buf.len);
	memcpy(handle->outBuffer + handle->outEnd, &nval, sizeof(nval));
	handle->outEnd += sizeof(nval);
	memcpy(handle->outBuffer + handle->outEnd, buf.data, buf.len);
	handle->outEnd += buf.len;
	pfree(buf.data);

	return 0;
}

int
pgxc_node_send_timestamp(PGXCNodeHandle *handle, TimestampTz timestamp)
{
	int		msglen = 12; /* 4 bytes for msglen and 8 bytes for timestamp (int64) */
	uint32	n32;
	int64	i = (int64) timestamp;

	/* Invalid connection state, return error */
	if (handle->state != DN_CONNECTION_STATE_IDLE)
		return EOF;

	/* msgType + msgLen */
	if (ensure_out_buffer_capacity(handle->outEnd + 1 + msglen, handle) != 0)
	{
		add_error_message(handle, "out of memory");
		return EOF;
	}
	handle->outBuffer[handle->outEnd++] = 't';
	msglen = htonl(msglen);
	memcpy(handle->outBuffer + handle->outEnd, &msglen, 4);
	handle->outEnd += 4;

	/* High order half first */
#ifdef INT64_IS_BUSTED
	/* don't try a right shift of 32 on a 32-bit word */
	n32 = (i < 0) ? -1 : 0;
#else
	n32 = (uint32) (i >> 32);
#endif
	n32 = htonl(n32);
	memcpy(handle->outBuffer + handle->outEnd, &n32, 4);
	handle->outEnd += 4;

	/* Now the low order half */
	n32 = (uint32) i;
	n32 = htonl(n32);
	memcpy(handle->outBuffer + handle->outEnd, &n32, 4);
	handle->outEnd += 4;

	return 0;
}

/*
 * Add another message to the list of errors to be returned back to the client
 * at the convenient time
 */
void
add_error_message(PGXCNodeHandle *handle, const char *fmt, ...)
{
	va_list ap;
	StringInfoData buf;

	handle->transaction_status = 'E';
	initStringInfo(&buf);
	va_start(ap, fmt);
	appendStringInfoVA(&buf, fmt, ap);

	if (handle->error)
	{
		/* PGXCTODO append */
	} else
	{
		handle->error = MemoryContextStrdup(TopMemoryContext, buf.data);
	}

	pfree(buf.data);
	va_end(ap);
}

/*
 * for specified list return array of PGXCNodeHandles
 * acquire from pool if needed.
 * the lenth of returned array is the same as of nodelist
 * For Datanodes, Special case is empty or NIL nodeList, in this case return all the nodes.
 * The returned list should be pfree'd when no longer needed.
 * For Coordinator, do not get a connection if Coordinator list is NIL,
 * Coordinator fds is returned only if transaction uses a DDL
 */
PGXCNodeAllHandles *
get_handles(List *datanodelist, List *coordlist, bool is_coord_only_query)
{
	PGXCNodeAllHandles	*result;
	ListCell			*node_list_item;
	List				*dn_allocate = NIL;
	List				*co_allocate = NIL;
	PGXCNodeHandle		*node_handle;

	/* index of the result array */
	int					 i = 0;

	result = (PGXCNodeAllHandles *) palloc(sizeof(PGXCNodeAllHandles));
	if (!result)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	result->primary_handle = NULL;
	result->datanode_handles = NULL;
	result->coord_handles = NULL;
	result->co_conn_count = list_length(coordlist);
	result->dn_conn_count = list_length(datanodelist);

	/*
	 * Get Handles for Datanodes
	 * If node list is empty execute request on current nodes.
	 * It is also possible that the query has to be launched only on Coordinators.
	 */
	if (!is_coord_only_query)
	{
		if (list_length(datanodelist) == 0)
		{
			/*
			 * We do not have to zero the array - on success all items will be set
			 * to correct pointers, on error the array will be freed
			 */
			result->datanode_handles = (PGXCNodeHandle **)
									   palloc(NumDataNodes * sizeof(PGXCNodeHandle *));
			if (!result->datanode_handles)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			for (i = 0; i < NumDataNodes; i++)
			{
				node_handle = &dn_handles[i];
				FreeHandleError(node_handle);
				result->datanode_handles[i] = node_handle;
				if (node_handle->sock == NO_SOCKET)
					dn_allocate = lappend_int(dn_allocate, i);
			}
		}
		else
		{
			/*
			 * We do not have to zero the array - on success all items will be set
			 * to correct pointers, on error the array will be freed
			 */

			result->datanode_handles = (PGXCNodeHandle **)
				palloc(list_length(datanodelist) * sizeof(PGXCNodeHandle *));
			if (!result->datanode_handles)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			i = 0;
			foreach(node_list_item, datanodelist)
			{
				int	node = lfirst_int(node_list_item);

				if (node < 0 || node >= NumDataNodes)
				{
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("Invalid Datanode number")));
				}

				node_handle = &dn_handles[node];
				FreeHandleError(node_handle);
				result->datanode_handles[i++] = node_handle;
				if (node_handle->sock == NO_SOCKET)
					dn_allocate = lappend_int(dn_allocate, node);
			}
		}
	}

	/*
	 * Get Handles for Coordinators
	 * If node list is empty execute request on current nodes
	 * There are transactions where the Coordinator list is NULL Ex:COPY
	 */

	if (coordlist)
	{
		if (list_length(coordlist) == 0)
		{
			/*
			 * We do not have to zero the array - on success all items will be set
			 * to correct pointers, on error the array will be freed
			 */
			result->coord_handles = (PGXCNodeHandle **)palloc(NumCoords * sizeof(PGXCNodeHandle *));
			if (!result->coord_handles)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			for (i = 0; i < NumCoords; i++)
			{
				node_handle = &co_handles[i];
				FreeHandleError(node_handle);
				result->coord_handles[i] = node_handle;
				if (node_handle->sock == NO_SOCKET)
					co_allocate = lappend_int(co_allocate, i);
			}
		}
		else
		{
			/*
			 * We do not have to zero the array - on success all items will be set
			 * to correct pointers, on error the array will be freed
			 */
			result->coord_handles = (PGXCNodeHandle **)
									palloc(list_length(coordlist) * sizeof(PGXCNodeHandle *));
			if (!result->coord_handles)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			i = 0;
			/* Some transactions do not need Coordinators, ex: COPY */
			foreach(node_list_item, coordlist)
			{
				int			node = lfirst_int(node_list_item);

				if (node < 0 || node >= NumCoords)
				{
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("Invalid coordinator number")));
				}

				node_handle = &co_handles[node];
				FreeHandleError(node_handle);
				result->coord_handles[i++] = node_handle;
				if (node_handle->sock == NO_SOCKET)
					co_allocate = lappend_int(co_allocate, node);
			}
		}
	}

	/*
	 * Pooler can get activated even if list of Coordinator or Datanode is NULL
	 * If both lists are NIL, we don't need to call Pooler.
	 */
	if (dn_allocate || co_allocate)
	{
		int	j = 0;
		int	*fds = PoolManagerGetConnections(dn_allocate, co_allocate);

		if (!fds)
		{
			if (coordlist)
				if (result->coord_handles)
					pfree(result->coord_handles);
			if (datanodelist)
				if (result->datanode_handles)
					pfree(result->datanode_handles);

			pfree(result);
			if (dn_allocate)
				list_free(dn_allocate);
			if (co_allocate)
				list_free(co_allocate);
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("Failed to get pooled connections")));
		}
		/* Initialisation for Datanodes */
		if (dn_allocate)
		{
			foreach(node_list_item, dn_allocate)
			{
				int			node = lfirst_int(node_list_item);
				int			fdsock = fds[j++];

				if (node < 0 || node >= NumDataNodes)
				{
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("Invalid Datanode number")));
				}

				node_handle = &dn_handles[node];
				pgxc_node_init(node_handle, fdsock);
				dn_handles[node] = *node_handle;
				datanode_count++;
			}
		}
		/* Initialisation for Coordinators */
		if (co_allocate)
		{
			foreach(node_list_item, co_allocate)
			{
				int			node = lfirst_int(node_list_item);
				int			fdsock = fds[j++];

				if (node < 0 || node >= NumCoords)
				{
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("Invalid coordinator number")));
				}

				node_handle = &co_handles[node];
				pgxc_node_init(node_handle, fdsock);
				co_handles[node] = *node_handle;
				coord_count++;
			}
		}

		pfree(fds);

		if (co_allocate)
			list_free(co_allocate);
		if (dn_allocate)
			list_free(dn_allocate);
	}

	return result;
}

/* Free PGXCNodeAllHandles structure */
void
pfree_pgxc_all_handles(PGXCNodeAllHandles *pgxc_handles)
{
	if (!pgxc_handles)
		return;

	if (pgxc_handles->primary_handle)
		pfree(pgxc_handles->primary_handle);
	if (pgxc_handles->datanode_handles && pgxc_handles->dn_conn_count != 0)
		pfree(pgxc_handles->datanode_handles);
	if (pgxc_handles->coord_handles && pgxc_handles->co_conn_count != 0)
		pfree(pgxc_handles->coord_handles);

	pfree(pgxc_handles);
}

/*
 * PGXCNode_getNodeId
 *		Look at the data cached for handles and return node position
 */
int
PGXCNodeGetNodeId(Oid nodeoid, char node_type)
{
	PGXCNodeHandle *handles;
	int				num_nodes, i;
	int				res = -1;

	switch (node_type)
	{
		case PGXC_NODE_COORDINATOR:
			num_nodes = NumCoords;
			handles = co_handles;
			break;
		case PGXC_NODE_DATANODE:
			num_nodes = NumDataNodes;
			handles = dn_handles;
			break;
		default:
			/* Should not happen */
			Assert(0);
			return res;
	}

	/* Look into the handles and return correct position in array */
	for (i = 0; i < num_nodes; i++)
	{
		if (handles[i].nodeoid == nodeoid)
		{
			res = i;
			break;
		}
	}
	return res;
}

/*
 * PGXCNode_getNodeOid
 *		Look at the data cached for handles and return node Oid
 */
Oid
PGXCNodeGetNodeOid(int nodeid, char node_type)
{
	PGXCNodeHandle *handles;

	switch (node_type)
	{
		case PGXC_NODE_COORDINATOR:
			handles = co_handles;
			break;
		case PGXC_NODE_DATANODE:
			handles = dn_handles;
			break;
		default:
			/* Should not happen */
			Assert(0);
			return InvalidOid;
	}

	return handles[nodeid].nodeoid;
}

List *PGXCNodeGetNodeOidList(List *list, char node_type)
{
	List	   *oid_list;
	ListCell   *lc;
	PGXCNodeHandle *handles;
	int array_size;

	switch (node_type)
	{
	case PGXC_NODE_COORDINATOR:
		handles = co_handles;
		array_size = NumCoords;
		break;
	case PGXC_NODE_DATANODE:
		handles = dn_handles;
		array_size = NumDataNodes;
		break;
	default:
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			errmsg("unknown node type %u", node_type)));
		return NIL;	/* keep compiler quiet */
	}

	oid_list = NIL;
	foreach(lc, list)
	{
		register int x = lfirst_int(lc);
		if(x >= array_size || x < 0)
		{
			ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("Invalid node ID index %d", x),
				errhint("node type '%c', number must between 0 and %d", node_type, array_size-1)));
		}
		oid_list = lappend_oid(oid_list, handles[x].nodeoid);
	}
	return oid_list;
}

/*
 * pgxc_node_str
 *
 * get the name of the node
 */
Datum
pgxc_node_str(PG_FUNCTION_ARGS)
{
	PG_RETURN_NAME(PGXCNodeName);
}

/*
 * PGXCNodeGetNodeIdFromName
 *		Return node position in handles array
 */
int
PGXCNodeGetNodeIdFromName(char *node_name, char node_type)
{
	char *nm;
	Oid nodeoid;

	if (node_name == NULL)
		return -1;

	nm = str_tolower(node_name, strlen(node_name), DEFAULT_COLLATION_OID);

	nodeoid = get_pgxc_nodeoid(nm);
	pfree(nm);
	if (!OidIsValid(nodeoid))
		return -1;

	return PGXCNodeGetNodeId(nodeoid, node_type);
}

#ifdef DEBUG_ADB
static const char *
DNConnectionStateAsString(DNConnectionState state)
{
	switch (state)
	{
		case DN_CONNECTION_STATE_IDLE:
			return "DN_CONNECTION_STATE_IDLE";
		case DN_CONNECTION_STATE_QUERY:
			return "DN_CONNECTION_STATE_QUERY";
		case DN_CONNECTION_STATE_ERROR_FATAL:
			return "DN_CONNECTION_STATE_ERROR_FATAL";
		case DN_CONNECTION_STATE_COPY_IN:
			return "DN_CONNECTION_STATE_COPY_IN";
		case DN_CONNECTION_STATE_COPY_OUT:
			return "DN_CONNECTION_STATE_COPY_OUT";
		default:
			break;
	}
	return "UNKNOWN HANDLE STATE";
}
#endif

/*
 * Has the Datanode sent Ready For Query
 */
bool
is_data_node_ready(PGXCNodeHandle * conn)
{
	char	   *msg;
	int			msg_len;
	char		msg_type;

	for (;;)
	{
		/*
		 * If we are in the process of shutting down, we
		 * may be rolling back, and the buffer may contain other messages.
		 * We want to avoid a procarray exception
		 * as well as an error stack overflow.
		 */
		if (proc_exit_inprogress)
			conn->state = DN_CONNECTION_STATE_ERROR_FATAL;

		/* don't read from from the connection if there is a fatal error */
		if (conn->state == DN_CONNECTION_STATE_ERROR_FATAL)
			return true;

		/* No data available, exit */
		if (!HAS_MESSAGE_BUFFERED(conn))
			return false;

		msg_type = get_message(conn, &msg_len, &msg);
#ifdef DEBUG_ADB
		adb_ereport(LOG,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("[process] %d [handle] %s [sock] %d [state] %s [msg_type] %c",
			 		MyProcPid, NameStr(conn->name), conn->sock,
			 		DNConnectionStateAsString(conn->state),msg_type)));
#endif
		switch (msg_type)
		{
			case 's':			/* PortalSuspended */
				break;

			case 'Z':			/* ReadyForQuery */

				/*
				 * Return result depends on previous connection state.
				 * If it was PORTAL_SUSPENDED Coordinator want to send down
				 * another EXECUTE to fetch more rows, otherwise it is done
				 * with the connection
				 */
				conn->transaction_status = msg[0];
				conn->state = DN_CONNECTION_STATE_IDLE;
				conn->combiner = NULL;
				return true;
		}
	}
	/* never happen, but keep compiler quiet */
	return false;
}

