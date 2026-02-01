#include "tpl0log.h"
#include "dyn0buf.h"
#include "que0que.h"
#include "trx0rec.h"
#include "trx0undo.h"
#include "lock0lock.h"

// /** Iterate over a memo block in reverse. */
// template <typename Functor>
// struct Iterate {

// 	/** Release specific object */
// 	explicit Iterate(Functor& functor)
// 		:
// 		m_functor(functor)
// 	{
// 		/* Do nothing */
// 	}

// 	/** @return block if the functor returns block. */
// 	bool operator()(in_memory_ppl_buf_t::block_t* block)
// 	{
// 		if (!m_functor(block)) {
// 			return(false);
// 		}
// 		return(true);
// 	}

// 	Functor&	m_functor;
// };

/** Find specific block */
struct FindTPL {

	/** Constructor */
	FindTPL(ulint space, ulint page_no)
		:
		t_log_start(),
		t_space(space),
		t_page_no(page_no){}

	/** @return false if the object was found. */
	bool operator()(in_memory_ppl_buf_t::block_t* block)
	{
        byte* start_ptr = (byte *)(block->begin());
        byte* end_ptr = (byte *)(block->end());
		byte* apply_ptr = start_ptr;
		byte* log_start;

        ulint space_id, page_no, body_len;

		while (apply_ptr < end_ptr) {
			log_start = apply_ptr;
			space_id = mach_read_from_4(apply_ptr); apply_ptr += 4;
			page_no = mach_read_from_4(apply_ptr); apply_ptr += 4;
			apply_ptr += 1; // log_type
			body_len = mach_read_from_2(apply_ptr); apply_ptr += 2;

			if (t_space == space_id && t_page_no == page_no) {
				// 찾으면 바로 중단
				// fprintf(stderr, "[DEBUG] FindTPL: %p (%d %d) vs (%d %d)\n", start_ptr, t_space, t_page_no, space_id, page_no);
				t_log_start = log_start;
				return(false);
			}
			apply_ptr += body_len;
		}
		return(true);
	}


	/** log if found */
	byte* t_log_start;

	/** space  to look for */
	ulint		t_space;

	/** page_no to look for */
	ulint	t_page_no;
};


//추후 mini transaction log 객체를 활용해서 최적화 가능성 있음
//현재는 mtr내 log에 작성하고 copy를 진행하지만, 충돌난 transaction의 경우 TPL log space에 로그를 저장하게 구현할 것.
void capture_redo_log_to_tpl(unsigned char *log, page_id_t page_id, mlog_id_t type, ulint len, in_memory_ppl_buf_t * in_memory_ppl_buf, trx_t* m_trx){
	byte * write_pointer = in_memory_ppl_buf->open(len);
	// fprintf(stderr, "[DEBUG] (%d) capture_redo_log_to_tpl: %p\n", m_trx->id, write_pointer);

	unsigned char store_type = type;
	unsigned short log_body_size = len - APPLY_LOG_HDR_SIZE;

	//Step1. Page ID 작성
	mach_write_to_4(write_pointer, page_id.space()); // Table Number
	write_pointer += 4;
	mach_write_to_4(write_pointer, page_id.page_no()); //Page Number
	write_pointer += 4;

	//Step2. Apply log header 작성
	mach_write_to_1(write_pointer, store_type); // mtr_log type
	write_pointer += 1;
	mach_write_to_2(write_pointer, log_body_size); //mtr_log body
	write_pointer += 2;

	//Step2. Apply log body 작성
	memcpy(write_pointer, log, log_body_size);
	write_pointer += log_body_size;
	in_memory_ppl_buf->close(write_pointer);
	
}

buf_block_t * tpl_get_block(byte * start_ptr){
	ulint space, page_no;
	space = mach_read_from_4(start_ptr);
	if (space == 0) {
		return NULL;
	}
		
	start_ptr += 4;
	page_no = mach_read_from_4(start_ptr);
	const page_id_t	page_id(space, page_no);
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	buf_page_t * buf_page = buf_page_hash_get(buf_pool, page_id);
	return ((buf_block_t *) buf_page);
}

// // [TPL] - group commit
// // Install future_vers to Physical page
// // Group commit: LSN=T1's
// void tpl_install(trx_t* trx) {
// 	mtr_t tpl_apply_mtr;

// 	buf_block_t *block;
// 	page_t *page = nullptr;
// 	page_zip_des_t *page_zip = nullptr;
// 	ulint rec_offset;
// 	rec_t* rec;
// 	mem_heap_t *heap;
// 	heap = mem_heap_create(256, UT_LOCATION_HERE);
// 	ulint *offsets;
// 	ulint extra_len;
// 	ulint data_len;

// 	trx_t* next_trx;
	
// 	trx_t* cur_trx;
// 	rec_t* apply_vers = nullptr;
// 	byte* apply_log = nullptr;
	
// 	space_id_t space_id;
//     page_no_t page_no;  
// 	ulint body_len;
// 	mlog_id_t log_type;
// 	byte * apply_ptr;
// 	lock_t* conf_lock;

// 	bool flag = false;
// 	byte*	mlog_start;

// 	for (conflict_t* elem : trx->lock.conf_lock_pool) {
// 		// fprintf(stderr, "[DEBUG] (%d) tpl_install - elem->future_vers: %p\n", trx->id, elem->future_vers);
// 		if (elem && elem->future_log) {
// 			apply_vers = elem->future_vers;
// 			apply_log = elem->future_log;
// 			conf_lock = elem->conf_lock;
// 			// fprintf(stderr, "[DEBUG] (%d) tpl_install - conf_lock: %p\n", trx->id, conf_lock);

// 			future_apply_t fa = trx_last_future_vers(conf_lock);
// 			if (fa.future_vers) {
// 				apply_vers = fa.future_vers;
// 				apply_log = fa.future_log;
// 			}

// 			apply_ptr = apply_log;
// 			// fprintf(stderr, "[DEBUG] (%d) tpl_install - FINAL apply_vers: %p, apply_log: %p\n", trx->id, apply_vers, apply_log);

// 			tpl_apply_mtr.start();
// 			tpl_apply_mtr.set_m_trx(trx);

// 			// Step 1. Log type 읽어오기
// 			space_id = mach_read_from_4(apply_ptr);
// 			apply_ptr += 4;
// 			page_no = mach_read_from_4(apply_ptr);
// 			apply_ptr += 4;
// 			log_type = mlog_id_t(mach_read_from_1(apply_ptr));
// 			apply_ptr += 1;
// 			body_len = mach_read_from_2(apply_ptr);
// 			apply_ptr += 2;
// 			const byte* temp_ptr = recv_install_log_rec_body(log_type, apply_ptr, apply_ptr + body_len, space_id, page_no, &tpl_apply_mtr, apply_vers);

// 			// buf_block_t *block = buf_page_get(elem->page_id, univ_page_size, RW_X_LATCH, UT_LOCATION_HERE, &tpl_apply_mtr);
// 			// page = block->frame;
// 			// page_zip = buf_block_get_page_zip(block);
// 			// rec_offset = elem->rec_offset;
// 			// rec = page + rec_offset;

// 			// offsets = rec_get_offsets(rec, elem->index, nullptr, ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);

// 			// // fprintf(stderr, "[DEBUG] (%d) tpl_install - lock: %p, rec: %p\n", trx->id, elem->conf_lock, rec);
// 			// extra_len = rec_offs_extra_size(offsets);
// 			// data_len = rec_offs_data_size(offsets);
			
// 			// ut_memcpy(rec - extra_len, apply_vers - extra_len, extra_len + data_len);

// 			mtr_set_log_mode(&tpl_apply_mtr, MTR_LOG_NONE);
// 			tpl_apply_mtr.discard_modifications();
// 			tpl_apply_mtr.commit();


// 			// mlog_open(&tpl_apply_mtr, 11 + body_len, mlog_start);
// 			// tpl_apply_mtr.added_rec();
// 			// memcpy(mlog_start, apply_log, 11 + body_len);
// 			// mlog_close(&tpl_apply_mtr, mlog_start + 11 + body_len);
// 			// tpl_apply_mtr.commit();
// 		}
// 		else {
// 			// fprintf(stderr, "[DEBUG] (%d) tpl_install - Skip, lock: %p\n", trx->id, elem->conf_lock);
// 		}
// 	}
// }

void tpl_apply(byte * start_ptr, byte * end_ptr, trx_t * trx){
	byte * apply_ptr = start_ptr;

	byte*		log_start;
	byte*	mlog_start;

	space_id_t space_id;
    page_no_t page_no;  
	ulint body_len;
	mlog_id_t log_type;
	mtr_t		tpl_apply_mtr;

	int cnt = 0;

	while (apply_ptr < end_ptr) {
		// fprintf(stderr, "[DEBUG] (%d) tpl_apply: %p\n", trx->id, apply_ptr);
		// Step 1. Log type 읽어오기
		space_id = mach_read_from_4(apply_ptr);
		apply_ptr += 4;
		page_no = mach_read_from_4(apply_ptr);
		apply_ptr += 4;
		log_type = mlog_id_t(mach_read_from_1(apply_ptr));
		apply_ptr += 1;
		body_len = mach_read_from_2(apply_ptr);
		apply_ptr += 2;
		
		log_start = apply_ptr;

		if(log_type == MLOG_COMP_PAGE_CREATE || space_id == 0){
			// space == NULL: disabled
			// fprintf(stderr, "[DEBUG] (%d) log_type: %d, space: %u\n",trx->id, log_type, space);
			apply_ptr += body_len;
			continue;
		}


		tpl_apply_mtr.start();
		tpl_apply_mtr.set_m_trx(trx);

		// fprintf(stderr, "[DEBUG] (%d) Apply in (%u, %u), log_type: %d, body_len: %u\n", trx->id, space_id, page_no, log_type, body_len);
		
		const byte* temp_ptr = tpl_recv_parse_or_apply_log_rec_body(
									log_type, apply_ptr
									, apply_ptr + body_len, space_id, page_no, &tpl_apply_mtr);
		apply_ptr += body_len;
		tpl_apply_mtr.commit();
		if (temp_ptr){
			cnt++;
		} else {
			break;
		}
		

	} // end-of-while
		// fprintf(stderr, "[DEBUG] (%d) Apply END\n", trx->id);
	// if (cnt > 0) fprintf(stderr, "%d\n", cnt);
}

// // [TPL] - async commit
// // 1. 나의 log buf를 순회하면서
// // 2. 해당 log의 page id, space no을 가지는 락이 있는지 본다 (trx_conf_lock_conf)
// // 3. 그럼 그 blocker에게 push back
// // 4. conf lock 없으면 apply 하면 됨
// void tpl_apply_or_move(byte * start_ptr, byte * end_ptr, trx_t * trx){
// 	byte * apply_ptr = start_ptr;

// 	byte*		log_start;
// 	byte*	mlog_start;

// 	space_id_t space_id;
//     page_no_t page_no;  
// 	ulint body_len;
// 	mlog_id_t log_type;
// 	mtr_t		tpl_apply_mtr;

// 	int cnt = 0;

// 	trx_t* blocker;

// 	while (apply_ptr < end_ptr) {
// 		bool stop = false;
// 		// fprintf(stderr, "[DEBUG] (%d) tpl_apply: %p\n", trx->id, apply_ptr);
// 		log_start = apply_ptr;

// 		// Step 1. Log type 읽어오기
// 		space_id = mach_read_from_4(apply_ptr);
// 		apply_ptr += 4;
// 		page_no = mach_read_from_4(apply_ptr);
// 		apply_ptr += 4;
// 		log_type = mlog_id_t(mach_read_from_1(apply_ptr));
// 		apply_ptr += 1;
// 		body_len = mach_read_from_2(apply_ptr);
// 		apply_ptr += 2;

// 		if(log_type == MLOG_COMP_PAGE_CREATE || space_id == 0){
// 			// space == NULL: disabled
// 			// fprintf(stderr, "[DEBUG] (%d) log_type: %d, space: %u\n",trx->id, log_type, space);
// 			apply_ptr += body_len;
// 			continue;
// 		}

// 		// 이 space_id, page_no을 가지는 락이 있는가?

// 		// COPY to blocker
// 		{
// 			page_id_t		page_id(space_id, page_no);
// 			locksys::Shard_latch_guard guard{UT_LOCATION_HERE, page_id};
// 			blocker = trx_page_lock_conf(trx, page_id);
// 			while(blocker) {
// 				// trx_mutex_enter(blocker);

// 				// fprintf(stderr, "[DEBUG] (%d) tpl_apply_or_move - blocker: %d\n",trx->id, blocker->id);
// 				if (blocker->pre_commit == TRX_COMMIT) {
// 					// T3: T2가 이미 커밋해버림(log 날려버림) -> T1에게 줘야 함
// 					// T2가 아직 pre-commit 중이면(move 중) 그냥 T2에게 append?
// 					blocker = trx_page_lock_conf(blocker, page_id);
// 					// fprintf(stderr, "[DEBUG] (%d) tpl_apply_or_move - NEW blocker\n",trx->id);
// 				} else {
// 					fprintf(stderr, "[DEBUG] (%d) tpl_apply_or_move - %p %d %p\n",trx->id, apply_ptr, body_len + APPLY_LOG_HDR_SIZE, &blocker->in_memory_ppl_buf);
// 					// T1에게 T2의 log 복사
// 					page_id_t	page_id(space_id, page_no);
// 					capture_redo_log_to_tpl(apply_ptr, page_id, log_type, body_len + APPLY_LOG_HDR_SIZE, &blocker->in_memory_ppl_buf, blocker);
// 					apply_ptr += body_len;

// 					// T1에게 T2의 future vers 복사
// 					trx_conf_overwrite_future_vers(blocker, trx, page_id);
// 					stop = true;
// 					// trx_mutex_exit(blocker);
// 					break;
// 				}
// 				// trx_mutex_exit(blocker);
// 			}
// 			if (stop) continue;
// 		}
		

// 		// APPLY
// 		tpl_apply_mtr.start();
// 		tpl_apply_mtr.set_m_trx(trx);

// 		// fprintf(stderr, "[DEBUG] (%d) Apply in (%u, %u), log_type: %d, body_len: %u\n", trx->id, space_id, page_no, log_type, body_len);
// 		// fprintf(stderr, "[DEBUG] (%d) tpl_apply_or_move - Apply (%u, %u)\n", trx->id, page_no, space_id);
// 		const byte* temp_ptr = tpl_recv_parse_or_apply_log_rec_body(
// 									log_type, apply_ptr
// 									, apply_ptr + body_len, space_id, page_no, &tpl_apply_mtr);
// 		if (temp_ptr){
// 			apply_ptr += body_len;

// 			// Copy to redo buffer
// 			// TODO: mtr로 카피하지 말고 바로 global redo buffer로
// 			mlog_open(&tpl_apply_mtr, 11 + body_len, mlog_start);
// 			tpl_apply_mtr.added_rec();
// 			memcpy(mlog_start, log_start, 11 + body_len);
// 			mlog_close(&tpl_apply_mtr, mlog_start + 11 + body_len);
// 			tpl_apply_mtr.commit();
// 			cnt++;
// 		} else {
// 			tpl_apply_mtr.commit();
// 			break;
// 		}

// 	} // end-of-while
// 		// fprintf(stderr, "[DEBUG] (%d) Apply END\n", trx->id);
// 	// if (cnt > 0) fprintf(stderr, "%d\n", cnt);
// } // end async

void 
tpl_apply_trx(trx_t* trx) 
{
	if(trx->in_memory_ppl_buf.size() != 0) {
		// tpl_install(trx); // [TPL] - group commit
		// fprintf(stderr, "[DEBUG] (%d) Log Apply\n", trx->id);
		// Total size of future log
		// fprintf(stderr, "%d\n",  trx->in_memory_ppl_buf.size());


		tpl_apply_t apply_t;
		apply_t.init(trx);
		
		trx->in_memory_ppl_buf.for_each_block(apply_t); // 각 블록에 대해 tpl_apply 실행

		//Step 3. TPL Erase and commit the MTR
		trx->in_memory_ppl_buf.erase();

		// fprintf(stderr, "[DEBUG] (%d) Log Apply END\n", trx->id);
	}

}

void
tpl_disable_log(byte* log_start) {
	mach_write_to_4(log_start, 0);
}
