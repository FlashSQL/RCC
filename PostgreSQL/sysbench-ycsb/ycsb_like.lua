-- YCSB batchtx approximation: N operations per transaction, all keys distinct (as batchtx.distinct=true);
-- reads are PK point lookups only, writes are non-index UPDATEs. No range scans.
-- Read count = --point-selects, write count = --non-index-updates (both fixed);
-- operation order is shuffled in every transaction (as in YCSB CoreWorkload).
-- PostgreSQL version of the MySQL sysbench YCSB script; [PG] marks the PostgreSQL-specific parts.
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

-- Two abort-handling modes (mutually exclusive):
--   SB_ABUF=0 (default): [BACKOFF] sleep 0~SB_BACKOFF_US us, then retry in place with the same keys
--   SB_ABUF=1          : [ABUF] DBx1000 abort buffer; see event() below
local BACKOFF_US = tonumber(os.getenv("SB_BACKOFF_US") or "5000")
local ABUF = (os.getenv("SB_ABUF") or "0") ~= "0"
local EXPBO = (os.getenv("SB_EXPBO") or "0") ~= "0"
local PENALTY_US = tonumber(os.getenv("SB_PENALTY_US") or "5000")
-- Buffer cap. 0 = unbounded (default). If positive, as with DBx1000 ABORT_BUFFER_SIZE (config-std.h:85),
-- a full buffer admits no new transaction and sleeps until the earliest entry is ready (backpressure, thread.cpp:105).
local ABUF_SIZE = tonumber(os.getenv("SB_ABUF_SIZE") or "0")
-- [SORT] With SB_SORT=1, the transaction's 16 keys are accessed in ascending order. Since keys are
-- distinct within a transaction (distinct_keys), this fixes the 2PL record-lock acquisition order.
-- Under retire/dirty borrowing, RW/WR/WW dependencies form independently, so cycles remain possible even when sorted.
-- Note: the original YCSB/sysbench does not sort; this is a variant.
local SORTKEY = (os.getenv("SB_SORT") or "0") ~= "0"
local DUMPATT = tonumber(os.getenv("SB_DUMPATT") or "0")
local IGNORE = {[1213]=true,[1205]=true,[1020]=true,[1180]=true,[1614]=true}

-- Current transaction state. sysbench keeps a separate Lua state per thread, so these locals
-- are thread-local as is.
local seen = {}
local keys = {}
local isw = {}          -- whether each operation is a write
local nops = 0          -- number of operations in the current transaction
local tnum = 1
local pending = false   -- [SAMEKEY] previous attempt failed and is being retried

-- [ABUF] Per-worker-thread abort buffer.
local buf = {}
local maxbuf = 0
-- [ABUFDIAG] Count attempts as fresh vs. from-buffer, to check the implementation behaves as modeled.
local from_buf = false      -- current attempt was taken from the buffer
local att = 0               -- cumulative attempt count of the current transaction
local n_fresh, n_buf = 0, 0       -- attempts started
local a_fresh, a_buf = 0, 0       -- of those, attempts aborted
local commits, att_sum, att_max = 0, 0, 0
local nsleep = 0            -- times slept on backpressure because the buffer was full
-- [TIME] Execution / wasted time. Wall-clock time of each attempt, from just before begin
-- until it commits or aborts, summed separately for committed and aborted attempts.
-- Excludes time waiting in the buffer to become ready and backoff usleep (same metric as TPCC TOTAL_COMMIT / TOTAL_WASTE
-- and YCSB [COMMITTED] / [WASTED]).
local t_att = 0
local exec_us, wasted_us, n_abort = 0, 0, 0
-- [HIST] Distribution of attempts needed to commit + min key of high-retry transactions (= hottest key)
local BUCK = {1,2,4,8,16,32,64,128,256,512,1024}
local hist = {}
for i = 1, #BUCK + 1 do hist[i] = 0 end
local hot_lo, hot_n = 0, 0   -- for averaging the min key of transactions with att>=32
local all_lo, all_n = 0, 0

local tvv = ffi.new("struct timeval")
local function now_us()
   ffi.C.gettimeofday(tvv, nil)
   return tonumber(tvv.tv_sec) * 1000000 + tonumber(tvv.tv_usec)
end

-- Only downgrades errors the driver does not ignore by itself (1180, etc.) to retryable.
-- Actual abort handling is in before_restart_event, because sql_error_ignorable is
-- not called for errors matched by the --mysql-ignore-errors default (1213,1020,1205)
-- (short-circuit `or` in sysbench.sql.lua:254).
-- [PG] The pgsql driver always reports sql_errno 0 and itself marks only SQLSTATE 40001/40P01/23505
-- as retryable (drv_pgsql.c). RCC occasionally ends a transaction with elog(ERROR) (XX000, e.g. "unexpected
-- table_tuple_lock status"), so these are downgraded to retryable as well. To keep workers alive
-- without hiding the errors, per-SQLSTATE counts are reported as [PGERR] in thread_done().
local PG_IGNORE = {["XX000"]=true, ["25P02"]=true}
local pgerr = {}
function sysbench.hooks.sql_error_ignorable(e)
   if IGNORE[tonumber(e.sql_errno)] then return true end
   if e.sql_state ~= nil and PG_IGNORE[e.sql_state] then
      pgerr[e.sql_state] = (pgerr[e.sql_state] or 0) + 1
      return true
   end
   return false
end

-- Every retry passes through here (sysbench.lua:36), including 1213 ignored by the driver.
function sysbench.hooks.before_restart_event(e)
   -- 1180 (COMMIT failure), 1205, etc. may not end the whole transaction.
   -- Clean up SSN descriptor/locks before the next BEGIN; safe even after an automatic rollback.
   con:query("ROLLBACK")
   -- [TIME] time of the aborted attempt
   wasted_us = wasted_us + (now_us() - t_att)
   n_abort = n_abort + 1
   if ABUF then
      -- [ABUF] Enqueue the failed transaction with its keys unchanged. Clearing pending lets the next
      -- event() pick a ready entry or a new transaction (the worker never idles).
      local k, w = {}, {}
      for i = 1, nops do k[i] = keys[i]; w[i] = isw[i] end
      -- [EXPBO] With SB_EXPBO=1, the penalty doubles with each retry (capped at 64x).
      -- Keeps starving transactions from hammering the queue at the same rate.
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

-- Write the line and its newline in one stdio write. print() writes them separately, so
-- stats from several thread_done() calls can share a line and be missed by ^[TIME] aggregation.
local function emit_line(line)
   io.write(line .. "\n")
end

-- [ABUF] Report the maximum buffer length at thread exit. If it keeps growing, aborts
-- accumulate faster than commits and the run's numbers are not reliable.
function thread_done()
   -- [TIME] Per-thread totals; summed across all threads to obtain execution / wasted time.
   emit_line(string.format("[TIME] commits=%d aborts=%d exec_us=%d wasted_us=%d",
      commits, n_abort, exec_us, wasted_us))
   -- [PG] Errors downgraded because the driver does not mark them retryable (see sql_error_ignorable above)
   for st, n in pairs(pgerr) do emit_line(string.format("[PGERR] sqlstate=%s n=%d", st, n)) end
   if ABUF then
      emit_line(string.format("[ABUF] maxbuf=%d leftover=%d | fresh %d attempts %d aborts(%.1f%%)"
         .. " | from-buffer %d attempts %d aborts(%.1f%%) | commits %d attempts-per-commit %.2f max %d backpressure %d",
         maxbuf, #buf, n_fresh, a_fresh, a_fresh/math.max(n_fresh,1)*100,
         n_buf, a_buf, a_buf/math.max(n_buf,1)*100,
         commits, att_sum/math.max(commits,1), att_max, nsleep))
      local h = {}
      for i = 1, #BUCK do h[#h+1] = string.format("<=%d:%d", BUCK[i], hist[i]) end
      h[#h+1] = string.format(">%d:%d", BUCK[#BUCK], hist[#BUCK+1])
      emit_line("[HIST] " .. table.concat(h, " "))
      emit_line(string.format("[HOTKEY] all avg-min-key %.0f (n=%d) | att>=32 avg-min-key %.0f (n=%d)",
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

-- oltp_common's c_value_template is local and not accessible here, so the same value is redefined
-- (10 groups, 119 characters; same as in oltp_common.lua).
local c_tmpl = "###########-###########-###########-" ..
   "###########-###########-###########-" ..
   "###########-###########-###########-" ..
   "###########"

-- n distinct keys for one transaction. Duplicates are redrawn.
local function distinct_keys(n)
   for k in pairs(seen) do seen[k] = nil end
   local got = 0
   -- Enforce distinctness by redrawing without a retry cap. A capped fallback that fills
   -- keys with 1,2,...,n would select exactly the hottest zipfian keys and create an
   -- artificial hotspot under skew. (Terminates only if table_size >= n.)
   while got < n do
      local id = sysbench.rand.default(1, sysbench.opt.table_size)
      if not seen[id] then
         seen[id] = true
         got = got + 1
         keys[got] = id
      end
   end
end

-- Draw one new transaction into keys/isw/tnum.
local function new_tx(nr, n)
   distinct_keys(n)
   -- get_table_num() is local to oltp_common and not callable here; draw the table directly.
   tnum = sysbench.rand.uniform(1, sysbench.opt.tables)
   -- Fix nr reads / nw writes and shuffle only their order (Fisher-Yates).
   for i = 1, n do isw[i] = (i > nr) end
   for i = n, 2, -1 do
      local j = math.random(i)
      isw[i], isw[j] = isw[j], isw[i]
   end
   -- [SORT] Only the access order is fixed to ascending keys; which positions are writes is unchanged.
   if SORTKEY then table.sort(keys) end
end

function event()
   local nr = sysbench.opt.point_selects
   local n = nr + sysbench.opt.non_index_updates
   nops = n

   -- [SAMEKEY] On abort, sysbench re-invokes event() from scratch
   -- (repeat...until success in sysbench.lua:33). While pending is set, keys, operation order and
   -- table are reused instead of redrawn, giving a "same-key retry"
   -- (same model as DBx1000 Bamboo/RR thread.cpp).
   if not pending then
      -- [ABUF] Scan the whole buffer and take the first entry whose ready time has passed. With only the head
      -- checked and uniformly random delays, one unready head would block every ready entry behind it
      -- (same reason as CoreWorkload.java:852). The rest rotate to the back, order preserved.
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
         -- Nothing is ready. If the buffer has room, create a new transaction (fall through below);
         -- if it is full, sleep until the earliest ready time and rescan (backpressure).
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
   exec_us = exec_us + (now_us() - t_att)   -- [TIME] time of the committed attempt
   -- [DUMPATT] SB_DUMPATT=N: on commit, print the key set of transactions that took N or more attempts
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
   pending = false            -- committed, so the next event draws new keys
   check_reconnect()
end
