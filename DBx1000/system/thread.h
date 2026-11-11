#pragma once

#include "global.h"

class workload;
class base_query;
class txn_man;

class thread_t {
public:
    uint64_t _thd_id;
    // attempts started by this thread so far, across the warmup and the measured phase: attempt ids keep increasing
    // (ERW's detector treats an id below one it has seen end as an attempt that already left)
    uint64_t _txn_seq;
    workload * _wl;

    uint64_t 	get_thd_id();

    uint64_t 	get_host_cid();
    void 	 	set_host_cid(uint64_t cid);

    uint64_t 	get_cur_cid();
    void 		set_cur_cid(uint64_t cid);

    void 		init(uint64_t thd_id, workload * workload);
    // the following function must be in the form void* (*)(void*)
    // to run with pthread.
    // conversion is done within the function.
    RC 			run();

    // moved from private to global for clv
    ts_t 		get_next_ts();
    ts_t 		get_next_n_ts(int n);

    std::vector<std::string> *wound_entry;
    void   insert_wound(std::string entry){
        wound_entry->push_back(entry);
    }
    bool   insert_hotspots(uint64_t hots){
        auto ret = hotspots->insert(hots);
        if (ret.second){
            return true;
        }else{
            return false;
        }
    }

private:
    uint64_t 	_host_cid;
    uint64_t 	_cur_cid;
    ts_t 		_curr_ts;

    RC	 		runTest(txn_man * txn);
    drand48_data buffer;

    // added for wound wait
    base_query * curr_query;
    ts_t         starttime;

    std::set<uint64_t> *hotspots;


    // A restart buffer for aborted txns.
    struct AbortBufferEntry	{
        ts_t ready_time;
        base_query * query;
        ts_t starttime;
    };
    AbortBufferEntry * _abort_buffer;
    size_t _abort_buffer_size;
    size_t _abort_buffer_empty_slots;
    bool _abort_buffer_enable;
    uint64_t _abort_buffer_grows, _abort_buffer_peak, _abort_buffer_full_sleeps;
    uint64_t _abort_buffer_retries, _abort_buffer_new_while_pending;
    void report_abort_buffer() const;
};
