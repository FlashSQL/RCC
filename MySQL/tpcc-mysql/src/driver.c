/*
 * driver.c
 * driver for the tpcc transactions
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/times.h>
#include <time.h>
#include "tpc.h"      /* prototypes for misc. functions */
#include "trans_if.h" /* prototypes for transacation interface calls */
#include "sequence.h"
#include "rthist.h"
#include "sb_percentile.h"

static int other_ware (int home_ware);
static int do_neword (int t_num);
static int do_payment (int t_num);
static int do_ordstat (int t_num);
static int do_delivery (int t_num);
static int do_slev (int t_num);

extern int num_ware;
extern int num_conn;
extern int activate_transaction;
extern int counting_on;

extern int num_node;
extern int time_count;
extern FILE *freport_file;

extern int success[];
extern int late[];
extern int retry[];
extern int failure[];

extern int random_retry[];

extern int* success2[];
extern int* late2[];
extern int* retry2[];
extern int* failure2[];
extern int* random_retry2[];

extern double max_rt[];
extern double total_rt[];
extern double commit_rt[];
extern double waste_rt[];
extern double* commit_rt2[];
extern double* waste_rt2[];

extern int rt_limit[];

extern long clk_tck;
extern sb_percentile_t local_percentile;
extern sb_percentile_t payment_percentile;

extern int    backoff_on;
extern double backoff_base_us;
extern double backoff_cap_us;
extern double backoff_factor;
extern int    backoff_jitter;
extern double backoff_spin_us;

/*
 * abort 후 재시도 직전에 호출한다.
 * attempt 는 0-base (방금 attempt+1 번째 시도가 abort된 상태),
 * 대기시간 = min(base * factor^attempt, cap) 이고 jitter 가 켜져 있으면
 * [0, 그 값) 에서 균등 추출한다 (full jitter).
 * 대기시간은 트랜잭션 응답시간(rt) 측정 구간 밖이므로 rt 통계에는 잡히지 않는다.
 */
static void retry_backoff (int t_num, int attempt)
{
    static __thread unsigned int backoff_seed = 0;
    double wait_us;
    struct timespec ts;
    int k;

    if(!backoff_on)
	return;

    wait_us = backoff_base_us;
    for (k = 0; k < attempt && wait_us < backoff_cap_us; k++)
	wait_us *= backoff_factor;
    if(wait_us > backoff_cap_us)
	wait_us = backoff_cap_us;

    if(backoff_jitter){
	if(backoff_seed == 0)
	    backoff_seed = (unsigned int)(time(NULL) ^ ((unsigned int)t_num * 2654435761u) ^ 1u);
	wait_us *= (double)rand_r(&backoff_seed) / ((double)RAND_MAX + 1.0);
    }

    if(wait_us <= 0.0)
	return;

    if(wait_us <= backoff_spin_us){
	/* 짧은 대기는 nanosleep 의 타이머 slack(수십 us) 이 더 커서 스핀이 정확하다 */
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	do {
	    clock_gettime(CLOCK_MONOTONIC, &t1);
	} while ((t1.tv_sec - t0.tv_sec) * 1000000.0
		 + (t1.tv_nsec - t0.tv_nsec) / 1000.0 < wait_us);
	return;
    }

    ts.tv_sec  = (time_t)(wait_us / 1000000.0);
    ts.tv_nsec = (long)((wait_us - (double)ts.tv_sec * 1000000.0) * 1000.0);
    nanosleep(&ts, NULL);
}

// #define MAX_RETRY 2000
#define MAX_RETRY 1000000000 //10억

int driver (int t_num)
{
    int i, j;
    

    /* Actually, WaitTimes are needed... */
    while( activate_transaction ){
      switch(seq_get()){
      case 0:
	do_neword(t_num);
	break;
      case 1:
	do_payment(t_num);
	break;
      case 2:
	do_ordstat(t_num);
	break;
      case 3:
	do_delivery(t_num);
	break;
      case 4:
	do_slev(t_num);
	break;
      default:
	printf("Error - Unknown sequence.\n");
      }

    }

    return(0);

}

/*
 * prepare data and execute the new order transaction for one order
 * officially, this is supposed to be simulated terminal I/O
 */
static int do_neword (int t_num)
{
    int c_num;
    int i,ret;
    clock_t clk1,clk2;
    double rt;
    struct timespec tbuf1;
    struct timespec tbuf2;
    int  w_id, d_id, c_id, ol_cnt;
    int  all_local = 1;
    /* -I must match the restored dataset so this item does not exist. */
    int  notfound = max_items+1;
    int rbk;
    int  itemid[MAX_NUM_ITEMS];
    int  supware[MAX_NUM_ITEMS];
    int  qty[MAX_NUM_ITEMS];

    if(num_node==0){
	w_id = RandomNumber(1, num_ware);
    }else{
	c_num = ((num_node * t_num)/num_conn); /* drop moduls */
	w_id = RandomNumber(1 + (num_ware * c_num)/num_node,
			    (num_ware * (c_num + 1))/num_node);
    }
    d_id = RandomNumber(1, DIST_PER_WARE);
    c_id = NURand(1023, 1, CUST_PER_DIST);

    ol_cnt = RandomNumber(5, 15);
    rbk = RandomNumber(1, 100);

    for (i = 0; i < ol_cnt; i++) {
	itemid[i] = NURand(8191, 1, max_items);
	if ((i == ol_cnt - 1) && (rbk == 1)) {
	    itemid[i] = notfound;
	}
	if (RandomNumber(1, 100) != 1) {
	    supware[i] = w_id;
	}
	else {
	    supware[i] = other_ware(w_id);
	    all_local = 0;
	}
	qty[i] = RandomNumber(1, 10);
    }

    for (i = 0; i < MAX_RETRY; i++) {
      clk1 = clock_gettime(CLOCK_MONOTONIC, &tbuf1 );
      ret = neword(t_num, w_id, d_id, c_id, ol_cnt, all_local, itemid, supware, qty);
      clk2 = clock_gettime(CLOCK_MONOTONIC, &tbuf2 );

      rt = (double)(tbuf2.tv_sec * 1000.0 + tbuf2.tv_nsec/1000000.0-tbuf1.tv_sec * 1000.0 - tbuf1.tv_nsec/1000000.0);

      if(ret == 1){

	if(counting_on){
	  //printf("NOT : %.3f\n", rt);
	  if (freport_file != NULL) {
	    fprintf(freport_file,"%d %.3f\n", time_count, rt);
	  }

	  if(rt > max_rt[0])
	    max_rt[0]=rt;
	  total_rt[0] += rt;
	  commit_rt[0] += rt;
	  commit_rt2[0][t_num] += rt;
	  sb_percentile_update(&local_percentile, rt);
	  hist_inc(0, rt);
	  if( rt < rt_limit[0]){
	    success[0]++;
	    success2[0][t_num]++;
	  }else{
	    late[0]++;
	    late2[0][t_num]++;
	  }
	}

	return (1); /* end */
      }else{

	if(counting_on){
	  total_rt[0] += rt;
	  waste_rt[0] += rt;
	  waste_rt2[0][t_num] += rt;
	  retry[0]++;
	  retry2[0][t_num]++;
	  if(ret == 2){ /* injected (random) abort */
	    random_retry[0]++;
	    random_retry2[0][t_num]++;
	  }
	}

	retry_backoff(t_num, i);

      }
    }


    if(counting_on){
      retry[0]--;
      retry2[0][t_num]--;
      failure[0]++;
      failure2[0][t_num]++;
    }

    return (0);
}

/*
 * produce the id of a valid warehouse other than home_ware
 * (assuming there is one)
 */
static int other_ware (int home_ware)
{
    int tmp;

    if (num_ware == 1) return home_ware;
    while ((tmp = RandomNumber(1, num_ware)) == home_ware);
    return tmp;
}

/*
 * prepare data and execute payment transaction
 */
static int do_payment (int t_num)
{
    int c_num;
    int byname,i,ret;
    clock_t clk1,clk2;
    double rt;
    struct timespec tbuf1;
    struct timespec tbuf2;
    int  w_id, d_id, c_w_id, c_d_id, c_id, h_amount;
    char c_last[17];

    if(num_node==0){
	w_id = RandomNumber(1, num_ware);
    }else{
	c_num = ((num_node * t_num)/num_conn); /* drop moduls */
	w_id = RandomNumber(1 + (num_ware * c_num)/num_node,
			    (num_ware * (c_num + 1))/num_node);
    }
    d_id = RandomNumber(1, DIST_PER_WARE);
    c_id = NURand(1023, 1, CUST_PER_DIST); 
    Lastname(NURand(255,0,999), c_last); 
    h_amount = RandomNumber(1,5000);
    if (RandomNumber(1, 100) <= 60) {
        byname = 1; /* select by last name */
    }else{
        byname = 0; /* select by customer id */
    }
    if (RandomNumber(1, 100) <= 85) {
        c_w_id = w_id;
        c_d_id = d_id;
    }else{
        c_w_id = other_ware(w_id);
        c_d_id = RandomNumber(1, DIST_PER_WARE);
    }

    for (i = 0; i < MAX_RETRY; i++) {
      clk1 = clock_gettime(CLOCK_MONOTONIC, &tbuf1 );
      ret = payment(t_num, w_id, d_id, byname, c_w_id, c_d_id, c_id, c_last, h_amount);
      clk2 = clock_gettime(CLOCK_MONOTONIC, &tbuf2 );

      rt = (double)(tbuf2.tv_sec * 1000.0 + tbuf2.tv_nsec/1000000.0-tbuf1.tv_sec * 1000.0 - tbuf1.tv_nsec/1000000.0);

      if(ret == 1){

	if(counting_on){
	  if(rt > max_rt[1])
	    max_rt[1]=rt;
	  total_rt[1] += rt;
	  commit_rt[1] += rt;
	  commit_rt2[1][t_num] += rt;
	  sb_percentile_update(&payment_percentile, rt);
	  hist_inc(1, rt);
	  if( rt < rt_limit[1]){
	    success[1]++;
	    success2[1][t_num]++;
	  }else{
	    late[1]++;
	    late2[1][t_num]++;
	  }
	}

	return (1); /* end */
      }else{

	if(counting_on){
	  total_rt[1] += rt;
	  waste_rt[1] += rt;
	  waste_rt2[1][t_num] += rt;
	  retry[1]++;
	  retry2[1][t_num]++;
	  if(ret == 2){ /* injected (random) abort */
	    random_retry[1]++;
	    random_retry2[1][t_num]++;
	  }
	}

	retry_backoff(t_num, i);

      }
    }


    if(counting_on){
      retry[1]--;
      retry2[1][t_num]--;
      failure[1]++;
      failure2[1][t_num]++;
    }

    return (0);
}

/*
 * prepare data and execute order status transaction
 */
static int do_ordstat (int t_num)
{
    int c_num;
    int byname,i,ret;
    clock_t clk1,clk2;
    double rt;
    struct timespec tbuf1;
    struct timespec tbuf2;
    int  w_id, d_id, c_id;
    char c_last[16];

    if(num_node==0){
	w_id = RandomNumber(1, num_ware);
    }else{
	c_num = ((num_node * t_num)/num_conn); /* drop moduls */
	w_id = RandomNumber(1 + (num_ware * c_num)/num_node,
			    (num_ware * (c_num + 1))/num_node);
    }
    d_id = RandomNumber(1, DIST_PER_WARE);
    c_id = NURand(1023, 1, CUST_PER_DIST); 
    Lastname(NURand(255,0,999), c_last); 
    if (RandomNumber(1, 100) <= 60) {
        byname = 1; /* select by last name */
    }else{
        byname = 0; /* select by customer id */
    }

    for (i = 0; i < MAX_RETRY; i++) {
      clk1 = clock_gettime(CLOCK_MONOTONIC, &tbuf1 );
      ret = ordstat(t_num, w_id, d_id, byname, c_id, c_last);
      clk2 = clock_gettime(CLOCK_MONOTONIC, &tbuf2 );

      rt = (double)(tbuf2.tv_sec * 1000.0 + tbuf2.tv_nsec/1000000.0-tbuf1.tv_sec * 1000.0 - tbuf1.tv_nsec/1000000.0);

      if(ret){

	if(counting_on){
	  if(rt > max_rt[2])
	    max_rt[2]=rt;
	  total_rt[2] += rt;
	  commit_rt[2] += rt;
	  commit_rt2[2][t_num] += rt;
	  hist_inc(2, rt);
	  if( rt < rt_limit[2]){
	    success[2]++;
	    success2[2][t_num]++;
	  }else{
	    late[2]++;
	    late2[2][t_num]++;
	  }
	}

	return (1); /* end */
      }else{

	if(counting_on){
	  total_rt[2] += rt;
	  waste_rt[2] += rt;
	  waste_rt2[2][t_num] += rt;
	  retry[2]++;
	  retry2[2][t_num]++;
	}

	retry_backoff(t_num, i);

      }
    }

    if(counting_on){
      retry[2]--;
      retry2[2][t_num]--;
      failure[2]++;
      failure2[2][t_num]++;
    }

    return (0);

}

/*
 * execute delivery transaction
 */
static int do_delivery (int t_num)
{
    int c_num;
    int i,ret;
    clock_t clk1,clk2;
    double rt;
    struct timespec tbuf1;
    struct timespec tbuf2;
    int  w_id, o_carrier_id;

    if(num_node==0){
	w_id = RandomNumber(1, num_ware);
    }else{
	c_num = ((num_node * t_num)/num_conn); /* drop moduls */
	w_id = RandomNumber(1 + (num_ware * c_num)/num_node,
			    (num_ware * (c_num + 1))/num_node);
    }
    o_carrier_id = RandomNumber(1, 10);

    for (i = 0; i < MAX_RETRY; i++) {
      clk1 = clock_gettime(CLOCK_MONOTONIC, &tbuf1 );
      ret = delivery(t_num, w_id, o_carrier_id);
      clk2 = clock_gettime(CLOCK_MONOTONIC, &tbuf2 );

      rt = (double)(tbuf2.tv_sec * 1000.0 + tbuf2.tv_nsec/1000000.0-tbuf1.tv_sec * 1000.0 - tbuf1.tv_nsec/1000000.0);

      if(ret){

	if(counting_on){
	  if(rt > max_rt[3])
	    max_rt[3]=rt;
	  total_rt[3] += rt;
	  commit_rt[3] += rt;
	  commit_rt2[3][t_num] += rt;
	  hist_inc(3, rt );
	  if( rt < rt_limit[3]){
	    success[3]++;
	    success2[3][t_num]++;
	  }else{
	    late[3]++;
	    late2[3][t_num]++;
	  }
	}

	return (1); /* end */
      }else{

	if(counting_on){
	  total_rt[3] += rt;
	  waste_rt[3] += rt;
	  waste_rt2[3][t_num] += rt;
	  retry[3]++;
	  retry2[3][t_num]++;
	}

	retry_backoff(t_num, i);

      }
    }

    if(counting_on){
      retry[3]--;
      retry2[3][t_num]--;
      failure[3]++;
      failure2[3][t_num]++;
    }

    return (0);

}

/* 
 * prepare data and execute the stock level transaction
 */
static int do_slev (int t_num)
{
    int c_num;
    int i,ret;
    clock_t clk1,clk2;
    double rt;
    struct timespec tbuf1;
    struct timespec tbuf2;
    int  w_id, d_id, level;

    if(num_node==0){
	w_id = RandomNumber(1, num_ware);
    }else{
	c_num = ((num_node * t_num)/num_conn); /* drop moduls */
	w_id = RandomNumber(1 + (num_ware * c_num)/num_node,
			    (num_ware * (c_num + 1))/num_node);
    }
    d_id = RandomNumber(1, DIST_PER_WARE); 
    level = RandomNumber(10, 20); 

    for (i = 0; i < MAX_RETRY; i++) {
      clk1 = clock_gettime(CLOCK_MONOTONIC, &tbuf1 );
      ret = slev(t_num, w_id, d_id, level);
      clk2 = clock_gettime(CLOCK_MONOTONIC, &tbuf2 );

      rt = (double)(tbuf2.tv_sec * 1000.0 + tbuf2.tv_nsec/1000000.0-tbuf1.tv_sec * 1000.0 - tbuf1.tv_nsec/1000000.0);

      if(ret){

	if(counting_on){
	  if(rt > max_rt[4])
	    max_rt[4]=rt;
	  total_rt[4] += rt;
	  commit_rt[4] += rt;
	  commit_rt2[4][t_num] += rt;
	  hist_inc(4, rt );
	  if( rt < rt_limit[4]){
	    success[4]++;
	    success2[4][t_num]++;
	  }else{
	    late[4]++;
	    late2[4][t_num]++;
	  }
	}

	return (1); /* end */
      }else{

	if(counting_on){
	  total_rt[4] += rt;
	  waste_rt[4] += rt;
	  waste_rt2[4][t_num] += rt;
	  retry[4]++;
	  retry2[4][t_num]++;
	}

	retry_backoff(t_num, i);

      }
    }

    if(counting_on){
      retry[4]--;
      retry2[4][t_num]--;
      failure[4]++;
      failure2[4][t_num]++;
    }

    return (0);

}
