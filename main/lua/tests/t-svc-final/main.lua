-- Regression test (not shipped): the two last saves of shared storage overlap.
-- Each launch is one round; the next launch prints
-- "t-svc-final check n=<n> r=<r> s=<s>" from data.json.
--  odd rounds: the service pads storage (a ~14 KB write) and stops itself, so
--   its last save is on the card while the app sets "r" and exits d ms later;
--   the service first spins 0-25 ms (by round), so its write sweeps across the
--   app's timer at 1 ms steps (the timer alone has 10 ms ticks);
--  even rounds: the app pads storage and exits at once, so its last save is on
--   the card while the service sets "s" and stops (the service's task gets the
--   CPU when the app's blocks on the card).
-- Kept: r == n every round, and s == n after an even round.
-- The race is hardware only (the emulator's tasks take turns; svc-run.ts runs a
-- few rounds of the path). On a badge: lua recv app.json, main.lua and
-- service.lua (bare path), lua run it once and press SELECT (the owner's
-- consent), then run it a few hundred times, waiting for "lua: t-svc-final:
-- closed" and "svc: t-svc-final: ended" each time, and count check lines that
-- break the rule above. Measured on 4ZT2BX (2.3.0): see firmware/CHANGELOG.md.
ui.label(8, 8, "t-svc-final", {size = 12})
local n = (storage.get("n") or 0)
print(("t-svc-final check n=%d r=%d s=%d"):format(n, storage.get("r") or 0, storage.get("s") or 0))
n = n + 1
storage.set("n", n)
local d = 110 + 10 * (n % 3)            -- the app's timer (10 ms ticks)
service.start(function(ok, err)
  if not ok then print("t-svc-final start: " .. tostring(err)) end
end)
local function pad()
  return string.rep(string.char(65 + n % 26), 14000)
end
function on_service(msg)
  if msg ~= "ready" then return end
  if n % 2 == 0 then
    storage.set("r", n)
    storage.set("pad", pad())
    service.send("go")
    print(("t-svc-final round n=%d app first"):format(n))
    sys.exit()
    return
  end
  service.send("go")
  -- A timer, not a busy wait: the app's task outranks the service's, which
  -- must get the CPU to start its last save meanwhile.
  local function finish()
    storage.set("r", n)
    print(("t-svc-final round n=%d d=%d"):format(n, d))
    sys.exit()
  end
  if d == 0 then finish() else timer.after(d, finish) end
end
