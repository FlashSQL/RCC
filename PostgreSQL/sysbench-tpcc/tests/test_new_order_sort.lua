-- Run with LuaJIT; all database and sysbench calls are mocked.
package.loaded.tpcc_common = true
local test_dir = debug.getinfo(1, "S").source:sub(2):match("^(.*)/")
dofile(test_dir .. "/../tpcc_run.lua")

DIST_PER_WARE, CUST_PER_DIST, MAXITEMS = 10, 3000, 100000
local ignored_errors = 0
ffi = {C = {SB_CNT_ERROR = 1, sb_counter_inc = function()
    ignored_errors = ignored_errors + 1
end}}
now_us = function() return 0 end
retry_backoff = function(attempt)
    assert(attempt == 0, "unexpected repeated failure")
    return 0
end

local function equal(actual, expected)
    assert(actual == expected,
           string.format("expected %s, got %s", tostring(expected), tostring(actual)))
end

local function same_sequence(actual, expected)
    equal(#actual, #expected)
    for i = 1, #expected do equal(actual[i], expected[i]) end
end

-- Each input line is {item, supply warehouse, quantity}.
local function set_inputs(lines, rollback)
    local uniform = {1, 1, 1, #lines, rollback and 1 or 2}
    local nurand = {1}
    local remote = {}
    for _, line in ipairs(lines) do
        nurand[#nurand + 1] = line[1]
        uniform[#uniform + 1] = line[2] == 1 and 2 or 1
        uniform[#uniform + 1] = line[3]
        if line[2] ~= 1 then remote[#remote + 1] = line[2] end
    end
    local u, n, r = 0, 0, 0
    sysbench = {tid = 0, opt = {tables = 1, scale = 4, splittable = "no",
                              retry_on_fail = "yes"}, rand = {}}
    sysbench.rand.uniform = function(lo, hi)
        u = u + 1
        local value = assert(uniform[u], "inputs redrawn during retry")
        assert(value >= lo and value <= hi)
        return value
    end
    NURand = function(_, lo, hi)
        n = n + 1
        local value = assert(nurand[n], "inputs redrawn during retry")
        assert(value >= lo and value <= hi)
        return value
    end
    other_ware = function()
        r = r + 1
        return assert(remote[r])
    end
    return function()
        equal(u, #uniform)
        equal(n, #nurand)
        equal(r, #remote)
    end
end

local function database(fail_commit)
    local db = {attempts = {}}
    local current
    local function record(sql)
        sql = sql:gsub("%s+", " ")
        current.sql[#current.sql + 1] = sql
        return sql
    end
    function db:query_row(sql)
        sql = record(sql)
        if sql:match("^SELECT c_discount") then return 0, "last", "GC", 0 end
        if sql:match("^SELECT d_next_o_id") then return 100, 0 end
        assert(sql:match("^SELECT s_quantity"), sql)
        local item, warehouse = sql:match("s_i_id = (%d+) AND s_w_id= (%d+) FOR UPDATE")
        assert(item and warehouse, sql)
        local key = warehouse .. ":" .. item
        current.locks[#current.locks + 1] = key
        return current.stock[key] or 100, "data", "dist"
    end
    function db:query(sql)
        if sql == "BEGIN" then
            current = {sql = {}, locks = {}, updates = {}, lines = {},
                       items = {}, stock = {}}
            self.attempts[#self.attempts + 1] = current
        end
        sql = record(sql)
        if sql:match("^SELECT i_price") then
            local item = tonumber(sql:match("i_id = (%-?%d+)"))
            current.items[#current.items + 1] = item
            return {nrows = item >= 1 and item <= MAXITEMS and 1 or 0,
                    nfields = 3, fetch_row = function() return {2, "item", "data"} end}
        elseif sql:match("^UPDATE stock") then
            local quantity, item, warehouse = sql:match(
                "SET s_quantity = (%d+) WHERE s_i_id = (%d+) AND s_w_id= (%d+)")
            assert(quantity and item and warehouse, sql)
            local key = warehouse .. ":" .. item
            current.stock[key] = tonumber(quantity)
            current.updates[#current.updates + 1] = key
        elseif sql:match("^INSERT INTO order_line") then
            local values = assert(sql:match("VALUES %((.*)%)"), sql)
            local row = {}
            for value in values:gmatch("[^,]+") do row[#row + 1] = tonumber(value) or value end
            current.lines[#current.lines + 1] = row
        elseif sql == "COMMIT" then
            if fail_commit and #self.attempts == 1 then error("injected commit abort") end
            current.committed = true
        elseif sql == "ROLLBACK" then
            current.rolled_back = true
        elseif sql ~= "BEGIN" then
            assert(sql:match("^UPDATE district") or sql:match("^INSERT INTO orders")
                   or sql:match("^INSERT INTO new_orders"), sql)
        end
    end
    return db
end

local function check_attempt(p, attempt)
    local keys, stocks = {}, {}
    local rollback = false
    for position, index in ipairs(p.ol_num_seq) do
        equal(attempt.items[position], p.itemid[index])
        if p.itemid[index] == MAXITEMS + 1 then
            rollback = true
            equal(#attempt.items, position)
            break
        end
        local key = p.supware[index] .. ":" .. p.itemid[index]
        keys[#keys + 1] = key
        local row = assert(attempt.lines[position])
        same_sequence({row[1], row[2], row[3], row[4]}, {100, p.d_id, p.w_id, position})
        same_sequence({row[5], row[6], row[7], row[8]},
                      {p.itemid[index], p.supware[index], p.qty[index], 2 * p.qty[index]})
        local quantity = stocks[key] or 100
        stocks[key] = quantity > p.qty[index] and quantity - p.qty[index]
                     or quantity - p.qty[index] + 91
    end
    same_sequence(attempt.locks, keys)
    same_sequence(attempt.updates, keys)
    equal(#attempt.lines, #keys)
    for key, quantity in pairs(stocks) do equal(attempt.stock[key], quantity) end
    if rollback then
        assert(attempt.rolled_back and not attempt.committed)
        equal(attempt.sql[#attempt.sql], "ROLLBACK")
    else
        equal(#attempt.items, p.ol_cnt)
    end
end

local function run_case(lines, order, rollback)
    local consumed = set_inputs(lines, rollback)
    local p = new_order_prep()
    consumed()
    same_sequence(p.ol_num_seq, order)
    for i, line in ipairs(lines) do
        equal(p.itemid[i], rollback and i == #lines and MAXITEMS + 1 or line[1])
        equal(p.supware[i], line[2])
        equal(p.qty[i], line[3])
    end
    con = database()
    local errors_before = ignored_errors
    new_order_impl(p)
    check_attempt(p, con.attempts[1])
    equal(ignored_errors - errors_before, rollback and 1 or 0)
    if not rollback then assert(con.attempts[1].committed) end
    return p, con.attempts[1]
end

local _, first = run_case({{65735, 1, 2}, {56895, 1, 3}, {90000, 1, 4},
                          {10, 1, 5}, {80000, 1, 6}}, {4, 2, 1, 5, 3})
local _, second = run_case({{56895, 1, 3}, {65735, 1, 2}, {80000, 1, 6},
                           {90000, 1, 4}, {10, 1, 5}}, {5, 1, 2, 3, 4})
same_sequence(first.locks, second.locks)

local mixed = {{20, 2, 2}, {80, 1, 3}, {10, 2, 4}, {80, 1, 5}, {1, 3, 6}}
local mixed_order = {2, 4, 3, 1, 5}
local p = run_case(mixed, mixed_order)
run_case({{500, 1, 2}, {100, 1, 3}, {900, 1, 4}, {10, 1, 5}, {1, 1, 6}},
         {4, 2, 1, 3, 5}, true)
-- A missing item sorts last within its warehouse, not after all warehouses.
run_case({{400, 2, 2}, {600, 1, 3}, {100, 3, 4}, {900, 1, 5}, {7, 1, 6}},
         {2, 4, 5, 1, 3}, true)

local consumed = set_inputs(mixed, false)
con = database(true)
no_att, no_abrt, no_commits = 0, 0, 0
new_order()
consumed()
equal(no_att, 2)
equal(no_abrt, 1)
equal(no_commits, 1)
equal(#con.attempts, 2)
check_attempt(p, con.attempts[1])
check_attempt(p, con.attempts[2])
assert(con.attempts[1].rolled_back and con.attempts[2].committed)
for i, sql in ipairs(con.attempts[2].sql) do equal(con.attempts[1].sql[i], sql) end
equal(#con.attempts[1].sql, #con.attempts[2].sql + 1)

set_inputs(mixed, false)
sysbench.opt.retry_on_fail = "no"
con = database()
new_order()
equal(#con.attempts, 1)
assert(con.attempts[1].committed)
check_attempt(p, con.attempts[1])

-- Check larger generated orders against tpcc-mysql's numeric sort key.
math.randomseed(19)
for _ = 1, 200 do
    local lines, order = {}, {}
    for i = 1, math.random(5, 15) do
        lines[i] = {math.random(1, MAXITEMS), math.random(1, 4), math.random(1, 10)}
        order[i] = i
    end
    local rollback = math.random(1, 2) == 1
    table.sort(order, function(a, b)
        local function key(i)
            local item = rollback and i == #lines and MAXITEMS + 1 or lines[i][1]
            return (MAXITEMS + 1) * lines[i][2] + item
        end
        return key(a) < key(b) or (key(a) == key(b) and a < b)
    end)
    run_case(lines, order, rollback)
end

print("PASS: NewOrder SQL order, paired inputs, duplicates, rollback, retry, and 200 generated orders")
