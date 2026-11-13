-- Runs the Squads pager on a computer against mocked RPC answers (Lua 5.5:
-- `lua test.lua` in this folder). Not part of the app. The accounts are built
-- here byte for byte as Squads v4 lays them out; the badge's modules are
-- stand-ins that record what the app shows. Each case checks what the owner
-- would see: a small multisig, one whose members don't fit the first read
-- (the linked wallet last), one too big to read, proposals that are missing,
-- one that exists but is garbage, one owned by another program, and one with
-- more approvals than the first read holds.

local B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
local function b58(b)
  local digits, zeros = {0}, 0
  while zeros < #b and b:byte(zeros + 1) == 0 do zeros = zeros + 1 end
  for i = 1, #b do
    local carry = b:byte(i)
    for j = 1, #digits do carry = carry + digits[j] * 256; digits[j] = carry % 58; carry = carry // 58 end
    while carry > 0 do digits[#digits + 1] = carry % 58; carry = carry // 58 end
  end
  local out = {}
  for _ = 1, zeros do out[#out + 1] = "1" end
  for j = #digits, 1, -1 do
    if not (j == 1 and digits[1] == 0 and #digits == 1) then out[#out + 1] = B58:sub(digits[j] + 1, digits[j] + 1) end
  end
  return table.concat(out)
end
local function unb58(t)
  local bytes, zeros = {0}, 0
  while zeros < #t and t:sub(zeros + 1, zeros + 1) == "1" do zeros = zeros + 1 end
  for i = 1, #t do
    local v = B58:find(t:sub(i, i), 1, true)
    if not v then return nil, "not base58" end
    local carry = v - 1
    for j = 1, #bytes do carry = carry + bytes[j] * 58; bytes[j] = carry % 256; carry = carry // 256 end
    while carry > 0 do bytes[#bytes + 1] = carry % 256; carry = carry // 256 end
  end
  local out = {}
  for _ = 1, zeros do out[#out + 1] = "\0" end
  for j = #bytes, 1, -1 do if not (j == 1 and bytes[1] == 0 and #bytes == 1) then out[#out + 1] = string.char(bytes[j]) end end
  return table.concat(out)
end
local function fake32(s)
  local h, out = 5381, {}
  for i = 1, #s do h = (h * 33 + s:byte(i)) % 4294967296 end
  for i = 1, 32 do h = (h * 1103515245 + 12345) % 4294967296; out[i] = string.char(h >> 16 & 255) end
  return table.concat(out)
end
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function base64(s)
  local out = {}
  for i = 1, #s, 3 do
    local a, b, c = s:byte(i, i + 2)
    local v = (a << 16) | ((b or 0) << 8) | (c or 0)
    out[#out + 1] = B64:sub((v >> 18) + 1, (v >> 18) + 1) .. B64:sub((v >> 12 & 63) + 1, (v >> 12 & 63) + 1)
      .. (b and B64:sub((v >> 6 & 63) + 1, (v >> 6 & 63) + 1) or "=") .. (c and B64:sub((v & 63) + 1, (v & 63) + 1) or "=")
  end
  return table.concat(out)
end

local V4 = "SQDS4ep65T869zMMBKyuUq6aD6EgTu8psMjkvj52pCf"
local MULTISIG = b58(fake32("the multisig"))
local ME = fake32("my wallet")
local function member(i) return fake32("member " .. i) end
local function pda(seeds, program) return fake32("pda" .. table.concat(seeds, "|") .. program) end
local function proposal_address(n)
  return b58(pda({"multisig", unb58(MULTISIG), "transaction", string.pack("<i8", n), "proposal"}, unb58(V4)))
end

-- Squads v4: a multisig (members: list of {key, permissions}) and a proposal.
local function multisig(threshold, tx, stale, members)
  local parts = {("\0"):rep(8), fake32("create key"), ("\0"):rep(32),
    string.pack("<I2I4I4I4I4I4", threshold, 0, tx, 0, stale, 0), "\0", "\255", string.pack("<I4", #members)}
  for _, m in ipairs(members) do parts[#parts + 1] = m[1] .. string.char(m[2]) end
  return table.concat(parts)
end
local function proposal(status, tx, approved, rejected)
  local function list(t)
    local p = {string.pack("<I4", #t)}
    for _, k in ipairs(t) do p[#p + 1] = k end
    return table.concat(p)
  end
  return ("\0"):rep(8) .. unb58(MULTISIG) .. string.pack("<I4I4", tx, 0) .. string.char(status)
    .. (status == 4 and "" or ("\0"):rep(8)) .. "\254" .. list(approved) .. list(rejected or {}) .. list({})
end

local ALL = 7                   -- initiate, vote, execute

-- ---------------------------------------------------------------------------
-- One run of the app against `accounts` (address -> {owner, data})

local function run(name, accounts, checks, later)
  local shown = {note = "", title = "", items = {}, toasts = {}}
  local calls, queue = {}, {}
  local timers, leds = {}, {}
  local env = setmetatable({}, {__index = _G})
  env.ui = {
    W = 320, H = 218,
    bg = function() end, hint = function() end, toast = function(t) shown.toasts[#shown.toasts + 1] = t end,
    input = function() end,
    label = function(_, y, _, _)
      local l = {}
      function l:text(t) if y == 24 then shown.note = t elseif y == 4 then shown.title = t end end
      return l
    end,
    list = function(_, _, _, _, items)
      shown.items = items
      return {items = function(_, t) shown.items = t end}
    end,
  }
  env.storage = {get = function() return nil end, set = function() end}
  env.fs = {exists = function(f) return f == "multisig.txt" end, read = function() return MULTISIG end}
  env.timer = {every = function(ms, f) timers[#timers + 1] = {ms = ms, f = f} end, after = function() end}
  env.led = {count = 8, set = function(i, r, g, b) leds[i] = {r, g, b} end}
  local now = 0
  env.sys = {millis = function() return now end}
  env.profile = {me = function(cb) cb(true, {solana = b58(ME)}) end}
  env.solana = {encode = b58, decode = unb58, pda = pda}
  env.rpc = {call = function(method, params, opts, cb)
    calls[#calls + 1] = {method = method, params = params}
    queue[#queue + 1] = function()
      local slice = params[2].dataSlice
      local function one(addr)
        local a = accounts[addr]
        if not a then return nil end
        local data = a.data:sub(slice.offset + 1, slice.offset + slice.length)
        return {owner = a.owner, data = {base64(data), "base64"}}
      end
      local value, size = nil, 0
      if method == "getAccountInfo" then
        value = one(params[1])
        size = value and #value.data[1] or 0
      else
        value = {}
        for i, addr in ipairs(params[1]) do
          value[i] = one(addr)
          size = size + (value[i] and #value[i].data[1] or 0)
        end
      end
      assert(size + 120 * (method == "getAccountInfo" and 1 or #params[1]) <= 3900,
             name .. ": an answer over 3900 bytes (" .. size .. " of base64)")
      cb(true, {value = value})
    end
  end}
  local chunk = assert(loadfile("main.lua", "t", env))
  chunk()
  local function settle()
    for _ = 1, 400 do              -- ticks and answers, one by one, until nothing is left to do
      now = now + 30
      if env.on_tick then env.on_tick() end
      local q = table.remove(queue, 1)
      if q then q() end
    end
  end
  local function fire(ms) for _, t in ipairs(timers) do if t.ms == ms then t.f() end end end
  settle()
  if later then                    -- the accounts change, and the next round reads them
    later(accounts)
    now = now + 20000
    fire(20000)
    settle()
  end
  fire(5000)                       -- the "s ago" line
  fire(500)                        -- the LEDs, lit
  shown.alarm = leds[1] and leds[1][1] == 255
  local ok, err = pcall(checks, shown, calls)
  print(("%s %s%s"):format(ok and "ok  " or "FAIL", name, ok and "" or ": " .. tostring(err)))
  if not ok then
    print("     note: " .. shown.note)
    for _, it in ipairs(shown.items) do print("     row: " .. (type(it) == "table" and (it.title .. " | " .. it.sub .. " | " .. it.right) or it)) end
  end
  return ok
end

local function owned(data) return {owner = V4, data = data} end
local function has_row(shown, title, right)
  for _, it in ipairs(shown.items) do
    if type(it) == "table" and it.title:find(title, 1, true) and (right == nil or it.right == right) then return it end
  end
  error("no row " .. title .. (right and " " .. right or ""))
end
local function count(calls, method)
  local n = 0
  for _, c in ipairs(calls) do n = n + (c.method == method and 1 or 0) end
  return n
end

local failed = 0
local function case(...) if not run(...) then failed = failed + 1 end end

-- A small multisig: 3 members, me among them; #5 waits for my vote, #4 has no
-- proposal, #3 is executed.
case("small multisig", {
  [MULTISIG] = owned(multisig(2, 5, 0, {{member(1), ALL}, {ME, ALL}, {member(2), ALL}})),
  [proposal_address(5)] = owned(proposal(1, 5, {member(1)})),
  [proposal_address(3)] = owned(proposal(5, 3, {member(1), ME})),
}, function(shown, calls)
  assert(shown.note:find("you are a member"), "member: " .. shown.note)
  has_row(shown, "#5  Active", "SIGN")
  has_row(shown, "#3  Executed", "")
  assert(#shown.items == 2, #shown.items .. " rows")
  assert(shown.alarm, "LEDs not blinking")
  assert(count(calls, "getAccountInfo") == 1, "multisig read more than once")
end)

-- 120 members (about 4 KB): the first read holds about 40, and my wallet is last.
local big = {}
for i = 1, 119 do big[i] = {member(i), ALL} end
big[120] = {ME, ALL}
case("120 members, me last", {
  [MULTISIG] = owned(multisig(60, 2, 0, big)),
  [proposal_address(2)] = owned(proposal(1, 2, {member(1)})),
}, function(shown, calls)
  assert(shown.note:find("you are a member"), "member: " .. shown.note)
  has_row(shown, "#2  Active", "SIGN")
  assert(shown.alarm, "LEDs not blinking")
  assert(count(calls, "getAccountInfo") == 3, count(calls, "getAccountInfo") .. " multisig reads")
end)

-- 300 members: more than four reads hold. An error, never a partial list.
local huge = {}
for i = 1, 299 do huge[i] = {member(i), ALL} end
huge[300] = {ME, ALL}
case("300 members", {
  [MULTISIG] = owned(multisig(2, 2, 0, huge)),
  [proposal_address(2)] = owned(proposal(1, 2, {member(1)})),
}, function(shown)
  assert(shown.note:find("Too many members"), "note: " .. shown.note)
  assert(not shown.note:find("not a member"), "said not a member")
end)

-- No proposals at all: a fine round.
case("missing proposals", {
  [MULTISIG] = owned(multisig(2, 3, 0, {{ME, ALL}, {member(1), ALL}})),
}, function(shown)
  assert(shown.items[1] == "No open proposals", "rows: " .. tostring(shown.items[1]))
  assert(shown.note:find("you are a member"), "note: " .. shown.note)
end)

-- A proposal account that exists but is garbage: the round fails, it never
-- reads as "no proposal".
case("garbage proposal", {
  [MULTISIG] = owned(multisig(2, 3, 0, {{ME, ALL}, {member(1), ALL}})),
  [proposal_address(3)] = owned("garbage"),
  [proposal_address(2)] = owned(proposal(1, 2, {member(1)})),
}, function(shown)
  assert(shown.note:find("Can't read proposal #3"), "note: " .. shown.note)
  assert(shown.items[1] ~= "No open proposals", "said no open proposals")
end)

-- A good round, then the proposal turns to garbage: the error stays on screen
-- (the "s ago" line says the rows are the last good round's), not "No open
-- proposals" and not the old line as if all were well.
case("good round, then garbage", {
  [MULTISIG] = owned(multisig(2, 3, 0, {{ME, ALL}, {member(1), ALL}})),
  [proposal_address(3)] = owned(proposal(1, 3, {member(1)})),
}, function(shown)
  assert(shown.note:find("Can't read proposal #3, last read"), "note: " .. shown.note)
  has_row(shown, "#3  Active", "SIGN")
end, function(accounts) accounts[proposal_address(3)] = owned("garbage") end)

case("proposal with a bad status", {
  [MULTISIG] = owned(multisig(2, 3, 0, {{ME, ALL}, {member(1), ALL}})),
  [proposal_address(3)] = owned(proposal(9, 3, {})),
}, function(shown)
  assert(shown.note:find("Can't read proposal #3"), "note: " .. shown.note)
end)

case("proposal owned by another program", {
  [MULTISIG] = owned(multisig(2, 3, 0, {{ME, ALL}, {member(1), ALL}})),
  [proposal_address(3)] = {owner = "11111111111111111111111111111111", data = proposal(1, 3, {})},
}, function(shown)
  assert(shown.note:find("Can't read proposal #3"), "note: " .. shown.note)
end)

-- 15 approvals, mine the 14th: past the first 420 bytes. Read on, so the badge
-- doesn't ask me to sign what I signed.
local approvers = {}
for i = 1, 15 do approvers[i] = i == 14 and ME or member(i) end
local voters = {{ME, ALL}}
for i = 1, 15 do voters[#voters + 1] = {member(i), ALL} end
case("15 approvals, mine 14th", {
  [MULTISIG] = owned(multisig(16, 1, 0, voters)),
  [proposal_address(1)] = owned(proposal(1, 1, approvers, {member(15)})),
}, function(shown)
  local row = has_row(shown, "#1  Active", "")
  assert(row.sub == "15 of 16 approved, 1 rejected", row.sub)
  assert(not shown.alarm, "asked me to sign again")
end)

print(failed == 0 and "squads-pager: all ok" or ("squads-pager: " .. failed .. " failed"))
os.exit(failed == 0 and 0 or 1)
