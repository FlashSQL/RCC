// The 5-txn TPC-C (NewOrder/Payment/OrderStatus/Delivery/StockLevel).
// Notes:
//  - the neworder index insert used custKey (now neword_key)
//  - a bool helper called finish(Abort) (now returns false; the caller calls finish)
//  - the TPCC_USER_ABORT check came before ol_i_id was assigned
//  - payment used the home warehouse's part_id for a remote customer access
#include "tpcc.h"
#include "tpcc_query.h"
#include "tpcc_helper.h"
#include "query.h"
#include "wl.h"
#include "thread.h"
#include "table.h"
#include "row.h"
#include "index_hash.h"
#include "index_btree.h"
#include "index_mbtree.h"
#include "tpcc_const.h"


#define RETIRE_ROW(row_cnt) { \
  access_cnt = row_cnt - 1; \
  if (retire_row(access_cnt) == Abort) \
    return finish(Abort); \
}

void tpcc_txn_man::init(thread_t * h_thd, workload * h_wl, uint64_t thd_id) {
    txn_man::init(h_thd, h_wl, thd_id);
    _wl = (tpcc_wl *) h_wl;
}

RC tpcc_txn_man::run_txn(base_query * query) {
    tpcc_query * m_query = (tpcc_query *) query;
#if CC_ALG == IC3
    curr_type = m_query->type;
    curr_piece = 0;
#endif
    switch (m_query->type) {
        case TPCC_PAYMENT :
            return run_payment(m_query); break;
        case TPCC_NEW_ORDER :
            return run_new_order(m_query); break;
        case TPCC_ORDER_STATUS :
            return run_order_status(m_query); break;
        case TPCC_DELIVERY :
            return run_delivery(m_query); break;
        case TPCC_STOCK_LEVEL :
            return run_stock_level(m_query); break;
        default:
            assert(false); return Abort;
    }
}

/**
 * 1. Update WAREHOUSE, DISTRICT, CUSTOMER table.
 * 2. Insert a tuple into HISTORY table.
 */
RC tpcc_txn_man::run_payment(tpcc_query * query) {

    auto& arg = query->args.payment;

#if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT > 1)
    int access_cnt;
#endif
    // declare all variables
    RC rc = RCOK;
    uint64_t key;
    // rows
    row_t * r_cust;
    row_t * r_cust_local;
    row_t * r_hist;
    uint64_t row_id;
    // values
#if !COMMUTATIVE_OPS
    double tmp_value;
#endif
    char w_name[11];
    char * tmp_str;
    char d_name[11];
    double c_balance;
    double c_ytd_payment;
    uint64_t c_payment_cnt;   // schema type is uint64_t (was read/written as double)
    char * c_credit;

    uint64_t w_id = arg.w_id;
    uint64_t c_w_id = arg.c_w_id;
    /*====================================================+
        EXEC SQL UPDATE warehouse SET w_ytd = w_ytd + :h_amount
        WHERE w_id=:w_id;
    +====================================================*/
    /*===================================================================+
        EXEC SQL SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name
        INTO :w_street_1, :w_street_2, :w_city, :w_state, :w_zip, :w_name
        FROM warehouse WHERE w_id=:w_id;
    +===================================================================*/
#if CC_ALG == IC3
    warehouse_piece:
    begin_piece(0);
#endif
    //1. update warehouse
    key = arg.w_id;
    auto index_warehouse = _wl->i_warehouse;
    auto part_id = wh_to_part(w_id);
    row_t * r_wh_local = search(index_warehouse, key, part_id, g_wh_update ? WR : RD);
    if (r_wh_local == NULL) {
        return finish(Abort);
    }

    #if !COMMUTATIVE_OPS
        //update the balance to the warehouse
        r_wh_local->get_value(W_YTD, tmp_value);
        if (g_wh_update) {
            r_wh_local->set_value(W_YTD, tmp_value + arg.h_amount);
        }
    #else
        inc_value(W_YTD, arg.h_amount); // will increment at commit time
    #endif
    if (g_wh_update) {
    #if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT > 1) && !COMMUTATIVE_OPS
        RETIRE_ROW(row_cnt)
    #endif
    }
    //get a copy of warehouse name
    tmp_str = r_wh_local->get_value(W_NAME);
    memcpy(w_name, tmp_str, 10);
    w_name[10] = '\0';
#if CC_ALG == IC3
    if (end_piece(0) != RCOK)
        goto warehouse_piece;
#endif


#if CC_ALG == IC3
    district_piece:
    begin_piece(1);
#endif
    /*====================================================================+
      EXEC SQL SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name
      INTO :d_street_1, :d_street_2, :d_city, :d_state, :d_zip, :d_name
      FROM district
      WHERE d_w_id=:w_id AND d_id=:d_id;
    +====================================================================*/
    /*=====================================================+
        EXEC SQL UPDATE district SET d_ytd = d_ytd + :h_amount
        WHERE d_w_id=:w_id AND d_id=:d_id;
    +=====================================================*/
    //2. update district
    key = distKey(arg.d_id, arg.w_id);
    auto r_dist_local = search(_wl->i_district, key, part_id, WR);
    if (r_dist_local == NULL) {
        return finish(Abort);
    }

#if !COMMUTATIVE_OPS
    r_dist_local->get_value(D_YTD, tmp_value);
    r_dist_local->set_value(D_YTD, tmp_value + arg.h_amount);
#else
    inc_value(D_YTD, arg.h_amount); // will increment at commit time
#endif
#if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT > 1) && !COMMUTATIVE_OPS
    RETIRE_ROW(row_cnt)
#endif
    tmp_str = r_dist_local->get_value(D_NAME);
    memcpy(d_name, tmp_str, 10);
    d_name[10] = '\0';


#if CC_ALG == IC3
    if(end_piece(1) != RCOK)
        goto district_piece;

    customer_piece:
    begin_piece(2);
#endif
    //3. update customer
    // [fix] the customer is in the c_w_id partition (not the home w_id partition)
    auto cust_part_id = wh_to_part(c_w_id);
    if (arg.by_last_name) {
        uint64_t key = custNPKey(arg.c_last, arg.c_d_id, arg.c_w_id);
        auto index = _wl->i_customer_last;
        itemid_t* items[100];
        size_t count = 100;
        auto rc = index_read_multiple(index, key, items, count, cust_part_id);
        if (rc != RCOK || count == 0) {
            return finish(Abort);
        }
        auto mid = items[count / 2];
        r_cust = ((row_t *)mid->location);
        r_cust_local = get_row(r_cust, WR);
    }
    else { // search customers by cust_id
        key = custKey(arg.c_id, arg.c_d_id, arg.c_w_id);
        auto index = _wl->i_customer_id;
        r_cust_local = search(index, key, cust_part_id, WR);
    }
    /*======================================================================+
         EXEC SQL UPDATE customer SET c_balance = :c_balance, c_data = :c_new_data
         WHERE c_w_id = :c_w_id AND c_d_id = :c_d_id AND c_id = :c_id;
     +======================================================================*/
    if (r_cust_local == NULL) {
        return finish(Abort);
    }
    r_cust_local->get_value(C_BALANCE, c_balance);
    r_cust_local->set_value(C_BALANCE, c_balance - arg.h_amount);
    r_cust_local->get_value(C_YTD_PAYMENT, c_ytd_payment);
    r_cust_local->set_value(C_YTD_PAYMENT, c_ytd_payment + arg.h_amount);
    r_cust_local->get_value(C_PAYMENT_CNT, c_payment_cnt);
    r_cust_local->set_value(C_PAYMENT_CNT, c_payment_cnt + 1);

    c_credit = r_cust_local->get_value(C_CREDIT);
    if ( strstr(c_credit, "BC") && !TPCC_SMALL ) {
        /*=====================================================+
            EXEC SQL SELECT c_data
            INTO :c_data
            FROM customer
            WHERE c_w_id=:c_w_id AND c_d_id=:c_d_id AND c_id=:c_id;
        +=====================================================*/
        char c_new_data[501];
        sprintf(c_new_data,"| %zu %zu %zu %zu %zu $%7.2f",
                arg.c_id, arg.c_d_id, c_w_id, arg.d_id, w_id, arg.h_amount);
        r_cust_local->set_value("C_DATA", c_new_data);
    }

    // retire after the last write to customer
#if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT > 1)
    RETIRE_ROW(row_cnt)
#endif


#if CC_ALG == IC3
    if (end_piece(2) != RCOK)
        goto customer_piece;
#endif
    //update h_data according to spec
    char h_data[25];
    strncpy(h_data, w_name, 10);
    int length = strlen(h_data);
    if (length > 10) length = 10;
    strcpy(&h_data[length], "    ");
    strncpy(&h_data[length + 4], d_name, 10);
    h_data[length+14] = '\0';
    /*=============================================================================+
      EXEC SQL INSERT INTO
      history (h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, h_data)
      VALUES (:c_d_id, :c_w_id, :c_id, :d_id, :w_id, :datetime, :h_amount, :h_data);
      +=============================================================================*/
    //not causing the buffer overflow
    _wl->t_history->get_new_row(r_hist, 0, row_id);
    r_hist->set_value(H_C_ID, arg.c_id);
    r_hist->set_value(H_C_D_ID, arg.c_d_id);
    r_hist->set_value(H_C_W_ID, c_w_id);
    r_hist->set_value(H_D_ID, arg.d_id);
    r_hist->set_value(H_W_ID, w_id);
    int64_t date = 2013;
    r_hist->set_value(H_DATE, date);
    r_hist->set_value(H_AMOUNT, arg.h_amount);
#if !TPCC_SMALL
    r_hist->set_value(H_DATA, h_data);
#endif
    // XXX(zhihan): no index maintained for history table.

    assert( rc == RCOK );
    return finish(rc);
}


/**
 * 1. Read WAREHOUSE, CUSTOMER table. Update DISTRICT table.
 * 2. Insert a tuple in NEW_ORDER, ORDER table.
 * 3. Read ol_cnt tuples in ITEM table; update STOCK; insert ORDER-LINE.
 */
RC tpcc_txn_man::run_new_order(tpcc_query * query) {
    auto& arg = query->args.new_order;

    RC rc = RCOK;
    uint64_t key;
    itemid_t * item;
    uint64_t w_id = arg.w_id;
    uint64_t d_id = arg.d_id;
    uint64_t c_id = arg.c_id;
    uint64_t ol_cnt = arg.ol_cnt;
    uint64_t part_id = wh_to_part(w_id);

    // declare vars
    double w_tax;
    row_t * r_cust_local;
    row_t * r_dist_local;
    double d_tax;
    int64_t o_id;
    uint64_t row_id;
    row_t * r_order;
    row_t * r_no;
    // order
    int sum = 0;
    uint64_t ol_i_id;
    uint64_t ol_supply_w_id;
    uint64_t ol_quantity;
    row_t * r_item;
    row_t * r_item_local;
    row_t * r_stock;
    row_t * r_stock_local;
    int64_t i_price;
    uint64_t stock_key;
    index_base * stock_index;
    itemid_t * stock_item;
    UInt64 s_quantity;
    int64_t s_remote_cnt;
    int64_t s_ytd;
    int64_t s_order_cnt;
    uint64_t quantity;
    int64_t ol_amount;
    row_t * r_ol;
#if IC3_MODIFIED_TPCC
    double tmp_value;
#endif

    /*=======================================================================+
    EXEC SQL SELECT c_discount, c_last, c_credit, w_tax
        INTO :c_discount, :c_last, :c_credit, :w_tax
        FROM customer, warehouse
        WHERE w_id = :w_id AND c_w_id = w_id AND c_d_id = :d_id AND c_id = :c_id;
    +========================================================================*/

#if CC_ALG == IC3
    warehouse_piece: // 0
    begin_piece(0);
#endif
    // 1. search warehouse
    key = w_id;
    row_t * r_wh_local = search(_wl->i_warehouse, key, part_id, RD);
    if (r_wh_local == NULL) {
        return finish(Abort);
    }
    //retrieve the tax of warehouse
    r_wh_local->get_value(W_TAX, w_tax);

#if IC3_MODIFIED_TPCC
    r_wh_local->get_value(W_YTD, tmp_value);
#endif
#if CC_ALG == IC3
    if (end_piece(0) != RCOK)
        goto warehouse_piece;

    district_piece:
    begin_piece(1);
#endif
    /*==================================================+
    EXEC SQL SELECT d_next_o_id, d_tax
        INTO :d_next_o_id, :d_tax
        FROM district WHERE d_id = :d_id AND d_w_id = :w_id;
    EXEC SQL UPDATE district SET d_next_o_id = :d_next_o_id + 1
        WHERE d_id = :d_id AND d_w_id = :w_id;
    +===================================================*/
    //2. update district
    key = distKey(d_id, w_id);
    r_dist_local = search(_wl->i_district, key, part_id, WR);
    if (r_dist_local == NULL) {
        return finish(Abort);
    }

    r_dist_local->get_value(D_TAX, d_tax);
    r_dist_local->get_value(D_NEXT_O_ID, o_id);

    // TPC-C 2.4.2.3 / tpcc-mysql neword.c: the new order takes o_id = D_NEXT_O_ID and the district
    // advances to D_NEXT_O_ID + 1. (Taking the incremented value as the o_id would leave order 3001
    // missing and the newest order at D_NEXT_O_ID: consistency tests T3/T4 would fail.)
    { int64_t next = o_id + 1; r_dist_local->set_value(D_NEXT_O_ID, next); }

#if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT != 1)
    if (retire_row(row_cnt-1) == Abort){
        return finish(Abort);
    }
#endif

#if CC_ALG == IC3
    if (end_piece(1) != RCOK)
        goto district_piece;

    customer_piece:
    begin_piece(2);
#endif
    //3. search customer
    key = custKey(c_id, d_id, w_id);
    r_cust_local = search(_wl->i_customer_id, key, part_id, RD);
    if (r_cust_local == NULL) {
        return finish(Abort);
    }
    //retrieve data
    uint64_t c_discount;
    if(!TPCC_SMALL) {
        r_cust_local->get_value(C_LAST);
        r_cust_local->get_value(C_CREDIT);
    }
    r_cust_local->get_value(C_DISCOUNT, c_discount);

#if CC_ALG == IC3
    if (end_piece(2) != RCOK)
        goto customer_piece;

    neworder_piece: // 3
    begin_piece(3);
#endif
    /*=======================================================+
    EXEC SQL INSERT INTO NEW_ORDER (no_o_id, no_d_id, no_w_id)
        VALUES (:o_id, :d_id, :w_id);
    +=======================================================*/
    //4. insert neworder
    _wl->t_neworder->get_new_row(r_no, 0, row_id);
    uint64_t neword_key = neworderKey(o_id, d_id, w_id);
    r_no->set_primary_key(neword_key);
    r_no->set_value(NO_O_ID, o_id);
    r_no->set_value(NO_D_ID, d_id);
    r_no->set_value(NO_W_ID, w_id);
    insert_row(r_no, _wl->t_neworder);
    // [fix] the key is neword_key (not custKey)
    if (!insert_idx(_wl->i_neworder, neword_key, r_no, part_id)) {
        return finish(Abort);
    }
#if CC_ALG == IC3
    if (end_piece(3) != RCOK)
        goto neworder_piece;

    order_piece: // 4
    begin_piece(4);
#endif
    /*========================================================================================+
    EXEC SQL INSERT INTO ORDERS (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local)
        VALUES (:o_id, :d_id, :w_id, :c_id, :datetime, :o_ol_cnt, :o_all_local);
    +========================================================================================*/
    //5. insert order
    _wl->t_order->get_new_row(r_order, 0, row_id);
    uint64_t ord_key = orderKey(o_id, d_id, w_id);
    r_order->set_primary_key(ord_key);
    r_order->set_value(O_ID, o_id);
    r_order->set_value(O_C_ID, c_id);
    r_order->set_value(O_D_ID, d_id);
    r_order->set_value(O_W_ID, w_id);
    r_order->set_value(O_ENTRY_D, arg.o_entry_d);
    r_order->set_value(O_OL_CNT, ol_cnt);
    r_order->set_value(O_CARRIER_ID, uint64_t(0));   // undelivered (spec: NULL); the row buffer is not zeroed
    insert_row(r_order, _wl->t_order);
    // For update consistency the same row is inserted into both indexes (as at load time).
    if (!insert_idx(_wl->i_order, ord_key, r_order, part_id)) {
        return finish(Abort);
    }
    if (!insert_idx(_wl->i_order_cust, orderCustKey(o_id, c_id, d_id, w_id),
                    r_order, part_id)) {
        return finish(Abort);
    }
#if CC_ALG == IC3
    if (end_piece(4) != RCOK)
        goto order_piece;

    item_piece: // 5
    begin_piece(5);
    /*===========================================+
    EXEC SQL SELECT i_price, i_name , i_data
        INTO :i_price, :i_name, :i_data
        FROM item
        WHERE i_id = :ol_i_id;
    +===========================================*/
    for (UInt32 ol_number = 0; ol_number < ol_cnt; ol_number++) {
        ol_i_id = arg.items[ol_number].ol_i_id;
#if TPCC_USER_ABORT
        // XXX(zhihan): if key is invalid, abort. user-initiated abort
        // according to tpc-c documentation
        if (ol_i_id == 0)
            return finish(ERROR);
#endif
        ol_supply_w_id = arg.items[ol_number].ol_supply_w_id;
        ol_quantity = arg.items[ol_number].ol_quantity;
        key = ol_i_id;
        item = index_read(_wl->i_item, key, 0);
        assert(item != NULL);
        r_item = ((row_t *)item->location);
        r_item_local = get_row(r_item, RD);
        if (r_item_local == NULL) {
            return finish(Abort);
        }
        r_item_local->get_value(I_PRICE, i_price);
        r_item_local->get_value(I_NAME);
        r_item_local->get_value(I_DATA);
        assert(r_item_local->data);
    }

    if (end_piece(5) != RCOK)
        goto item_piece;


    stock_piece: // 6
    begin_piece(6);
    /*===================================================================+
    EXEC SQL SELECT s_quantity, s_data,
            s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05,
            s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10
        INTO :s_quantity, :s_data,
            :s_dist_01, :s_dist_02, :s_dist_03, :s_dist_04, :s_dist_05,
            :s_dist_06, :s_dist_07, :s_dist_08, :s_dist_09, :s_dist_10
        FROM stock
        WHERE s_i_id = :ol_i_id AND s_w_id = :ol_supply_w_id;
    EXEC SQL UPDATE stock SET s_quantity = :s_quantity
        WHERE s_i_id = :ol_i_id
        AND s_w_id = :ol_supply_w_id;
    +===============================================*/
    for (UInt32 ol_number = 0; ol_number < ol_cnt; ol_number++) {
        ol_i_id = arg.items[ol_number].ol_i_id;
        ol_supply_w_id = arg.items[ol_number].ol_supply_w_id;
        ol_quantity = arg.items[ol_number].ol_quantity;
        stock_key = stockKey(ol_i_id, ol_supply_w_id);
        stock_index = _wl->i_stock;
        index_read(stock_index, stock_key, wh_to_part(ol_supply_w_id), stock_item);
        assert(stock_item != NULL);
        r_stock = ((row_t *)stock_item->location);
        r_stock_local = get_row(r_stock, WR);
        if (r_stock_local == NULL) {
            return finish(Abort);
        }
        assert(r_stock_local->data);
        // XXX s_dist_xx are not retrieved.
        s_quantity = *(int64_t *)r_stock_local->get_value(S_QUANTITY);
#if !TPCC_SMALL
        r_stock_local->get_value(S_YTD, s_ytd);
        r_stock_local->set_value(S_YTD, s_ytd + ol_quantity);
        r_stock_local->get_value(S_ORDER_CNT, s_order_cnt);
        r_stock_local->set_value(S_ORDER_CNT, s_order_cnt + 1);
#endif
        bool remote = ol_supply_w_id != w_id;
        if (remote) {
            s_remote_cnt = *(int64_t*)r_stock_local->get_value(S_REMOTE_CNT);
            s_remote_cnt ++;
            r_stock_local->set_value(S_REMOTE_CNT, &s_remote_cnt);
        }
        if (s_quantity > ol_quantity + 10) {
            quantity = s_quantity - ol_quantity;
        } else {
            quantity = s_quantity - ol_quantity + 91;
        }
        r_stock_local->set_value(S_QUANTITY, &quantity);
    }
    if (end_piece(6) != RCOK)
        goto stock_piece;

    orderline_piece: // 7
    begin_piece(7);
    // XXX(IC3) orderline insert is skipped in the IC3 path .
    if (end_piece(7) != RCOK)
        goto orderline_piece;
#else // if CC_ALG != IC3

    for (uint64_t ol_number = 0; ol_number < ol_cnt; ol_number++) {
        ol_i_id = arg.items[ol_number].ol_i_id;
#if TPCC_USER_ABORT
        // XXX(zhihan): if key is invalid, abort. user-initiated abort
        // according to tpc-c documentation
        // [uniform] no CC condition: every algorithm gets the same user aborts.
        if (ol_i_id == 0) {
            return finish(ERROR);
        }
#endif
        ol_supply_w_id = arg.items[ol_number].ol_supply_w_id;
        ol_quantity = arg.items[ol_number].ol_quantity;

        //6. search item
        key = ol_i_id;
        r_item_local = search(_wl->i_item, key, 0, RD);
        if (r_item_local == NULL) {
            return finish(Abort);
        }
        r_item_local->get_value(I_PRICE, i_price);
        r_item_local->get_value(I_NAME);
        r_item_local->get_value(I_DATA);
        /*===================================================================+
        EXEC SQL SELECT s_quantity, s_data,
                s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05,
                s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10
            INTO :s_quantity, :s_data,
                :s_dist_01, :s_dist_02, :s_dist_03, :s_dist_04, :s_dist_05,
                :s_dist_06, :s_dist_07, :s_dist_08, :s_dist_09, :s_dist_10
            FROM stock
            WHERE s_i_id = :ol_i_id AND s_w_id = :ol_supply_w_id;
        EXEC SQL UPDATE stock SET s_quantity = :s_quantity
            WHERE s_i_id = :ol_i_id
            AND s_w_id = :ol_supply_w_id;
        +===============================================*/

        // 7. update stock
        stock_key = stockKey(ol_i_id, ol_supply_w_id);
        r_stock_local = search(_wl->i_stock, stock_key,
                               wh_to_part(ol_supply_w_id), WR);
        if (r_stock_local == NULL) {
            return finish(Abort);
        }

        // XXX s_dist_xx are not retrieved.
        s_quantity = *(int64_t *)r_stock_local->get_value(S_QUANTITY);
#if !TPCC_SMALL
        r_stock_local->get_value(S_YTD, s_ytd);
        r_stock_local->set_value(S_YTD, s_ytd + ol_quantity);
        r_stock_local->get_value(S_ORDER_CNT, s_order_cnt);
        r_stock_local->set_value(S_ORDER_CNT, s_order_cnt + 1);
#endif
        bool remote = ol_supply_w_id != arg.w_id;
        if (remote) {
            s_remote_cnt = *(int64_t*)r_stock_local->get_value(S_REMOTE_CNT);
            s_remote_cnt ++;
            r_stock_local->set_value(S_REMOTE_CNT, &s_remote_cnt);
        }

        if (s_quantity > ol_quantity + 10) {
            quantity = s_quantity - ol_quantity;
        } else {
            quantity = s_quantity - ol_quantity + 91;
        }
        r_stock_local->set_value(S_QUANTITY, &quantity);

#if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT != 1)
        if (retire_row(row_cnt-1) == Abort){
            return finish(Abort);
        }
#endif

        /*====================================================+
        EXEC SQL INSERT
            INTO order_line(ol_o_id, ol_d_id, ol_w_id, ol_number,
                ol_i_id, ol_supply_w_id,
                ol_quantity, ol_amount, ol_dist_info)
            VALUES(:o_id, :d_id, :w_id, :ol_number,
                :ol_i_id, :ol_supply_w_id,
                :ol_quantity, :ol_amount, :ol_dist_info);
        +====================================================*/
        // XXX district info is not inserted.
        // 8. insert orderline
        _wl->t_orderline->get_new_row(r_ol, 0, row_id);
        uint64_t ordline_key = orderlineKey(ol_number + 1, o_id, d_id, w_id);
        r_ol->set_primary_key(ordline_key);
        r_ol->set_value(OL_O_ID, &o_id);
        r_ol->set_value(OL_D_ID, &d_id);
        r_ol->set_value(OL_W_ID, &w_id);
        r_ol->set_value(OL_NUMBER, &ol_number);
        r_ol->set_value(OL_I_ID, &ol_i_id);
#if !TPCC_SMALL
        ol_amount = ol_quantity * i_price * (1 + w_tax + d_tax) * (1 - c_discount);
        r_ol->set_value(OL_SUPPLY_W_ID, &ol_supply_w_id);
        r_ol->set_value(OL_QUANTITY, &ol_quantity);
        r_ol->set_value(OL_AMOUNT, &ol_amount);
        sum += ol_amount;
#endif
        insert_row(r_ol, _wl->t_orderline);
        if (!insert_idx(_wl->i_orderline, ordline_key, r_ol, part_id)) {
            return finish(Abort);
        }
    }

#endif // if CC_ALG == IC3

    assert( rc == RCOK );
    return finish(rc);
}

RC tpcc_txn_man::run_delivery(tpcc_query * query) {
    auto& arg = query->args.delivery;
    int64_t o_id = 0;

    for (uint64_t d_id = 1; d_id <= DIST_PER_WARE; d_id++) {
        o_id = 0;
        if (!delivery_getNewOrder_deleteNewOrder(d_id, arg.w_id, o_id)) {
            return finish(Abort);
        }
        // No new order for this district.
        if (o_id == -1) {
            continue;
        }
        //1. neworder (to be deleted): the last access is the WR on the neworder row
#if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT != 1)
        if (retire_row(row_cnt-1) == Abort){
            return finish(Abort);
        }
#endif

        auto order = delivery_getCId(o_id, d_id, arg.w_id);
        if (order == NULL) {
            return finish(Abort);
        }
        uint64_t c_id;
        order->get_value(O_C_ID, c_id);

        delivery_updateOrders(order, arg.o_carrier_id);
        //2. update order
#if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT != 1)
        if (retire_row(row_cnt-1) == Abort){
            return finish(Abort);
        }
#endif

        double ol_total;
        if (!delivery_updateOrderLine_sumOLAmount(arg.ol_delivery_d, o_id, d_id,
                                                  arg.w_id, &ol_total)) {
            return finish(Abort);
        }

        if (!delivery_updateCustomer(ol_total, c_id, d_id, arg.w_id)) {
            return finish(Abort);
        }
        //4. update customer
#if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT != 1)
        if (retire_row(row_cnt-1) == Abort){
            return finish(Abort);
        }
#endif
    }

    auto rc = finish(RCOK);
    return rc;
}

RC tpcc_txn_man::run_order_status(tpcc_query * query) {
    auto& arg = query->args.order_status;

    auto c_id = arg.c_id;
    row_t* customer = NULL;
    if (!arg.by_last_name)
        customer = order_status_getCustomerByCustomerId(arg.w_id, arg.d_id, arg.c_id);
    else
        customer = order_status_getCustomerByLastName(arg.w_id, arg.d_id, arg.c_last, &c_id);

    if (customer == NULL) {
        return finish(Abort);
    }

    auto order = order_status_getLastOrder(arg.w_id, arg.d_id, c_id);
    if (order != NULL) {
        int64_t o_id;
        order->get_value(O_ID, o_id);
        if (!order_status_getOrderLines(arg.w_id, arg.d_id, o_id)) {
            return finish(Abort);
        }
    }

    return finish(RCOK);
}

RC tpcc_txn_man::run_stock_level(tpcc_query * query) {
    auto& arg = query->args.stock_level;

    auto district = stock_level_getOId(arg.w_id, arg.d_id);
    if (district == NULL) {
        return finish(Abort);
    }
    int64_t o_id;
    district->get_value(D_NEXT_O_ID, o_id);

    uint64_t distinct_count;
    if (!stock_level_getStockCount(arg.w_id, arg.d_id, o_id, arg.w_id,
                                   arg.threshold, &distinct_count)) {
        return finish(Abort);
    }
    (void)distinct_count;

    return finish(RCOK);
}

row_t* tpcc_txn_man::order_status_getCustomerByCustomerId(uint64_t w_id,
                                                          uint64_t d_id,
                                                          uint64_t c_id) {
    // SELECT C_ID, C_FIRST, C_MIDDLE, C_LAST, C_BALANCE FROM CUSTOMER WHERE C_W_ID = ? AND C_D_ID = ? AND C_ID = ?
    auto index = _wl->i_customer_id;
    auto key = custKey(c_id, d_id, w_id);
    auto part_id = wh_to_part(w_id);
    return search(index, key, part_id, RD);
}

row_t* tpcc_txn_man::order_status_getCustomerByLastName(uint64_t w_id,
                                                        uint64_t d_id,
                                                        char* c_last,
                                                        uint64_t* out_c_id) {
    // SELECT C_ID, C_FIRST, C_MIDDLE, C_LAST, C_BALANCE FROM CUSTOMER WHERE C_W_ID = ? AND C_D_ID = ? AND C_LAST = ? ORDER BY C_FIRST
    auto index = _wl->i_customer_last;
    auto key = custNPKey(c_last, d_id, w_id);
    auto part_id = wh_to_part(w_id);

    itemid_t* items[100];
    size_t count = 100;
    auto rc = index_read_multiple(index, key, items, count, part_id);
    if (rc != RCOK) {
        assert(false);
        return NULL;
    }
    if (count == 0) return NULL;

    auto mid = items[count / 2];
    auto local = get_row((row_t *)mid->location, RD);
    if (local != NULL) local->get_value(C_ID, *out_c_id);
    return local;
}

row_t* tpcc_txn_man::order_status_getLastOrder(uint64_t w_id, uint64_t d_id,
                                               uint64_t c_id) {
    // SELECT O_ID, O_CARRIER_ID, O_ENTRY_D FROM ORDERS WHERE O_W_ID = ? AND O_D_ID = ? AND O_C_ID = ? ORDER BY O_ID DESC LIMIT 1
    auto index = _wl->i_order_cust;
    // o_id is packed in reverse, so min = most recent (largest o_id), max = oldest (o_id=1)
    auto key = orderCustKey(g_max_orderline, c_id, d_id, w_id);
    auto max_key = orderCustKey(1, c_id, d_id, w_id);
    auto part_id = wh_to_part(w_id);

    itemid_t* items[1];
    size_t count = 1;

    auto idx_rc = index_read_range(index, key, max_key, items, count, part_id);
    if (idx_rc == Abort) return NULL;
    assert(idx_rc == RCOK);

    if (count == 0) {
        // every customer has an order from load time, so this does not happen normally
        return NULL;
    }

    auto shared = items[0];
    auto local = get_row((row_t *)shared->location, RD);
    if (local == NULL) return NULL;
    return local;
}

bool tpcc_txn_man::order_status_getOrderLines(uint64_t w_id, uint64_t d_id,
                                              int64_t o_id) {
    // SELECT OL_SUPPLY_W_ID, OL_I_ID, OL_QUANTITY, OL_AMOUNT, OL_DELIVERY_D FROM ORDER_LINE WHERE OL_W_ID = ? AND OL_D_ID = ? AND OL_O_ID = ?
    auto index = _wl->i_orderline;
    auto key = orderlineKey(1, o_id, d_id, w_id);
    auto max_key = orderlineKey(15, o_id, d_id, w_id);
    auto part_id = wh_to_part(w_id);

    itemid_t* items[16];
    size_t count = 16;

    auto idx_rc = index_read_range(index, key, max_key, items, count, part_id);
    if (idx_rc == Abort) return false;
    assert(idx_rc == RCOK);
    assert(count != 16);

    for (uint64_t i = 0; i < count; i++) {
        auto shared = items[i];
        auto local = get_row((row_t *)shared->location, RD);
        if (local == NULL) {
            return false;
        }
        (void)local;
    }

    return true;
}

bool tpcc_txn_man::delivery_updateCustomer(double ol_total, uint64_t c_id,
                                           uint64_t d_id, uint64_t w_id) {
    // UPDATE CUSTOMER SET C_BALANCE = C_BALANCE + ?, C_DELIVERY_CNT = C_DELIVERY_CNT + 1 WHERE C_ID = ? AND C_D_ID = ? AND C_W_ID = ?
    auto index = _wl->i_customer_id;
    auto key = custKey(c_id, d_id, w_id);
    auto part_id = wh_to_part(w_id);
    auto row = search(index, key, part_id, WR);
    if (row == NULL) return false;

    double c_balance;
    row->get_value(C_BALANCE, c_balance);
    row->set_value(C_BALANCE, c_balance + ol_total);

    return true;
}

bool tpcc_txn_man::delivery_getNewOrder_deleteNewOrder(uint64_t d_id,
                                                       uint64_t w_id,
                                                       int64_t &out_o_id) {
    // SELECT NO_O_ID FROM NEW_ORDER WHERE NO_D_ID = ? AND NO_W_ID = ? AND NO_O_ID > -1 LIMIT 1
    // DELETE FROM NEW_ORDER WHERE NO_D_ID = ? AND NO_W_ID = ? AND NO_O_ID = ?

    auto index = _wl->i_neworder;
    // TODO: This may cause a match with other district with a negative order ID.  It is safe for now because the lowest order ID is 1, but we should give more gap (or use tuple keys) to avoid accidental matches.
    auto key = neworderKey(g_max_orderline, d_id, w_id);
    auto max_key = neworderKey(0, d_id, w_id);  // Use key ">= 0" for "> -1"
    auto part_id = wh_to_part(w_id);

    itemid_t* items[1];
    size_t count = 1;

    auto idx_rc = index_read_range_rev(index, key, max_key, items, count, part_id);
    if (idx_rc == Abort) return false;
    assert(idx_rc == RCOK);

    // No new order; this is acceptable and we do not need to abort TX.
    if (count == 0) {
        out_o_id = -1;
        return true;
    }

    auto shared = items[0];
    auto local = get_row((row_t *)shared->location, WR);
    if (local == NULL) return false;

    int64_t o_id;
    local->get_value(NO_O_ID, o_id);
    out_o_id = o_id;

    {
        auto idx = _wl->i_neworder;
        auto key = neworderKey(o_id, d_id, w_id);
        if (!remove_idx(idx, key, (row_t *)shared->location, part_id)) return false;
        if (!remove_row((row_t *)shared->location)) return false;
    }

    return true;
}

row_t* tpcc_txn_man::delivery_getCId(int64_t no_o_id, uint64_t d_id,
                                     uint64_t w_id) {
    // SELECT O_C_ID FROM ORDERS WHERE O_ID = ? AND O_D_ID = ? AND O_W_ID = ?
    auto index = _wl->i_order;
    auto key = orderKey(no_o_id, d_id, w_id);
    auto part_id = wh_to_part(w_id);
    return search(index, key, part_id, WR);
}

void tpcc_txn_man::delivery_updateOrders(row_t* row, uint64_t o_carrier_id) {
    // UPDATE ORDERS SET O_CARRIER_ID = ? WHERE O_ID = ? AND O_D_ID = ? AND O_W_ID = ?
    row->set_value(O_CARRIER_ID, o_carrier_id);
}

bool tpcc_txn_man::delivery_updateOrderLine_sumOLAmount(uint64_t o_entry_d,
                                                        int64_t no_o_id,
                                                        uint64_t d_id,
                                                        uint64_t w_id,
                                                        double* out_ol_total) {
    // UPDATE ORDER_LINE SET OL_DELIVERY_D = ? WHERE OL_O_ID = ? AND OL_D_ID = ? AND OL_W_ID = ?
    // SELECT SUM(OL_AMOUNT) FROM ORDER_LINE WHERE OL_O_ID = ? AND OL_D_ID = ? AND OL_W_ID = ?
    double ol_total = 0.0;

    auto index = _wl->i_orderline;
    auto key = orderlineKey(1, no_o_id, d_id, w_id);
    auto max_key = orderlineKey(15, no_o_id, d_id, w_id);
    auto part_id = wh_to_part(w_id);

    itemid_t* items[16];
    size_t count = 16;

    auto idx_rc = index_read_range(index, key, max_key, items, count, part_id);
    if (idx_rc != RCOK) return false;
    assert(count != 16);

    for (uint64_t i = 0; i < count; i++) {
        auto shared = items[i];
        auto local = get_row((row_t *)shared->location, WR);
        if (local == NULL) return false;
        double ol_amount;
        local->get_value(OL_AMOUNT, ol_amount);
        local->set_value(OL_DELIVERY_D, o_entry_d);
        ol_total += ol_amount;

        //3. update orderline
        // [fix] a bool helper must not call finish(): it returns false and
        // the caller (run_delivery) calls finish(Abort).
#if (CC_ALG == BAMBOO || CC_ALG == RCC) && (THREAD_CNT != 1)
        if (retire_row(row_cnt-1) == Abort) {
            return false;
        }
#endif
    }

    *out_ol_total = ol_total;
    return true;
}

row_t* tpcc_txn_man::stock_level_getOId(uint64_t d_w_id, uint64_t d_id) {
    // SELECT D_NEXT_O_ID FROM DISTRICT WHERE D_W_ID = ? AND D_ID = ?
    auto index = _wl->i_district;
    auto key = distKey(d_id, d_w_id);
    auto part_id = wh_to_part(d_w_id);
    return search(index, key, part_id, RD);
}

bool tpcc_txn_man::stock_level_getStockCount(uint64_t ol_w_id, uint64_t ol_d_id,
                                             int64_t ol_o_id, uint64_t s_w_id,
                                             uint64_t threshold,
                                             uint64_t* out_distinct_count) {
    // SELECT COUNT(DISTINCT(OL_I_ID)) FROM ORDER_LINE, STOCK
    // WHERE ... OL_O_ID < ? AND OL_O_ID >= ? - 20 AND S_QUANTITY < ?
    uint64_t n_orders = 20;
    uint64_t count = n_orders * 15 + 1;

    uint64_t ol_i_id_list[count];
    size_t list_size = 0;

    auto index = _wl->i_orderline;
    auto key = orderlineKey(1, ol_o_id - 1, ol_d_id, ol_w_id);
    auto max_key = orderlineKey(15, ol_o_id - n_orders, ol_d_id, ol_w_id);
    auto part_id = wh_to_part(ol_w_id);

    itemid_t* items[count];
    size_t item_cnt = count;

    auto idx_rc = index_read_range(index, key, max_key, items, item_cnt, part_id);
    if (idx_rc == Abort) {
        return false;
    }
    assert(idx_rc == RCOK);

    for (uint64_t i = 0; i < item_cnt; i++) {
        auto orderline_shared = items[i];
        auto orderline_shared_row = (row_t *)orderline_shared->location;
        if (orderline_shared_row->is_deleted) continue;
        auto orderline = get_row(orderline_shared_row, RD);
        if (orderline == NULL) {
            // A failed read can mean this attempt was aborted. Let the caller
            // finish it now instead of scanning the rest of ORDER_LINE/STOCK.
            return false;
        }

        uint64_t ol_i_id, ol_supply_w_id;
        orderline->get_value(OL_SUPPLY_W_ID, ol_supply_w_id);
        if (ol_supply_w_id != s_w_id) continue;

        orderline->get_value(OL_I_ID, ol_i_id);
        assert(list_size < sizeof(ol_i_id_list) / sizeof(ol_i_id_list[0]));
        ol_i_id_list[list_size] = ol_i_id;
        list_size++;
    }
    assert(list_size <= count);

    uint64_t distinct_ol_i_id_list[count];
    uint64_t distinct_ol_i_id_count = 0;
    uint64_t result = 0;

    for (uint64_t i = 0; i < list_size; i++) {
        uint64_t ol_i_id = ol_i_id_list[i];

        bool duplicate = false;
        for (uint64_t j = 0; j < distinct_ol_i_id_count; j++)
            if (distinct_ol_i_id_list[j] == ol_i_id) {
                duplicate = true;
                break;
            }
        if (duplicate) continue;

        distinct_ol_i_id_list[distinct_ol_i_id_count++] = ol_i_id;

        auto key = stockKey(ol_i_id, s_w_id);
        auto index = _wl->i_stock;
        auto part_id = wh_to_part(s_w_id);
        auto row = search(index, key, part_id, RD);
        if (row == NULL) {
            return false;
        }

        uint64_t s_quantity;
        row->get_value(S_QUANTITY, s_quantity);
        if (s_quantity < threshold) result++;
    }

    *out_distinct_count = result;
    return true;
}
