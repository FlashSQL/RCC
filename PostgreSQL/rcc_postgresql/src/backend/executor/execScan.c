/*-------------------------------------------------------------------------
 *
 * execScan.c
 *	  This code provides support for generalized relation scans. ExecScan
 *	  is passed a node and a pointer to a function to "do the right thing"
 *	  and return a tuple from the relation. ExecScan then does the tedious
 *	  stuff - checking the qualification and projecting the tuple
 *	  appropriately.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execScan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#ifdef RCC_SR
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/rcc.h"
#include "access/xact.h"
#include "storage/predicate.h"
#include "storage/procarray.h"

/*
 * RCC_SR: a SELECT just read the latest (speculative) version of a row (the scan
 * runs under SnapshotDirty, see RccSrGetReadSnapshot).  If that version was
 * written by an in-progress writer W, the reader now depends on W's uncommitted
 * data -> record a WR read dependency so the reader waits for W and cascade-aborts
 * if W aborts.
 *
 * It also leaves an SSI SIRead predicate lock on the version it read, so a later
 * writer that overwrites it discovers this reader (CheckForSerializableConflictIn
 * -> RccRecordRwDep) and the RW anti-dependency is recorded.  Read-latest scans
 * run under SnapshotDirty (non-MVCC), which the SSI gate skips, so the lock is
 * registered with the query's MVCC snapshot (es_snapshot).
 *
 * (Only plain SELECT calls this; UPDATE/SFU record their own dependency in heapam.c.)
 *
 * Returns true when the tuple must be SUPPRESSED (not returned): it is a
 * speculative version, born while this scan was already running, of a row this
 * very scan already returned.  Without this guard the scan would return the
 * same row twice (once per version) and the second read would manufacture an
 * RW+WR 2-cycle with the row's writer -- the dominant abort pattern on hot
 * rows.  Suppressed tuples record no dependency and leave no SIRead.
 */
static inline bool
RccSrRecordSpecRead(ScanState *node, TupleTableSlot *slot)
{
	bool		shouldFree;
	HeapTuple	ht;
	TransactionId wxid;
	TransactionId myxid;

	if (TupIsNull(slot) || node->ss_currentRelation == NULL)
		return false;
	if (IsInParallelMode())
		return false;			/* cannot assign an xid in a parallel worker */

	/*
	 * Only a plain read-only SELECT (no rowMarks) is a speculative read.  A
	 * SELECT FOR UPDATE/SHARE locks/writes the row, so it must NOT register an
	 * SIREAD predicate lock -- doing so makes the row's writer see it as a
	 * reader and form a bogus RW anti-dependency against same-row writers,
	 * aborting both.  Matches RccSrGetReadSnapshot's read-latest condition.
	 * (FOR UPDATE's own WR dependency is recorded in heap_lock_tuple.)
	 */
	if (node->ps.state->es_plannedstmt == NULL ||
		node->ps.state->es_plannedstmt->rowMarks != NIL)
		return false;

	/*
	 * Force an xid for this reader.  A writer that later overwrites the row
	 * records its RW anti-dependency against the reader's xid (the SSI
	 * sxact->topXid).  A read-only reader would otherwise have no xid, be
	 * invisible to the overwriting writer, and write skew would slip through.
	 */
	myxid = GetTopTransactionId();

	ht = ExecFetchSlotHeapTuple(slot, false, &shouldFree);
	wxid = HeapTupleHeaderGetRawXmin(ht->t_data);

	/* WR: depend on an in-progress writer whose (speculative) version we read. */
	if (TransactionIdIsInProgress(wxid) &&
		!TransactionIdIsCurrentTransactionId(wxid))
	{
		/*
		 * Dup guard: only an in-progress writer can have created a version
		 * mid-scan, so only speculative tuples need the chain check.
		 */
		for (int i = 0; i < node->rcc_nreturned; i++)
		{
			if (RccVersionChainReaches(node->ss_currentRelation,
									   &node->rcc_returned[i],
									   &slot->tts_tid, wxid))
			{
				if (shouldFree)
					heap_freetuple(ht);
				return true;	/* newer version of an already-returned row */
			}
		}
		RecordReadDependency(wxid, myxid);
	}

	/* RW: register an SIRead lock so an overwriting writer finds this read. */
	PredicateLockTID(node->ss_currentRelation, &slot->tts_tid,
					 node->ps.state->es_snapshot, wxid);

	/* Remember the returned TID for the dup guard above. */
	if (node->rcc_nreturned < lengthof(node->rcc_returned))
		node->rcc_returned[node->rcc_nreturned++] = slot->tts_tid;

	if (shouldFree)
		heap_freetuple(ht);
	return false;
}
#endif



/*
 * ExecScanFetch -- check interrupts & fetch next potential tuple
 *
 * This routine is concerned with substituting a test tuple if we are
 * inside an EvalPlanQual recheck.  If we aren't, just execute
 * the access method's next-tuple routine.
 */
static inline TupleTableSlot *
ExecScanFetch(ScanState *node,
			  ExecScanAccessMtd accessMtd,
			  ExecScanRecheckMtd recheckMtd)
{
	EState	   *estate = node->ps.state;

	CHECK_FOR_INTERRUPTS();

	if (estate->es_epq_active != NULL)
	{
		EPQState   *epqstate = estate->es_epq_active;

		/*
		 * We are inside an EvalPlanQual recheck.  Return the test tuple if
		 * one is available, after rechecking any access-method-specific
		 * conditions.
		 */
		Index		scanrelid = ((Scan *) node->ps.plan)->scanrelid;

		if (scanrelid == 0)
		{
			/*
			 * This is a ForeignScan or CustomScan which has pushed down a
			 * join to the remote side.  The recheck method is responsible not
			 * only for rechecking the scan/join quals but also for storing
			 * the correct tuple in the slot.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			if (!(*recheckMtd) (node, slot))
				ExecClearTuple(slot);	/* would not be returned by scan */
			return slot;
		}
		else if (epqstate->relsubs_done[scanrelid - 1])
		{
			/*
			 * Return empty slot, as either there is no EPQ tuple for this rel
			 * or we already returned it.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			return ExecClearTuple(slot);
		}
		else if (epqstate->relsubs_slot[scanrelid - 1] != NULL)
		{
			/*
			 * Return replacement tuple provided by the EPQ caller.
			 */

			TupleTableSlot *slot = epqstate->relsubs_slot[scanrelid - 1];

			Assert(epqstate->relsubs_rowmark[scanrelid - 1] == NULL);

			/* Mark to remember that we shouldn't return it again */
			epqstate->relsubs_done[scanrelid - 1] = true;

			/* Return empty slot if we haven't got a test tuple */
			if (TupIsNull(slot))
				return NULL;

			/* Check if it meets the access-method conditions */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
		else if (epqstate->relsubs_rowmark[scanrelid - 1] != NULL)
		{
			/*
			 * Fetch and return replacement tuple using a non-locking rowmark.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			/* Mark to remember that we shouldn't return more */
			epqstate->relsubs_done[scanrelid - 1] = true;

			if (!EvalPlanQualFetchRowMark(epqstate, scanrelid, slot))
				return NULL;

			/* Return empty slot if we haven't got a test tuple */
			if (TupIsNull(slot))
				return NULL;

			/* Check if it meets the access-method conditions */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
	}

	/*
	 * Run the node-type-specific access method function to get the next tuple
	 */
	return (*accessMtd) (node);
}

/* ----------------------------------------------------------------
 *		ExecScan
 *
 *		Scans the relation using the 'access method' indicated and
 *		returns the next qualifying tuple.
 *		The access method returns the next tuple and ExecScan() is
 *		responsible for checking the tuple returned against the qual-clause.
 *
 *		A 'recheck method' must also be provided that can check an
 *		arbitrary tuple of the relation against any qual conditions
 *		that are implemented internal to the access method.
 *
 *		Conditions:
 *		  -- the "cursor" maintained by the AMI is positioned at the tuple
 *			 returned previously.
 *
 *		Initial States:
 *		  -- the relation indicated is opened for scanning so that the
 *			 "cursor" is positioned before the first qualifying tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecScan(ScanState *node,
		 ExecScanAccessMtd accessMtd,	/* function returning a tuple */
		 ExecScanRecheckMtd recheckMtd)
{
	ExprContext *econtext;
	ExprState  *qual;
	ProjectionInfo *projInfo;

	/*
	 * Fetch data from node
	 */
	qual = node->ps.qual;
	projInfo = node->ps.ps_ProjInfo;
	econtext = node->ps.ps_ExprContext;

	/* interrupt checks are in ExecScanFetch */

	/*
	 * If we have neither a qual to check nor a projection to do, just skip
	 * all the overhead and return the raw scan tuple.
	 */
	if (!qual && !projInfo)
	{
		ResetExprContext(econtext);
#ifdef RCC_SR
		for (;;)
		{
			TupleTableSlot *slot = ExecScanFetch(node, accessMtd, recheckMtd);

			/*
			 * Register this row as read (safe here: no buffer lock held).
			 * Only genuine SELECT reads count -- the locate-scan of an
			 * UPDATE/DELETE must NOT register the row, otherwise blind writers
			 * would be treated as readers and form spurious RW cycles.
			 * A true return means the tuple is a mid-scan-born duplicate of an
			 * already-returned row: skip it and fetch the next tuple.
			 */
			if (!TupIsNull(slot) && node->ss_currentRelation != NULL &&
				node->ps.state->es_plannedstmt != NULL &&
				node->ps.state->es_plannedstmt->commandType == CMD_SELECT &&
				RccSrRecordSpecRead(node, slot))
				continue;
			return slot;
		}
#else
		return ExecScanFetch(node, accessMtd, recheckMtd);
#endif
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * get a tuple from the access method.  Loop until we obtain a tuple that
	 * passes the qualification.
	 */
	for (;;)
	{
		TupleTableSlot *slot;

		slot = ExecScanFetch(node, accessMtd, recheckMtd);

		/*
		 * if the slot returned by the accessMtd contains NULL, then it means
		 * there is nothing more to scan so we just return an empty slot,
		 * being careful to use the projection result slot so it has correct
		 * tupleDesc.
		 */
		if (TupIsNull(slot))
		{
			if (projInfo)
				return ExecClearTuple(projInfo->pi_state.resultslot);
			else
				return slot;
		}

		/*
		 * place the current tuple into the expr context
		 */
		econtext->ecxt_scantuple = slot;

		/*
		 * check that the current tuple satisfies the qual-clause
		 *
		 * check for non-null qual here to avoid a function call to ExecQual()
		 * when the qual is null ... saves only a few cycles, but they add up
		 * ...
		 */
		if (qual == NULL || ExecQual(qual, econtext))
		{
			/*
			 * Found a satisfactory scan tuple.
			 */
#ifdef RCC_SR
			/*
			 * Register this returned row as read (no buffer lock held).
			 * Only genuine SELECT reads count -- exclude UPDATE/DELETE
			 * locate-scans (see note in the no-qual fast path above).
			 * A true return means the tuple is a mid-scan-born duplicate
			 * of an already-returned row: skip it like a failed qual.
			 */
			if (node->ss_currentRelation != NULL &&
				node->ps.state->es_plannedstmt != NULL &&
				node->ps.state->es_plannedstmt->commandType == CMD_SELECT &&
				RccSrRecordSpecRead(node, slot))
				continue;
#endif
			if (projInfo)
			{
				/*
				 * Form a projection tuple, store it in the result tuple slot
				 * and return it.
				 */
				return ExecProject(projInfo);
			}
			else
			{
				/*
				 * Here, we aren't projecting, so just return scan tuple.
				 */
				return slot;
			}
		}
		else
			InstrCountFiltered1(node, 1);

		/*
		 * Tuple fails qual, so free per-tuple memory and try again.
		 */
		ResetExprContext(econtext);
	}
}

/*
 * ExecAssignScanProjectionInfo
 *		Set up projection info for a scan node, if necessary.
 *
 * We can avoid a projection step if the requested tlist exactly matches
 * the underlying tuple type.  If so, we just set ps_ProjInfo to NULL.
 * Note that this case occurs not only for simple "SELECT * FROM ...", but
 * also in most cases where there are joins or other processing nodes above
 * the scan node, because the planner will preferentially generate a matching
 * tlist.
 *
 * The scan slot's descriptor must have been set already.
 */
void
ExecAssignScanProjectionInfo(ScanState *node)
{
	Scan	   *scan = (Scan *) node->ps.plan;
	TupleDesc	tupdesc = node->ss_ScanTupleSlot->tts_tupleDescriptor;

	ExecConditionalAssignProjectionInfo(&node->ps, tupdesc, scan->scanrelid);
}

/*
 * ExecAssignScanProjectionInfoWithVarno
 *		As above, but caller can specify varno expected in Vars in the tlist.
 */
void
ExecAssignScanProjectionInfoWithVarno(ScanState *node, int varno)
{
	TupleDesc	tupdesc = node->ss_ScanTupleSlot->tts_tupleDescriptor;

	ExecConditionalAssignProjectionInfo(&node->ps, tupdesc, varno);
}

/*
 * ExecScanReScan
 *
 * This must be called within the ReScan function of any plan node type
 * that uses ExecScan().
 */
void
ExecScanReScan(ScanState *node)
{
	EState	   *estate = node->ps.state;

	/*
	 * We must clear the scan tuple so that observers (e.g., execCurrent.c)
	 * can tell that this plan node is not positioned on a tuple.
	 */
	ExecClearTuple(node->ss_ScanTupleSlot);

	/*
	 * Rescan EvalPlanQual tuple(s) if we're inside an EvalPlanQual recheck.
	 * But don't lose the "blocked" status of blocked target relations.
	 */
	if (estate->es_epq_active != NULL)
	{
		EPQState   *epqstate = estate->es_epq_active;
		Index		scanrelid = ((Scan *) node->ps.plan)->scanrelid;

		if (scanrelid > 0)
			epqstate->relsubs_done[scanrelid - 1] =
				epqstate->relsubs_blocked[scanrelid - 1];
		else
		{
			Bitmapset  *relids;
			int			rtindex = -1;

			/*
			 * If an FDW or custom scan provider has replaced the join with a
			 * scan, there are multiple RTIs; reset the epqScanDone flag for
			 * all of them.
			 */
			if (IsA(node->ps.plan, ForeignScan))
				relids = ((ForeignScan *) node->ps.plan)->fs_base_relids;
			else if (IsA(node->ps.plan, CustomScan))
				relids = ((CustomScan *) node->ps.plan)->custom_relids;
			else
				elog(ERROR, "unexpected scan node: %d",
					 (int) nodeTag(node->ps.plan));

			while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
			{
				Assert(rtindex > 0);
				epqstate->relsubs_done[rtindex - 1] =
					epqstate->relsubs_blocked[rtindex - 1];
			}
		}
	}
}
