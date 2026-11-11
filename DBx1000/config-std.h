#ifndef _CONFIG_H_
#define _CONFIG_H_

/***********************************************/
// Simulation + Hardware
/***********************************************/
#define TERMINATE_BY_COUNT         true
#define THREAD_CNT					20
#define PART_CNT					1
// each transaction only accesses 1 virtual partition. But the lock/ts manager and index are not aware of such partitioning. VIRTUAL_PART_CNT describes the request distribution and is only used to generate queries. For HSTORE, VIRTUAL_PART_CNT should be the same as PART_CNT.
#define VIRTUAL_PART_CNT			1
#define PAGE_SIZE					4096
#define CL_SIZE						64
// CPU_FREQ is used to get accurate timing info
#define CPU_FREQ 					2.25 // in GHz/s; TSC rate of the 128-core server (dmesg | grep tsc: 2250.001 MHz)

// # of transactions to run for warmup
#define WARMUP						1000
// YCSB or TPCC
#define WORKLOAD                    YCSB
// print the transaction latency distribution
#define PRT_LAT_DISTR				false
#define STATS_ENABLE				true
#define TIME_ENABLE					true

#define MEM_ALLIGN					8

// [THREAD_ALLOC]
#define THREAD_ALLOC				false
#define THREAD_ARENA_SIZE			(1UL << 22)
#define MEM_PAD 					true

// [PART_ALLOC]
#define PART_ALLOC 					false
#define MEM_SIZE					(1UL << 30)
#define NO_FREE						false

/***********************************************/
// Concurrency Control
/***********************************************/
// WAIT_DIE, NO_WAIT, DL_DETECT, TIMESTAMP, MVCC, HEKATON, HSTORE, OCC, VLL, TICTOC, SILO
// TODO TIMESTAMP does not work at this moment
#define CC_ALG                      RCC
#define ISOLATION_LEVEL 			SERIALIZABLE

// latch options
#define LATCH					    LH_SPINLOCK

// all transactions acquire tuples according to the primary key order.
#define KEY_ORDER					false
#define YCSB_GLOBAL_HOTKEY          false   // [global-hotkey] every YCSB txn writes the hottest key (row_id 1); with KEY_ORDER it is the first access (TPC-C WH-like)
#define YCSB_SORT_PREFIX            0       // [prefix-sort] sort only the first N requests of a YCSB txn by key (KEY_ORDER=false); 0 = off
// transaction roll back changes after abort
#define ROLL_BACK					true
// per-row lock/ts management or central lock/ts management
#define CENTRAL_MAN					false
#define BUCKET_CNT					31
#define ABORT_PENALTY 				10000
#define ABORT_BUFFER_SIZE			1
#define ABORT_BUFFER_UNBOUNDED true
#define ABORT_BUFFER_ENABLE			true
// [ INDEX ]
#define ENABLE_LATCH				false
#define CENTRAL_INDEX				false
#define CENTRAL_MANAGER 			false
#define INDEX_STRUCT				IDX_HASH
#define BTREE_ORDER 				16

// [DL_DETECT]
#define DL_LOOP_DETECT				1000 	// 100 us
#define DL_LOOP_TRIAL				100	// 1 us
#define NO_DL						KEY_ORDER
#define TIMEOUT						1000000 // 1ms
// [TIMESTAMP]
#define TS_TWR						false
#define TS_ALLOC					TS_CAS
#define TS_BATCH_ALLOC				false
#define TS_BATCH_NUM				1
// [MVCC]
// when read/write history is longer than HIS_RECYCLE_LEN
// the history should be recycled.
//#define HIS_RECYCLE_LEN		    10
//#define MAX_PRE_REQ				1024
//#define MAX_READ_REQ				1024
#define MIN_TS_INTVL				5000000 //5 ms. In nanoseconds
// [OCC]
#define MAX_WRITE_SET				10
#define PER_ROW_VALID				true
// [TICTOC]
#define WRITE_COPY_FORM				"data" // ptr or data
#define TICTOC_MV					false
#define WR_VALIDATION_SEPARATE		true
#define WRITE_PERMISSION_LOCK		false
#define ATOMIC_TIMESTAMP			"false"
// [TICTOC, SILO]
#define VALIDATION_LOCK				"no-wait" // no-wait or waiting
#define PRE_ABORT					"true"
#define ATOMIC_WORD					true
// [HSTORE]
// when set to true, hstore will not access the global timestamp.
// This is fine for single partition transactions.
#define HSTORE_LOCAL_TS				false
// [VLL]
#define TXN_QUEUE_SIZE_LIMIT		THREAD_CNT
// [BAMBOO]
#define BB_DYNAMIC_TS					true
#define BB_OPT_RAW                  true
#define BB_LAST_RETIRE                 0
#define BB_PRECOMMIT                false
#define BB_AUTORETIRE               false
#define BB_ALWAYS_RETIRE_READ       true
// [BAMBOO] split the prevention work done under the row latch in lock_get (commit-only, like time_get_cs):
// time_bb_ts / bb_ts_cnt = dynamic timestamp assignments, time_bb_wound / bb_wound_cnt = wound checks and wounds,
// bb_get_cnt = lock_get calls (per-acquisition averages of time_get_latch / time_get_cs). Off: no timer in the latch.
#define BB_PF_PREVENT               false
// [BAMBOO] experiment only: spin this many ns in every lock_get right after taking the row latch (latch held), to
// lengthen the latch hold artificially and see how lock wait and commit wait follow. 0 = off.
#define BB_PF_DELAY_NS              0
// [BAMBOO] commit wait by queue position: entries ahead of a transaction in its retired lists when its commit wait
// starts (0..31, 32 = 32 or more), commit wait of committed attempts summed per position (bb_cwpos_t<i>, bb_cwpos_n<i>)
#define BB_PF_CWPOS                 false
// [BAMBOO] transactions ahead, commit-only: bb_lw_ahead = owner + waiters ahead when a lock request has to wait
// (bb_lw_cnt waits), bb_cw_ahead = predecessors left when the commit wait starts (bb_cw_cnt commit waits).
// time_wait / bb_lw_ahead and time_commit / bb_cw_ahead = wait per transaction ahead.
#define BB_PF_AHEAD                 false
// [BAMBOO] split a committed transaction's commit wait by what the predecessor that released it last was doing:
// time_cws_exec (still executing), time_cws_cascade (in its own commit wait), time_cws_release (committed, returning its
// rows until it released me), time_cws_notice (released -> my commit), time_cws_other (timestamps out of order: races).
// bb_cws_cnt = commit waits split (commit-only). time_cwf_* = the same, flattened over the whole chain of predecessors
// that fall in my commit wait; bb_cwf_depth = chain links in it, x1000.
#define BB_PF_CWSPLIT               false
// [BAMBOO] exact split of commit waits along the chain of releasing predecessors (system/stats.h): logged in memory
// during the run, walked after it; time_cwx_* / bb_cwx_* in the summary.
#define BB_PF_CWX                   false
// [BAMBOO] with BB_PF_CWX: split the row-latch waits of lock_release by what the latch holder was doing (row_bamboo.h
// BB_PH_*): time_cwx_rlh<k> on the commit-wait chain, time_rlh<k> over every release of a committed attempt.
#define BB_PF_HOLDER                false
// [BAMBOO] how much of a committed transaction's lock wait and commit wait was caused by deadlock prevention
// (system/stats.h): time_pw_* in the summary. Not together with BB_PF_HOLDER.
#define BB_PF_PW                    false
// [BAMBOO] with BB_PF_PW: also log the latch-wait pieces behind non-prevention holders (only needed to split the
// non-prevention waiting into release work / latch / wake-up / other). false = minimal logging.
#define BB_PF_PW_FULL               false
// [BB_PF_PW] attempts (all workers) the trace buffers are reserved and pre-faulted for before the run
#define BB_PF_PW_ATTEMPTS           12000000
// [WW]
#define WW_STARV_FREE               false // set false if compared w/ bamboo
// [IC3]
#define IC3_EAGER_EXEC              true 
#define IC3_RENDEZVOUS              true
#define IC3_FIELD_LOCKING           false // should not be true
#define IC3_MODIFIED_TPCC           false

/***********************************************/
// Logging
/***********************************************/
#define LOG_COMMAND					false
#define LOG_REDO					false
#define LOG_BATCH_TIME				10 // in ms

/***********************************************/
// Benchmark
/***********************************************/
#define INSERT_ENABLED              false
#define THINKTIME				    0
#define MAX_RUNTIME                 30 // in s, used only if !TERMINATE_BY_TIME
// max number of rows touched per transaction
#define MAX_ROW_PER_TXN				64
#define QUERY_INTVL 				1UL
#define MAX_TXN_PER_PART 			10000
#define FIRST_PART_LOCAL 			true
#define MAX_TUPLE_SIZE				1024 // in bytes
#define MAX_FIELD_SIZE                          50
// ==== [YCSB] ====
#define INIT_PARALLELISM			40
#define SYNTH_TABLE_SIZE 			(1024 * 5)
#define ZIPF_THETA 					0.9
#define READ_PERC 					1
#define WRITE_PERC 					1  // if want no scan, write + read >= 1
#define SCAN_PERC 					0
#define SCAN_LEN					20
#define PART_PER_TXN 				1
#define PERC_MULTI_PART				1
#define REQ_PER_QUERY				16
#define LONG_TXN_RATIO			        0
#define LONG_TXN_READ_RATIO			0.5
#define FIELD_PER_TUPLE				10
// ==== [YCSB-synthetic] ====
#define SYNTHETIC_YCSB              true
#define POS_HS                      TOP
#define SPECIFIED_RATIO             0
#define FLIP_RATIO                  0
#define NUM_HS                      1
#define FIRST_HS                    WR
#define SECOND_HS                   WR
#define FIXED_HS                    0
// ==== [TPCC] ====
// For large warehouse count, the tables do not fit in memory
// small tpcc schemas shrink the table size.
#define TPCC_SMALL					false
// Some of the transactions read the data but never use them.
// If TPCC_ACCESS_ALL == fales, then these parts of the transactions
// are not modeled.
#define TPCC_ACCESS_ALL 			false
#define WH_UPDATE					true
#define NUM_WH 						1
//
enum TPCCTxnType {
  TPCC_PAYMENT,
  TPCC_NEW_ORDER,
  TPCC_DELIVERY,
  TPCC_ORDER_STATUS,
  TPCC_STOCK_LEVEL,
  TPCC_ALL};
extern TPCCTxnType 					g_tpcc_txn_type;

//#define TXN_TYPE					TPCC_ALL
#define PERC_PAYMENT 				0.5
#define PERC_DELIVERY               0
#define PERC_ORDERSTATUS            0
#define PERC_STOCKLEVEL             0
// PERC_NEWORDER is (1 - the sum of above).
#define FIRSTNAME_MINLEN 			8
#define FIRSTNAME_LEN 				16
#define LASTNAME_LEN 				16

#define DIST_PER_WARE				10
#define TPCC_MAX_ITEMS				100000   // [item-sweep] ITEM/STOCK rows per warehouse and the New-Order item range (spec: 100000)
// enable user-initiated aborts in new-order txn according to TPC-C doc.
#define TPCC_USER_ABORT             true
// After the run, verify the TPC-C consistency conditions (spec 3.3.2, same checks as
// MySQL/scripts/consistency_check.sql) directly on the in-memory tables. Test builds only.
#define TPCC_CONSISTENCY            false

// Optimizations used in IC3
#define COMMUTATIVE_OPS          false

/***********************************************/
// TODO centralized CC management.
/***********************************************/
#define MAX_LOCK_CNT				(20 * THREAD_CNT)
#define TSTAB_SIZE                  50 * THREAD_CNT
#define TSTAB_FREE                  TSTAB_SIZE
#define TSREQ_FREE                  4 * TSTAB_FREE
#define MVHIS_FREE                  4 * TSTAB_FREE
#define SPIN                        false

/***********************************************/
// Test cases
/***********************************************/
#define TEST_ALL					true
enum TestCases {
  READ_WRITE,
  CONFLICT
};
extern TestCases					g_test_case;

/***********************************************/
// DEBUG info
/***********************************************/
#define WL_VERB						true
#define IDX_VERB					false
#define VERB_ALLOC					true
#define DEBUG_LOCK					false
#define DEBUG_TIMESTAMP					false
#define DEBUG_SYNTH					false
#define DEBUG_ASSERT                			false
#define DEBUG_CC					false
#define DEBUG_WW                    			false
#define DEBUG_BENCHMARK             			false
#define DEBUG_BAMBOO                			false
#define DEBUG_TMP					false

/***********************************************/
// PROFILING 
/***********************************************/
#define PF_BASIC					false
#define PF_CS          					false // profiling inside critical path
#define PF_ABORT_LENGTH          			false
#define PF_MODEL          false

/***********************************************/
// Constant
/***********************************************/
// INDEX_STRUCT
#define IDX_HASH 					1
#define IDX_BTREE					2
// WORKLOAD
#define YCSB						1
#define TPCC						2
#define TEST						3
// latch options
#define LH_SPINLOCK                   1
#define LH_MUTEX                      2
#define LH_MCSLOCK                    3
// Concurrency Control Algorithm
#define NO_WAIT						1
#define WAIT_DIE					2
#define DL_DETECT					3
#define TIMESTAMP					4
#define MVCC						5
#define HSTORE						6
#define OCC							7
#define TICTOC						8
#define SILO						9
#define VLL							10
#define HEKATON 					11
#define WOUND_WAIT                  12
#define BAMBOO                      13
#define IC3                         14
//Isolation Levels
#define SERIALIZABLE				1
#define SNAPSHOT					2
#define REPEATABLE_READ				3
// TIMESTAMP allocation method.
#define TS_MUTEX					1
#define TS_CAS						2
#define TS_HW						3
#define TS_CLOCK					4
// Synthetic YCSB - HOTSPOT POSITION
#define TOP                         1
#define MID                         2
#define BOT                         3
#define TM                          4
#define MB                          5
#define SPECIFIED                   6

#endif
#define MAX_THREAD 256
#define NEXT_TS 0
#define SORTOPT 0
#define CHILDOPT 0   // paper O1 (authors: CHILDOPT). Off as in the published code: this per-worker slot version is not latch-free and measured slower (YCSB 64: 231.0K vs 253.5K)
#define PREFETCH 0
#define READ_ONLY_OPTIMIZATION_ENABLE 1
#define BB_TRACK_DEPENDENS 0
#define READ_HOTSPOT_RATIO 0.5
#define RANDOM_READ_SYNTHETIC_YCSB 0
#define TS_LOCAL 5

// ==== [RCC port from DBx1000-Bamboo] ====
#define RCC                         16
#define RCC_DETECT_INTERVAL         5000       // 5 us
#define RCC_LOCK_TIMEOUT            100000000  // 100 ms
#define RCC_COMMIT_TIMEOUT          100000000  // 100 ms
#define RCC_ADD RCC_ODH   // ADD (asynchronous deadlock detector threads) is part of ODH: on / off with RCC_ODH
// ERW (concurrency_control/rcc_srdet.h): a transaction commits once the serialization detector approved it and its
// write predecessors (wr / ww) committed; it no longer waits for read predecessors (rw). Detector thread 0 runs the
// serialization detector. RCC_BATCH builds on it. false: the previous RCC (commit waits for every predecessor on
// every row, pred-edge deadlock detector).
#define RCC_ERW true
// [ablation] RCC_ODH false: RCC without optimistic deadlock handling (ODH) -- deadlocks are PREVENTED as in Bamboo: wound-wait,
// timestamps assigned at the first conflict under the row latch (scan of retired / owner, wound of younger ones, waiters
// in timestamp order). A wound only marks the victim aborted; its own row cleanup removes its entries (as for any RCC
// abort). Needs RCC_ERW false (RCC_ADD is off with RCC_ODH): no detector at all (the lock / commit timeouts stay as a
// backstop and must stay 0). Ablation: 1 = LUI (RCC_ODH false, RCC_ERW false), 2 = LUI + ODH (RCC_ERW false), 3 = all (RCC).
#define RCC_ODH true
// [ablation] RCC_LUI false: no lazy update installation (LUI) -- Bamboo's in-place writes: a writer saves the before-image when
// it gets the row, writes its data into the row at its retire (under the latch), and an abort restores the before-image
// under the latch and cascades to everything after it on the row; readers read the row (dirty data included). No private
// versions (no version copy at the grant, no install at the commit of a retired writer). Needs RCC_ERW false.
// Ablation order: Bamboo -> RCC_LUI false (+ optimistic deadlock handling) -> RCC_ERW false (+ lightweight rollback) -> RCC.
#define RCC_LUI true
// ERW: when a transaction sends its edges to the (asynchronous) detector.
//   0 right after each row grant (a writer then still owns the row: the send lengthens the owner window of hot rows)
//   1 early but outside the owner window: a read's edges right after the grant, a write's right after its retire
//   2 all at once when it asks for approval
#define RCC_ERW_EDGE_SEND 1
#define RCC_ADD_THREADS 2
// Dedicated detector: idle time between rounds in ns (0 = back-to-back rounds, the old behaviour).
#define RCC_DETECT_PAUSE_NS 0
// [cw-stat] position/progress statistics taken right before commit wait (Bamboo and RCC paths in txn_man::finish).
// They walk the row's lock list through other workers' entries without the latch; keep off for measurements.
#define CW_POS_STAT false
// Pin worker i to CPU_MAP[i] (true). false = Bamboo-Public behaviour: set_affinity() does nothing.
#define PIN_THREADS true
// Experimental event scheduling disabled; use continuous async scans.
#define RCC_DETECT_EVENT false
#define RCC_DETECT_EVENT_RETRY_NS 1000000UL
#define RCC_DEBUG_STUCK             false
#define BB_FAKE_DETECT              false
#define OP_DELAY                    0
#define ROLLBACK_DELAY              0
#define DEBUG_HEARTBEAT             false
// RCC publishes worker-owned entry slots; the detector derives edges from pred.
// Version separation: retire retains the private write buffer. A dependent copies
// the producer's version under the row latch; an independent reader uses base.
// Commit installs the private version; abort discards it and cascades to consumers.
// RCC_MEASURE and batch/install switches below belong to the archived graph port.
// This restored row implementation does not use those optional paths.
#define RCC_MEASURE                 false
// RCC uses Bamboo barriers, private speculative versions, and cleanup installation.
// Install versions before unlinking entries and releasing successor barriers.
#define RCC_INSTALL_CLEANUP true
// RCC_BATCH (presence switch, rebuild after changing it) = ERW + dep push + asynchronous batch release:
//   dep push       a committing transaction decrements its write dependents' barriers itself (no row latch); each
//                  dependent then commits by itself as in ERW (approved and barrier 0)
//   batch release  a committed transaction only marks its queued entries committed (sweeping the row itself if its
//                  latch is free) and starts its next transaction with Access objects from a pool; whoever holds a
//                  row latch next removes the committed entries from the front up to the first writer not marked yet,
//                  installs the newest of their versions once, and returns the Access objects to their owners
// #define RCC_BATCH

#define RCC_SNAP_RETRY 4

#define RCC_PRED_CACHE_PROBE false
// [measurement only] per-table row critical-section timing (lock_get / release / retire and the ERW parts inside them),
// printed as [rcc_probe] lines at the end. Off for real runs.
#define RCC_ERW_PROBE false

#define RCC_COMMIT_DETECT_INTERVAL 5000       // 5 us

#define RCC_COMMIT_STUCK_WARN 1000000000 // 1 s

// Experimental: committed row install/release shared with commit-wait workers.
// No row-count threshold; abort cleanup remains on its owner.
#define RCC_PARALLEL_CLEANUP false


// Cascade consumers use one attempt-ID slot per worker; no 16-entry cap.

#define RCC_TRIGGER_DETECT false

#define RCC_PERSIST_NS 0

#define RCC_PERSIST_ROUNDS 1
