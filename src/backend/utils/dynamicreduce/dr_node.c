#include "postgres.h"

#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "storage/latch.h"
#include "utils/memutils.h"

#include "utils/dynamicreduce.h"
#include "utils/dr_private.h"

/*
 * sizeof(msg len) = 4 bytes
 * sizeof(msg type) = 1 byte
 * sizeof(plan id) = 4 bytes
 */
#define NODE_MSG_HEAD_LEN	9

static HTAB		   *htab_node_info = NULL;

#ifdef WITH_REDUCE_RDMA
static HTAB		   *htab_rsnode_info = NULL;
typedef struct dr_rshandle_item
{
	int				fd;
	DRNodeEventData *ptr;
}dr_rshandle_item;
DRNodeEventData *dr_rhandle_list;
#endif

static void OnNodeEvent(DROnEventArgs);
static void OnNodeError(DROnErrorArgs);
static void OnPreWaitNode(DROnPreWaitArgs);

static int PorcessNodeEventData(DRNodeEventData *ned);
static void OnNodeSendMessage(DRNodeEventData *ned, pgsocket fd);

void DROnNodeConectSuccess(DRNodeEventData *ned)
{
	ned->base.OnEvent = OnNodeEvent;
	ned->base.OnPreWait = OnPreWaitNode;
	ned->base.OnError = OnNodeError;
	ned->status = DRN_WORKING;

	DR_NODE_DEBUG((errmsg("node %u(%p) connect successed", ned->nodeoid, ned)));
	if (ned->recvBuf.cursor < ned->recvBuf.len)
	{
		/* process other message(s) */
		PorcessNodeEventData(ned);
	}else
	{
		/* reset receive buffer */
		ned->recvBuf.cursor = ned->recvBuf.len = 0;
	}

	ActiveWaitingPlan(ned);
}

bool PutMessageToNode(DRNodeEventData *ned, char msg_type, const char *data, uint32 len, int plan_id)
{
	uint32				free_space;
	uint32				need_space;
	bool				is_empty;
	Assert(len >= 0);
	Assert(plan_id >= -1);

	Assert(ned && ned->base.type == DR_EVENT_DATA_NODE);
	Assert(OidIsValid(ned->nodeoid));
	if (ned->status == DRN_WAIT_CLOSE)
	{
		/* just return success */
		return true;
	}

	if (ned->sendBuf.cursor == ned->sendBuf.len)
	{
		is_empty = true;
		ned->sendBuf.cursor = ned->sendBuf.len = 0;
		free_space = ned->sendBuf.maxlen;
	}else
	{
		is_empty = false;

		if (ned->sendBuf.cursor > 0 &&
			ned->sendBuf.len - ned->sendBuf.cursor <= (ned->sendBuf.maxlen >> 2))
		{
			memmove(ned->sendBuf.data,
					ned->sendBuf.data + ned->sendBuf.cursor,
					ned->sendBuf.len - ned->sendBuf.cursor);
			ned->sendBuf.len -= ned->sendBuf.cursor;
			ned->sendBuf.cursor = 0;
		}

		free_space = ned->sendBuf.maxlen - ned->sendBuf.len;
	}

	need_space = NODE_MSG_HEAD_LEN + len;

	if (is_empty &&
		free_space < need_space)
	{
		/*
		 * need enlarge space
		 * now free_space equal ned->sendBuf.maxlen
		 */
		int max_len = ned->sendBuf.maxlen + DR_SOCKET_BUF_SIZE_STEP;
		while (max_len < need_space)
			max_len += DR_SOCKET_BUF_SIZE_STEP;
		enlargeStringInfo(&ned->sendBuf, max_len);
		free_space = ned->sendBuf.maxlen;
		Assert(free_space >= need_space);
	}

	if (free_space < need_space)
	{
		DR_NODE_DEBUG((errmsg("PutMessageToNode(node=%u, type=%d, len=%u, plan=%d) == false",
							  ned->nodeoid, msg_type, len, plan_id)));
		return false;
	}

	appendBinaryStringInfoNT(&ned->sendBuf, (char*)&len, sizeof(len));					/* message length */
	appendStringInfoCharMacro(&ned->sendBuf, msg_type);									/* message type */
	appendBinaryStringInfoNT(&ned->sendBuf, (char*)&plan_id, sizeof(plan_id));			/* plan ID */
	if (len > 0)
		appendBinaryStringInfoNT(&ned->sendBuf, data, len);								/* message data */

	DR_NODE_DEBUG((errmsg("PutMessageToNode(node=%u, type=%d, len=%u, plan=%d) == true",
						  ned->nodeoid, msg_type, len, plan_id)));

	return true;
}

static void OnNodeEvent(DROnEventArgs)
{
#ifdef WITH_REDUCE_RDMA
	DRNodeEventData *ned = (DRNodeEventData*)base;
	Assert(ned->base.type == DR_EVENT_DATA_NODE);
	DR_NODE_DEBUG((errmsg("node %d got events %d", ned->nodeoid, events)));
	if ((events & (POLLIN|POLLPRI|POLLHUP|POLLERR)) &&
		ned->status != DRN_WAIT_CLOSE &&
		RecvMessageFromNode(ned, ned->base.fd) > 0)
		PorcessNodeEventData(ned);
	if ((events & POLLOUT) &&
		ned->status != DRN_WAIT_CLOSE)
		OnNodeSendMessage(ned, ned->base.fd);
#elif defined DR_USING_EPOLL
	DRNodeEventData *ned = (DRNodeEventData*)base;
	Assert(ned->base.type == DR_EVENT_DATA_NODE);
	DR_NODE_DEBUG((errmsg("node %d got events %d", ned->nodeoid, events)));
	if ((events & (EPOLLIN|EPOLLPRI|EPOLLHUP|EPOLLERR)) &&
		ned->status != DRN_WAIT_CLOSE &&
		RecvMessageFromNode(ned, ned->base.fd) > 0)
		PorcessNodeEventData(ned);
	if ((events & EPOLLOUT) &&
		ned->status != DRN_WAIT_CLOSE)
		OnNodeSendMessage(ned, ned->base.fd);
#else
	DRNodeEventData *ned = ev->user_data;
	Assert(ned->base.type == DR_EVENT_DATA_NODE);
	DR_NODE_DEBUG((errmsg("node %d got events %d", ned->nodeoid, ev->events)));
	if ((ev->events & WL_SOCKET_READABLE) &&
		RecvMessageFromNode(ned, ev->fd) > 0)
		PorcessNodeEventData(ned);
	if ((ev->events & WL_SOCKET_WRITEABLE) &&
		ned->status != DRN_WAIT_CLOSE)
		OnNodeSendMessage(ned, ev->fd);
#endif
}

static void OnNodeError(DROnErrorArgs)
{
	FreeNodeEventInfo((DRNodeEventData*)base);
}

static void OnPreWaitNode(DROnPreWaitArgs)
{
	DRNodeEventData *ned = (DRNodeEventData*)base;
	uint32 need_event;
	Assert(base->type == DR_EVENT_DATA_NODE);
#if (!defined DR_USING_EPOLL) && (!defined WITH_REDUCE_RDMA)
	Assert(GetWaitEventData(dr_wait_event_set, pos) == base);
#endif

	if (OidIsValid(ned->nodeoid))
	{
		need_event = 0;
#ifdef WITH_REDUCE_RDMA
		if (ned->recvBuf.maxlen > ned->recvBuf.len &&
			DRCurrentPlanCount() > 0)
			need_event |= POLLIN;
		if (ned->sendBuf.len > ned->sendBuf.cursor)
			need_event |= POLLOUT;

		if (need_event != ned->waiting_events)
		{
			DR_NODE_DEBUG((errmsg("need_event %d ned->waiting_events %d", need_event, ned->waiting_events)));
			DR_NODE_DEBUG((errmsg("node %d set wait events %d", ned->nodeoid, need_event)));
			RDRCtlWaitEvent(ned->base.fd, need_event, ned, RPOLL_EVENT_MOD);
			ned->waiting_events = need_event;
		}
#elif defined DR_USING_EPOLL
		if (ned->recvBuf.maxlen > ned->recvBuf.len &&
			DRCurrentPlanCount() > 0)
			need_event |= EPOLLIN;
		if (ned->sendBuf.len > ned->sendBuf.cursor)
			need_event |= EPOLLOUT;
		if (need_event != ned->waiting_events)
		{
			DR_NODE_DEBUG((errmsg("node %d set wait events %d", ned->nodeoid, need_event)));
			DRCtlWaitEvent(ned->base.fd, need_event, ned, EPOLL_CTL_MOD);
			ned->waiting_events = need_event;
		}
#else /* DR_USING_EPOLL */
		if (ned->recvBuf.maxlen > ned->recvBuf.len &&
			DRCurrentPlanCount() > 0)
			need_event |= WL_SOCKET_READABLE;
		if (ned->sendBuf.len > ned->sendBuf.cursor)
			need_event |= WL_SOCKET_WRITEABLE;
		DR_NODE_DEBUG((errmsg("node %d set wait events %d", ned->nodeoid, need_event)));
		ModifyWaitEvent(dr_wait_event_set, pos, need_event, NULL);
#endif /* DR_USING_EPOLL */
	}
}

ssize_t RecvMessageFromNode(DRNodeEventData *ned, pgsocket fd)
{
	ssize_t size;
	int space;
	if (ned->recvBuf.cursor != 0 &&
		ned->recvBuf.len == ned->recvBuf.cursor)
	{
		ned->recvBuf.cursor = ned->recvBuf.len = 0;
		space = ned->recvBuf.maxlen;
	}else
	{
		space = ned->recvBuf.maxlen - ned->recvBuf.len;

		if (space == 0)
		{
			if (ned->recvBuf.cursor)
			{
				memmove(ned->recvBuf.data,
						ned->recvBuf.data + ned->recvBuf.cursor,
						ned->recvBuf.len - ned->recvBuf.cursor);
				ned->recvBuf.len -= ned->recvBuf.cursor;
				ned->recvBuf.cursor = 0;
			}else
			{
				enlargeStringInfo(&ned->recvBuf, ned->recvBuf.maxlen + 4096);
			}
			space = ned->recvBuf.maxlen - ned->recvBuf.len;
		}
	}
	Assert(space > 0);

rerecv_:
#ifdef WITH_REDUCE_RDMA
	size = adb_rrecv(fd,
				ned->recvBuf.data + ned->recvBuf.len,
				space,
				0);
#else
	size = recv(fd,
				ned->recvBuf.data + ned->recvBuf.len,
				space,
				0);
#endif
	if (size < 0)
	{
		if (errno == EINTR)
			goto rerecv_;
		if (errno == EWOULDBLOCK)
			return 0;
		ned->status = DRN_WAIT_CLOSE;
		if (dr_status == DRS_STARTUPED)
			return 0;
		/* is in working nodes? */
		if (ned->nodeoid != InvalidOid &&
			DRFindNodeInfo(ned->nodeoid) == NULL)
			return 0;
		ereport(ERROR,
				(errmsg("could not recv message from node %u:%m", ned->nodeoid),
				 DRKeepError()));
	}else if (size == 0)
	{
		ned->status = DRN_WAIT_CLOSE;
		if (dr_status == DRS_STARTUPED)
			return 0;
		/* is in working nodes? */
		if (ned->nodeoid != InvalidOid &&
			DRFindNodeInfo(ned->nodeoid) == NULL)
			return 0;

		ereport(ERROR,
				(errmsg("remote node %u closed socket", ned->nodeoid),
				 DRKeepError()));
	}
	ned->recvBuf.len += size;
	DR_NODE_DEBUG((errmsg("node %u got message of length %zd from remote", ned->nodeoid, size)));
	return size;
}

static DRPlanCacheData* NodeGetPlanCache(DRNodeEventData *ned, int plan_id)
{
	DRPlanCacheData *result;
	bool found;

	if (ned->cached_data == NULL)
	{
		HASHCTL ctl;
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(int);
		ctl.entrysize = sizeof(DRPlanCacheData);
		ctl.hash = uint32_hash;
		ctl.hcxt = GetMemoryChunkContext(ned->sendBuf.data);
		ned->cached_data = hash_create("node cache plan data",
									   512,
									   &ctl,
									   HASH_ELEM|HASH_FUNCTION|HASH_CONTEXT);
	}

	result = hash_search(ned->cached_data, &plan_id, HASH_ENTER, &found);
	if (found == false)
	{
		char name[MAXPGPATH];
		MemoryContext oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(ned->sendBuf.data));
		MemSet(result, 0, sizeof(*result));
		result->plan_id = plan_id;
		result->file_no = DRNextSharedFileSetNumber();
		result->file = BufFileCreateShared(dr_shared_fs,
										   DynamicReduceSharedFileName(name, result->file_no));
		MemoryContextSwitchTo(oldcontext);
	}

	return result;
}

static int PorcessNodeEventData(DRNodeEventData *ned)
{
	PlanInfo	   *pi;
	DRPlanCacheData*cache;
	StringInfoData	buf;
	uint32			msglen;
	int				msgtype;
	int				plan_id;
	int				msg_count = 0;
	bool			pause_recv = false;
	bool			need_more = false;
	Assert(OidIsValid(ned->nodeoid));

	ned->waiting_plan_id = INVALID_PLAN_ID;
	pi = NULL;
	cache = NULL;
	for (;;)
	{
		buf = ned->recvBuf;
		if (buf.len - buf.cursor < NODE_MSG_HEAD_LEN)
		{
			need_more = true;
			break;
		}

		pq_copymsgbytes(&buf, (char*)&msglen, sizeof(msglen));
		msgtype = pq_getmsgbyte(&buf);
		pq_copymsgbytes(&buf, (char*)&plan_id, sizeof(plan_id));

		if (buf.len - buf.cursor < msglen)
		{
			DR_NODE_DEBUG((errmsg("node %u need message data length %u, but now is only %d",
								  ned->nodeoid, msglen, buf.len-buf.cursor)));
			need_more = true;
			break;
		}

		if (pi == NULL ||
			pi->plan_id != plan_id)
		{
			pi = DRPlanSearch(plan_id, HASH_FIND, NULL);
			if (pi == NULL &&
				ned->cached_data &&
				(cache = hash_search(ned->cached_data, &plan_id, HASH_FIND, NULL)) != NULL &&
				cache->locked)
			{
				/* have cache data, we do not process data */
				pause_recv = true;
				break;
			}
		}

		DR_NODE_DEBUG((errmsg("node %u processing message %d plan %d(%p) length %u",
					   ned->nodeoid, msgtype, plan_id, pi, msglen)));

		if (msgtype == ADB_DR_MSG_TUPLE)
		{
			if (pi == NULL)
			{
				if (cache == NULL ||
					cache->plan_id != plan_id)
					cache = NodeGetPlanCache(ned, plan_id);
				Assert(cache->locked == false);
				DynamicReduceWriteSFSMsgTuple(cache->file, buf.data+buf.cursor, msglen);
			}else if((*pi->OnNodeRecvedData)(pi, buf.data+buf.cursor, msglen, ned->nodeoid) == false)
			{
				DR_NODE_DEBUG((errmsg("node %u put tuple to plan %d(%p) return false", ned->nodeoid, plan_id, pi)));
				ned->waiting_plan_id = plan_id;
				break;
			}
		}else if (msgtype == ADB_DR_MSG_END_OF_PLAN)
		{
			if (pi == NULL)
			{
				if (cache == NULL ||
					cache->plan_id != plan_id)
					cache = NodeGetPlanCache(ned, plan_id);
				Assert(cache->got_eof == false);
				Assert(cache->locked == false);
				cache->got_eof = true;
			}else if((*pi->OnNodeEndOfPlan)(pi, ned->nodeoid) == false)
			{
				DR_NODE_DEBUG((errmsg("node %u put end of plan %d(%p) return false", ned->nodeoid, plan_id, pi)));
				ned->waiting_plan_id = plan_id;
				break;
			}
		}else
		{
			ned->status = DRN_WAIT_CLOSE;
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unknown message type %d from node %u", msgtype, ned->nodeoid)));
		}

		buf.cursor += msglen;
		++msg_count;

		/* update buffer */
		ned->recvBuf.cursor = buf.cursor;
	}

	if ((msg_count == 0 || need_more) &&
		pause_recv == false &&
		ned->waiting_plan_id == INVALID_PLAN_ID &&
		ned->recvBuf.len == ned->recvBuf.maxlen)
	{
		/* enlarge free space */
		if (ned->recvBuf.cursor > 0)
		{
			memmove(ned->recvBuf.data,
					ned->recvBuf.data + ned->recvBuf.cursor,
					ned->recvBuf.len - ned->recvBuf.cursor);
			ned->recvBuf.len -= ned->recvBuf.cursor;
			ned->recvBuf.cursor = 0;
		}else
		{
			enlargeStringInfo(&ned->recvBuf,
							  ned->recvBuf.maxlen + DR_SOCKET_BUF_SIZE_STEP);
		}
	}

	if (msg_count > 0)
	{
		if (ned->recvBuf.cursor == ned->recvBuf.len)
		{
			ned->recvBuf.cursor = ned->recvBuf.len = 0;
		}else if (ned->recvBuf.len - ned->recvBuf.cursor <= (ned->recvBuf.maxlen >> 2))
		{
			memmove(ned->recvBuf.data,
					ned->recvBuf.data + ned->recvBuf.cursor,
					ned->recvBuf.len - ned->recvBuf.cursor);
			ned->recvBuf.len -= ned->recvBuf.cursor;
			ned->recvBuf.cursor = 0;
		}
	}

	return msg_count;
}

bool DRNodeFetchTuple(DRNodeEventData *ned, int fetch_plan_id, char **data, int *len)
{
	StringInfoData	buf;
	uint32			msglen;
	int				msgtype;
	int				plan_id;

	if (ned->waiting_plan_id != INVALID_PLAN_ID &&
		ned->waiting_plan_id != fetch_plan_id)
	{
		DR_NODE_DEBUG((errmsg("node %u return false for plan %d fetch, because it waiting plan %d",
							  ned->nodeoid, fetch_plan_id, ned->waiting_plan_id)));
		return false;	/* quick return */
	}

	if (ned->recvBuf.len - ned->recvBuf.cursor < NODE_MSG_HEAD_LEN)
	{
		DR_NODE_DEBUG((errmsg("node %u return false for plan %d fetch, because data less then message head",
							  ned->nodeoid, fetch_plan_id)));
		return false;
	}

	buf = ned->recvBuf;
	pq_copymsgbytes(&buf, (char*)&msglen, sizeof(msglen));
	msgtype = pq_getmsgbyte(&buf);
	pq_copymsgbytes(&buf, (char*)&plan_id, sizeof(plan_id));
	if (buf.len - buf.cursor < msglen)
	{
		DR_NODE_DEBUG((errmsg("node %u return false for plan %d fetch, because data length not enough",
							  ned->nodeoid, fetch_plan_id)));
		return false;
	}
	if (plan_id != fetch_plan_id)
	{
		DR_NODE_DEBUG((errmsg("node %u return false for plan %d fetch, because current message is for plan %d",
							  ned->nodeoid, fetch_plan_id, plan_id)));
		return false;
	}

	if (msgtype == ADB_DR_MSG_TUPLE)
	{
		*data = buf.data + buf.cursor;
		*len = msglen;
	}else if (msgtype == ADB_DR_MSG_END_OF_PLAN)
	{
		*data = NULL;
		*len = 0;
	}else
	{
		ned->status = DRN_WAIT_CLOSE;
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("unknown message type %d from node %u", msgtype, ned->nodeoid)));
	}

	ned->recvBuf.cursor += msglen;
	ned->waiting_plan_id = INVALID_PLAN_ID;
	return true;
}

static void OnNodeSendMessage(DRNodeEventData *ned, pgsocket fd)
{
	ssize_t result;

	Assert(ned->base.type == DR_EVENT_DATA_NODE);
	Assert(ned->sendBuf.len > ned->sendBuf.cursor);
	if (ned->status == DRN_WAIT_CLOSE)
		return;

resend_:
#ifdef WITH_REDUCE_RDMA
	result = adb_rsend(fd,
				  ned->sendBuf.data + ned->sendBuf.cursor,
				  ned->sendBuf.len - ned->sendBuf.cursor,
				  0);
#else
	result = send(fd,
				  ned->sendBuf.data + ned->sendBuf.cursor,
				  ned->sendBuf.len - ned->sendBuf.cursor,
				  0);
#endif
	if (result >= 0)
	{
		DR_NODE_DEBUG((errmsg("node %u send message of length %zd to remote success", ned->nodeoid, result)));
		ned->sendBuf.cursor += result;
		if (ned->sendBuf.cursor == ned->sendBuf.len)
			ned->sendBuf.cursor = ned->sendBuf.len = 0;
		ActiveWaitingPlan(ned);
	}else
	{
		if (errno == EINTR)
			goto resend_;
		ereport(ERROR,
				(errmsg("can not send message to node %u: %m", ned->nodeoid),
				 DRKeepError()));
	}
}

#ifdef WITH_REDUCE_RDMA
static uint32 hash_rsnode_data(const void *key, Size keysize)
{
	Assert(keysize == sizeof(dr_rshandle_item));

	return ((dr_rshandle_item*)key)->fd;
}

static int compare_rsnode_data(const void *key1, const void *key2, Size keysize)
{
	Assert(keysize == sizeof(dr_rshandle_item));

	return (int)((dr_rshandle_item*)key1)->fd - (int)((dr_rshandle_item*)key2)->fd;
}
#endif

static uint32 hash_node_event_data(const void *key, Size keysize)
{
	Assert(keysize == sizeof(DRNodeEventData));

	return ((DRNodeEventData*)key)->nodeoid;
}

static int compare_node_event_data(const void *key1, const void *key2, Size keysize)
{
	Assert(keysize == sizeof(DRNodeEventData));

	return (int)((DRNodeEventData*)key1)->nodeoid - (int)((DRNodeEventData*)key2)->nodeoid;
}

static void* copy_node_event_key(void *dest, const void *src, Size keysize)
{
	MemSet(dest, 0, sizeof(DRNodeEventData));
	((DRNodeEventData*)dest)->nodeoid = ((DRNodeEventData*)src)->nodeoid;
	((DRNodeEventData*)dest)->waiting_plan_id = INVALID_PLAN_ID;

	return dest;
}

void DRNodeReset(DRNodeEventData *ned)
{
	if (ned->status != DRN_WORKING ||
		ned->waiting_plan_id != INVALID_PLAN_ID ||
		ned->recvBuf.len != ned->recvBuf.cursor ||
		ned->sendBuf.len != ned->sendBuf.cursor ||
		(ned->cached_data && hash_get_num_entries(ned->cached_data) > 0))
		FreeNodeEventInfo(ned);
}

static bool ProcessNodeCacheData(DRNodeEventData *ned, int planid)
{
	DRPlanCacheData *cache;
	if (ned->cached_data &&
		(cache = hash_search(ned->cached_data, &planid, HASH_FIND, NULL)) != NULL)
	{
		PlanInfo *pi = DRPlanSearch(planid, HASH_FIND, NULL);
		Assert(pi != NULL && pi->plan_id == planid);
		DR_NODE_DEBUG((errmsg("plan %d activing node %u for cache", planid, ned->nodeoid)));
		if (pi->ProcessCachedData)
		{
			(*pi->ProcessCachedData)(pi, cache, ned->nodeoid);
			CleanNodePlanCacheData(cache, false);
			hash_search(ned->cached_data, &planid, HASH_REMOVE, NULL);
			return false;
		}
		if (cache->buf.data == NULL)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(cache->file));
			initStringInfo(&cache->buf);
			MemoryContextSwitchTo(oldcontext);
		}
		if (cache->locked == false)
		{
			if (BufFileSeek(cache->file, 0, 0, SEEK_SET) != 0)
			{
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("can not seek buffer file to head")));
			}
			cache->locked = true;
		}

		for(;;)
		{
			if (cache->buf.len == 0 &&	/* have no last readed data */
				DRReadSFSTupleData(cache->file, &cache->buf) == false)
				break;
			Assert(cache->buf.len > 0);
			if ((*pi->OnNodeRecvedData)(pi, cache->buf.data, cache->buf.len, ned->nodeoid))
				resetStringInfo(&cache->buf);
			else
				return true;
		}

		if (cache->got_eof == false ||
			(*pi->OnNodeEndOfPlan)(pi, ned->nodeoid))
		{
			CleanNodePlanCacheData(cache, true);
			hash_search(ned->cached_data, &planid, HASH_REMOVE, NULL);
			return false;
		}
		return true;
	}

	return false;
}

void DRActiveNode(int planid)
{
	DRNodeEventData	   *ned;
#if (defined DR_USING_EPOLL) || (defined WITH_REDUCE_RDMA)
	HASH_SEQ_STATUS		seq;

	hash_seq_init(&seq, htab_node_info);
	while ((ned = hash_seq_search(&seq)) != NULL)
	{
		if (ned->base.type == DR_EVENT_DATA_NODE &&
			ProcessNodeCacheData(ned, planid) == false &&
			(ned->waiting_plan_id == planid ||
			 ned->recvBuf.len - ned->recvBuf.cursor >= NODE_MSG_HEAD_LEN))
		{
			DR_NODE_DEBUG((errmsg("plan %d activing node %u", planid, ned->nodeoid)));
			PorcessNodeEventData(ned);
		}
	}
#else /* DR_USING_EPOLL */
	Size				i;

	for (i=0;i<dr_wait_count;++i)
	{
		ned = GetWaitEventData(dr_wait_event_set, i);
		if (ned->base.type == DR_EVENT_DATA_NODE &&
			ProcessNodeCacheData(ned, planid) == false &&
			(ned->waiting_plan_id == planid ||
			 ned->recvBuf.len - ned->recvBuf.cursor >= NODE_MSG_HEAD_LEN))
		{
			DR_NODE_DEBUG((errmsg("plan %d activing node %u", planid, ned->nodeoid)));
			PorcessNodeEventData(ned);
		}
	}
#endif /* DR_USING_EPOLL */
}

void DRInitNodeSearch(void)
{
	HASHCTL ctl;
	if (htab_node_info == NULL)
	{
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(DRNodeEventData);
		ctl.entrysize = sizeof(DRNodeEventData);
		ctl.hash = hash_node_event_data;
		ctl.match = compare_node_event_data;
		ctl.keycopy = copy_node_event_key;
		ctl.hcxt = TopMemoryContext;
		htab_node_info = hash_create("Dynamic reduce node info",
									 DR_HTAB_DEFAULT_SIZE,
									 &ctl,
									 HASH_ELEM|HASH_CONTEXT|HASH_FUNCTION|HASH_COMPARE|HASH_KEYCOPY);
	}
}

DRNodeEventData* DRSearchNodeEventData(Oid nodeoid, HASHACTION action, bool *found)
{
	DRNodeEventData ned;
	Assert(OidIsValid(nodeoid));

	ned.nodeoid = nodeoid;
	return hash_search_with_hash_value(htab_node_info, &ned, nodeoid, action, found);
}

#ifdef WITH_REDUCE_RDMA
void dr_create_rhandle_list(void)
{
	HASHCTL ctl;
	if (htab_rsnode_info == NULL)
	{
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(dr_rshandle_item);
		ctl.entrysize = sizeof(dr_rshandle_item);
		ctl.hash = hash_rsnode_data;
		ctl.match = compare_rsnode_data;
		ctl.hcxt = TopMemoryContext;
		htab_rsnode_info = hash_create("Dynamic reduce rs node info",
									 DR_HTAB_DEFAULT_SIZE,
									 &ctl,
									 HASH_ELEM|HASH_CONTEXT|HASH_FUNCTION|HASH_COMPARE);
	}
}

DRNodeEventData* dr_rhandle_find(int fd)
{
	bool found;
	dr_rshandle_item* item;
	found = false;
	item = hash_search(htab_rsnode_info, &fd, HASH_FIND, &found);
	Assert(found);

	return item->ptr;
}

void dr_rhandle_add(int fd, void *info)
{
	bool found;
	dr_rshandle_item* item;
	found = false;
	item = hash_search(htab_rsnode_info, &fd, HASH_ENTER, &found);

	Assert(!found);
	
	item->ptr = (DRNodeEventData*)info;
}

void dr_rhandle_del(int fd)
{
	bool found;
	found = false;
	hash_search(htab_rsnode_info, &fd, HASH_REMOVE, &found);
	Assert(found);
}

void dr_rhandle_mod(int fd, void *info)
{
	bool found;
	dr_rshandle_item* item;
	found = false;
	item = hash_search(htab_rsnode_info, &fd, HASH_FIND, &found);
	Assert(found);

	item->ptr = (DRNodeEventData*)info;
}
#endif /* WITH_REDUCE_RDMA */

#if (defined DR_USING_EPOLL) || (defined WITH_REDUCE_RDMA)
bool DRNodeSeqInit(HASH_SEQ_STATUS *seq)
{
	if(htab_node_info)
	{
		hash_seq_init(seq, htab_node_info);
		return true;
	}
	return false;
}
#endif /* DR_USING_EPOLL || WITH_REDUCE_RDMA*/

void CleanNodePlanCacheData(DRPlanCacheData *cache, bool delete_file)
{
	char name[MAXPGPATH];
	if (cache->file)
	{
		BufFileClose(cache->file);
		cache->file = NULL;
		if (delete_file)
		{
			BufFileDeleteShared(dr_shared_fs,
								DynamicReduceSharedFileName(name, cache->file_no));
		}
	}
	if (cache->buf.data)
	{
		pfree(cache->buf.data);
		cache->buf.data = NULL;
	}
}

void DropNodeAllPlanCacheData(DRNodeEventData *ned, bool delete_file)
{
	HASH_SEQ_STATUS seq;
	DRPlanCacheData *cache;
	if (ned->cached_data == NULL)
		return;

	hash_seq_init(&seq, ned->cached_data);
	while ((cache=hash_seq_search(&seq)) != NULL)
		CleanNodePlanCacheData(cache, delete_file);
	hash_destroy(ned->cached_data);
	ned->cached_data = NULL;
}
