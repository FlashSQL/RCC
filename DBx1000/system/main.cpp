#include "rcc_srdet.h"
#include "global.h"
#include "ycsb.h"
#include "tpcc.h"
#if WORKLOAD == TPCC && TPCC_CONSISTENCY
#include "tpcc_check.h"
#endif
#include "test.h"
#include "thread.h"
#include "manager.h"
#include "mem_alloc.h"
#include "query.h"
#include "plock.h"
#include "occ.h"
#include "vll.h"

void * f(void *);

thread_t ** m_thds;

// defined in parser.cpp
void parser(int argc, char * argv[]);

#if CC_ALG == RCC && RCC_ADD
// [RCC async] dedicated detector thread: runs rounds continuously until halted
#if RCC_ADD_THREADS > 1
// Separate cache lines for each detector's private mutable state.
struct alignas(64) RccDetectorContext { DL_detect detector; };
static RccDetectorContext rcc_additional_detectors[RCC_ADD_THREADS - 1];
#endif
static DL_detect * rcc_detectors[RCC_ADD_THREADS] = {
    &dl_detector
};
static void * rcc_detector_f(void * arg) {
    const int id = (int)(uintptr_t)arg;
    const char * list = getenv("RCC_DETECT_CPUS");
    const char * single = getenv("RCC_DETECT_CPU");
    int cpu = 56 + id; // current 32-worker benchmark: SMT siblings of worker CPUs 8/9
    if (list) {
        const char * item = list;
        for (int n = 0; n <= id; ++n) {
            char * end = nullptr;
            long value = strtol(item, &end, 10);
            if (end == item || value < 0 || value >= CPU_SETSIZE ||
                (n < id && *end != ',') || (n == id && *end != ',' && *end != '\0')) {
                fprintf(stderr, "Invalid RCC_DETECT_CPUS for detector %d\n", id);
                exit(EXIT_FAILURE);
            }
            cpu = (int)value;
            item = end + (*end == ',');
        }
    } else if (single) {
        cpu = atoi(single) + id; // legacy base CPU override
    }
    if (cpu < 0 || cpu >= CPU_SETSIZE) exit(EXIT_FAILURE);
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(cpu, &cs);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    if (rc) {
        fprintf(stderr, "RCC detector %d affinity CPU%d failed: %d\n", id, cpu, rc);
        exit(EXIT_FAILURE);
    }
    printf("[rcc_detector_affinity] id=%d requested_cpu=%d actual_cpu=%d\n", id, cpu, sched_getcpu());
#if RCC_ERW_ON
    // [ERW] detector thread 0 is the serialization detector; it replaces the pred-edge deadlock rounds
    if (id == 0) rcc_srdet.loop();
    else printf("[rcc_srdet_detector_unused] id=%d cpu=%d\n", id, cpu);
    return nullptr;
#endif
	rcc_detectors[id]->rcc_detector_loop();
	return NULL;
}
#endif

int main(int argc, char* argv[])
{
	parser(argc, argv);
	
	mem_allocator.init(g_part_cnt, MEM_SIZE / g_part_cnt); 
	stats.init();
	glob_manager = (Manager *) _mm_malloc(sizeof(Manager), 64);
	glob_manager->init();
	if (g_cc_alg == DL_DETECT) 
		dl_detector.init();
#if CC_ALG == RCC
	dl_detector.rcc_init();   // [RCC port] round-based detector
#if RCC_ADD
    for (int i = 1; i < RCC_ADD_THREADS; ++i) {
#if RCC_ADD_THREADS > 1
        rcc_detectors[i] = &rcc_additional_detectors[i - 1].detector;
#endif
        rcc_detectors[i]->rcc_init(&dl_detector, i);
    }
#endif
#endif
#if RCC_ERW_ON
    rcc_srdet.init(g_thread_cnt);
#endif
	printf("mem_allocator initialized!\n");

#if CC_ALG == WOUND_WAIT
	printf("WOUND_WAIT\n");
#elif CC_ALG == NO_WAIT
	printf("NO_WAIT\n");
#elif CC_ALG == WAIT_DIE
	printf("WAIT_DIE\n");
#elif CC_ALG == BAMBOO
	printf("BAMBOO\n");
#elif CC_ALG == SILO
    printf("SILO\n");
#elif CC_ALG == TICTOC
	printf("TICTOC\n");
#elif CC_ALG == IC3
	printf("IC3\n");
#elif CC_ALG == RCC
	printf("RCC\n");
#endif
	
 
	workload * m_wl;
	switch (WORKLOAD) {
		case YCSB :
			m_wl = new ycsb_wl; break;
		case TPCC :
			m_wl = new tpcc_wl; break;
		case TEST :
			m_wl = new TestWorkload; 
			((TestWorkload *)m_wl)->tick();
			break;
		default:
			assert(false);
	}
	m_wl->init();
	printf("workload initialized!\n");
	
	uint64_t thd_cnt = g_thread_cnt;
	pthread_t p_thds[thd_cnt - 1];
	m_thds = new thread_t * [thd_cnt];
	for (uint32_t i = 0; i < thd_cnt; i++)
		m_thds[i] = (thread_t *) _mm_malloc(sizeof(thread_t), 64);
	// query_queue should be the last one to be initialized!!!
	// because it collects txn latency
	query_queue = (Query_queue *) _mm_malloc(sizeof(Query_queue), 64);
	if (WORKLOAD != TEST)
		query_queue->init(m_wl);
	pthread_barrier_init( &warmup_bar, NULL, g_thread_cnt );
	printf("query_queue initialized!\n");
#if CC_ALG == HSTORE
	part_lock_man.init();
#elif CC_ALG == OCC
	occ_man.init();
#elif CC_ALG == VLL
	vll_man.init();
#endif

	for (uint32_t i = 0; i < thd_cnt; i++) 
		m_thds[i]->init(i, m_wl);

#if CC_ALG == RCC && RCC_ADD
	pthread_t rcc_detect_thd[RCC_ADD_THREADS];
    for (int i = 0; i < RCC_ADD_THREADS; ++i) {
        int rc = pthread_create(&rcc_detect_thd[i], NULL, rcc_detector_f, (void *)(uintptr_t)i);
        if (rc) {
            fprintf(stderr, "RCC detector %d creation failed: %d\n", i, rc);
            exit(EXIT_FAILURE);
        }
    }
#endif
#if BB_PF_PW
	bb_pw_prepare(thd_cnt);   // the trace buffers, faulted in before any measured work
#endif
	if (WARMUP > 0){
		printf("WARMUP start!\n");
		for (uint32_t i = 0; i < thd_cnt - 1; i++) {
			uint64_t vid = i;
			pthread_create(&p_thds[i], NULL, f, (void *)vid);
		}
		f((void *)(thd_cnt - 1));
		for (uint32_t i = 0; i < thd_cnt - 1; i++)
			pthread_join(p_thds[i], NULL);
		printf("WARMUP finished!\n");
	}
	warmup_finish = true;
	pthread_barrier_init( &warmup_bar, NULL, g_thread_cnt );
#ifndef NOGRAPHITE
	CarbonBarrierInit(&enable_barrier, g_thread_cnt);
#endif
	pthread_barrier_init( &warmup_bar, NULL, g_thread_cnt );

	// spawn and run txns again.
	int64_t starttime = get_server_clock();
	for (uint32_t i = 0; i < thd_cnt - 1; i++) {
		uint64_t vid = i;
		pthread_create(&p_thds[i], NULL, f, (void *)vid);
	}
	f((void *)(thd_cnt - 1));
	for (uint32_t i = 0; i < thd_cnt - 1; i++) 
		pthread_join(p_thds[i], NULL);
#if CC_ALG == RCC && RCC_ADD
#if RCC_ERW_ON
    rcc_srdet.request_stop();
#endif
    for (int i = 0; i < RCC_ADD_THREADS; ++i) rcc_detectors[i]->rcc_detector_halt();
    for (int i = 0; i < RCC_ADD_THREADS; ++i) pthread_join(rcc_detect_thd[i], NULL);
    for (int i = 0; i < RCC_ADD_THREADS; ++i) rcc_detectors[i]->rcc_detector_collect();
#endif
	int64_t endtime = get_server_clock();
#if RCC_ERW_ON
    rcc_srdet.print();
#endif
#if CC_ALG == RCC && RCC_ERW_PROBE
    { extern void rcc_probe_print(); rcc_probe_print(); }
#endif
	
#if WORKLOAD == TPCC && TPCC_CONSISTENCY
	tpcc_consistency_check((tpcc_wl *) m_wl);
#endif
	if (WORKLOAD != TEST) {
		printf("PASS! SimTime = %ld\n", endtime - starttime);
#if BB_PF_CWX
		bb_cwx_analyze();
#endif
#if BB_PF_PW
		bb_pw_analyze();
#endif
		if (STATS_ENABLE)
			stats.print();
	} else {
		((TestWorkload *)m_wl)->summarize();
	}
	return 0;
}

void * f(void * id) {
	uint64_t tid = (uint64_t)id;
	m_thds[tid]->run();
	return NULL;
}
