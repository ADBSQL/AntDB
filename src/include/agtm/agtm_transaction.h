/*-------------------------------------------------------------------------
 *
 * agtm_transaction.h
 *
 *	  Definitions for deal transaction gxid/snapshot/timestamp command message form coordinator
 *
 * Portions Copyright (c) 2016, ASIAINFO BDX ADB Group
 *
 * src/include/agtm/agtm_transaction.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef AGTM_TRANSACTION_H
#define AGTM_TRANSACTION_H

#include "postgres.h"

#include "lib/stringinfo.h"

StringInfo ProcessGetGXIDCommand(StringInfo message, StringInfo output);

StringInfo ProcessGetTimestamp(StringInfo message, StringInfo output);

StringInfo ProcessGetSnapshot(StringInfo message, StringInfo output);

StringInfo ProcessXactLockTableWait(StringInfo message, StringInfo output);

StringInfo ProcessLockTransaction(StringInfo message, StringInfo output);

StringInfo ProcessXactLockReleaseAll(StringInfo message, StringInfo output);

void agtm_AtXactNodeClose(int pq_id);

#endif