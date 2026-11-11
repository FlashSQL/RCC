#include "tpl0log.h"
#include "dyn0buf.h"
#include "que0que.h"
#include "trx0rec.h"
#include "trx0undo.h"
#include "lock0lock.h"





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


void capture_redo_log_to_tpl(unsigned char *log, page_id_t page_id, mlog_id_t type, ulint len, in_memory_ppl_buf_t * in_memory_ppl_buf, trx_t* m_trx){
	byte * write_pointer = in_memory_ppl_buf->open(len);

	unsigned char store_type = type;
	unsigned short log_body_size = len - APPLY_LOG_HDR_SIZE;

	mach_write_to_4(write_pointer, page_id.space()); // Table Number
	write_pointer += 4;
	mach_write_to_4(write_pointer, page_id.page_no()); //Page Number
	write_pointer += 4;

	mach_write_to_1(write_pointer, store_type); // mtr_log type
	write_pointer += 1;
	mach_write_to_2(write_pointer, log_body_size); //mtr_log body
	write_pointer += 2;

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
			apply_ptr += body_len;
			continue;
		}


		tpl_apply_mtr.start();
		tpl_apply_mtr.set_m_trx(trx);

		
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
}

void 
tpl_apply_trx(trx_t* trx) 
{
	if(trx->in_memory_ppl_buf.size() != 0) {


		tpl_apply_t apply_t;
		apply_t.init(trx);
		
		trx->in_memory_ppl_buf.for_each_block(apply_t);

		//Step 3. TPL Erase and commit the MTR
		trx->in_memory_ppl_buf.erase();

	}

}

void
tpl_disable_log(byte* log_start) {
	mach_write_to_4(log_start, 0);
}
