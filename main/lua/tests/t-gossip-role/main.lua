-- Regression test (not shipped): the role side of the gossip module (Solana
-- OS 2.4.0, LUA_API.md "gossip"). Run as itself it checks that every acting
-- call is the Lab's alone; installed under the id gossip-lab (the headless
-- suite tests/gossip-role/run.ts does that) it checks they answer for real.
-- on_gossip prints the role side's events. Lines start "t-gossip-role".
-- Which one we are: only the Lab gets past "lab only" (no API names the app id).
local lab = select(2, gossip.fork()) ~= "lab only"
local fails = 0

local function check(cond, what)
  if not cond then fails = fails + 1 end
  print(("t-gossip-role %s %s"):format(cond and "ok" or "FAIL", what))
end

check(type(gossip) == "table", "the gossip table")
for _, f in ipairs({"role", "boss", "bids", "voters", "bid", "claim", "cure", "vote", "role_answer",
                    "role_end", "block", "fork"}) do
  check(type(gossip[f]) == "function", "gossip." .. f)
end

local r = gossip.role()
check(r == nil or type(r) == "table", "role(): nil or a table")
check(r == nil or r.key == nil, "role(): never the role key")
local b = gossip.boss()
check(b == nil or (type(b.beat) == "number" and type(b.ms) == "number"), "boss(): nil or a fight")

local acting = {
  {"bid", function() return gossip.bid("RABCDE", "59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruo", false) end},
  {"claim", function() return gossip.claim("RABCDE", 3) end},
  {"cure", function() return gossip.cure("ABCDEF") end},
  {"vote", function() return gossip.vote(1, 1) end},
  {"role_answer", function() return gossip.role_answer(true) end},
  {"role_end", function() return gossip.role_end() end},
  {"block", function() return gossip.block({{p = "59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruo", by = "ABCDEF"}}) end},
  {"fork", function() return gossip.fork() end},
}
for _, a in ipairs(acting) do
  local ok, err = a[2]()
  if lab then
    -- Nothing is near and no role is on: each says why, never "lab only".
    check(ok == nil and type(err) == "string" and err ~= "lab only", a[1] .. ": " .. tostring(err))
  else
    check(ok == nil and err == "lab only", a[1] .. ": lab only")
  end
end
local bids, berr = gossip.bids()
check(lab and (bids == nil and berr == nil) or (not lab and berr == "lab only"), "bids()")
local voters, verr = gossip.voters()
check(lab and (voters == nil and verr == nil) or (not lab and verr == "lab only"), "voters()")

-- Bad arguments are the caller's error (raised), not the service's.
check(not pcall(gossip.claim, "RABCDE"), "claim without a count raises")
check(select(2, gossip.claim("RABCDE", 10000)) == (lab and "count: 0-9999" or "lab only"), "claim count bounded")
check(select(2, gossip.vote(1, 64)) == (lab and "votes 0-9999, lockout 0-32" or "lab only"), "lockout bounded")
check(not pcall(gossip.block, "x"), "block wants a table")

function on_gossip(ev)
  print(("t-gossip-role event %s %s"):format(tostring(ev.t), tostring(ev.state or ev.role or ev.from or "")))
end

print(("t-gossip-role done %s, %d failed"):format(lab and "as the Lab" or "as an app", fails))
