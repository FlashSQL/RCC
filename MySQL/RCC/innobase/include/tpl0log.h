
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <cassert>
#include <iostream>
#include <queue>
#include <time.h>
#include <vector>
#include <time.h>
#include <sys/time.h>

#include "buf0buf.h"
#include "dyn0buf.h"
#include "trx0trx.h"
#include "row0mysql.h"

static inline void clflush(volatile char* __p) {
  asm volatile("clflush %0" : "+m" (*__p));
}
static inline void mfence() {
  asm volatile("mfence":::"memory");
  return;
}
static inline void flush_cache(void *ptr, size_t size){
  unsigned int  i=0;
  uint64_t addr = (uint64_t)ptr;

  mfence();
  for (i =0; i < size; i=i+ut::INNODB_CACHE_LINE_SIZE) {
    clflush((volatile char*)addr);
    addr += ut::INNODB_CACHE_LINE_SIZE;
  }
  mfence();
}

/* space (4) | page_no (4) | First_Dynamic_index (4) | length (4) | LSN (8) | Normalize Flag(1) | mtr_log | mtr_log | ... */
#define IPL_LOG_HEADER_SIZE 29UL

#define STATIC_IPL_PAGE_SIZE 256

#define APPLY_LOG_HDR_SIZE 11


typedef struct APPLY_LOG_INFO
{
  byte * static_start_pointer;
  uint ipl_log_length;
  buf_block_t * block;

}apply_log_info;

//Log Capture Function
void capture_redo_log_to_tpl(unsigned char *log, page_id_t page_id, mlog_id_t type, ulint len, in_memory_ppl_buf_t * in_memory_ppl_buf, trx_t* m_trx);

buf_block_t * tpl_get_block(byte * start_ptr);

dberr_t tpl_apply(byte * start_ptr, byte * end_ptr, trx_t * trx);
// void tpl_apply_or_move(byte * start_ptr, byte * end_ptr, trx_t * trx);
// void tpl_install(trx_t* trx);

struct tpl_apply_t{
	trx_t * trx;
  dberr_t error = DB_SUCCESS;
	void init(trx_t * trx){
		this->trx = trx;
	}

	bool operator()(const mtr_buf_t::block_t* block)
	{
    // [TPL] - async commit
    error = tpl_apply((byte *)(block->begin()), (byte *)(block->end()), this->trx);
		// tpl_apply_or_move((byte *)(block->begin()), (byte *)(block->end()), this->trx); // async
    // end async
    return error == DB_SUCCESS;
	}
};


dberr_t
tpl_apply_trx(trx_t* trx);

void
tpl_disable_log(byte* log_start);
