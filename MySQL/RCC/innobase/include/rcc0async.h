#ifndef RCC0ASYNC_H
#define RCC0ASYNC_H
#include "univ.i"
#ifdef ASYNC
#include "rcc0async_core.h"
#include "trx0types.h"
#include "dict0types.h"
#include "db0err.h"
uint64_t rcc_profile_value(size_t index);
std::string rcc_wait_sample();
void rcc_profile_phase(trx_t *, unsigned);
void rcc_profile_retired();
struct conflict_t;
bool rcc_table(const dict_index_t *index);
rec_t *rcc_read(trx_t *, dict_index_t *, const rec_t *, const ulint *, conflict_t *);
dberr_t rcc_write(trx_t *, dict_index_t *, const rec_t *, const rec_t *, const ulint *, void (*)(void *), void *);
dberr_t rcc_certify(trx_t *);
void rcc_abort(trx_t *);
void rcc_note_engine_abort(trx_t *, rcc::AbortCause);
void rcc_finish(trx_t *, bool committed);
void rcc_reset(trx_t *);
void rcc_mark_engine_abort(rcc::WaitKey key);
void rcc_srd_notify();
void rcc_srd_detect();
std::vector<rcc::WaitKey> rcc_take_aborts();
#endif
#endif
