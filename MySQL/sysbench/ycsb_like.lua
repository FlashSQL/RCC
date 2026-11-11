-- YCSB batchtx 근사: 트랜잭션당 N개 연산, 키는 전부 서로 다름(batchtx.distinct=true 와 동일),
-- 읽기는 PK 점 조회만, 쓰기는 non-index UPDATE. 범위 스캔 없음.
-- 읽기 개수 = --point-selects, 쓰기 개수 = --non-index-updates (개수는 고정),
-- 연산 순서는 매 트랜잭션 무작위로 섞는다 (YCSB CoreWorkload 와 동일).
require("oltp_common")

-- sysbench skips thread_done() when thread_run() raises a fatal error. Close
-- that worker's connection before rethrowing, otherwise its unfinished
-- transaction can keep every surviving worker waiting during shutdown.
local common_thread_done = thread_done
local common_thread_run = thread_run
function thread_run(thread_id)
   local ok, result = pcall(common_thread_run, thread_id)
   if not ok then
      if con ~= nil then pcall(function() con:disconnect() end) end
      error(result, 0)
   end
   return result
end

local ffi = require("ffi")
ffi.cdef[[
  int usleep(unsigned int usec);
  typedef long time_t;
  struct timeval { time_t tv_sec; time_t tv_usec; };
  int gettimeofday(struct timeval *tv, void *tz);
]]

-- 어보트 처리 방식 두 가지 (배타적):
--   SB_ABUF=0 (기본) : [BACKOFF] 0~SB_BACKOFF_US us 자고 그 자리에서 같은 키로 재시도
--   SB_ABUF=1        : [ABUF] DBx1000 abort buffer. 아래 event() 참조
local BACKOFF_US = tonumber(os.getenv("SB_BACKOFF_US") or "5000")
local ABUF = (os.getenv("SB_ABUF") or "0") ~= "0"
local EXPBO = (os.getenv("SB_EXPBO") or "0") ~= "0"
local PENALTY_US = tonumber(os.getenv("SB_PENALTY_US") or "5000")
-- 버퍼 상한. 0 이면 무제한(기본). 양수면 DBx1000 config-std.h:85 ABORT_BUFFER_SIZE 처럼
-- 버퍼가 차면 새 트랜잭션을 만들지 않고 가장 빨리 익는 항목까지 잠근다(배압, thread.cpp:105).
local ABUF_SIZE = tonumber(os.getenv("SB_ABUF_SIZE") or "0")
-- [SORT] SB_SORT=1 이면 트랜잭션의 16개 키를 오름차순으로 접근한다. 키가 트랜잭션 안에서
-- 서로 다르므로(distinct_keys) 2PL의 레코드 락 획득 순서가 생긴다.
-- retire/dirty borrowing에서는 RW/WR/WW 의존이 별도로 생겨 정렬해도 사이클이 가능하다.
-- 주의: 원본 YCSB/sysbench 는 정렬하지 않는다. 이건 변형판이다.
local SORTKEY = (os.getenv("SB_SORT") or "0") ~= "0"
local DUMPATT = tonumber(os.getenv("SB_DUMPATT") or "0")
local IGNORE = {[1213]=true,[1205]=true,[1020]=true,[1180]=true,[1614]=true}

-- 현재 트랜잭션의 상태. sysbench 는 스레드마다 Lua 상태가 따로라 이 지역변수들이
-- 그대로 스레드 지역이다.
local seen = {}
local keys = {}
local isw = {}          -- 각 연산이 쓰기인가
local nops = 0          -- 현재 트랜잭션의 연산 수
local tnum = 1
local pending = false   -- [SAMEKEY] 직전 시도가 실패해 재시도 중인가

-- [ABUF] 워커 스레드별 abort buffer.
local buf = {}
local maxbuf = 0
-- [ABUFDIAG] 시도를 신규/버퍼복귀로 나눠 센다. 구현이 이론대로 도는지 확인용.
local from_buf = false      -- 지금 실행 중인 시도가 버퍼에서 꺼낸 것인가
local att = 0               -- 지금 트랜잭션의 누적 시도 횟수
local n_fresh, n_buf = 0, 0       -- 시작한 시도 수
local a_fresh, a_buf = 0, 0       -- 그중 어보트한 수
local commits, att_sum, att_max = 0, 0, 0
local nsleep = 0            -- 버퍼가 꽉 차 배압으로 잠든 횟수
-- [TIME] temp.out 3~4번째 줄(execution / wasted time)용. 시도 하나가 begin 직전부터
-- 커밋 또는 어보트로 끝날 때까지의 벽시계 시간을 커밋/어보트로 나눠 합산한다.
-- 버퍼에서 익기를 기다린 시간, 백오프 usleep 은 포함하지 않는다 (TPCC 의 TOTAL_COMMIT / TOTAL_WASTE,
-- YCSB 의 [COMMITTED] / [WASTED] 와 같은 잣대).
local t_att = 0
local exec_us, wasted_us, n_abort = 0, 0, 0
-- [HIST] 커밋까지 걸린 시도 횟수 분포 + 고재시도 트랜잭션의 최소키(= 가장 뜨거운 키)
local BUCK = {1,2,4,8,16,32,64,128,256,512,1024}
local hist = {}
for i = 1, #BUCK + 1 do hist[i] = 0 end
local hot_lo, hot_n = 0, 0   -- att>=32 인 것들의 최소키 평균용
local all_lo, all_n = 0, 0

local tvv = ffi.new("struct timeval")
local function now_us()
   ffi.C.gettimeofday(tvv, nil)
   return tonumber(tvv.tv_sec) * 1000000 + tonumber(tvv.tv_usec)
end

-- 드라이버가 자체 무시하지 않는 에러(1180 등)를 재시도 대상으로 강등만 한다.
-- 실제 어보트 처리는 before_restart_event 에서 한다 — sql_error_ignorable 은
-- --mysql-ignore-errors 기본값(1213,1020,1205)에 걸리는 에러에 대해서는
-- 호출되지 않기 때문이다 (sysbench.sql.lua:254 의 or 단축평가).
function sysbench.hooks.sql_error_ignorable(e)
   return IGNORE[tonumber(e.sql_errno)] or false
end

-- 모든 재시도가 여기를 지난다 (sysbench.lua:36). 드라이버가 무시한 1213 도 포함.
function sysbench.hooks.before_restart_event(e)
   -- 1180(COMMIT 실패), 1205 등은 전체 트랜잭션을 끝내지 않을 수 있다.
   -- 다음 BEGIN 전에 SSN descriptor/lock을 정리한다. 자동 rollback된 경우도 안전하다.
   con:query("ROLLBACK")
   -- [TIME] 어보트한 시도의 시간
   wasted_us = wasted_us + (now_us() - t_att)
   n_abort = n_abort + 1
   if ABUF then
      -- [ABUF] 실패한 트랜잭션을 키 그대로 큐에 넣는다. pending 을 풀어 다음
      -- event() 가 익은 항목이나 새 트랜잭션을 고르게 한다 (워커는 놀지 않는다).
      local k, w = {}, {}
      for i = 1, nops do k[i] = keys[i]; w[i] = isw[i] end
      -- [EXPBO] SB_EXPBO=1 이면 재시도 횟수에 따라 penalty 를 2배씩(상한 64배) 늘린다.
      -- 굶고 있는 트랜잭션이 같은 속도로 큐를 두드리는 것을 막는다.
      local pen = PENALTY_US
      if EXPBO then
         local mul = 1
         for _ = 2, math.min(att, 7) do mul = mul * 2 end
         pen = PENALTY_US * mul
      end
      buf[#buf + 1] = {k = k, w = w, t = tnum, att = att,
                       ready = now_us() + math.random(0, pen)}
      if #buf > maxbuf then maxbuf = #buf end
      if from_buf then a_buf = a_buf + 1 else a_fresh = a_fresh + 1 end
      pending = false
   elseif BACKOFF_US > 0 then
      ffi.C.usleep(math.random(0, BACKOFF_US))
   end
   return true
end

-- 한 번의 stdio 쓰기로 줄과 개행을 함께 출력한다. print()는 둘을 따로 써서
-- 여러 thread_done()의 통계가 같은 줄에 붙고 ^[TIME] 집계에서 누락될 수 있다.
local function emit_line(line)
   io.write(line .. "\n")
end

-- [ABUF] 스레드 종료 시 버퍼 최대 길이를 남긴다. 이 값이 계속 커지면 어보트가
-- 커밋보다 빨리 쌓였다는 뜻이라 그 판의 수치는 신뢰할 수 없다.
function thread_done()
   -- [TIME] 스레드별 합계. run-ALL-s.sh 가 전 스레드를 더해 temp.out 3~6번째 줄을 만든다.
   emit_line(string.format("[TIME] commits=%d aborts=%d exec_us=%d wasted_us=%d",
      commits, n_abort, exec_us, wasted_us))
   if ABUF then
      emit_line(string.format("[ABUF] maxbuf=%d leftover=%d | 신규 %d 시도 %d 어보트(%.1f%%)"
         .. " | 버퍼복귀 %d 시도 %d 어보트(%.1f%%) | 커밋 %d 커밋당시도 %.2f max %d 배압 %d",
         maxbuf, #buf, n_fresh, a_fresh, a_fresh/math.max(n_fresh,1)*100,
         n_buf, a_buf, a_buf/math.max(n_buf,1)*100,
         commits, att_sum/math.max(commits,1), att_max, nsleep))
      local h = {}
      for i = 1, #BUCK do h[#h+1] = string.format("<=%d:%d", BUCK[i], hist[i]) end
      h[#h+1] = string.format(">%d:%d", BUCK[#BUCK], hist[#BUCK+1])
      emit_line("[HIST] " .. table.concat(h, " "))
      emit_line(string.format("[HOTKEY] 전체 평균 최소키 %.0f (n=%d) | att>=32 평균 최소키 %.0f (n=%d)",
         all_lo/math.max(all_n,1), all_n, hot_lo/math.max(hot_n,1), hot_n))
   end
   common_thread_done()
end

function prepare_statements()
   if not sysbench.opt.skip_trx then
      prepare_begin()
      prepare_commit()
   end
   prepare_point_selects()
   prepare_non_index_updates()
end

-- oltp_common 의 c_value_template 은 local 이라 밖에서 못 쓴다. 동일한 값을 다시 정의한다
-- (10 groups, 119 characters — oltp_common.lua:139 과 같음).
local c_tmpl = "###########-###########-###########-" ..
   "###########-###########-###########-" ..
   "###########-###########-###########-" ..
   "###########"

-- 트랜잭션 하나에 쓸 서로 다른 키 n 개.  중복이 나오면 다시 뽑는다.
local function distinct_keys(n)
   for k in pairs(seen) do seen[k] = nil end
   local got = 0
   -- distinct 를 무제한 재추첨으로 보장한다. 예전에는 n*1000 번 시도 후 포기하고
   -- keys 를 1,2,...,n 으로 채웠는데, 하필 zipf 에서 가장 뜨거운 키들이라
   -- 치우친 판에서 인위적 핫스팟을 만든다. (table_size >= n 이어야 종료한다.)
   while got < n do
      local id = sysbench.rand.default(1, sysbench.opt.table_size)
      if not seen[id] then
         seen[id] = true
         got = got + 1
         keys[got] = id
      end
   end
end

-- 새 트랜잭션 하나를 뽑아 keys/isw/tnum 에 싣는다.
local function new_tx(nr, n)
   distinct_keys(n)
   -- get_table_num() 은 oltp_common 의 local 이라 밖에서 못 부른다. 직접 뽑는다.
   tnum = sysbench.rand.uniform(1, sysbench.opt.tables)
   -- 읽기 nr 개 / 쓰기 nw 개를 고정해두고 순서만 섞는다 (Fisher-Yates).
   for i = 1, n do isw[i] = (i > nr) end
   for i = n, 2, -1 do
      local j = math.random(i)
      isw[i], isw[j] = isw[j], isw[i]
   end
   -- [SORT] 접근 순서만 키 오름차순으로 고정한다. 어느 자리가 쓰기인지는 그대로 둔다.
   if SORTKEY then table.sort(keys) end
end

function event()
   local nr = sysbench.opt.point_selects
   local n = nr + sysbench.opt.non_index_updates
   nops = n

   -- [SAMEKEY] 어보트 시 sysbench 는 event() 를 통째로 다시 부른다
   -- (sysbench.lua:33 의 repeat...until success). pending 이 서 있으면 키·연산순서·
   -- 테이블을 다시 뽑지 않고 그대로 재사용해서 "같은 키 재시도"를 만든다
   -- (DBx1000 Bamboo/RR thread.cpp 와 동일 모델).
   if not pending then
      -- [ABUF] 버퍼 전체를 훑어 ready 가 지난 첫 항목을 꺼낸다. head 만 보면
      -- 지연이 균등난수라 아직 안 익은 head 하나가 뒤의 익은 항목들을 전부 막는다
      -- (CoreWorkload.java:852 와 같은 이유). 나머지는 순서를 유지한 채 뒤로 회전.
      while ABUF and #buf > 0 do
         local t0 = now_us()
         local picked, min_ready = nil, nil
         for _ = 1, #buf do
            local p = table.remove(buf, 1)
            if picked == nil and t0 >= p.ready then
               picked = p
            else
               if min_ready == nil or p.ready < min_ready then min_ready = p.ready end
               buf[#buf + 1] = p
            end
         end
         if picked then
            for i = 1, n do keys[i] = picked.k[i]; isw[i] = picked.w[i] end
            tnum = picked.t
            att = picked.att
            from_buf = true
            pending = true
            break
         end
         -- 익은 게 없다. 버퍼에 여유가 있으면 새 트랜잭션을 만들고(아래로 빠짐),
         -- 꽉 찼으면 가장 빨리 익는 시각까지 자고 다시 훑는다 (배압).
         if ABUF_SIZE == 0 or #buf < ABUF_SIZE then break end
         nsleep = nsleep + 1
         ffi.C.usleep(math.max(min_ready - t0, 1))
      end
      if not pending then
         new_tx(nr, n)
         att = 0
         from_buf = false
         pending = true
      end
   end
   att = att + 1
   if from_buf then n_buf = n_buf + 1 else n_fresh = n_fresh + 1 end

   t_att = now_us()   -- [TIME]
   if not sysbench.opt.skip_trx then begin() end

   for i = 1, n do
      if isw[i] then
         param[tnum].non_index_updates[1]:set_rand_str(c_tmpl)
         param[tnum].non_index_updates[2]:set(keys[i])
         stmt[tnum].non_index_updates:execute()
      else
         param[tnum].point_selects[1]:set(keys[i])
         stmt[tnum].point_selects:execute()
      end
   end

   if not sysbench.opt.skip_trx then commit() end
   commits = commits + 1
   exec_us = exec_us + (now_us() - t_att)   -- [TIME] 커밋한 시도의 시간
   -- [DUMPATT] SB_DUMPATT=N: 시도 N회 이상 걸린 트랜잭션의 키 집합을 커밋 시 출력
   if DUMPATT > 0 and att >= DUMPATT then
      local ks = {}
      for i = 1, n do ks[i] = (isw[i] and 'W' or 'R') .. keys[i] end
      emit_line(string.format('[ATT] att=%d keys=%s', att, table.concat(ks, ' ')))
   end
   att_sum = att_sum + att
   if att > att_max then att_max = att end
   -- [HIST]
   local bi = #BUCK + 1
   for i = 1, #BUCK do if att <= BUCK[i] then bi = i; break end end
   hist[bi] = hist[bi] + 1
   local lo = keys[1]
   for i = 2, n do if keys[i] < lo then lo = keys[i] end end
   all_lo = all_lo + lo; all_n = all_n + 1
   if att >= 32 then hot_lo = hot_lo + lo; hot_n = hot_n + 1 end
   pending = false            -- 성공했으니 다음 이벤트는 새 키를 뽑는다
   check_reconnect()
end
