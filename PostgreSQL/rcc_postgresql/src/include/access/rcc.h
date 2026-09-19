#ifndef RCC_H
#define RCC_H

#include "access/xact.h"
#include "storage/lwlock.h"
#include "storage/itemptr.h"

/* Shared Memory Size & Initialization */
extern void InitTxnDependency(void);
extern Size RccShmemSize(void);
extern void RccShmemInit(void);

/*
 * Set true while executing a statement that has rowMarks (SELECT ... FOR
 * UPDATE/SHARE).  Such a read takes a row lock (write intent), so we treat it
 * as a writer and skip placing an SIRead on it -- the row lock already
 * serializes it, so the SIRead would only create a bogus RW anti-dependency.
 */
extern bool RccStmtForUpdate;

/*
 * Set true while executing an UPDATE statement.  Its target-locate index
 * descent defers leaf SIReads exactly like an SFU (same wait-window cycle
 * otherwise); the anchor is heapam_tuple_update success.
 */
extern bool RccStmtUpdateScan;

/*
 * Set by HeapTupleSatisfiesDirty when a read-latest scan defers a COMMITTED
 * base version to an in-progress newer version (the only case that can lose a
 * committed row if that writer aborts).  ExecutePlan uses it to tell a genuine
 * read-latest vanish (retry) from a legitimately empty result like item
 * i_id=-1 (no deferral -> don't retry, avoids an infinite loop).
 */
extern bool RccReadLatestDeferredCommitted;

/* Dependency Graph Operations */
extern void RecordReadDependency(TransactionId xmin, TransactionId myxid);


extern void CleanupTxnDependency(void);
extern bool CheckAbortedDependencies(void);

/*
 * Background deadlock detector (GUC rcc_bg_detect, default off).  A bgworker
 * sweeps the shared reverse-dependency graph whenever an edge is added and
 * dooms the youngest member of each cycle; the victim aborts at its next safe
 * point.  With the GUC off, cycles are instead detected by each transaction at
 * commit arrival (RccCommitCycleCheck).
 */
extern bool rcc_bg_detect;
extern void RccDeadlockDetectorRegister(void);
extern void RccDeadlockDetectorMain(Datum main_arg);
extern bool RccIsMyDependency(TransactionId xid);
extern void RccSetChainWalk(Oid relid, ItemPointer root, ItemPointer last, TransactionId last_xmin);
extern void RccCarryTupleLock(Oid dbid, Oid relid, ItemPointer tid, int hwlock);
extern void RccReleaseCarriedTupleLock(void);
extern bool RccGetChainWalk(Oid relid, ItemPointer root, ItemPointer last, TransactionId *last_xmin);
extern void RccNoteSupersededIfWalked(Oid relid, ItemPointer updated);
extern bool RccIsSuperseded(Oid relid, ItemPointer tid);
extern void RccNoteMyVersion(Oid relid, ItemPointer tid, CommandId cmin);
extern bool RccLookupMyVersionCmin(Oid relid, ItemPointer tid, CommandId *cmin);

/* Result of WaitForTxnDependencies: why (if at all) the committer must abort. */
typedef enum RccDepResult
{
	RCC_DEP_OK = 0,			/* no conflict -> may commit */
	RCC_DEP_ABORTED,		/* a WR dependency aborted -> cascade abort */
	RCC_DEP_CYCLE			/* a serialization cycle (write skew / deadlock) */
} RccDepResult;

extern RccDepResult WaitForTxnDependencies(void);

#ifdef RCC_SR
/*
 * RW anti-dependency (write skew): a writer records, against each reader that
 * read a row it overwrote, the edge "writer depends on reader".  Readers are
 * discovered from PostgreSQL's SSI SIRead list (predicate.c); at commit the
 * writer waits for each reader and aborts only on a true serialization cycle.
 */
extern void RccRecordRwDep(TransactionId reader, TransactionId writer);

/*
 * Does the version chain starting at `from` reach `cand` (created by
 * `cand_xmin`)?  Read-latest dup guard for scans; see heapam.c.
 */
struct RelationData;
extern bool RccVersionChainReaches(struct RelationData *relation,
								   ItemPointer from, ItemPointer cand,
								   TransactionId cand_xmin);
#endif							/* RCC_SR */

#endif /* RCC_H */
