/*-------------------------------------------------------------------------
 *
 * heapam_handler.c
 *	  heap table access method code
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_handler.c
 *
 *
 * NOTES
 *	  This files wires up the lower level heapam.c et routines with the
 *	  tableam abstraction.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"


static const TableAmRoutine heapam_methods;


/* ------------------------------------------------------------------------
 * Slot related callbacks for heap AM
 * ------------------------------------------------------------------------
 */

static const TupleTableSlotOps *
heapam_slot_callbacks(Relation relation)
{
	return &TTSOpsBufferHeapTuple;
}


/* ------------------------------------------------------------------------
 * Index Scan Callbacks for heap AM
 * ------------------------------------------------------------------------
 */

static IndexFetchTableData *
heapam_index_fetch_begin(Relation rel)
{
	IndexFetchHeapData *hscan = palloc0(sizeof(IndexFetchHeapData));

	hscan->xs_base.rel = rel;
	hscan->xs_cbuf = InvalidBuffer;

	return &hscan->xs_base;
}

static void
heapam_index_fetch_reset(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	if (BufferIsValid(hscan->xs_cbuf))
	{
		ReleaseBuffer(hscan->xs_cbuf);
		hscan->xs_cbuf = InvalidBuffer;
	}
}

static void
heapam_index_fetch_end(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	heapam_index_fetch_reset(scan);

	pfree(hscan);
}

static bool
heapam_index_fetch_tuple(struct IndexFetchTableData *scan,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot,
						 bool *call_again, bool *all_dead)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		got_heap_tuple;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	/* We can skip the buffer-switching logic if we're in mid-HOT chain. */
	if (!*call_again)
	{
		/* Switch to correct buffer if we don't have it already */
		Buffer		prev_buf = hscan->xs_cbuf;

		hscan->xs_cbuf = ReleaseAndReadBuffer(hscan->xs_cbuf,
											  hscan->xs_base.rel,
											  ItemPointerGetBlockNumber(tid));

		/*
		 * Prune page, but only if we weren't already on this page
		 */
		if (prev_buf != hscan->xs_cbuf)
			heap_page_prune_opt(hscan->xs_base.rel, hscan->xs_cbuf);
	}

	/* Obtain share-lock on the buffer so we can examine visibility */
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
	got_heap_tuple = heap_hot_search_buffer(tid,
											hscan->xs_base.rel,
											hscan->xs_cbuf,
											snapshot,
											&bslot->base.tupdata,
											all_dead,
											!*call_again);
	bslot->base.tupdata.t_self = *tid;
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);

	if (got_heap_tuple)
	{
		/*
		 * Only in a non-MVCC snapshot can more than one member of the HOT
		 * chain be visible.
		 */
		*call_again = !IsMVCCSnapshot(snapshot);

		slot->tts_tableOid = RelationGetRelid(scan->rel);
		ExecStoreBufferHeapTuple(&bslot->base.tupdata, slot, hscan->xs_cbuf);
	}
	else
	{
		/* We've reached the end of the HOT chain. */
		*call_again = false;
	}

	return got_heap_tuple;
}


/* ------------------------------------------------------------------------
 * Callbacks for non-modifying operations on individual tuples for heap AM
 * ------------------------------------------------------------------------
 */

static bool
heapam_fetch_row_version(Relation relation,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	Buffer		buffer;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	bslot->base.tupdata.t_self = *tid;
	if (heap_fetch(relation, snapshot, &bslot->base.tupdata, &buffer))
	{
		/* store in slot, transferring existing pin */
		ExecStorePinnedBufferHeapTuple(&bslot->base.tupdata, slot, buffer);
		slot->tts_tableOid = RelationGetRelid(relation);

		return true;
	}

	return false;
}

static bool
heapam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								Snapshot snapshot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	bool		res;

	Assert(TTS_IS_BUFFERTUPLE(slot));
	Assert(BufferIsValid(bslot->buffer));

	/*
	 * We need buffer pin and lock to call HeapTupleSatisfiesVisibility.
	 * Caller should be holding pin, but not lock.
	 */
	LockBuffer(bslot->buffer, BUFFER_LOCK_SHARE);
	res = HeapTupleSatisfiesVisibility(bslot->base.tuple, snapshot,
									   bslot->buffer);
	LockBuffer(bslot->buffer, BUFFER_LOCK_UNLOCK);

	return res;
}


/* ----------------------------------------------------------------------------
 *  Functions for manipulations of physical tuples for heap AM.
 * ----------------------------------------------------------------------------
 */

static void
heapam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					int options, BulkInsertState bistate)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	/* Perform the insertion, and copy the resulting ItemPointer */
	heap_insert(relation, tuple, cid, options, bistate);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static void
heapam_tuple_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid,
								int options, BulkInsertState bistate, uint32 specToken)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	HeapTupleHeaderSetSpeculativeToken(tuple->t_data, specToken);
	options |= HEAP_INSERT_SPECULATIVE;

	/* Perform the insertion, and copy the resulting ItemPointer */
	heap_insert(relation, tuple, cid, options, bistate);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static void
heapam_tuple_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 spekToken,
								  bool succeeded)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

	/* adjust the tuple's state accordingly */
	if (!succeeded)
		heap_finish_speculative(relation, &slot->tts_tid);
	else
		heap_abort_speculative(relation, &slot->tts_tid);

	if (shouldFree)
		pfree(tuple);
}

static TM_Result
heapam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
					Snapshot snapshot, Snapshot crosscheck, bool wait,
					TM_FailureData *tmfd, bool changingPart)
{
	/*
	 * Currently Deleting of index tuples are handled at vacuum, in case if
	 * the storage itself is cleaning the dead tuples by itself, it is the
	 * time to call the index tuple deletion also.
	 */
	return heap_delete(relation, tid, cid, crosscheck, wait, tmfd, changingPart);
}


static TM_Result
heapam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, bool *update_indexes)
{
	bool		shouldFree = true;
	HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
	TM_Result	result;

	/* Update the tuple with table oid */
	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	result = heap_update(relation, otid, tuple, cid, crosscheck, wait,
						 tmfd, lockmode);
	ItemPointerCopy(&tuple->t_self, &slot->tts_tid);

	/*
	 * Decide whether new index entries are needed for the tuple
	 *
	 * Note: heap_update returns the tid (location) of the new tuple in the
	 * t_self field.
	 *
	 * If it's a HOT update, we mustn't insert new index entries.
	 */
	*update_indexes = result == TM_Ok && !HeapTupleIsHeapOnly(tuple);

	if (shouldFree)
		pfree(tuple);

	return result;
}

static TM_Result
heapam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	TM_Result	result;
	Buffer		buffer;
	HeapTuple	tuple = &bslot->base.tupdata;
	bool		follow_updates;

	follow_updates = (flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) != 0;
	tmfd->traversed = false;

	Assert(TTS_IS_BUFFERTUPLE(slot));

tuple_lock_retry:
	tuple->t_self = *tid;
	result = heap_lock_tuple(relation, tuple, cid, mode, wait_policy,
							 follow_updates, &buffer, tmfd);

	if (result == TM_Updated &&
		(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
	{
		ReleaseBuffer(buffer);
		/* Should not encounter speculative tuple on recheck */
		Assert(!HeapTupleHeaderIsSpeculative(tuple->t_data));

		if (!ItemPointerEquals(&tmfd->ctid, &tuple->t_self))
		{
			SnapshotData SnapshotDirty;
			TransactionId priorXmax;

			/* it was updated, so look at the updated version */
			*tid = tmfd->ctid;
			/* updated row should have xmin matching this xmax */
			priorXmax = tmfd->xmax;

			/* signal that a tuple later in the chain is getting locked */
			tmfd->traversed = true;

			/*
			 * fetch target tuple
			 *
			 * Loop here to deal with updated or busy tuples
			 */
			InitDirtySnapshot(SnapshotDirty);
			for (;;)
			{
				if (ItemPointerIndicatesMovedPartitions(tid))
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));

				tuple->t_self = *tid;
				if (heap_fetch(relation, &SnapshotDirty, tuple, &buffer))
				{
					/*
					 * If xmin isn't what we're expecting, the slot must have
					 * been recycled and reused for an unrelated tuple.  This
					 * implies that the latest version of the row was deleted,
					 * so we need do nothing.  (Should be safe to examine xmin
					 * without getting buffer's content lock.  We assume
					 * reading a TransactionId to be atomic, and Xmin never
					 * changes in an existing tuple, except to invalid or
					 * frozen, and neither of those can match priorXmax.)
					 */
					if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple->t_data),
											 priorXmax))
					{
						ReleaseBuffer(buffer);
						return TM_Deleted;
					}

					/* otherwise xmin should not be dirty... */
					if (TransactionIdIsValid(SnapshotDirty.xmin))
						elog(ERROR, "t_xmin is uncommitted in tuple to be updated");

					/*
					 * If tuple is being updated by other transaction then we
					 * have to wait for its commit/abort, or die trying.
					 */
					if (TransactionIdIsValid(SnapshotDirty.xmax))
					{
						ReleaseBuffer(buffer);
						switch (wait_policy)
						{
							case LockWaitBlock:
								XactLockTableWait(SnapshotDirty.xmax,
												  relation, &tuple->t_self,
												  XLTW_FetchUpdated);
								break;
							case LockWaitSkip:
								if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
									/* skip instead of waiting */
									return TM_WouldBlock;
								break;
							case LockWaitError:
								if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
									ereport(ERROR,
											(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
											 errmsg("could not obtain lock on row in relation \"%s\"",
													RelationGetRelationName(relation))));
								break;
						}
						continue;	/* loop back to repeat heap_fetch */
					}

					/*
					 * If tuple was inserted by our own transaction, we have
					 * to check cmin against cid: cmin >= current CID means
					 * our command cannot see the tuple, so we should ignore
					 * it. Otherwise heap_lock_tuple() will throw an error,
					 * and so would any later attempt to update or delete the
					 * tuple.  (We need not check cmax because
					 * HeapTupleSatisfiesDirty will consider a tuple deleted
					 * by our transaction dead, regardless of cmax.)  We just
					 * checked that priorXmax == xmin, so we can test that
					 * variable instead of doing HeapTupleHeaderGetXmin again.
					 */
					if (TransactionIdIsCurrentTransactionId(priorXmax) &&
						HeapTupleHeaderGetCmin(tuple->t_data) >= cid)
					{
						ReleaseBuffer(buffer);
						return TM_Invisible;
					}

					/*
					 * This is a live tuple, so try to lock it again.
					 */
					ReleaseBuffer(buffer);
					goto tuple_lock_retry;
				}

				/*
				 * If the referenced slot was actually empty, the latest
				 * version of the row must have been deleted, so we need do
				 * nothing.
				 */
				if (tuple->t_data == NULL)
				{
					return TM_Deleted;
				}

				/*
				 * As above, if xmin isn't what we're expecting, do nothing.
				 */
				if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple->t_data),
										 priorXmax))
				{
					if (BufferIsValid(buffer))
						ReleaseBuffer(buffer);
					return TM_Deleted;
				}

				/*
				 * If we get here, the tuple was found but failed
				 * SnapshotDirty. Assuming the xmin is either a committed xact
				 * or our own xact (as it certainly should be if we're trying
				 * to modify the tuple), this must mean that the row was
				 * updated or deleted by either a committed xact or our own
				 * xact.  If it was deleted, we can ignore it; if it was
				 * updated then chain up to the next version and repeat the
				 * whole process.
				 *
				 * As above, it should be safe to examine xmax and t_ctid
				 * without the buffer content lock, because they can't be
				 * changing.
				 */
				if (ItemPointerEquals(&tuple->t_self, &tuple->t_data->t_ctid))
				{
					/* deleted, so forget about it */
					if (BufferIsValid(buffer))
						ReleaseBuffer(buffer);
					return TM_Deleted;
				}

				/* updated, so look at the updated row */
				*tid = tuple->t_data->t_ctid;
				/* updated row should have xmin matching this xmax */
				priorXmax = HeapTupleHeaderGetUpdateXid(tuple->t_data);
				if (BufferIsValid(buffer))
					ReleaseBuffer(buffer);
				/* loop back to fetch next in chain */
			}
		}
		else
		{
			/* tuple was deleted, so give up */
			return TM_Deleted;
		}
	}

	slot->tts_tableOid = RelationGetRelid(relation);
	tuple->t_tableOid = slot->tts_tableOid;

	/* store in slot, transferring existing pin */
	ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);

	return result;
}


/* ------------------------------------------------------------------------
 * DDL related callbacks for heap AM.
 * ------------------------------------------------------------------------
 */

static double
heapam_index_build_range_scan(Relation heapRelation,
							  Relation indexRelation,
							  IndexInfo *indexInfo,
							  bool allow_sync,
							  bool anyvisible,
							  BlockNumber start_blockno,
							  BlockNumber numblocks,
							  IndexBuildCallback callback,
							  void *callback_state,
							  TableScanDesc scan)
{
	HeapScanDesc hscan;
	bool		is_system_catalog;
	bool		checking_uniqueness;
	HeapTuple	heapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples;
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;
	bool		need_unregister_snapshot = false;
	TransactionId OldestXmin;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/* Remember if it's a system catalog */
	is_system_catalog = IsSystemRelation(heapRelation);

	/* See whether we're verifying uniqueness/exclusion properties */
	checking_uniqueness = (indexInfo->ii_Unique ||
						   indexInfo->ii_ExclusionOps != NULL);

	/*
	 * "Any visible" mode is not compatible with uniqueness checks; make sure
	 * only one of those is requested.
	 */
	Assert(!(anyvisible && checking_uniqueness));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heapRelation, NULL);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * Prepare for scan of the base relation.  In a normal index build, we use
	 * SnapshotAny because we must retrieve all tuples and do our own time
	 * qual checks (because we have to index RECENTLY_DEAD tuples). In a
	 * concurrent build, or during bootstrap, we take a regular MVCC snapshot
	 * and index whatever's live according to that.
	 */
	OldestXmin = InvalidTransactionId;

	/* okay to ignore lazy VACUUMs here */
	if (!IsBootstrapProcessingMode() && !indexInfo->ii_Concurrent)
		OldestXmin = GetOldestXmin(heapRelation, PROCARRAY_FLAGS_VACUUM);

	if (!scan)
	{
		/*
		 * Serial index build.
		 *
		 * Must begin our own heap scan in this case.  We may also need to
		 * register a snapshot whose lifetime is under our direct control.
		 */
		if (!TransactionIdIsValid(OldestXmin))
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			need_unregister_snapshot = true;
		}
		else
			snapshot = SnapshotAny;

		scan = table_beginscan_strat(heapRelation,	/* relation */
									 snapshot,	/* snapshot */
									 0, /* number of keys */
									 NULL,	/* scan key */
									 true,	/* buffer access strategy OK */
									 allow_sync);	/* syncscan OK? */
	}
	else
	{
		/*
		 * Parallel index build.
		 *
		 * Parallel case never registers/unregisters own snapshot.  Snapshot
		 * is taken from parallel heap scan, and is SnapshotAny or an MVCC
		 * snapshot, based on same criteria as serial case.
		 */
		Assert(!IsBootstrapProcessingMode());
		Assert(allow_sync);
		snapshot = scan->rs_snapshot;
	}

	hscan = (HeapScanDesc) scan;

	/*
	 * Must call GetOldestXmin() with SnapshotAny.  Should never call
	 * GetOldestXmin() with MVCC snapshot. (It's especially worth checking
	 * this for parallel builds, since ambuild routines that support parallel
	 * builds must work these details out for themselves.)
	 */
	Assert(snapshot == SnapshotAny || IsMVCCSnapshot(snapshot));
	Assert(snapshot == SnapshotAny ? TransactionIdIsValid(OldestXmin) :
		   !TransactionIdIsValid(OldestXmin));
	Assert(snapshot == SnapshotAny || !anyvisible);

	/* set our scan endpoints */
	if (!allow_sync)
		heap_setscanlimits(scan, start_blockno, numblocks);
	else
	{
		/* syncscan can only be requested on whole relation */
		Assert(start_blockno == 0);
		Assert(numblocks == InvalidBlockNumber);
	}

	reltuples = 0;

	/*
	 * Scan all tuples in the base relation.
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		bool		tupleIsAlive;

		CHECK_FOR_INTERRUPTS();

		/*
		 * When dealing with a HOT-chain of updated tuples, we want to index
		 * the values of the live tuple (if any), but index it under the TID
		 * of the chain's root tuple.  This approach is necessary to preserve
		 * the HOT-chain structure in the heap. So we need to be able to find
		 * the root item offset for every tuple that's in a HOT-chain.  When
		 * first reaching a new page of the relation, call
		 * heap_get_root_tuples() to build a map of root item offsets on the
		 * page.
		 *
		 * It might look unsafe to use this information across buffer
		 * lock/unlock.  However, we hold ShareLock on the table so no
		 * ordinary insert/update/delete should occur; and we hold pin on the
		 * buffer continuously while visiting the page, so no pruning
		 * operation can occur either.
		 *
		 * Also, although our opinions about tuple liveness could change while
		 * we scan the page (due to concurrent transaction commits/aborts),
		 * the chain root locations won't, so this info doesn't need to be
		 * rebuilt after waiting for another transaction.
		 *
		 * Note the implied assumption that there is no more than one live
		 * tuple per HOT-chain --- else we could create more than one index
		 * entry pointing to the same root tuple.
		 */
		if (hscan->rs_cblock != root_blkno)
		{
			Page		page = BufferGetPage(hscan->rs_cbuf);

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			root_blkno = hscan->rs_cblock;
		}

		if (snapshot == SnapshotAny)
		{
			/* do our own time qual check */
			bool		indexIt;
			TransactionId xwait;

	recheck:

			/*
			 * We could possibly get away with not locking the buffer here,
			 * since caller should hold ShareLock on the relation, but let's
			 * be conservative about it.  (This remark is still correct even
			 * with HOT-pruning: our pin on the buffer prevents pruning.)
			 */
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

			/*
			 * The criteria for counting a tuple as live in this block need to
			 * match what analyze.c's acquire_sample_rows() does, otherwise
			 * CREATE INDEX and ANALYZE may produce wildly different reltuples
			 * values, e.g. when there are many recently-dead tuples.
			 */
			switch (HeapTupleSatisfiesVacuum(heapTuple, OldestXmin,
											 hscan->rs_cbuf))
			{
				case HEAPTUPLE_DEAD:
					/* Definitely dead, we can ignore it */
					indexIt = false;
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_LIVE:
					/* Normal case, index and unique-check it */
					indexIt = true;
					tupleIsAlive = true;
					/* Count it as live, too */
					reltuples += 1;
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * If tuple is recently deleted then we must index it
					 * anyway to preserve MVCC semantics.  (Pre-existing
					 * transactions could try to use the index after we finish
					 * building it, and may need to see such tuples.)
					 *
					 * However, if it was HOT-updated then we must only index
					 * the live tuple at the end of the HOT-chain.  Since this
					 * breaks semantics for pre-existing snapshots, mark the
					 * index as unusable for them.
					 *
					 * We don't count recently-dead tuples in reltuples, even
					 * if we index them; see acquire_sample_rows().
					 */
					if (HeapTupleIsHotUpdated(heapTuple))
					{
						indexIt = false;
						/* mark the index as unsafe for old snapshots */
						indexInfo->ii_BrokenHotChain = true;
					}
					else
						indexIt = true;
					/* In any case, exclude the tuple from unique-checking */
					tupleIsAlive = false;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:

					/*
					 * In "anyvisible" mode, this tuple is visible and we
					 * don't need any further checks.
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = true;
						reltuples += 1;
						break;
					}

					/*
					 * Since caller should hold ShareLock or better, normally
					 * the only way to see this is if it was inserted earlier
					 * in our own transaction.  However, it can happen in
					 * system catalogs, since we tend to release write lock
					 * before commit there.  Give a warning if neither case
					 * applies.
					 */
					xwait = HeapTupleHeaderGetXmin(heapTuple->t_data);
					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent insert in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * If we are performing uniqueness checks, indexing
						 * such a tuple could lead to a bogus uniqueness
						 * failure.  In that case we wait for the inserting
						 * transaction to finish and check again.
						 */
						if (checking_uniqueness)
						{
							/*
							 * Must drop the lock on the buffer before we wait
							 */
							LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait, heapRelation,
											  &heapTuple->t_self,
											  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();
							goto recheck;
						}
					}
					else
					{
						/*
						 * For consistency with acquire_sample_rows(), count
						 * HEAPTUPLE_INSERT_IN_PROGRESS tuples as live only
						 * when inserted by our own transaction.
						 */
						reltuples += 1;
					}

					/*
					 * We must index such tuples, since if the index build
					 * commits then they're good.
					 */
					indexIt = true;
					tupleIsAlive = true;
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:

					/*
					 * As with INSERT_IN_PROGRESS case, this is unexpected
					 * unless it's our own deletion or a system catalog; but
					 * in anyvisible mode, this tuple is visible.
					 */
					if (anyvisible)
					{
						indexIt = true;
						tupleIsAlive = false;
						reltuples += 1;
						break;
					}

					xwait = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
					if (!TransactionIdIsCurrentTransactionId(xwait))
					{
						if (!is_system_catalog)
							elog(WARNING, "concurrent delete in progress within table \"%s\"",
								 RelationGetRelationName(heapRelation));

						/*
						 * If we are performing uniqueness checks, assuming
						 * the tuple is dead could lead to missing a
						 * uniqueness violation.  In that case we wait for the
						 * deleting transaction to finish and check again.
						 *
						 * Also, if it's a HOT-updated tuple, we should not
						 * index it but rather the live tuple at the end of
						 * the HOT-chain.  However, the deleting transaction
						 * could abort, possibly leaving this tuple as live
						 * after all, in which case it has to be indexed. The
						 * only way to know what to do is to wait for the
						 * deleting transaction to finish and check again.
						 */
						if (checking_uniqueness ||
							HeapTupleIsHotUpdated(heapTuple))
						{
							/*
							 * Must drop the lock on the buffer before we wait
							 */
							LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
							XactLockTableWait(xwait, heapRelation,
											  &heapTuple->t_self,
											  XLTW_InsertIndexUnique);
							CHECK_FOR_INTERRUPTS();
							goto recheck;
						}

						/*
						 * Otherwise index it but don't check for uniqueness,
						 * the same as a RECENTLY_DEAD tuple.
						 */
						indexIt = true;

						/*
						 * Count HEAPTUPLE_DELETE_IN_PROGRESS tuples as live,
						 * if they were not deleted by the current
						 * transaction.  That's what acquire_sample_rows()
						 * does, and we want the behavior to be consistent.
						 */
						reltuples += 1;
					}
					else if (HeapTupleIsHotUpdated(heapTuple))
					{
						/*
						 * It's a HOT-updated tuple deleted by our own xact.
						 * We can assume the deletion will commit (else the
						 * index contents don't matter), so treat the same as
						 * RECENTLY_DEAD HOT-updated tuples.
						 */
						indexIt = false;
						/* mark the index as unsafe for old snapshots */
						indexInfo->ii_BrokenHotChain = true;
					}
					else
					{
						/*
						 * It's a regular tuple deleted by our own xact. Index
						 * it, but don't check for uniqueness nor count in
						 * reltuples, the same as a RECENTLY_DEAD tuple.
						 */
						indexIt = true;
					}
					/* In any case, exclude the tuple from unique-checking */
					tupleIsAlive = false;
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					indexIt = tupleIsAlive = false; /* keep compiler quiet */
					break;
			}

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			if (!indexIt)
				continue;
		}
		else
		{
			/* heap_getnext did the time qual check */
			tupleIsAlive = true;
			reltuples += 1;
		}

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		/* Set up for predicate or expression evaluation */
		ExecStoreBufferHeapTuple(heapTuple, slot, hscan->rs_cbuf);

		/*
		 * In a partial index, discard tuples that don't satisfy the
		 * predicate.
		 */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * For the current heap tuple, extract all the attributes we use in
		 * this index, and note which are null.  This also performs evaluation
		 * of any expressions needed.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/*
		 * You'd think we should go ahead and build the index tuple here, but
		 * some index AMs want to do further processing on the data first.  So
		 * pass the values[] and isnull[] arrays, instead.
		 */

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			/*
			 * For a heap-only tuple, pretend its TID is that of the root. See
			 * src/backend/access/heap/README.HOT for discussion.
			 */
			HeapTupleData rootTuple;
			OffsetNumber offnum;

			rootTuple = *heapTuple;
			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_self);

			if (!OffsetNumberIsValid(root_offsets[offnum - 1]))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("failed to find parent tuple for heap-only tuple at (%u,%u) in table \"%s\"",
										 ItemPointerGetBlockNumber(&heapTuple->t_self),
										 offnum,
										 RelationGetRelationName(heapRelation))));

			ItemPointerSetOffsetNumber(&rootTuple.t_self,
									   root_offsets[offnum - 1]);

			/* Call the AM's callback routine to process the tuple */
			callback(indexRelation, &rootTuple, values, isnull, tupleIsAlive,
					 callback_state);
		}
		else
		{
			/* Call the AM's callback routine to process the tuple */
			callback(indexRelation, heapTuple, values, isnull, tupleIsAlive,
					 callback_state);
		}
	}

	table_endscan(scan);

	/* we can now forget our snapshot, if set and registered by us */
	if (need_unregister_snapshot)
		UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return reltuples;
}

static void
heapam_index_validate_scan(Relation heapRelation,
						   Relation indexRelation,
						   IndexInfo *indexInfo,
						   Snapshot snapshot,
						   ValidateIndexState * state)
{
	TableScanDesc scan;
	HeapScanDesc hscan;
	HeapTuple	heapTuple;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	BlockNumber root_blkno = InvalidBlockNumber;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];
	bool		in_index[MaxHeapTuplesPerPage];

	/* state variables for the merge */
	ItemPointer indexcursor = NULL;
	ItemPointerData decoded;
	bool		tuplesort_empty = false;

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRelation),
									&TTSOpsHeapTuple);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * Prepare for scan of the base relation.  We need just those tuples
	 * satisfying the passed-in reference snapshot.  We must disable syncscan
	 * here, because it's critical that we read from block zero forward to
	 * match the sorted TIDs.
	 */
	scan = table_beginscan_strat(heapRelation,	/* relation */
								 snapshot,	/* snapshot */
								 0, /* number of keys */
								 NULL,	/* scan key */
								 true,	/* buffer access strategy OK */
								 false);	/* syncscan not OK */
	hscan = (HeapScanDesc) scan;

	/*
	 * Scan all tuples matching the snapshot.
	 */
	while ((heapTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		ItemPointer heapcursor = &heapTuple->t_self;
		ItemPointerData rootTuple;
		OffsetNumber root_offnum;

		CHECK_FOR_INTERRUPTS();

		state->htups += 1;

		/*
		 * As commented in table_index_build_scan, we should index heap-only
		 * tuples under the TIDs of their root tuples; so when we advance onto
		 * a new heap page, build a map of root item offsets on the page.
		 *
		 * This complicates merging against the tuplesort output: we will
		 * visit the live tuples in order by their offsets, but the root
		 * offsets that we need to compare against the index contents might be
		 * ordered differently.  So we might have to "look back" within the
		 * tuplesort output, but only within the current page.  We handle that
		 * by keeping a bool array in_index[] showing all the
		 * already-passed-over tuplesort output TIDs of the current page. We
		 * clear that array here, when advancing onto a new heap page.
		 */
		if (hscan->rs_cblock != root_blkno)
		{
			Page		page = BufferGetPage(hscan->rs_cbuf);

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(page, root_offsets);
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			memset(in_index, 0, sizeof(in_index));

			root_blkno = hscan->rs_cblock;
		}

		/* Convert actual tuple TID to root TID */
		rootTuple = *heapcursor;
		root_offnum = ItemPointerGetOffsetNumber(heapcursor);

		if (HeapTupleIsHeapOnly(heapTuple))
		{
			root_offnum = root_offsets[root_offnum - 1];
			if (!OffsetNumberIsValid(root_offnum))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("failed to find parent tuple for heap-only tuple at (%u,%u) in table \"%s\"",
										 ItemPointerGetBlockNumber(heapcursor),
										 ItemPointerGetOffsetNumber(heapcursor),
										 RelationGetRelationName(heapRelation))));
			ItemPointerSetOffsetNumber(&rootTuple, root_offnum);
		}

		/*
		 * "merge" by skipping through the index tuples until we find or pass
		 * the current root tuple.
		 */
		while (!tuplesort_empty &&
			   (!indexcursor ||
				ItemPointerCompare(indexcursor, &rootTuple) < 0))
		{
			Datum		ts_val;
			bool		ts_isnull;

			if (indexcursor)
			{
				/*
				 * Remember index items seen earlier on the current heap page
				 */
				if (ItemPointerGetBlockNumber(indexcursor) == root_blkno)
					in_index[ItemPointerGetOffsetNumber(indexcursor) - 1] = true;
			}

			tuplesort_empty = !tuplesort_getdatum(state->tuplesort, true,
												  &ts_val, &ts_isnull, NULL);
			Assert(tuplesort_empty || !ts_isnull);
			if (!tuplesort_empty)
			{
				itemptr_decode(&decoded, DatumGetInt64(ts_val));
				indexcursor = &decoded;

				/* If int8 is pass-by-ref, free (encoded) TID Datum memory */
#ifndef USE_FLOAT8_BYVAL
				pfree(DatumGetPointer(ts_val));
#endif
			}
			else
			{
				/* Be tidy */
				indexcursor = NULL;
			}
		}

		/*
		 * If the tuplesort has overshot *and* we didn't see a match earlier,
		 * then this tuple is missing from the index, so insert it.
		 */
		if ((tuplesort_empty ||
			 ItemPointerCompare(indexcursor, &rootTuple) > 0) &&
			!in_index[root_offnum - 1])
		{
			MemoryContextReset(econtext->ecxt_per_tuple_memory);

			/* Set up for predicate or expression evaluation */
			ExecStoreHeapTuple(heapTuple, slot, false);

			/*
			 * In a partial index, discard tuples that don't satisfy the
			 * predicate.
			 */
			if (predicate != NULL)
			{
				if (!ExecQual(predicate, econtext))
					continue;
			}

			/*
			 * For the current heap tuple, extract all the attributes we use
			 * in this index, and note which are null.  This also performs
			 * evaluation of any expressions needed.
			 */
			FormIndexDatum(indexInfo,
						   slot,
						   estate,
						   values,
						   isnull);

			/*
			 * You'd think we should go ahead and build the index tuple here,
			 * but some index AMs want to do further processing on the data
			 * first. So pass the values[] and isnull[] arrays, instead.
			 */

			/*
			 * If the tuple is already committed dead, you might think we
			 * could suppress uniqueness checking, but this is no longer true
			 * in the presence of HOT, because the insert is actually a proxy
			 * for a uniqueness check on the whole HOT-chain.  That is, the
			 * tuple we have here could be dead because it was already
			 * HOT-updated, and if so the updating transaction will not have
			 * thought it should insert index entries.  The index AM will
			 * check the whole HOT-chain and correctly detect a conflict if
			 * there is one.
			 */

			index_insert(indexRelation,
						 values,
						 isnull,
						 &rootTuple,
						 heapRelation,
						 indexInfo->ii_Unique ?
						 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
						 indexInfo);

			state->tups_inserted += 1;
		}
	}

	table_endscan(scan);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}


/* ------------------------------------------------------------------------
 * Definition of the heap table access method.
 * ------------------------------------------------------------------------
 */

static const TableAmRoutine heapam_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = heapam_slot_callbacks,

	.scan_begin = heap_beginscan,
	.scan_end = heap_endscan,
	.scan_rescan = heap_rescan,
	.scan_getnextslot = heap_getnextslot,

	.parallelscan_estimate = table_block_parallelscan_estimate,
	.parallelscan_initialize = table_block_parallelscan_initialize,
	.parallelscan_reinitialize = table_block_parallelscan_reinitialize,

	.index_fetch_begin = heapam_index_fetch_begin,
	.index_fetch_reset = heapam_index_fetch_reset,
	.index_fetch_end = heapam_index_fetch_end,
	.index_fetch_tuple = heapam_index_fetch_tuple,

	.tuple_insert = heapam_tuple_insert,
	.tuple_insert_speculative = heapam_tuple_insert_speculative,
	.tuple_complete_speculative = heapam_tuple_complete_speculative,
	.tuple_delete = heapam_tuple_delete,
	.tuple_update = heapam_tuple_update,
	.tuple_lock = heapam_tuple_lock,

	.tuple_fetch_row_version = heapam_fetch_row_version,
	.tuple_get_latest_tid = heap_get_latest_tid,
	.tuple_satisfies_snapshot = heapam_tuple_satisfies_snapshot,
	.compute_xid_horizon_for_tuples = heap_compute_xid_horizon_for_tuples,

	.index_build_range_scan = heapam_index_build_range_scan,
	.index_validate_scan = heapam_index_validate_scan,
};


const TableAmRoutine *
GetHeapamTableAmRoutine(void)
{
	return &heapam_methods;
}

Datum
heap_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&heapam_methods);
}
