1. Build binaries
   * `cd src ; make`
   ( you should have mysql_config available in $PATH)

2. Load data
   * create database
     `mysqladmin create tpcc1000`
   * create tables
     `mysql tpcc1000 < create_table.sql`
   * create indexes and FK ( this step can be done after loading data)
     `mysql tpcc1000 < add_fkey_idx.sql`
   * populate data
     - simple step
       `tpcc_load -h127.0.0.1 -d tpcc1000 -u root -p "" -w 1000`
                 |hostname:port| |dbname| |user| |password| |WAREHOUSES|
       ref. tpcc_load --help for all options
     - load data in parallel 
       check load.sh script

3. Start benchmark
   * `./tpcc_start -h127.0.0.1 -P3306 -dtpcc1000 -uroot -w1000 -c32 -r10 -l10800`
   * |hostname| |port| |dbname| |user| |WAREHOUSES| |CONNECTIONS| |WARMUP TIME| |BENCHMARK TIME|
   * ref. tpcc_start --help for all options 

Item 수별 로드·백업과 실험 (1K / 10K / 100K / 1M)
===================================

데이터 크기까지 비교하려면 각 크기로 전체 데이터를 새로 로드하고 별도로 백업합니다.
ITEM은 DB당 N행, STOCK은 warehouse당 N행이며, 초기 ORDER_LINE의 item ID도
1..N에서 선택됩니다. district/customer/초기 order 수는 기존 설정을 유지합니다.
현재 item ID 컬럼은 `int`이므로 1M을 위해 테이블 정의를 변경할 필요는 없습니다.

로더에는 item 개수 옵션을 추가하지 않았습니다. **`src/tpc.h`의 `MAXITEMS`를
변경하고 빌드한 뒤 로드**합니다. 현재 값은 1000000입니다.
`MAX_ITEM_COUNT`는 실행 옵션의 상한(1M)이므로 변경하지 않습니다.

| 백업 | `MAXITEMS` 하드코딩 값 | 실행 시 옵션 |
| --- | ---: | --- |
| 1K | 1000 | `-I 1K` |
| 10K | 10000 | `-I 10K` |
| 100K | 100000 | `-I 100K` |
| 1M | 1000000 | `-I 1M` |

각 크기별로 아래 절차를 반복합니다. DB 이름, 접속 정보, warehouse 수는
실험 환경에 맞춥니다. 아래 예시는 `MAXITEMS`를 1000으로 변경한 경우입니다.

```bash
cd /home/ubuntu/tpcc-mysql
# src/tpc.h에서 #define MAXITEMS 1000 으로 변경
make -C src
export PATH=/home/ubuntu/mysql-8.4.5-install/bin:$PATH
export LD_LIBRARY_PATH=/home/ubuntu/mysql-8.4.5-install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

DB=tpcc_i1k
WH=1
mysqladmin -h127.0.0.1 -P3306 -uroot create "$DB"
mysql -h127.0.0.1 -P3306 -uroot "$DB" < create_table.sql
./tpcc_load -h127.0.0.1 -P3306 -d "$DB" -uroot -p "" -w "$WH"
mysql -h127.0.0.1 -P3306 -uroot "$DB" < add_fkey_idx.sql
# 로드와 인덱스 생성 완료 후 이 크기의 DB를 별도로 백업합니다.
```

`MAXITEMS` 변경 시 `make -C src`가 관련 오브젝트와 실행 파일을 다시 빌드합니다.
단순히 ITEM/STOCK만 줄이면 초기 주문의 item 참조가 남으므로, 각 크기마다
빈 DB에 **전체 로드**를 수행하세요. 로드가 끝난 뒤에 다음 크기로 재빌드합니다.

실험 시에는 해당 백업을 복원하고 **로드 크기와 같은 대문자 `-I`**를 전달합니다.
실행기는 같은 바이너리로 네 크기를 모두 지원하므로 실험마다 재빌드할 필요가 없습니다.
이전 실험의 주문/재고 변경이 누적되지 않도록 각 실행 전에 원하는 백업을 복원합니다.

```bash
# 1K 백업 복원 후
./tpcc_start -h127.0.0.1 -P3306 -d tpcc_i1k -uroot -p "" \
  -w1 -I 1K -c32 -r10 -l300
# 다른 크기의 백업을 복원했으면 DB 이름과 -I를 함께 변경
```

`-I 1K`, `-I 10K`, `-I 100K`, `-I 1M` 또는 각각
`1000`, `10000`, `100000`, `1000000`을 사용합니다.
K/M은 1000/1000000이며 소문자도 허용합니다. 지원 범위는 1..1000000입니다.
생략 시 **마지막 빌드의 `MAXITEMS` 값**을 사용하므로 실험에서는 `-I`를 명시하세요.
소문자 `-i`는 기존 report interval 옵션입니다.
기존 `-a`, `TPCC_BACKOFF_*`와 함께 사용할 수 있고,
`run_with_check.sh`도 전달받은 `-I` 인자를 그대로 실행기에 넘깁니다.

New-Order의 정상 item 선택은 기존 `NURand(8191, 1, N)` 방식으로 1..N을 사용하고,
의도된 rollback용 invalid item ID는 N+1입니다. 실행기가 DB의 item 수를 자동으로
확인하지 않으므로, `-I`가 복원한 데이터와 일치해야 합니다.
1M DB에서 `-I 1K`만 지정하면 DB 크기/초기 주문은 1M으로 남고 ID 1001도 실제로
존재하므로, 별도 크기의 백업을 사용하는 실험을 대신할 수 없습니다.
100K 이외는 기본 item 수를 변경한 스케일 실험으로 기록하세요.

백업 전에 아래 조회로 ITEM은 N행/ID 1..N, STOCK은 warehouse마다 N행/ID 1..N,
범위를 벗어난 ORDER_LINE은 0행인지 확인할 수 있습니다 (`@items`는 실제 로드 값).

```sql
SET @items = 1000;
SELECT COUNT(*) AS items, MIN(i_id) AS min_id, MAX(i_id) AS max_id FROM item;
SELECT s_w_id, COUNT(*) AS stocks, MIN(s_i_id) AS min_id, MAX(s_i_id) AS max_id
FROM stock GROUP BY s_w_id;
SELECT COUNT(*) AS invalid_order_lines
FROM order_line WHERE ol_i_id < 1 OR ol_i_id > @items;
```

코드 위치: 고정 로드 크기는 `src/tpc.h`, ITEM/STOCK/초기 ORDER_LINE 생성은
`src/load.c`, 실행 옵션은 `src/main.c`와 `src/support.c`, New-Order item 선택은
`src/driver.c`입니다. 1M 로드를 위해 ORIGINAL 플래그를 힙에 할당하고 전부
0으로 초기화하며, `src/neword.c`의 STOCK 정렬 키는 64비트로 계산합니다.

Output
===================================

With the defined interval (-i option), the tool will produce the following output:
```
  10, trx: 12920, 95%: 9.483, 99%: 18.738, max_rt: 213.169, 12919|98.778, 1292|101.096, 1293|443.955, 1293|670.842
  20, trx: 12666, 95%: 7.074, 99%: 15.578, max_rt: 53.733, 12668|50.420, 1267|35.846, 1266|58.292, 1267|37.421
  30, trx: 13269, 95%: 6.806, 99%: 13.126, max_rt: 41.425, 13267|27.968, 1327|32.242, 1327|40.529, 1327|29.580
  40, trx: 12721, 95%: 7.265, 99%: 15.223, max_rt: 60.368, 12721|42.837, 1271|34.567, 1272|64.284, 1272|22.947
  50, trx: 12573, 95%: 7.185, 99%: 14.624, max_rt: 48.607, 12573|45.345, 1258|41.104, 1258|54.022, 1257|26.626
```

Where: 
* 10 - the seconds from the start of the benchmark
* trx: 12920 - New Order transactions executed during the gived interval (in this case, for the previous 10 sec). Basically this is the throughput per interval. The more the better
* 95%: 9.483: - The 95% Response time of New Order transactions per given interval. In this case it is 9.483 sec
* 99%: 18.738: - The 99% Response time of New Order transactions per given interval. In this case it is 18.738 sec
* max_rt: 213.169: - The Max Response time of New Order transactions per given interval. In this case it is 213.169 sec
* the rest: `12919|98.778, 1292|101.096, 1293|443.955, 1293|670.842` is throughput and max response time for the other kind of transactions and can be ignored

