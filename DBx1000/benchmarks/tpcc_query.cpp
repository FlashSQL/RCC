#include "query.h"
#include "tpcc_query.h"
#include "tpcc.h"
#include "tpcc_helper.h"
#include "mem_alloc.h"
#include "wl.h"
#include "table.h"

/**
 * Generate a tpcc_query.
 * The mix is controlled by PERC_* (g_perc_*) in the config.
 * The full mix (NewOrder 45 / Payment 43 / OrderStatus 4 / Delivery 4 / StockLevel 4) is
 * PERC_PAYMENT=0.43 PERC_DELIVERY=0.04 PERC_ORDERSTATUS=0.04 PERC_STOCKLEVEL=0.04.
 */
void tpcc_query::init(uint64_t thd_id, workload * h_wl) {
  double x = (double)(rand() % 100) / 100.0;
  part_to_access = (uint64_t *)
      mem_allocator.alloc(sizeof(uint64_t) * g_part_cnt, thd_id);
  if (x < g_perc_payment)
    gen_payment(thd_id);
  else if (x < (g_perc_payment + g_perc_delivery))
    gen_delivery(thd_id);
  else if (x < (g_perc_payment + g_perc_delivery + g_perc_orderstatus))
    gen_order_status(thd_id);
  else if (x < (g_perc_payment + g_perc_delivery + g_perc_orderstatus +
      g_perc_stocklevel))
    gen_stock_level(thd_id);
  else
    gen_new_order(thd_id);
}

void tpcc_query::gen_payment(uint64_t thd_id) {
  type = TPCC_PAYMENT;
  tpcc_query_payment& arg = args.payment;

  if (FIRST_PART_LOCAL)
    arg.w_id = thd_id % g_num_wh + 1;
  else
    arg.w_id = URand(1, g_num_wh, thd_id % g_num_wh);

  uint64_t part_id = wh_to_part(arg.w_id);
  part_to_access[0] = part_id;
  part_num = 1;

  arg.d_id = URand(1, DIST_PER_WARE, arg.w_id-1);
  arg.h_amount = URand(1, 5000, arg.w_id-1);
  int x = URand(1, 100, arg.w_id-1);            // access home/remote warehouse
  int y = URand(1, 100, arg.w_id-1);            // select customer by last_name/cust_id

  if(x <= 85) {
    // home warehouse
    arg.c_d_id = arg.d_id;
    arg.c_w_id = arg.w_id;
  } else {
    // remote warehouse
    arg.c_d_id = URand(1, DIST_PER_WARE, arg.w_id-1);
    if(g_num_wh > 1) {
      while((arg.c_w_id = URand(1, g_num_wh, arg.w_id-1)) == arg.w_id) {}
      if (wh_to_part(arg.w_id) != wh_to_part(arg.c_w_id)) {
        part_to_access[1] = wh_to_part(arg.c_w_id);
        part_num = 2;
      }
    } else
      arg.c_w_id = arg.w_id;
  }
  if(y <= 60) {
    // by last name
    arg.by_last_name = true;
    Lastname(NURand(255,0,999,arg.w_id-1), arg.c_last);
  } else {
    // by cust id
    arg.by_last_name = false;
    arg.c_id = NURand(1023, 1, g_cust_per_dist, arg.w_id-1);
  }
}

void tpcc_query::gen_new_order(uint64_t thd_id) {
  type = TPCC_NEW_ORDER;
  tpcc_query_new_order& arg = args.new_order;

  if (FIRST_PART_LOCAL)
    arg.w_id = thd_id % g_num_wh + 1;
  else
    arg.w_id = URand(1, g_num_wh, thd_id % g_num_wh);

  arg.d_id = URand(1, DIST_PER_WARE, arg.w_id-1);
  arg.c_id = NURand(1023, 1, g_cust_per_dist, arg.w_id-1);
  arg.rbk = URand(1, 100, arg.w_id-1);
  arg.ol_cnt = URand(5, 15, arg.w_id-1);
  arg.o_entry_d = 2013;
  arg.items = (Item_no *) _mm_malloc(sizeof(Item_no) * arg.ol_cnt, 64);
  part_to_access[0] = wh_to_part(arg.w_id);
  part_num = 1;

  for (UInt32 oid = 0; oid < arg.ol_cnt; oid ++) {
    arg.items[oid].ol_i_id = NURand(8191, 1, g_max_items, arg.w_id-1);
#if TPCC_USER_ABORT
    // XXX(zhihan): 1% of the New-Order transactions are chosen at random to
    // simulate user data entry errors and exercise the performance of
    // rolling back update transactions.
    // [uniform] not a BAMBOO(/RCC)-only guard: for a fair comparison across CCs,
    // every algorithm gets the same user aborts.
    if ((oid == arg.ol_cnt - 1) && (arg.rbk == 1)) {
      arg.items[oid].ol_i_id = 0;
    }
#endif
    UInt32 x = URand(1, 100, arg.w_id-1);
    if (x > 1 || g_num_wh == 1)
      arg.items[oid].ol_supply_w_id = arg.w_id;
    else  {
      while((arg.items[oid].ol_supply_w_id = URand(1, g_num_wh, arg.w_id-1))
          == arg.w_id) {}
    }
    arg.items[oid].ol_quantity = URand(1, 10, arg.w_id-1);
  }
  // Remove duplicate items
  for (UInt32 i = 0; i < arg.ol_cnt; i ++) {
    for (UInt32 j = 0; j < i; j++) {
      if (arg.items[i].ol_i_id == arg.items[j].ol_i_id) {
        for (UInt32 k = i; k < arg.ol_cnt - 1; k++)
          arg.items[k] = arg.items[k + 1];
        arg.ol_cnt --;
        i--;
      }
    }
  }
  for (UInt32 i = 0; i < arg.ol_cnt; i ++)
    for (UInt32 j = 0; j < i; j++)
      assert(arg.items[i].ol_i_id != arg.items[j].ol_i_id);
  // Sort by (supply_w_id, i_id) ascending, as tpcc-mysql neword.c does ("sort orders to avoid
  // DeadLock"). Invalid items (id 0) go last. Enabled to match the access order of the MySQL RCC experiments.
#if 1
  for (UInt32 i = 1; i < arg.ol_cnt; i ++) {
    Item_no tmp = arg.items[i];
    uint64_t tmp_key = (tmp.ol_i_id == 0) ? (uint64_t)-1
        : tmp.ol_supply_w_id * (g_max_items + 1) + tmp.ol_i_id;
    UInt32 j = i;
    for (; j > 0; j --) {
      uint64_t prev_key = (arg.items[j-1].ol_i_id == 0) ? (uint64_t)-1
          : arg.items[j-1].ol_supply_w_id * (g_max_items + 1) + arg.items[j-1].ol_i_id;
      if (prev_key <= tmp_key)
        break;
      arg.items[j] = arg.items[j-1];
    }
    arg.items[j] = tmp;
  }
#endif
  // update part_to_access
  for (UInt32 i = 0; i < arg.ol_cnt; i ++) {
    UInt32 j;
    for (j = 0; j < part_num; j++ )
      if (part_to_access[j] == wh_to_part(arg.items[i].ol_supply_w_id))
        break;
    if (j == part_num) // not found! add to it.
      part_to_access[part_num ++] = wh_to_part( arg.items[i].ol_supply_w_id );
  }
}

void tpcc_query::gen_order_status(uint64_t thd_id) {
  type = TPCC_ORDER_STATUS;
  readonly = true;
  read_committed = false;
  is_long = true;
  tpcc_query_order_status& arg = args.order_status;

  if (FIRST_PART_LOCAL)
    arg.w_id = thd_id % g_num_wh + 1;
  else
    arg.w_id = URand(1, g_num_wh, thd_id % g_num_wh);

  arg.d_id = URand(1, DIST_PER_WARE, thd_id);

  int y = URand(1, 100, thd_id);
  if (y <= 60) {
    // by last name
    arg.by_last_name = true;
    Lastname(NURand(255, 0, 999, thd_id), arg.c_last);
  } else {
    // by cust id
    arg.by_last_name = false;
    arg.c_id = NURand(1023, 1, g_cust_per_dist, thd_id);
  }
}

void tpcc_query::gen_delivery(uint64_t thd_id) {
  type = TPCC_DELIVERY;
  read_committed = false;
  readonly = false;
  tpcc_query_delivery& arg = args.delivery;

  if (FIRST_PART_LOCAL)
    arg.w_id = thd_id % g_num_wh + 1;
  else
    arg.w_id = URand(1, g_num_wh, thd_id % g_num_wh);

  arg.o_carrier_id = URand(1, DIST_PER_WARE, thd_id);
  arg.ol_delivery_d = 2013;
}

void tpcc_query::gen_stock_level(uint64_t thd_id) {
  type = TPCC_STOCK_LEVEL;
  readonly = true;
  read_committed = false;
  is_long = true;
  tpcc_query_stock_level& arg = args.stock_level;

  if (FIRST_PART_LOCAL)
    arg.w_id = thd_id % g_num_wh + 1;
  else
    arg.w_id = URand(1, g_num_wh, thd_id % g_num_wh);

  arg.d_id = URand(1, DIST_PER_WARE, thd_id);
  arg.threshold = URand(10, 20, thd_id);
}
