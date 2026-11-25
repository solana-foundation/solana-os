-- Regression test (not shipped): profile.items and profile.pages (permission
-- "inventory", Solana OS 2.3.0). At launch it checks the arguments they refuse,
-- then walks both lists to the end through `next`, checking that no entry comes
-- twice and that the totals match, and shows how many of the Guide's pages the
-- player holds. SELECT walks again. Lines start "t-inv".
local status = ui.label(8, 8, "t-inv: reading the inventory", {size = 12, w = 304})
local log = ui.label(8, 30, "", {size = 12, w = 304})
ui.hint("SELECT again")

local function say(text)
  print("t-inv " .. text)
  log:text(text)
end

print("t-inv has items: " .. type(profile.items) .. ", pages: " .. type(profile.pages))
print("t-inv bad cursor: " .. tostring(pcall(profile.items, "not a cursor!", function() end)))
print("t-inv long cursor: " .. tostring(pcall(profile.pages, ("a"):rep(129), function() end)))
print("t-inv no callback: " .. tostring(pcall(profile.items)))
print("t-inv number cursor: " .. tostring(pcall(profile.items, {}, function() end)))

-- Walks one list: list(cursor, cb) until next is nil.
local function walk(list, field, done)
  local seen, n, calls = {}, 0, 0
  local function step(cursor)
    calls = calls + 1
    list(cursor, function(ok, r)
      if not ok then return done(false, r) end
      for _, e in ipairs(r[field]) do
        if seen[e.id] then print("t-inv FAIL twice: " .. e.id) end
        seen[e.id] = true
        n = n + 1
      end
      if r.next then step(r.next) else done(true, r, n, calls) end
    end)
  end
  step(nil)
end

local function run()
  status:text("t-inv: walking items")
  walk(profile.items, "items", function(ok, r, n, calls)
    if not ok then return say("items error: " .. tostring(r)) end
    say(("items %d of %d in %d calls"):format(n, r.total, calls))
    if n ~= r.total then print("t-inv FAIL items count") end
    walk(profile.pages, "pages", function(ok2, p, m, calls2)
      if not ok2 then return say("pages error: " .. tostring(p)) end
      say(("pages %d of %d (owned %d) in %d calls"):format(m, p.total, p.owned, calls2))
      if m ~= p.owned then print("t-inv FAIL pages count") end
      status:text(("You hold %d of the Guide's %d pages"):format(p.owned, p.total))
      print("t-inv done")
    end)
  end)
  -- The first page alone, as an app that shows a few would ask for it.
  profile.items(function(ok, r)
    if ok and r.items[1] then
      local it = r.items[1]
      print(("t-inv first: %s %s %s app=%s quest=%s tradable=%s data=%s"):format(it.id, it.kind, it.name,
        tostring(it.app), tostring(it.quest), tostring(it.tradable), tostring(it.data)))
    end
  end)
end

run()

function on_key(key)
  if key == "select" then run() return true end
  return false
end
