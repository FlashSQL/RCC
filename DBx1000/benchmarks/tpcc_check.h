#pragma once
// TPC-C consistency checks run after the benchmark on the in-memory tables (TPCC_CONSISTENCY=true).
// Mirrors MySQL/scripts/consistency_check.sql (tests 1-6 and the district/new-order
// checks) plus balance checks against the amounts that actually committed.
#include "global.h"
class tpcc_wl;
class tpcc_query;
void tpcc_check_commit(uint64_t thd_id, tpcc_query * q);   // called by the worker on every RCOK
void tpcc_consistency_check(tpcc_wl * wl);                  // called once after all workers joined
