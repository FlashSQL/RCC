#!/usr/bin/env sysbench
-- Copyright (C) 2006-2017 Alexey Kopytov <akopytov@gmail.com>

-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; either version 2 of the License, or
-- (at your option) any later version.

-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.

-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

-- ----------------------------------------------------------------------
-- Write-Only OLTP benchmark
-- ----------------------------------------------------------------------

require("oltp_common")

-- [BACKOFF] 어보트 후 재시도 전에 0~SB_BACKOFF_US 마이크로초 대기.
-- sysbench 는 --mysql-ignore-errors 로 잡힌 에러를 '즉시' 재시도한다(무백오프).
-- YCSB 쪽 batchtx.abortpenaltyus 와 같은 조건을 만들기 위해 넣는다.
-- 끄려면 SB_BACKOFF_US=0.
local ffi = require("ffi")
ffi.cdef[[ int usleep(unsigned int usec); ]]
local BACKOFF_US = tonumber(os.getenv("SB_BACKOFF_US") or "5000")

-- [ERRLOG] 무시된 에러의 코드 분포를 주기적으로 찍는다 (원인 확정용, 확정 후 제거)
local err_n, err_cnt, err_last = 0, {}, 0
local ERRLOG_N = tonumber(os.getenv("SB_ERRLOG_N") or "20000")
-- 주의: --mysql-ignore-errors 로 지정한 코드는 드라이버가 IGNORABLE 로 표시해
-- 이 훅을 건너뛴다(sysbench.sql.lua:254). 백오프를 적용하려면 그 옵션을 쓰지 말고
-- 여기서 무시 여부를 판정해야 한다.
local IGNORE = {[1213]=true,[1205]=true,[1020]=true,[1180]=true,[1614]=true}
function sysbench.hooks.sql_error_ignorable(e)
   err_n = err_n + 1
   local k = tostring(e.sql_errno)
   err_cnt[k] = (err_cnt[k] or 0) + 1
   if err_n - err_last >= ERRLOG_N then
      err_last = err_n
      local parts = {}
      for code, n in pairs(err_cnt) do parts[#parts+1] = code .. "=" .. n end
      io.stderr:write(string.format("[ERRLOG] t=%d thd=%d n=%d  %s  last:[%s] %s\n",
         os.time(), sysbench.tid or -1, err_n, table.concat(parts, " "),
         tostring(e.sql_errno), tostring(e.sql_errmsg):sub(1,90)))
      io.stderr:flush()
   end
   if not IGNORE[tonumber(e.sql_errno)] then return false end
   if BACKOFF_US > 0 then
      ffi.C.usleep(math.random(0, BACKOFF_US))
   end
   return true
end

function prepare_statements()
   if not sysbench.opt.skip_trx then
      prepare_begin()
      prepare_commit()
   end

   prepare_index_updates()
   prepare_non_index_updates()
   prepare_delete_inserts()
end

function event()
   if not sysbench.opt.skip_trx then
      begin()
   end

   execute_index_updates()
   execute_non_index_updates()
   execute_delete_inserts()

   if not sysbench.opt.skip_trx then
      commit()
   end

   check_reconnect()
end
