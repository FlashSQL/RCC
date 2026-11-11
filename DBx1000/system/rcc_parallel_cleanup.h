#pragma once
#include "global.h"
class txn_man;
#if CC_ALG == RCC && RCC_PARALLEL_CLEANUP
// Caller is the sole executor of its worker identity and holds no row latch.
bool rcc_cleanup_help(txn_man *executor);
// Committed owner: publish all accesses, work alongside helpers, join all tasks.
void rcc_cleanup_rows(txn_man *owner);
#endif
