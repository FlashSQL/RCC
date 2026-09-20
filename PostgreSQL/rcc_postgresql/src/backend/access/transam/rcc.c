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
#include "storage/procsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "utils/guc.h"
#include "utils/wait_event.h"
#include "access/rcc.h"
#include "storage/lock.h"

/* See rcc.h: true while executing a FOR UPDATE/SHARE statement. */
bool RccStmtForUpdate = false;

/* See rcc.h: true while executing an UPDATE statement (target-locate scan). */
bool RccStmtUpdateScan = false;

/* See rcc.h: a read-latest scan deferred a committed base to an in-progress
 * newer version this statement (reset per ExecutePlan). */
bool RccReadLatestDeferredCommitted = false;


/* Local Dependency (from → to) */
#define MAX_DEP 512
static TransactionId MyReadDependencies[MAX_DEP];
static int MyReadDependenciesCount = 0;

#ifdef RCC_SR
/*
 * RW anti-dependencies (write skew): the WRITER records, against each READER
 * that read a row it overwrote, an edge "writer depends on reader".  These are
 * sourced from PostgreSQL's SSI SIRead list (predicate.c) -- see RccRecordRwDep.
 * Unlike WR (MyReadDependencies), these are recorded by the WRITER against each
 * READER.  At commit the writer WAITS for each (in-progress) reader and aborts
 * only on a true serialization (wait-deadlock) cycle (MySQL-style).
 */
static TransactionId MyRwDeps[MAX_DEP];
static int MyRwDepsCount = 0;
#endif

/* Global Reverse Dependency (to → from) - Partitioned for reduced contention */
typedef struct TxnReverseDepEntry
{
    TransactionId to;
    TransactionId from_list[MAX_DEP];
    /* from_list[i] tag: true = RW edge (writer waiting on reader `to`),
     * false = WR (the dependent consumed `to`'s uncommitted data).  Not
     * consulted by cycle detection itself; kept so the WR/RW distinction
     * stays available in the shared graph. */
    uint8 from_is_rw[MAX_DEP];
    int nfroms;
} TxnReverseDepEntry;

/* Partitioned Reverse Dependency Graph */
#define NUM_DEP_PARTITIONS 256
#define DEP_PARTITION(xid) ((xid) % NUM_DEP_PARTITIONS)

typedef struct DepGraphPartition
{
    HTAB *graph;
    LWLock *lock;
} DepGraphPartition;

static DepGraphPartition *TxnReverseDepPartitions = NULL;

/* GUC: background cycle detection on/off (see rcc.h). */
bool		rcc_bg_detect = false;

/*
 * Background detector shared state.  `latch` is the detector's own
 * procLatch while it runs (NULL otherwise); `pending` is set by whoever adds
 * an edge and cleared by the detector right before a sweep, so a burst of
 * edges costs one SetLatch, not one per edge.
 */
typedef struct RccBgShared
{
	Latch	   *latch;
	volatile bool pending;
} RccBgShared;

static RccBgShared *RccBg = NULL;

static void RccBgWake(void);
static void RccCheckDoomed(TransactionId myxid);

/* Local Initialization */
void
InitTxnDependency(void)
{
    MyReadDependenciesCount = 0;
#ifdef RCC_SR
    MyRwDepsCount = 0;
#endif
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
    /* Background detector state */
    sz += MAXALIGN(sizeof(RccBgShared));
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

    RccBg = ShmemInitStruct("RccBgDetector", sizeof(RccBgShared), &found);
    if (!found)
    {
        RccBg->latch = NULL;
        RccBg->pending = false;
    }
}

/*
 * Append "dependent depends on target" to the shared reverse graph
 * (reverse[target] += dependent).  No dedup scan: the caller has confirmed via
 * the local lists that this backend has not recorded this edge, and a backend
 * only ever inserts its own xid -- so no duplicate is possible.  Holding the
 * partition lock only for the O(1) enter+append (not an O(nfroms) scan) keeps
 * contention low on hot rows with many dependents.
 */
static void
RccAddReverseEdge(TransactionId target, TransactionId dependent, bool is_rw)
{
    int part = DEP_PARTITION(target);
    DepGraphPartition *partition = &TxnReverseDepPartitions[part];
    bool found;
    TxnReverseDepEntry *rev;

    LWLockAcquire(partition->lock, LW_EXCLUSIVE);
    rev = hash_search(partition->graph, &target, HASH_ENTER, &found);
    if (!found)
        rev->nfroms = 0;

    /*
     * from_list only ever grows while `target` lives, so on a hot row it
     * fills up with dependents that have long since finished (arrival rate x
     * writer lifetime, not concurrency).  When full, compact away entries
     * that are no longer in progress: cycle detection skips those anyway
     * (finished txns cannot be part of a live wait cycle), so this loses
     * nothing.  Only when the LIVE dependents alone exceed the cap is an
     * edge truly dropped -- warn, since that can hide a cycle.
     */
    if (rev->nfroms >= MAX_DEP)
    {
        int         n = 0;

        for (int i = 0; i < rev->nfroms; i++)
            if (TransactionIdIsInProgress(rev->from_list[i]))
            {
                rev->from_is_rw[n] = rev->from_is_rw[i];
                rev->from_list[n++] = rev->from_list[i];
            }
        rev->nfroms = n;
    }

    if (rev->nfroms < MAX_DEP)
    {
        rev->from_is_rw[rev->nfroms] = is_rw;
        rev->from_list[rev->nfroms++] = dependent;
    }
    else
        elog(WARNING, "[DepGraph] reverse dep list full for TX %u even after compaction (%d live): dropping edge from TX %u",
             target, rev->nfroms, dependent);
    LWLockRelease(partition->lock);

    /* A new edge is the only thing that can close a cycle. */
    RccBgWake();
}

/*
 * Wake the background detector.  The detector clears `pending` BEFORE it
 * takes the partition locks for a sweep, so an edge added after that clear
 * always sets the latch again and can never be missed; an edge added before
 * it is seen by the sweep itself.
 */
static void
RccBgWake(void)
{
    if (!rcc_bg_detect || RccBg->pending)
        return;
    RccBg->pending = true;
    if (RccBg->latch != NULL)
        SetLatch(RccBg->latch);
}

/*
 * Did the background detector pick me as a cycle victim?  The mark is my own
 * xid, so a stale mark left by a previous occupant of this PGPROC slot (or an
 * earlier transaction of mine) is ignored.  Called at every safe point via
 * CheckAbortedDependencies, and around the commit waits.
 */
static void
RccCheckDoomed(TransactionId myxid)
{
    if (MyProc->rccDoomedXid == myxid)
        ereport(ERROR,
                (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                 errmsg("could not serialize access due to read/write dependencies among transactions (bgdd), TX_ID: %u", myxid)));
}


#ifdef RCC_EDGECHECK
/*
 * Is `other` (still in progress) already recorded as depending on me?
 * Single SHARED-lock lookup of reverse[myxid]; taken AFTER the edge-add lock
 * is released, so no nested partition locks (a missed race just means the
 * cycle is caught at commit arrival instead, as today).
 */
static bool
rcc_dependent_on_me(TransactionId myxid, TransactionId other)
{
    DepGraphPartition *partition = &TxnReverseDepPartitions[DEP_PARTITION(myxid)];
    TxnReverseDepEntry *rev;
    bool        found = false;

    LWLockAcquire(partition->lock, LW_SHARED);
    rev = hash_search(partition->graph, &myxid, HASH_FIND, NULL);
    if (rev)
        for (int i = 0; i < rev->nfroms; i++)
            if (rev->from_list[i] == other)
            {
                found = TransactionIdIsInProgress(other);
                break;
            }
    LWLockRelease(partition->lock);
    return found;
}
#endif

void
RecordReadDependency(TransactionId xmin, TransactionId myxid)
{
    if (!TransactionIdIsValid(xmin) || xmin == myxid)
        return;

    /*
     * Dedup against the WR list ONLY.  WR (wait + cascade-abort) and RW (no-wait
     * cycle) are different dependency kinds with different commit semantics; the
     * same xid can legitimately be BOTH.  Cross-checking MyRwDeps here would drop
     * the WR edge when an RW edge to the same xid already exists -- losing the
     * cascade-abort and letting this txn commit on data that later aborts.
     */
    for (int i = 0; i < MyReadDependenciesCount; i++)
        if (MyReadDependencies[i] == xmin)
            return;
    if (MyReadDependenciesCount >= MAX_DEP)
    {
        /*
         * Dropping a WR dep is NOT safe: it skips the commit wait AND the
         * cascade abort for this producer, so we could commit data built on
         * a transaction that later aborts (version-chain fork).  Warn loudly;
         * if this ever fires, MAX_DEP must be raised or the txn aborted.
         */
        elog(WARNING, "[DepGraph] local WR dep list full (%d): dropping dep on TX %u, TX_ID: %u",
             MyReadDependenciesCount, xmin, myxid);
        return;                         /* bound: skip extra edges */
    }
    MyReadDependencies[MyReadDependenciesCount++] = xmin;

    /*
     * Add the reverse edge only if the RW path hasn't already added one for this
     * target.  The same xid can be in both WR and RW lists, but the reverse
     * graph only needs the (target, me) edge once -- a second append would
     * duplicate `me` in from_list.
     */
    {
        bool add_reverse = true;
#ifdef RCC_SR
        for (int i = 0; i < MyRwDepsCount; i++)
            if (MyRwDeps[i] == xmin) { add_reverse = false; break; }
#endif
        if (add_reverse)
            RccAddReverseEdge(xmin, myxid, false);
    }
#ifdef RCC_EDGECHECK
    /*
     * 2-cycle check at CONSUMPTION time: I just recorded "I depend on xmin"
     * (WR).  If xmin already depends on ME, the cycle is complete and I am
     * the consumer side = the correct victim.  Dying here -- before running
     * any further statements and before anyone consumes MY versions -- keeps
     * this cycle's cascade cone at (near) zero, instead of letting it grow
     * until my commit-arrival check.  Longer cycles and the opposite edge
     * order are still caught at commit arrival as before.
     */
    if (rcc_dependent_on_me(myxid, xmin))
        ereport(ERROR,
                (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                 errmsg("could not serialize access due to read/write dependencies among transactions (edge), TX_ID: %u", myxid)));
#endif
#ifdef RCC_DEBUG
    elog(LOG, "[DepGraph] TX %u depends on TX %u", myxid, xmin);
#endif
}

#ifdef RCC_SR
/*
 * Record an RW anti-dependency "writer depends on reader": the writer overwrote
 * a row that `reader` had read (discovered via PostgreSQL's SSI SIRead list in
 * predicate.c).  Added to the local MyRwDeps list (waited on + cycle-checked at
 * commit) and to the shared reverse graph reverse[reader] += writer (so cycle
 * traversal starting from the writer can reach the reader).  Mirrors
 * RecordReadDependency but targets MyRwDeps instead of MyReadDependencies.
 */
void
RccRecordRwDep(TransactionId reader, TransactionId writer)
{
    if (!TransactionIdIsValid(reader) || !TransactionIdIsValid(writer) ||
        reader == writer)
        return;

    /*
     * Dedup against the RW list ONLY (see RecordReadDependency): WR and RW are
     * independent dependency kinds; the same xid may appear in both lists.
     */
    for (int i = 0; i < MyRwDepsCount; i++)
        if (MyRwDeps[i] == reader)
            return;
    if (MyRwDepsCount >= MAX_DEP)
    {
        /* Dropping an RW dep skips the writer's commit wait on this reader
         * (no cascade involved, but a cycle can go undetected).  Warn. */
        elog(WARNING, "[DepGraph] local RW dep list full (%d): dropping dep on TX %u, TX_ID: %u",
             MyRwDepsCount, reader, writer);
        return;                         /* bound: skip extra edges */
    }
    MyRwDeps[MyRwDepsCount++] = reader;

    /* Skip the reverse edge if the WR path already added one for this target. */
    {
        bool add_reverse = true;
        for (int i = 0; i < MyReadDependenciesCount; i++)
            if (MyReadDependencies[i] == reader) { add_reverse = false; break; }
        if (add_reverse)
            RccAddReverseEdge(reader, writer, true);
    }
}
#endif							/* RCC_SR */


/* Is `xid` one of MY OWN dependencies (WR or RW)? */
bool
RccIsMyDependency(TransactionId xid)
{
    for (int i = 0; i < MyReadDependenciesCount; i++)
        if (MyReadDependencies[i] == xid)
            return true;
#ifdef RCC_SR
    for (int i = 0; i < MyRwDepsCount; i++)
        if (MyRwDeps[i] == xid)
            return true;
#endif
    return false;
}

/* Is "from depends on to" still recorded right now? */
static bool
RccDependencyEdgeExists(TransactionId from, TransactionId to)
{
	DepGraphPartition *partition = &TxnReverseDepPartitions[DEP_PARTITION(to)];
	TxnReverseDepEntry *rev;
	bool        found = false;

	LWLockAcquire(partition->lock, LW_SHARED);
	rev = hash_search(partition->graph, &to, HASH_FIND, NULL);
	if (rev)
		for (int i = 0; i < rev->nfroms; i++)
			if (rev->from_list[i] == from)
			{
				found = true;
				break;
			}
	LWLockRelease(partition->lock);
	return found;
}

/* Recheck a candidate path copied while other transactions could finish. */
static bool
RccCyclePathIsLive(TransactionId from, TransactionId to, TransactionId myxid,
				   const TransactionId *visitedXids,
				   const TransactionId *visitedParent, int visitedCount)
{
	while (from != myxid)
	{
		if (!TransactionIdIsInProgress(from) ||
			!RccDependencyEdgeExists(from, to))
			return false;

		from = to;
		if (from == myxid)
			break;

		to = InvalidTransactionId;
		for (int i = 0; i < visitedCount; i++)
			if (visitedXids[i] == from)
			{
				to = visitedParent[i];
				break;
			}
		if (!TransactionIdIsValid(to))
			return false;
	}
	return true;
}

/*
 * Single commit-time cycle check.
 *
 * ONE reverse-graph DFS from myxid collects every transaction that
 * (transitively) depends on me; I am on a cycle iff one of MY OWN
 * dependencies (WR or RW, myxid -> D) is among them (D -> ... -> myxid).
 * This replaces the old per-dependency IsCircularDependency(D, myxid)
 * calls, which redid this same DFS once per dependency.
 *
 * Only in-progress transactions are traversed: every dependency edge (WR and RW)
 * makes the dependent WAIT for its target, so a cycle is a wait-deadlock among
 * live transactions.  A committed transaction already finished waiting on its
 * own deps and so cannot be part of a live cycle.
 *
 * Copy each node's adjacency list under only its partition lock, then release
 * it before traversing or consulting transaction status.  Never hold two graph
 * partition locks at once.  This is not an atomic snapshot: recheck candidate
 * paths to discard edges already removed by cleanup.  Cycles formed after a
 * node was copied are handled by a later committer or the native PG deadlock
 * detector, just like cycles formed after the old all-lock scan completed.
 */
static bool
RccCommitCycleCheck(TransactionId myxid)
{
    TransactionId depxid = InvalidTransactionId;    /* dep that closed the cycle */

    /*
     * DFS: Start from myxid, follow reverse edges (who depends on X).
     * If we reach one of my own dependencies, that dep depends on me -> cycle.
     *
     * Nodes are marked visited when PUSHED (not when popped), so every
     * in-progress xid enters the stack at most once and the stack can never
     * outgrow the visited capacity.  (The old pop-time dedup let a node be
     * pushed once per parent, overflowing the 256-entry stack under dense
     * graphs and silently truncating the search -- missed cycles.)
     *
	 * Only in-progress nodes are added, but transaction churn during the scan
	 * can grow the visited set beyond one snapshot's active backend count.
	 * If the capacity is exceeded, conservatively report a cycle: a spurious
	 * abort is safe, a missed cycle would need the native deadlock backstop.
     */
#define RCC_CYCLE_MAX_NODES 4096
    static TransactionId stack[RCC_CYCLE_MAX_NODES];
    static TransactionId visitedXids[RCC_CYCLE_MAX_NODES];
    static TransactionId visitedParent[RCC_CYCLE_MAX_NODES];
    int stackSize = 0;
    int visitedCount = 0;
    bool found = false;
    bool overflow = false;

	if (!TransactionIdIsValid(myxid))
		return false;

	if (MyReadDependenciesCount == 0
#ifdef RCC_SR
		&& MyRwDepsCount == 0
#endif
		)
		return false;

    stack[stackSize++] = myxid;
    visitedXids[visitedCount] = myxid;
    visitedParent[visitedCount] = InvalidTransactionId;
    visitedCount++;

    while (stackSize > 0 && !found)
    {
        TransactionId current = stack[--stackSize];
		TransactionId dependents[MAX_DEP];
		DepGraphPartition *partition =
			&TxnReverseDepPartitions[DEP_PARTITION(current)];
		TxnReverseDepEntry *rev;
		int ndependents = 0;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(partition->lock, LW_SHARED);
		rev = hash_search(partition->graph, &current, HASH_FIND, NULL);
		if (rev)
        {
			ndependents = rev->nfroms;
			Assert(ndependents >= 0 && ndependents <= MAX_DEP);
			memcpy(dependents, rev->from_list,
				   ndependents * sizeof(TransactionId));
		}
		LWLockRelease(partition->lock);

		for (int i = 0; i < ndependents; i++)
		{
			TransactionId from = dependents[i];
            bool alreadyVisited = false;

            /*
             * Only in-progress transactions can be part of a live wait
             * cycle (committed/aborted ones are not waiting and cannot
             * deadlock); skip everything else.
             */
            if (!TransactionIdIsInProgress(from))
                continue;

            /* One of MY dependencies depends back on me -> cycle closed. */
            if (RccIsMyDependency(from))
            {
				if (!RccCyclePathIsLive(from, current, myxid,
									   visitedXids, visitedParent, visitedCount))
					continue;

                if (visitedCount < RCC_CYCLE_MAX_NODES)
                {
                    visitedXids[visitedCount] = from;
                    visitedParent[visitedCount] = current;
                    visitedCount++;
                }
                depxid = from;
                found = true;
                break;
            }

            for (int j = 0; j < visitedCount; j++)
            {
                if (visitedXids[j] == from)
                {
                    alreadyVisited = true;
                    break;
                }
            }
            if (alreadyVisited)
                continue;

            if (visitedCount >= RCC_CYCLE_MAX_NODES)
            {
                overflow = true;
                continue;
            }

            visitedXids[visitedCount] = from;
            visitedParent[visitedCount] = current;
            visitedCount++;
            /* one push per visited entry -> stackSize < RCC_CYCLE_MAX_NODES */
            stack[stackSize++] = from;
        }
    }

    if (overflow && !found)
    {
        elog(WARNING, "[DepGraph] node overflow in RccCommitCycleCheck (cap=%d): assuming cycle, myxid=%u",
             RCC_CYCLE_MAX_NODES, myxid);
        found = true;
    }

#ifdef RCC_DEBUG
    /* RCC DEBUG (temporary): log the cycle length AND the actual member
     * path (xids), so the length can be recomputed and members inspected
     * from the log.  Arrows are forward dependencies: "A -> B" = A depends
     * on (waits for) B; the cycle is myxid -> depxid -> ... -> myxid.
     * Overflow-assumed cycles have no real path and are not logged. */
    if (found && !overflow)
    {
        StringInfoData buf;
        TransactionId path[64];
        int         pathLen = 0;
        TransactionId cur = depxid;

        while (TransactionIdIsValid(cur) && cur != myxid && pathLen < 64)
        {
            TransactionId next = InvalidTransactionId;

            path[pathLen++] = cur;
            for (int i = 0; i < visitedCount; i++)
                if (visitedXids[i] == cur)
                {
                    next = visitedParent[i];
                    break;
                }
            cur = next;
        }

        initStringInfo(&buf);
        appendStringInfo(&buf, "[RCC_cycle_len] len=%d cycle: %u",
                         pathLen + 1, myxid);
        for (int i = 0; i < pathLen; i++)
            appendStringInfo(&buf, " -> %u", path[i]);
        appendStringInfo(&buf, " -> %u", myxid);
        elog(LOG, "%s", buf.data);
        pfree(buf.data);
    }
#else
	/* only the debug path logging reads the closing dependency */
    (void) depxid;
#endif

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
    return found;
}

RccDepResult
WaitForTxnDependencies(void)
{
    TransactionId myxid = GetTopTransactionIdIfAny();
    if (!TransactionIdIsValid(myxid))
        return RCC_DEP_OK;

#ifdef RCC_DEBUG
    if (MyReadDependenciesCount == 0)
    {
        fprintf(stderr, "[DepGraph] No dependencies to wait for TX_ID: %u\n", myxid);
        return RCC_DEP_OK;
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

    /*
     * Commit-arrival cycle detection: ONE reverse-graph DFS from myxid,
     * before paying any wait.  This is the only inline cycle check -- the
     * dependency waits below run bare.  A cycle that forms after this scan
     * parks its members in XactLockTableWait (heavyweight xid locks), which
     * PostgreSQL's own deadlock detector resolves after deadlock_timeout
     * (set low, e.g. 1ms, in postgresql.conf).  In principle the LAST
     * cycle member to reach its own commit sees all recorded edges here
     * and self-aborts, so the PG detector is a backstop.
     *
     * With rcc_bg_detect on, no backend runs this DFS: the background
     * detector sweeps the graph as edges arrive and dooms one member per
     * cycle (RccDetectorSweep).  A doomed victim that reaches commit only
     * needs to notice its mark before parking in the wait below.
     */
    if (rcc_bg_detect)
        RccCheckDoomed(myxid);
    else if (RccCommitCycleCheck(myxid))
        return RCC_DEP_CYCLE;

    for (int i = 0; i < MyReadDependenciesCount; i++)
    {
        TransactionId depxid = MyReadDependencies[i];

        /*
         * Order matters: test in-progress FIRST and the abort status AFTER.
         * Checking DidAbort before IsInProgress leaves a window -- a dependency
         * that finishes aborting between the two tests is seen as "not aborted"
         * and then "not in progress", i.e. taken for committed, and this
         * transaction commits on top of a dead version (version-chain fork).
         */
        if (TransactionIdIsInProgress(depxid))
        {

            /* Breakdown Code - measure txwait time for dependency wait */
            instr_time txwait_start, txwait_end;
            INSTR_TIME_SET_CURRENT(txwait_start);

            XactLockTableWait(depxid, NULL, NULL, XLTW_None);

            INSTR_TIME_SET_CURRENT(txwait_end);
            INSTR_TIME_SUBTRACT(txwait_end, txwait_start);
            xact_count_txwait_time(INSTR_TIME_GET_MICROSEC(txwait_end));
            /* Breakdown Code End */

            RccCheckDoomed(myxid);
        }

        if (TransactionIdDidAbort(depxid))
            return RCC_DEP_ABORTED;
    }

#ifdef RCC_SR
    /*
     * RW anti-dependency resolution (write skew), MySQL-style: the writer waits
     * for each in-progress reader of a row it overwrote, aborting only on a true
     * serialization cycle (a wait-deadlock among live transactions).  A reader
     * that already committed is serialized before us; one that aborted leaves no
     * anti-dependency -- both are skipped.  Unlike WR, an aborted reader is not a
     * cascade trigger, so there is no DidAbort->abort after the wait.
     */
    for (int i = 0; i < MyRwDepsCount; i++)
    {
        TransactionId depxid = MyRwDeps[i];

        if (!TransactionIdIsInProgress(depxid))
            continue;


        /* Breakdown Code - measure txwait time for RW dependency wait */
        instr_time txwait_start, txwait_end;
        INSTR_TIME_SET_CURRENT(txwait_start);

        XactLockTableWait(depxid, NULL, NULL, XLTW_None);

        INSTR_TIME_SET_CURRENT(txwait_end);
        INSTR_TIME_SUBTRACT(txwait_end, txwait_start);
        xact_count_txwait_time(INSTR_TIME_GET_MICROSEC(txwait_end));
        /* Breakdown Code End */
    }
#endif							/* RCC_SR */
    return RCC_DEP_OK;
}

/*
 * "Superseded by me" set (RR visibility, see HeapTupleSatisfiesMVCC): committed
 * base tuples whose update chain now ends in a version written by this
 * transaction.  Filled by heap_update through the chain walk that
 * heapam_tuple_lock publishes with RccSetChainWalk.  Backend-local, per
 * transaction; cleared in CleanupTxnDependency.
 */
#define RCC_MAX_SUPERSEDED 1024
typedef struct { Oid relid; ItemPointerData tid; } RccSupersededEntry;
static RccSupersededEntry rccSuperseded[RCC_MAX_SUPERSEDED];
static int rccSupersededCount = 0;

static Oid rccWalkRel = InvalidOid;
static ItemPointerData rccWalkRoot;
static ItemPointerData rccWalkLast;
static TransactionId rccWalkLastXmin = InvalidTransactionId;

void
RccSetChainWalk(Oid relid, ItemPointer root, ItemPointer last, TransactionId last_xmin)
{
    rccWalkRel = relid;
    rccWalkRoot = *root;
    rccWalkLast = *last;
    rccWalkLastXmin = last_xmin;
}

/*
 * Hand-over-hand tuple lock (arrival-order fairness across versions).
 *
 * The heavyweight tuple lock is what gives a waiter its place in line
 * (README.tuplock).  When the locked version turns out to be superseded,
 * heap_lock_tuple does not drop that lock on its way out: it is carried here
 * while heapam_tuple_lock chases the chain, and released only once the tuple
 * lock (or the row lock itself) on the newer version has been obtained.
 * Dropping it first would wake the next waiter early and let it overtake us on
 * the way to the new version.
 */
static bool rccCarried = false;
static LOCKTAG rccCarriedTag;
static LOCKMODE rccCarriedMode;

void
RccCarryTupleLock(Oid dbid, Oid relid, ItemPointer tid, LOCKMODE hwlock)
{
    Assert(!rccCarried);
    SET_LOCKTAG_TUPLE(rccCarriedTag, dbid, relid,
                      ItemPointerGetBlockNumber(tid),
                      ItemPointerGetOffsetNumber(tid));
    rccCarriedMode = hwlock;
    rccCarried = true;
}

void
RccReleaseCarriedTupleLock(void)
{
    if (!rccCarried)
        return;
    rccCarried = false;
    LockRelease(&rccCarriedTag, rccCarriedMode, false);
}

/*
 * Did this transaction already walk relid's chain starting at root?  If so
 * return where it ended (the version it locked) and that version's creator.
 */
bool
RccGetChainWalk(Oid relid, ItemPointer root, ItemPointer last, TransactionId *last_xmin)
{
    if (rccWalkRel != relid || !ItemPointerEquals(&rccWalkRoot, root))
        return false;
    *last = rccWalkLast;
    *last_xmin = rccWalkLastXmin;
    return true;
}

void
RccNoteSupersededIfWalked(Oid relid, ItemPointer updated)
{
    if (rccWalkRel != relid || !ItemPointerEquals(&rccWalkLast, updated))
        return;
    rccWalkRel = InvalidOid;    /* consumed */
    for (int i = 0; i < rccSupersededCount; i++)
        if (rccSuperseded[i].relid == relid &&
            ItemPointerEquals(&rccSuperseded[i].tid, &rccWalkRoot))
            return;
    if (rccSupersededCount >= RCC_MAX_SUPERSEDED)
    {
        elog(WARNING, "[DepGraph] superseded list full (%d): a stale base may stay visible, TX_ID: %u",
             rccSupersededCount, GetTopTransactionIdIfAny());
        return;
    }
    rccSuperseded[rccSupersededCount].relid = relid;
    rccSuperseded[rccSupersededCount].tid = rccWalkRoot;
    rccSupersededCount++;
}

/*
 * Versions this transaction created by UPDATE, with the command id that
 * created them.  t_cid holds cmin OR cmax; when another in-progress
 * transaction chains onto our uncommitted version, heap_update overwrites it
 * with that transaction's cmax and our cmin is lost.  HeapTupleSatisfiesMVCC
 * uses this list to recover it (needed for same-command "Halloween"
 * protection as well as for later commands).  Per transaction, backend-local.
 */
#define RCC_MAX_MYVERSIONS 1024
typedef struct { Oid relid; ItemPointerData tid; CommandId cmin; } RccMyVersionEntry;
static RccMyVersionEntry rccMyVersions[RCC_MAX_MYVERSIONS];
static int rccMyVersionCount = 0;

void
RccNoteMyVersion(Oid relid, ItemPointer tid, CommandId cmin)
{
    if (rccMyVersionCount >= RCC_MAX_MYVERSIONS)
    {
        /* lookup miss falls back to "earlier command"; say so once per overflow */
        elog(WARNING, "[DepGraph] my-version list full (%d): cmin recovery disabled for further versions, TX_ID: %u",
             rccMyVersionCount, GetTopTransactionIdIfAny());
        return;
    }
    rccMyVersions[rccMyVersionCount].relid = relid;
    rccMyVersions[rccMyVersionCount].tid = *tid;
    rccMyVersions[rccMyVersionCount].cmin = cmin;
    rccMyVersionCount++;
}

bool
RccLookupMyVersionCmin(Oid relid, ItemPointer tid, CommandId *cmin)
{
    for (int i = rccMyVersionCount - 1; i >= 0; i--)
        if (rccMyVersions[i].relid == relid &&
            ItemPointerEquals(&rccMyVersions[i].tid, tid))
        {
            *cmin = rccMyVersions[i].cmin;
            return true;
        }
    return false;
}

bool
RccIsSuperseded(Oid relid, ItemPointer tid)
{
    for (int i = 0; i < rccSupersededCount; i++)
        if (rccSuperseded[i].relid == relid &&
            ItemPointerEquals(&rccSuperseded[i].tid, tid))
            return true;
    return false;
}

void
CleanupTxnDependency()
{
    TransactionId myxid = GetTopTransactionIdIfAny();

    rccSupersededCount = 0;
    rccWalkRel = InvalidOid;
    rccCarried = false;         /* the lock itself went with LockReleaseAll */
    rccMyVersionCount = 0;
    /* Whatever the detector marked applied to this transaction only. */
    MyProc->rccDoomedXid = InvalidTransactionId;

    /*
     * Always remove reverse[myxid]: other transactions may have recorded edges
     * pointing to us (reverse[myxid] is created by whoever depends on us) even
     * when we recorded no dependencies of our own.  Returning early here on
     * MyReadDependenciesCount==0 leaked those entries -> "out of shared memory".
     */
    if (!TransactionIdIsValid(myxid))
        return;

    /* Remove from reverse dependency graph (partitioned) */
    int part = DEP_PARTITION(myxid);
    DepGraphPartition *partition = &TxnReverseDepPartitions[part];

    LWLockAcquire(partition->lock, LW_EXCLUSIVE);
    hash_search(partition->graph, &myxid, HASH_REMOVE, NULL);
    LWLockRelease(partition->lock);

    MyReadDependenciesCount = 0;
#ifdef RCC_SR
    MyRwDepsCount = 0;
#endif
}

bool 
CheckAbortedDependencies(void)
{
    //여기서도, 다른 트랜잭션이 나를 abort시킬 수도 있으니, 내가 abort되었는지 체크
    TransactionId myxid = GetTopTransactionIdIfAny();
    if (!TransactionIdIsValid(myxid))
        return false;  /* No active transaction → no dependencies */
    RccCheckDoomed(myxid);
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

/* ----------------------------------------------------------------------
 * Background deadlock detector.
 *
 * One bgworker, woken by RccBgWake() whenever an edge is added (plus a
 * RCC_BG_BACKSTOP_MS timeout).  A sweep collects the xids of the backends
 * currently in a transaction and looks up reverse[xid] for each, one
 * partition lock at a time, copying the edges among them into local arrays;
 * the DFS then runs on the local copy with no locks held.  (An earlier
 * version held all 64 partition locks while scanning the whole hash: the
 * worker then sat on the locks ~100% of the time and every transaction's
 * edge insertion queued behind it -- t32 dropped from 6.8k to 0.5k TPS.)
 * Because the copy is not atomic, each candidate cycle is re-verified edge
 * by edge before acting.  The youngest member is doomed: its PGPROC gets
 * rccDoomedXid = xid and a PROCSIG_RCC_CASCADE_ABORT wake-up; it aborts at
 * its next safe point (RccCheckDoomed).  Because the mark is persistent
 * shared state (not a one-shot signal flag), a wake-up that finds the victim
 * idle is simply retried by its next safe point -- no lost wake-ups.
 *
 * Already-doomed transactions are left out of the snapshot (they are dying),
 * which is what keeps one cycle from producing a victim per sweep.
 * ---------------------------------------------------------------------- */

#define RCC_BG_BACKSTOP_MS	10
#define RCC_BG_MAX_NODES	4096
#define RCC_BG_MAX_EDGES	(RCC_BG_MAX_NODES * 8)
#define RCC_BG_MAX_DOOMED	256

/* victims marked but not yet gone (worker-private; pruned every sweep) */
static TransactionId doomed[RCC_BG_MAX_DOOMED];
static int	ndoomed = 0;

static bool
rcc_bg_is_doomed(TransactionId xid)
{
    for (int i = 0; i < ndoomed; i++)
        if (doomed[i] == xid)
            return true;
    return false;
}

/* Mark `victim` and wake it.  It compares the mark against its own xid. */
static void
rcc_bg_doom(TransactionId victim, int len)
{
    int         pid = BackendXidGetPid(victim);
    PGPROC     *proc = (pid > 0) ? BackendPidGetProc(pid) : NULL;

    if (proc == NULL)
        return;                 /* finished (or is finishing) on its own */
    if (ndoomed < RCC_BG_MAX_DOOMED)
        doomed[ndoomed++] = victim;
    proc->rccDoomedXid = victim;
    SendProcSignal(pid, PROCSIG_RCC_CASCADE_ABORT, InvalidBackendId);
    elog(LOG, "[RCC_bgdd] cycle len=%d victim=%u", len, victim);
}

/* index of xid in nodes[], or -1 (never adds) */
static int
rcc_bg_find_idx(TransactionId xid, const TransactionId *nodes, int nnodes)
{
    for (int i = 0; i < nnodes; i++)
        if (nodes[i] == xid)
            return i;
    return -1;
}

static void
RccDetectorSweep(void)
{
    static TransactionId nodes[RCC_BG_MAX_NODES];
    static int  ehead[RCC_BG_MAX_NODES];    /* adjacency list head per node */
    static int  eto[RCC_BG_MAX_EDGES];      /* edge target (node idx) */
    static int  enext[RCC_BG_MAX_EDGES];    /* next edge in adjacency list */
    static uint8 color[RCC_BG_MAX_NODES];   /* 0 white, 1 gray, 2 black */
    static int  parent[RCC_BG_MAX_NODES];
    static int  iter[RCC_BG_MAX_NODES];     /* DFS: next edge to scan */
    static int  stack[RCC_BG_MAX_NODES];
    int         nnodes = 0, nedges = 0;

    /* forget victims that have finished dying */
    {
        int         k = 0;

        for (int s = 0; s < ndoomed; s++)
            if (TransactionIdIsInProgress(doomed[s]))
                doomed[k++] = doomed[s];
        ndoomed = k;
    }

    /*
     * Nodes: the top-level xid of every backend currently in a transaction
     * (unlocked read of PGPROC.xid; a stale value only costs a useless
     * lookup).  This bounds a sweep by the number of active backends instead
     * of by the size of the shared hash tables.
     */
    for (uint32 i = 0; i < ProcGlobal->allProcCount && nnodes < RCC_BG_MAX_NODES; i++)
    {
        TransactionId xid = ProcGlobal->allProcs[i].xid;

        if (TransactionIdIsValid(xid) && !rcc_bg_is_doomed(xid))
            nodes[nnodes++] = xid;
    }
    if (nnodes < 2)
        return;

    /*
     * Edges: for each node, ONE lookup of reverse[node] under that
     * partition's shared lock, held only for the copy.  No hash-wide scan and
     * never more than one partition lock at a time, so writers
     * (RccAddReverseEdge, CleanupTxnDependency) are barely disturbed.  The
     * copy is therefore not atomic across partitions; a candidate cycle is
     * re-verified edge by edge before anyone is doomed.
     */
    for (int n = 0; n < nnodes; n++)
        ehead[n] = -1;
    for (int to = 0; to < nnodes; to++)
    {
        DepGraphPartition *partition = &TxnReverseDepPartitions[DEP_PARTITION(nodes[to])];
        TxnReverseDepEntry *rev;

        LWLockAcquire(partition->lock, LW_SHARED);
        rev = hash_search(partition->graph, &nodes[to], HASH_FIND, NULL);
        if (rev)
            for (int i = 0; i < rev->nfroms && nedges < RCC_BG_MAX_EDGES; i++)
            {
                int         fr = rcc_bg_find_idx(rev->from_list[i], nodes, nnodes);

                if (fr < 0 || fr == to)
                    continue;
                /* from depends on (waits for) to: edge fr -> to */
                eto[nedges] = to;
                enext[nedges] = ehead[fr];
                ehead[fr] = nedges;
                nedges++;
            }
        LWLockRelease(partition->lock);
    }
    if (nedges == 0)
        return;

    /* 3-color DFS over the local copy; one victim per verified cycle */
    memset(color, 0, nnodes);
    for (int root = 0; root < nnodes; root++)
    {
        int         sp = 0;

        if (color[root] != 0)
            continue;
        color[root] = 1;
        parent[root] = -1;
        iter[root] = ehead[root];
        stack[sp++] = root;
        while (sp > 0)
        {
            int         u = stack[sp - 1];
            int         e = iter[u];
            int         v;

            if (e < 0)
            {
                color[u] = 2;
                sp--;
                continue;
            }
            iter[u] = enext[e];
            v = eto[e];
            if (color[v] == 0)
            {
                color[v] = 1;
                parent[v] = u;
                iter[v] = ehead[v];
                stack[sp++] = v;
            }
            else if (color[v] == 1)
            {
                /*
                 * Back edge u -> v: the cycle is v -> ... -> u -> v along
                 * parent[].  Re-verify every edge and member against the live
                 * graph (the copy was not atomic), then doom the youngest.
                 */
                TransactionId victim = nodes[u];
                int         len = 1;
				bool        real = RccDependencyEdgeExists(nodes[u], nodes[v]) &&
                                   TransactionIdIsInProgress(nodes[u]);

                for (int w = u; real && w != v; w = parent[w])
                {
                    int         pw = parent[w];

                    if (pw < 0)
                    {
                        real = false;
                        break;
                    }
                    len++;
                    if (TransactionIdFollows(nodes[pw], victim))
                        victim = nodes[pw];
					real = RccDependencyEdgeExists(nodes[pw], nodes[w]) &&
                           TransactionIdIsInProgress(nodes[pw]);
                }
                if (real && !rcc_bg_is_doomed(victim))
                    rcc_bg_doom(victim, len);
            }
        }
    }
}

static void
RccBgDetectorExit(int code, Datum arg)
{
    RccBg->latch = NULL;
}

void
RccDeadlockDetectorMain(Datum main_arg)
{
    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    BackgroundWorkerUnblockSignals();

    before_shmem_exit(RccBgDetectorExit, (Datum) 0);
    RccBg->latch = MyLatch;

    elog(LOG, "[RCC_bgdd] background deadlock detector started (rcc_bg_detect=%s, backstop %d ms)",
         rcc_bg_detect ? "on" : "off", RCC_BG_BACKSTOP_MS);

    for (;;)
    {
        (void) WaitLatch(MyLatch,
                         WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                         RCC_BG_BACKSTOP_MS,
                         PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
        if (ShutdownRequestPending)
            proc_exit(0);
        if (ConfigReloadPending)
        {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }
        CHECK_FOR_INTERRUPTS();
        if (!rcc_bg_detect)
            continue;

        /* clear BEFORE reading the graph: see RccBgWake */
        RccBg->pending = false;
        pg_memory_barrier();
        RccDetectorSweep();
    }
}

void
RccDeadlockDetectorRegister(void)
{
    BackgroundWorker bgw;

    memset(&bgw, 0, sizeof(bgw));
    bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
    bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
    snprintf(bgw.bgw_library_name, sizeof(bgw.bgw_library_name), "postgres");
    snprintf(bgw.bgw_function_name, sizeof(bgw.bgw_function_name), "RccDeadlockDetectorMain");
    snprintf(bgw.bgw_name, sizeof(bgw.bgw_name), "rcc deadlock detector");
    snprintf(bgw.bgw_type, sizeof(bgw.bgw_type), "rcc deadlock detector");
    bgw.bgw_restart_time = 1;
    bgw.bgw_notify_pid = 0;
    bgw.bgw_main_arg = (Datum) 0;

    RegisterBackgroundWorker(&bgw);
}

#endif /* RCC */
