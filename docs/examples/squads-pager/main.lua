-- Squads pager: lights up when a proposal of your multisig waits for you.
--
-- The badge reads the multisig's account, works out where the proposals of its
-- latest transactions live (solana.pda), reads those, and compares who has
-- approved with the wallet you linked at /profile. A proposal you can still
-- vote on, or one that is approved and that you may execute, blinks the LEDs.
-- Signing stays where your key is: the badge only reads.
--
-- Permissions "rpc", "profile", "led". Needs the solana module (Solana OS
-- 2.1.0). Works for Squads v4 multisigs and smart accounts.
--
-- Which multisig: the address you typed on the badge (kept in storage), else
-- the first word of a bundled multisig.txt, else the app asks.

local POLL_MS = 20000           -- 3 RPC calls a round, of the 30 a minute a badge gets
local RECENT = 8                -- the newest transactions looked at
local BATCH = 4                 -- proposals per getMultipleAccounts: the answer must stay under 3.5 KB
local SLICE = 420               -- bytes read of each proposal: room for 10 approvals
local FIRST = 1500              -- bytes read of the multisig first: room for about 40 members
local CHUNK = 2400              -- a further read of one account (3.2 KB as base64)
local MORE = 3                  -- further reads an account may take a round: then it's too big

-- What differs between the two programs: where the fields sit (bytes from the
-- start of the account) and the first seed of their addresses.
local PROGRAMS = {
  SQDS4ep65T869zMMBKyuUq6aD6EgTu8psMjkvj52pCf =
    {name = "Squads v4", prefix = "multisig", threshold = 72, skip = 1, status = 48},
  SMRTzfY6DfH5ik3TKiyLFfXexV8uSG3d2UksSCYdunG =
    {name = "Smart account", prefix = "smart_account", threshold = 56, skip = 9, status = 80},
}
local STATUS = {[0] = "Draft", "Active", "Rejected", "Approved", "Executing", "Executed", "Cancelled"}
local ACTIVE, APPROVED = 1, 3
local VOTE, EXECUTE = 2, 4      -- bits of a member's permissions

ui.bg("bg")
local title = ui.label(8, 4, "Squads pager", {w = 304})
local note = ui.label(8, 24, "", {size = 12, color = "dim", w = 304})
if not (solana and solana.pda) then
  note:text("Needs a newer Solana OS (solana.pda)")
  ui.hint("CANCEL exit")
  return
end

for id, lay in pairs(PROGRAMS) do lay.key, lay.id = solana.decode(id), id end

local cluster = storage.get("cluster") == "devnet" and "devnet" or "mainnet"
local address                   -- the multisig, as text
local seeds_key                 -- and as its 32 bytes
local me                        -- the owner's wallet, 32 bytes, once known
local squad                     -- {lay, threshold, tx, stale, members = {[key] = permissions}}
local proposal_at = {}          -- transaction number -> its proposal's address, as text
local proposals = {}            -- transaction number -> what the last round read
local wanted = {}               -- the transaction numbers of this round, newest first
local phase = "idle"            -- "multisig" > "derive" > "proposals" > "idle"
local waiting = 0               -- proposal batches not answered yet
local round = 0                 -- answers of an older round, or of another multisig, are dropped
local last_ok                   -- sys.millis() of the last complete round
local problem                   -- why the last round failed, until one doesn't
local alerted = {}              -- transaction numbers already announced
local alarm = false             -- something waits for the owner
local progress, threshold = 0, 0
local who = ""                  -- the second line: which program, and whether the owner is a member
local refresh                   -- defined below; the list's SELECT calls it
local typed = false             -- the address came from the keyboard: keep it once it reads as a multisig

local list = ui.list(0, 42, ui.W, ui.H - 16 - 42, {"Starting..."}, {on_select = function() refresh() end})
ui.hint("SELECT refresh  RIGHT address  LEFT network")

local function short(a) return a:sub(1, 4) .. ".." .. a:sub(-4) end

-- ---------------------------------------------------------------------------
-- Reading accounts

local B64 = {}
for i = 1, 64 do
  B64[("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"):byte(i)] = i - 1
end

-- base64 -> bytes, or nil when it isn't base64. Account data is the node's
-- word: checked like anything else that arrives.
local function unbase64(s)
  if type(s) ~= "string" or #s % 4 ~= 0 then return nil end
  local out, pad = {}, 0
  if s:sub(-2) == "==" then pad = 2 elseif s:sub(-1) == "=" then pad = 1 end
  for i = 1, #s, 4 do
    local a, b, c, d = s:byte(i, i + 3)
    local last = i + 3 >= #s
    a, b = B64[a], B64[b]
    c = B64[c] or (last and pad == 2 and 0)
    d = B64[d] or (last and pad >= 1 and 0)
    if not (a and b and c and d) then return nil end
    local v = (a << 18) | (b << 12) | (c << 6) | d
    out[#out + 1] = string.char(v >> 16, (v >> 8) & 255, v & 255)
  end
  local bytes = table.concat(out)
  return pad > 0 and bytes:sub(1, -pad - 1) or bytes
end

-- The readers below take the bytes read so far and return what they read,
-- or nil and how many bytes they need when a list goes on past them (the
-- round then reads the rest, more_of): a list that is cut short is never
-- taken for the whole list. Anything else that doesn't fit raises an error:
-- the account isn't what it should be.

-- A list of addresses as the programs store one: a count, then 32 bytes each.
-- Returns the set, the count and where the list ends, or nil and the bytes
-- the list needs.
local function keys(data, pos)
  local n = string.unpack("<I4", data, pos)
  local stop = pos + 4 + n * 32
  if stop - 1 > #data then return nil, stop - 1 end
  local set = {}
  for i = 1, n do set[data:sub(pos + 4 + (i - 1) * 32, pos + 3 + i * 32)] = true end
  return set, n, stop
end

-- A number of 8 bytes, which must fit a badge's 32-bit integers.
local function u64(data, pos)
  local lo, hi = string.unpack("<I4I4", data, pos)
  if hi ~= 0 or lo < 0 then error("number too big") end
  return lo
end

local function read_multisig(data, lay)
  local pos = lay.threshold + 1
  local m = {lay = lay, members = {}}
  m.threshold = string.unpack("<I2", data, pos)
  m.tx = u64(data, pos + 6)                               -- after the threshold and the time lock
  m.stale = u64(data, pos + 14)
  pos = pos + 22
  pos = pos + (data:byte(pos) == 1 and 33 or 1) + lay.skip  -- an optional address, then the bump
  local n = string.unpack("<I4", data, pos)
  pos = pos + 4
  if pos + n * 33 - 1 > #data then return nil, pos + n * 33 - 1 end   -- more members than we have read
  for _ = 1, n do
    m.members[data:sub(pos, pos + 31)] = data:byte(pos + 32)
    pos = pos + 33
  end
  m.count = n
  return m
end

local function read_proposal(data, lay)
  local pos = lay.status + 1
  local p = {status = data:byte(pos)}
  if not STATUS[p.status] then error("no such status") end
  pos = pos + (p.status == 4 and 1 or 9) + 1              -- every status but Executing has a time; then the bump
  local need
  p.approved, need, pos = keys(data, pos)
  if not p.approved then return nil, need end
  p.approvals = need
  if pos + 3 > #data then return nil, pos + 3 end         -- the rejections' count is past what we read
  p.rejected, need = keys(data, pos)
  if not p.rejected then return nil, need end
  p.rejections = need
  return p
end

-- ---------------------------------------------------------------------------
-- What the owner sees

local function waits_for_me(p)
  local perms = me and squad and squad.members[me]
  if not perms then return nil end
  if p.status == ACTIVE and perms & VOTE ~= 0 and not p.approved[me] and not p.rejected[me] then return "SIGN" end
  if p.status == APPROVED and perms & EXECUTE ~= 0 then return "EXEC" end
  return nil
end

local function show()
  local rows, newest = {}, nil
  alarm = false
  for _, n in ipairs(wanted) do
    local p = proposals[n]
    if p then
      local what = waits_for_me(p)
      if what then
        alarm = true
        if not alerted[n] then
          alerted[n] = true
          ui.toast("Proposal " .. n .. (what == "SIGN" and " needs your signature" or " is ready to execute"))
        end
      end
      if p.status == ACTIVE and not newest then newest = p end
      local sub = p.approvals .. " of " .. squad.threshold .. " approved"
      if p.rejections > 0 then sub = sub .. ", " .. p.rejections .. " rejected" end
      rows[#rows + 1] = {title = "#" .. n .. "  " .. (STATUS[p.status] or "?"), sub = sub, right = what or ""}
    end
  end
  progress, threshold = newest and newest.approvals or 0, newest and squad.threshold or 0
  list:items(#rows > 0 and rows or {"No open proposals"})
  if not me then
    who = "link a wallet at /profile"
  else
    who = squad.members[me] and squad.lay.name .. ", you are a member" or "your wallet is not a member"
  end
  note:text(who)
  title:text(short(address) .. "  " .. squad.threshold .. " to approve  " .. cluster)
end

local function fail(text)
  phase = "idle"
  problem = text
  round = round + 1                                      -- the rest of this round's answers are dropped
  note:text(text)
end

-- The LEDs: blinking orange while something waits for the owner, else the
-- newest active proposal's approvals in green out of the threshold in grey.
local lit = false
timer.every(500, function()
  lit = not lit
  for i = 1, led.count do
    if alarm and lit then
      led.set(i, 255, 110, 0)
    elseif i <= progress then
      led.set(i, 0, 160, 60)
    elseif i <= threshold then
      led.set(i, 12, 12, 12)
    else
      led.set(i, 0, 0, 0)
    end
  end
end)

-- ---------------------------------------------------------------------------
-- A round: the multisig, the addresses of its newest proposals, the proposals

-- An account's data from rpc, checked: nil when it isn't base64 of the
-- program's account.
local function account_data(acc, lay)
  if type(acc) ~= "table" or acc.owner ~= lay.id or type(acc.data) ~= "table" then return nil end
  return unbase64(acc.data[1])
end

-- Reads on from `data` (the account's first bytes) until read(data, lay)
-- has all it needs, CHUNK bytes a call and at most MORE calls, then calls
-- done(value) or done(nil, why). `why` is "big" when it took too many reads.
local function more_of(addr, data, read, lay, mine, done)
  local tries = 0
  local function step()
    local good, v, need = pcall(read, data, lay)
    if not good then return done(nil, "bad") end
    if v then return done(v) end
    if tries == MORE then return done(nil, "big") end
    tries = tries + 1
    rpc.call("getAccountInfo", {addr, {encoding = "base64", commitment = "confirmed",
      dataSlice = {offset = #data, length = math.min(CHUNK, need - #data)}}}, {cluster = cluster}, function(ok, r)
        if mine ~= round then return end
        if not ok then return done(nil, tostring(r)) end
        local more = account_data(type(r) == "table" and r.value, lay)
        if not more or #more == 0 then return done(nil, "bad") end   -- gone, or shorter than its own lists
        data = data .. more
        step()
      end)
  end
  step()
end

local function round_done()
  phase = "idle"
  problem = nil
  last_ok = sys.millis()
  show()
end

local function fetch_proposals()
  local mine = round
  phase = "proposals"
  waiting = 0
  local function settle()
    if mine ~= round then return end                     -- the round failed meanwhile
    waiting = waiting - 1
    if waiting == 0 then round_done() end
  end
  for from = 1, #wanted, BATCH do
    local numbers, addrs = {}, {}
    for i = from, math.min(from + BATCH - 1, #wanted) do
      numbers[#numbers + 1] = wanted[i]
      addrs[#addrs + 1] = proposal_at[wanted[i]]
    end
    waiting = waiting + 1
    rpc.call("getMultipleAccounts",
      {addrs, {encoding = "base64", commitment = "confirmed", dataSlice = {offset = 0, length = SLICE}}},
      {cluster = cluster}, function(ok, r)
        if mine ~= round then return end
        if not ok or type(r) ~= "table" or type(r.value) ~= "table" then return fail("Proposals: " .. tostring(r)) end
        for i, n in ipairs(numbers) do
          if mine ~= round then return end
          local acc = r.value[i]                         -- null: no account, a transaction without a proposal
          if acc == nil then
            proposals[n] = nil
          else
            -- There is an account: it is read whole, or the round fails. One
            -- that can't be read is never shown as "no proposal".
            local data = account_data(acc, squad.lay)
            if not data then return fail("Can't read proposal #" .. n) end
            waiting = waiting + 1
            more_of(addrs[i], data, read_proposal, squad.lay, mine, function(p, why)
              if not p then
                return fail(why == "big" and "Proposal #" .. n .. " has too many votes to read"
                            or "Can't read proposal #" .. n .. (why == "bad" and "" or ": " .. why))
              end
              proposals[n] = p
              settle()
            end)
          end
        end
        settle()
      end)
  end
  if waiting == 0 then round_done() end
end

local took                      -- defined below poll

local function poll()
  if phase ~= "idle" or not address then return end
  round = round + 1
  local mine = round
  phase = "multisig"
  if not me then
    profile.me(function(ok, p)
      local k = ok and type(p) == "table" and type(p.solana) == "string" and solana.decode(p.solana)
      if k and #k == 32 then me = k end
    end)
  end
  rpc.call("getAccountInfo", {address, {encoding = "base64", commitment = "confirmed",
    dataSlice = {offset = 0, length = FIRST}}}, {cluster = cluster}, function(ok, r)
      if mine ~= round then return end
      if not ok then return fail("Can't read the multisig: " .. tostring(r)) end
      local acc = type(r) == "table" and r.value
      if type(acc) ~= "table" then return fail("No such account on " .. cluster .. ". RIGHT changes it") end
      local lay = PROGRAMS[acc.owner]
      if not lay then return fail("Not a Squads multisig (the vault's address?). RIGHT changes it") end
      local data = account_data(acc, lay)
      if not data then return fail("Can't read that account") end
      -- A big multisig's members go on past the first read: read them all,
      -- since the linked wallet may be among the last.
      more_of(address, data, read_multisig, lay, mine, function(m, why)
        if not m then
          if why == "big" then return fail("Too many members to read (" .. FIRST + MORE * CHUNK .. " bytes)") end
          return fail("Can't read that account" .. (why == "bad" and "" or ": " .. why))
        end
        took(m)
      end)
    end)
end

-- The multisig is read: which proposals to look at, then their addresses.
took = function(m)
  squad = m
  if typed then
    typed = false
    storage.set("multisig", address)
  end
  wanted = {}
  for n = m.tx, math.max(m.stale + 1, m.tx - RECENT + 1, 1), -1 do wanted[#wanted + 1] = n end
  phase = "derive"                                       -- on_tick works the addresses out, one a tick
end

function on_tick()
  if phase ~= "derive" then return end
  for _, n in ipairs(wanted) do
    if not proposal_at[n] then
      local a = solana.pda({squad.lay.prefix, seeds_key, "transaction", string.pack("<i8", n), "proposal"},
        squad.lay.key)
      proposal_at[n] = solana.encode(a)
      return
    end
  end
  fetch_proposals()
end

-- ---------------------------------------------------------------------------
-- Which multisig

local function watch(text)
  local k = solana.decode(text)
  if not k or #k ~= 32 then return false end
  address, seeds_key = text, k
  squad, proposal_at, proposals, wanted, alerted = nil, {}, {}, {}, {}
  alarm, progress, threshold, last_ok, problem = false, 0, 0, nil, nil
  round = round + 1                                      -- drops what the old address still has on its way
  phase = "idle"
  title:text(short(address) .. "  " .. cluster)
  note:text("Reading...")
  list:items({"Reading..."})
  poll()
  return true
end

local function ask()
  ui.input("Multisig address", 44, function(text)
    text = text and text:match("%S+")
    if not text then return end
    if watch(text) then
      typed = true
    else
      ui.toast("Not a Solana address")
    end
  end)
end

refresh = function()
  if address then
    note:text("Reading...")
    poll()
  else
    ask()
  end
end

function on_key(k)
  if k == "select" then
    refresh()
    return true
  elseif k == "right" then
    ask()
    return true
  elseif k == "left" then
    cluster = cluster == "mainnet" and "devnet" or "mainnet"
    storage.set("cluster", cluster)
    if address then watch(address) end
    return true
  end
  return false
end

timer.every(POLL_MS, poll)
timer.every(5000, function()
  if phase == "idle" and last_ok and squad then
    -- After a failed round the screen still shows the last good one: say why
    -- it is that old, rather than hiding the error.
    note:text((problem or who) .. ", " .. (problem and "last read " or "") .. (sys.millis() - last_ok) // 1000 .. " s ago")
  end
end)

local saved = storage.get("multisig")
local bundled = fs.exists("multisig.txt") and (fs.read("multisig.txt") or ""):match("%S+")
if not (type(saved) == "string" and watch(saved)) and not (bundled and watch(bundled)) then
  list:items({"No multisig yet"})
  note:text("SELECT to type its address")
end
