-- Regression test (not shipped): the gossip module's node side (LUA_API.md
-- "gossip"). Prints what the reads return; as `gossip-lab` (the run puts a
-- copy under that id) it also acts: picks a class and deploys a program.
-- Driven by main/lua/tests/gossip/run.ts in the emulator; on a badge with
-- `lua put` + `lua run t-gossip`. SELECT prints the reads again, RIGHT tries
-- every acting call's argument checks.
local id = (gossip.me() or {}).id or "?"
local function out(what, v) print("t-gossip " .. what .. ": " .. json.encode(v)) end
local failed = 0
local function check(name, ok)
  if not ok then failed = failed + 1 end
  print("t-gossip " .. name .. ": " .. (ok and "ok" or "FAIL"))
end

-- The genome of docs/GOSSIP.md §3's vector 3 (three traits).
local g = gossip.genome("31UFvJfpgAT4Paado1LNpEor9LL5DXMFAiCYzE2j5Cdj")
check("genome vector 3", g and g.colour == 0xF040FF and g.body == 3 and g.eyes == 4 and g.marks == 5
  and g.mark_seed == 22 and #g.traits == 3 and g.traits[1] == 2 and g.traits[2] == 4 and g.traits[3] == 0
  and g.vir == 5 and g.hard == 4 and g.stab == 4 and g.rarity == "rare")
check("genome bad address", gossip.genome("not-an-address-at-all-0000000000000000") == nil)
check("program unknown", gossip.program("31UFvJfpgAT4Paado1LNpEor9LL5DXMFAiCYzE2j5Cdj") == nil)

local function reads()
  out("me", gossip.me())
  out("programs", gossip.programs())
  out("nearby", gossip.nearby())
  out("hearing", gossip.hearing())
  local me = gossip.me()
  if me and me.act and me.act ~= "" then out("program", gossip.program(me.act)) end
end
reads()

local lab_ok, lab_err = gossip.set_class("r")
print("t-gossip set_class: " .. tostring(lab_ok) .. " " .. tostring(lab_err))
if lab_ok then
  local me = gossip.me() or {}
  if (me.deployed or 0) == 0 then
    print("t-gossip deploy: " .. tostring(gossip.deploy()))
  end
end
check("finished", true)

function on_gossip(ev)
  out("ev", ev)
end

function on_key(k)
  if k == "select" then
    reads()
    return true
  end
  if k == "right" then
    print("t-gossip args: " .. table.concat({
      tostring(select(2, gossip.place("31UFvJfpgAT4Paado1LNpEor9LL5DXMFAiCYzE2j5Cdj", 9))),
      tostring(select(2, gossip.set_class("x"))),
      tostring(select(2, gossip.name("31UFvJfpgAT4Paado1LNpEor9LL5DXMFAiCYzE2j5Cdj", ("x"):rep(17)))),
      tostring(select(2, gossip.activate("bogus"))),
      tostring(select(2, gossip.use("", {}))),
    }, ","))
    return true
  end
  return false
end
