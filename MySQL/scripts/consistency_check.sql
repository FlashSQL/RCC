-- TPC-C consistency checks (TPC-C spec §3.3.2)
-- Adapted from BenchmarkSQL's checks.sql for tpcc-mysql schema.
-- Table name mapping: bmsql_oorder→orders, bmsql_new_order→new_orders,
-- bmsql_order_line→order_line, bmsql_{warehouse,district,customer,history}→same.

-- Test 1: Undelivered ORDERs (o_carrier_id IS NULL) must exist in new_orders.
SELECT CASE count(*) WHEN 0 THEN 'OK   ' ELSE 'ERROR' END AS `check`,
       count(*) AS `count`, 'Undelivered ORDERs not found in NEW_ORDER' AS `Description`
  FROM orders
 WHERE o_carrier_id IS NULL
   AND NOT EXISTS (
       SELECT 1 FROM new_orders
        WHERE no_w_id = o_w_id AND no_d_id = o_d_id AND no_o_id = o_id
   );

-- Test 2: Delivered ORDERs must not be in new_orders.
SELECT CASE count(*) WHEN 0 THEN 'OK   ' ELSE 'ERROR' END AS `check`,
       count(*) AS `count`, 'Delivered ORDERs still found in NEW_ORDER' AS `Description`
  FROM orders
 WHERE o_carrier_id IS NOT NULL
   AND EXISTS (
       SELECT 1 FROM new_orders
        WHERE no_w_id = o_w_id AND no_d_id = o_d_id AND no_o_id = o_id
   );

-- Test 3: Every new_orders row must have a matching orders row.
SELECT CASE count(*) WHEN 0 THEN 'OK   ' ELSE 'ERROR' END AS `check`,
       count(*) AS `count`, 'Orphaned NEW_ORDER rows' AS `Description`
  FROM new_orders
 WHERE NOT EXISTS (
       SELECT 1 FROM orders
        WHERE no_w_id = o_w_id AND no_d_id = o_d_id AND no_o_id = o_id
   );

-- Test 4: Every order_line row must have a matching orders row.
SELECT CASE count(*) WHEN 0 THEN 'OK   ' ELSE 'ERROR' END AS `check`,
       count(*) AS `count`, 'Orphaned ORDER_LINE rows' AS `Description`
  FROM order_line
 WHERE NOT EXISTS (
       SELECT 1 FROM orders
        WHERE ol_w_id = o_w_id AND ol_d_id = o_d_id AND ol_o_id = o_id
   );

-- Test 5: orders.o_ol_cnt must equal COUNT(order_line) for that order.
SELECT CASE count(*) WHEN 0 THEN 'OK   ' ELSE 'ERROR' END AS `check`,
       count(*) AS `count`, 'ORDERs with wrong O_OL_CNT' AS `Description`
  FROM (
       SELECT o_w_id, o_d_id, o_id, o_ol_cnt, count(*) AS actual
         FROM orders
         LEFT JOIN order_line
           ON ol_w_id = o_w_id AND ol_d_id = o_d_id AND ol_o_id = o_id
        GROUP BY o_w_id, o_d_id, o_id, o_ol_cnt
       HAVING o_ol_cnt <> count(*)
  ) AS X;

-- Test 6: Per warehouse, w_ytd must equal SUM(d_ytd) of its 10 districts.
SELECT CASE count(*) WHEN 0 THEN 'OK   ' ELSE 'ERROR' END AS `check`,
       count(*) AS `count`,
       coalesce(sum(abs(w_ytd - sum_d_ytd)), 0) AS `diff_amount`,
       'Warehouses where W_YTD <> sum(D_YTD)' AS `Description`
  FROM (
       SELECT w_id, w_ytd, sum(d_ytd) AS sum_d_ytd
         FROM warehouse
         LEFT JOIN district ON d_w_id = w_id
        GROUP BY w_id, w_ytd
       HAVING w_ytd <> sum(d_ytd)
  ) AS X;

-- column 'sum_c_ytd_payment' doesn't exist in percona TPC-C.
-- -- Test 7: Global SUM(w_ytd) must equal SUM(c_ytd_payment).
-- SELECT CASE count(*) WHEN 0 THEN 'OK   ' ELSE 'ERROR' END AS `check`,
--        count(*) AS `count`,
--        coalesce(sum(abs(sum_w_ytd - sum_c_ytd_payment)), 0) AS `diff_amount`,
--        CASE count(*) WHEN 0 THEN 'sum(w_ytd) = sum(c_ytd_payment)'
--                      ELSE 'sum(w_ytd) <> sum(c_ytd_payment)' END AS `Description`
--   FROM (
--        SELECT sum_w_ytd, sum_c_ytd_payment
--          FROM (SELECT sum(w_ytd)         AS sum_w_ytd         FROM warehouse) AS W,
--               (SELECT sum(c_ytd_payment) AS sum_c_ytd_payment FROM customer)  AS C
--         WHERE sum_w_ytd <> sum_c_ytd_payment
--   ) AS X;

-- Test 8: Per customer, c_balance must equal
--   SUM(ol_amount) of delivered order_lines  -  SUM(h_amount).
--

SELECT CASE count(*) WHEN 0 THEN 'OK   ' ELSE 'ERROR' END AS `check`,
       count(*) AS `count`,
       coalesce(sum(abs(c_balance - (sum_ol_amount - sum_h_amount))), 0) AS `diff_amount`,
       'Customers where C_BALANCE <> sum(OL_AMOUNT) of undelivered orders minus sum(H_AMOUNT)' AS `Description`
  FROM (
       SELECT c_w_id, c_d_id, c_id, c_balance,
              coalesce(sum_ol_amount, 0.0) AS sum_ol_amount,
              coalesce(sum_h_amount,  0.0) AS sum_h_amount
         FROM customer
         LEFT JOIN (
              SELECT o_w_id, o_d_id, o_c_id, sum(ol_amount) AS sum_ol_amount
                FROM orders
                JOIN order_line
                  ON ol_w_id = o_w_id AND ol_d_id = o_d_id AND ol_o_id = o_id
               WHERE o_carrier_id IS NOT NULL AND ol_delivery_d IS NOT NULL
               GROUP BY o_w_id, o_d_id, o_c_id
         ) AS OL ON o_w_id = c_w_id AND o_d_id = c_d_id AND o_c_id = c_id
         LEFT JOIN (
              SELECT h_c_w_id, h_c_d_id, h_c_id, sum(h_amount) AS sum_h_amount
                FROM history
               GROUP BY h_c_w_id, h_c_d_id, h_c_id
         ) AS H ON h_c_w_id = c_w_id AND h_c_d_id = c_d_id AND h_c_id = c_id
        WHERE c_balance <> sum_ol_amount - sum_h_amount
  ) AS X;
