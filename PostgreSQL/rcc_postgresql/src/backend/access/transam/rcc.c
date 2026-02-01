#ifdef RCC
#include "postgres.h"
#include "portability/instr_time.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/proc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "access/xact.h"
#include "storage/lmgr.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/array.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "storage/procarray.h"
#include "access/rcc.h"

/* Local Dependency (from → to) */
#define MAX_DEP 128
static TransactionId MyReadDependencies[MAX_DEP];
static int MyReadDependenciesCount = 0;

/* Global Reverse Dependency (to → from) - Partitioned for reduced contention */
typedef struct TxnReverseDepEntry
{
    TransactionId to;
    TransactionId from_list[MAX_DEP];
    int nfroms;
} TxnReverseDepEntry;

/* Partitioned Reverse Dependency Graph */
#define NUM_DEP_PARTITIONS 64
#define DEP_PARTITION(xid) ((xid) % NUM_DEP_PARTITIONS)

typedef struct DepGraphPartition
{
    HTAB *graph;
    LWLock *lock;
} DepGraphPartition;

static DepGraphPartition *TxnReverseDepPartitions = NULL;

/* Local Initialization */
void
InitTxnDependency(void)
{
    MyReadDependenciesCount = 0;
}

/* Shared Memory Allocation */
#define RCC_HASH_SIZE 500000  /* Total entries across all partitions */

Size
RccShmemSize(void)
{
    Size sz = 0;
    /* Partitioned structure */
    sz += MAXALIGN(sizeof(DepGraphPartition) * NUM_DEP_PARTITIONS);
    /* Each partition's hash table */
    sz += MAXALIGN(hash_estimate_size(RCC_HASH_SIZE / NUM_DEP_PARTITIONS, sizeof(TxnReverseDepEntry))) * NUM_DEP_PARTITIONS;
    /* Each partition's lock */
    sz += MAXALIGN(sizeof(LWLockPadded)) * NUM_DEP_PARTITIONS;
    return BUFFERALIGN(sz);
}

void
RccShmemInit(void)
{
    bool found;
    char name[64];

    /* Allocate partition array */
    TxnReverseDepPartitions = ShmemInitStruct("RccRevDepPartitions",
                                              sizeof(DepGraphPartition) * NUM_DEP_PARTITIONS,
                                              &found);

    for (int i = 0; i < NUM_DEP_PARTITIONS; i++)
    {
        LWLockPadded *lock;
        HASHCTL ctl;

        /* Initialize lock for this partition */
        snprintf(name, sizeof(name), "RccRevDepLock_%d", i);
        lock = ShmemInitStruct(name, sizeof(LWLockPadded), &found);
        if (!found)
            LWLockInitialize(&(lock->lock), LWLockNewTrancheId());
        TxnReverseDepPartitions[i].lock = &(lock->lock);

        /* Initialize hash table for this partition */
        memset(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(TransactionId);
        ctl.entrysize = sizeof(TxnReverseDepEntry);
        snprintf(name, sizeof(name), "TxnRevDepGraph_%d", i);
        TxnReverseDepPartitions[i].graph = ShmemInitHash(name,
                                                         RCC_HASH_SIZE / NUM_DEP_PARTITIONS,
                                                         RCC_HASH_SIZE / NUM_DEP_PARTITIONS,
                                                         &ctl,
                                                         HASH_ELEM | HASH_BLOBS);
    }
}

void
RecordReadDependency(TransactionId xmin, TransactionId myxid)
{
    if (!TransactionIdIsValid(xmin) || xmin == myxid)
        return;

    /* Check local dependencies first (no lock needed) */
    for (int i = 0; i < MyReadDependenciesCount; i++)
    {
        if (MyReadDependencies[i] == xmin)
            return;
    }
    Assert(MyReadDependenciesCount < MAX_DEP);
    MyReadDependencies[MyReadDependenciesCount++] = xmin;

    /* Record in partitioned reverse dependency graph */
    int part = DEP_PARTITION(xmin);
    DepGraphPartition *partition = &TxnReverseDepPartitions[part];

    LWLockAcquire(partition->lock, LW_EXCLUSIVE);

    bool found;
    TxnReverseDepEntry *rev = hash_search(partition->graph, &xmin, HASH_ENTER, &found);
    if (!found)
        rev->nfroms = 0;

    /* Check if already recorded */
    for (int i = 0; i < rev->nfroms; i++)
    {
        if (rev->from_list[i] == myxid)
        {
            LWLockRelease(partition->lock);
            return;
        }
    }
    Assert(rev->nfroms < MAX_DEP);
    rev->from_list[rev->nfroms++] = myxid;

    LWLockRelease(partition->lock);
#ifdef RCC_DEBUG
    elog(LOG, "[DepGraph] TX %u depends on TX %u", myxid, xmin);
#endif    
}

/*
 * Check if there's a circular dependency.
 *
 * Current situation: myxid wants to wait for depxid (myxid -> depxid).
 * Circular if: depxid also depends on myxid (depxid -> ... -> myxid).
 *
 * To check if depxid depends on myxid, we use the reverse dependency graph.
 * reverse[X] = {transactions that depend on X}
 *
 * Start from myxid and follow reverse edges. If we reach depxid,
 * it means depxid is in the set of transactions that (transitively) depend on myxid.
 * Combined with myxid -> depxid, this forms a cycle.
 *
 * Acquires all partition locks in order to prevent deadlock during check.
 */
static bool
IsCircularDependency(TransactionId depxid, TransactionId myxid)
{
    if (!TransactionIdIsValid(depxid) || !TransactionIdIsValid(myxid))
        return false;

    /* Acquire all partition locks in order (prevent lock deadlock) */
    for (int i = 0; i < NUM_DEP_PARTITIONS; i++)
        LWLockAcquire(TxnReverseDepPartitions[i].lock, LW_SHARED);

    /*
     * DFS: Start from myxid, follow reverse edges (who depends on X).
     * If we find depxid, then depxid depends on myxid -> circular!
     */
    TransactionId stack[MAX_DEP];
    TransactionId parent[MAX_DEP];  /* Track parent for path reconstruction */
    int stackSize = 0;
    int visitedCount = 0;
    TransactionId visitedXids[MAX_DEP];
    TransactionId visitedParent[MAX_DEP];  /* Parent of each visited node */

    stack[stackSize] = myxid;
    parent[stackSize] = InvalidTransactionId;
    stackSize++;
    bool found = false;

    while (stackSize > 0 && !found)
    {
        stackSize--;
        TransactionId current = stack[stackSize];
        TransactionId currentParent = parent[stackSize];

        /* Found depxid! It depends on myxid (transitively) */
        if (current == depxid)
        {
            found = true;
            /* Record depxid and its parent for path reconstruction */
            if (visitedCount < MAX_DEP)
            {
                visitedXids[visitedCount] = current;
                visitedParent[visitedCount] = currentParent;
                visitedCount++;
            }
            break;
        }

        /* Check if already visited */
        bool alreadyVisited = false;
        for (int i = 0; i < visitedCount; i++)
        {
            if (visitedXids[i] == current)
            {
                alreadyVisited = true;
                break;
            }
        }
        if (alreadyVisited)
            continue;

        if (visitedCount >= MAX_DEP)
        {
            elog(WARNING, "[DepGraph] Visited overflow in IsCircularDependency: visitedCount=%d, MAX_DEP=%d, myxid=%u, depxid=%u",
                 visitedCount, MAX_DEP, myxid, depxid);
        }
        else
        {
            visitedXids[visitedCount] = current;
            visitedParent[visitedCount] = currentParent;
            visitedCount++;
        }

        /* Look up who depends on current */
        int part = DEP_PARTITION(current);
        TxnReverseDepEntry *rev = hash_search(TxnReverseDepPartitions[part].graph,
                                              &current, HASH_FIND, NULL);
        if (rev)
        {
            for (int i = 0; i < rev->nfroms; i++)
            {
                TransactionId from = rev->from_list[i];
                /* Skip aborted/committed transactions - they can't cause deadlock */
                if (!TransactionIdIsInProgress(from))
                    continue;

                if (stackSize >= MAX_DEP)
                {
                    elog(WARNING, "[DepGraph] Stack overflow in IsCircularDependency: stackSize=%d, MAX_DEP=%d, myxid=%u, depxid=%u",
                         stackSize, MAX_DEP, myxid, depxid);
                    break;
                }
                stack[stackSize] = from;
                parent[stackSize] = current;
                stackSize++;
            }
        }
    }

    /* Print cycle path if found */
#ifdef RCC_DEBUG    
    if (found)
    {
        StringInfoData buf;
        initStringInfo(&buf);

        /* Reconstruct path from depxid back to myxid */
        TransactionId path[MAX_DEP];
        int pathLen = 0;
        TransactionId cur = depxid;

        while (TransactionIdIsValid(cur) && pathLen < MAX_DEP)
        {
            path[pathLen++] = cur;
            /* Find parent of cur */
            TransactionId nextParent = InvalidTransactionId;
            for (int i = 0; i < visitedCount; i++)
            {
                if (visitedXids[i] == cur)
                {
                    nextParent = visitedParent[i];
                    break;
                }
            }
            cur = nextParent;
        }

        /* Print path: myxid -> depxid <- path[pathLen-2] <- ... <- path[0] */
        appendStringInfo(&buf, "[DepGraph] CYCLE DETECTED: TX %u -> TX %u", myxid, depxid);
        appendStringInfo(&buf, " | Reverse path (who depends on whom): ");
        for (int i = pathLen - 1; i >= 0; i--)
        {
            if (i == pathLen - 1)
                appendStringInfo(&buf, "%u", path[i]);
            else
                appendStringInfo(&buf, " <- %u", path[i]);
        }

        elog(LOG, "%s", buf.data);
        pfree(buf.data);
    }
#endif
    /* Release all partition locks in reverse order */
    for (int i = NUM_DEP_PARTITIONS - 1; i >= 0; i--)
        LWLockRelease(TxnReverseDepPartitions[i].lock);

    return found;
}

bool
WaitForTxnDependencies(void)
{
    TransactionId myxid = GetTopTransactionIdIfAny();
    if (!TransactionIdIsValid(myxid))
        return false;
        //커밋하기 전에도 자신이 abort되지 않았는지 마지막으로 체크함.
#ifdef RCC_DEBUG
    if (MyReadDependenciesCount == 0)
    {
        fprintf(stderr, "[DepGraph] No dependencies to wait for TX_ID: %u\n", myxid);
        return false;
    }

    StringInfoData buf;
    initStringInfo(&buf);
    appendStringInfo(&buf, "[DepGraph] Waiting for dependencies of TX_ID: %u:", myxid);
    for (int i = 0; i < MyReadDependenciesCount; i++)
        appendStringInfo(&buf, " %u", MyReadDependencies[i]);
    appendStringInfoChar(&buf, '\n');
    fputs(buf.data, stderr);
    pfree(buf.data);
#endif    

    for (int i = 0; i < MyReadDependenciesCount; i++)
    {
        TransactionId depxid = MyReadDependencies[i];
        if (TransactionIdDidAbort(depxid))
            return true;
            
        if (TransactionIdIsInProgress(depxid))
        {
            /*
             * Check for circular dependency BEFORE waiting.
             * If depxid depends on me (directly or transitively),
             * waiting would cause deadlock.
             */
            if (IsCircularDependency(depxid, myxid))
            {
#ifdef RCC_DEBUG
                elog(LOG, "[DepGraph] Circular dependency detected: TX %u -> TX %u, aborting",
                     myxid, depxid);
#endif
                return true;  /* Abort to break deadlock */
            }

            /* Breakdown Code - measure txwait time for dependency wait */
            instr_time txwait_start, txwait_end;
            INSTR_TIME_SET_CURRENT(txwait_start);

            XactLockTableWait(depxid, NULL, NULL, XLTW_None);

            INSTR_TIME_SET_CURRENT(txwait_end);
            INSTR_TIME_SUBTRACT(txwait_end, txwait_start);
            xact_count_txwait_time(INSTR_TIME_GET_MICROSEC(txwait_end));
            /* Breakdown Code End */

            if (TransactionIdDidAbort(depxid))
                return true;
        }
    }
    return false;
}

void
CleanupTxnDependency()
{
    TransactionId myxid = GetTopTransactionIdIfAny();
    if(!TransactionIdIsValid(myxid) ||
        MyReadDependenciesCount == 0)
        return;

    /* Remove from reverse dependency graph (partitioned) */
    int part = DEP_PARTITION(myxid);
    DepGraphPartition *partition = &TxnReverseDepPartitions[part];

    LWLockAcquire(partition->lock, LW_EXCLUSIVE);
    hash_search(partition->graph, &myxid, HASH_REMOVE, NULL);
    LWLockRelease(partition->lock);

    MyReadDependenciesCount = 0;
}


bool 
CheckAbortedDependencies(void)
{
    //여기서도, 다른 트랜잭션이 나를 abort시킬 수도 있으니, 내가 abort되었는지 체크
    TransactionId myxid = GetTopTransactionIdIfAny();
    if (!TransactionIdIsValid(myxid))
        return false;  /* No active transaction → no dependencies */
    if (TransactionIdDidAbort(myxid)){
        ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				errmsg("transaction aborted due to shadow conflict, TX_ID: %u", myxid)));
    }

    for (int i = 0; i < MyReadDependenciesCount; i++)
    {
        TransactionId depxid = MyReadDependencies[i];
        if (TransactionIdDidAbort(depxid)){
#ifdef RCC_DEBUG
            elog(LOG, "[DepGraph] Dependency %u has aborted: TX_ID: %u\n", depxid, myxid);
#endif            
            return true; /* ERROR 이후에도 안전하게 반환 */
        }
    }
    return false;  /* 모든 의존성이 유효함 */
}


#endif /* RCC */