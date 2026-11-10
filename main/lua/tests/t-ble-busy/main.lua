-- Regression test (not shipped): ble while busy, between two badges running this
-- app (two emulator tabs, or two badges). It depends only on docs/LUA_API.md "ble".
--   LEFT   role A: serve a service and advertise it, connectable
--   RIGHT  role B: advertise (connectable), find A, connect twice
-- Link 1: B's connectable advertisement stops while it connects out (A's scan
--   doesn't find it, and B can't start one again: "busy"), and A, connected
--   to, isn't advertising either (B's scan doesn't find it).
-- Link 2: B advertises plainly first, and that keeps going (A's scan finds it,
--   not connectable). Then A storms ble.connect from its own "busy" callbacks,
--   and a timer must still fire on time. Meanwhile B sends 30 writes and
--   subscribes in one go: A's queue of 8 fills, so A loses writes and the
--   subscribe callback, and ble.notify must still push (a badge's queue may
--   not fill: an INFO line says so).
-- Every line starts "t-ble-busy": "PASS <step>", "FAIL <step>: <why>", "INFO", "DONE ...".
local SVC = "b1e27001-0bad-4e5c-9a11-50c1a7a0b1e5"
local RX = "b1e27002-0bad-4e5c-9a11-50c1a7a0b1e5"   -- write only
local TX = "b1e27003-0bad-4e5c-9a11-50c1a7a0b1e5"   -- notify
local NAME_A, NAME_B = "tbb A", "tbb B"
local WRITES = 30

ui.label(8, 8, "t-ble-busy: LEFT serve (A), RIGHT connect (B)", {size = 12, w = 304})
local log = ui.label(8, 30, "", {size = 12, w = 304})
local passed, failed, role, finished = 0, 0, nil, false

local function say(text)
  print("t-ble-busy " .. text)
  log:text(text)
end

local function check(step, ok, why)
  if ok then
    passed = passed + 1
    say("PASS " .. step)
  else
    failed = failed + 1
    say("FAIL " .. step .. ": " .. tostring(why))
  end
  return ok
end

local function refuses(step, why, ...)
  local r, w = ...
  check(step, r == nil and w == why, ("got %s, %s"):format(tostring(r), tostring(w)))
end

local function done()
  if finished then return end
  finished = true
  say(("DONE %s: %d passed, %d failed"):format(role or "-", passed, failed))
end

local function deadline(step, ms)
  local fired = false
  local id = timer.after(ms, function()
    fired = true
    if finished then return end
    check(step, false, "timed out")
    done()
  end)
  return function() if not fired then timer.cancel(id) end end
end

-- Scans for `name` for `seconds`; cb(list of the dev tables heard).
local function listen(name, seconds, cb)
  local heard = {}
  ble.scan({name = name, active = true, duty = 100, seconds = seconds}, function(dev)
    heard[#heard + 1] = dev
  end)
  timer.after(seconds * 1000 + 300, function() cb(heard) end)
end

local function any_connectable(heard)
  for _, d in ipairs(heard) do
    if d.connectable then return true end
  end
  return false
end

check("on", ble.on())

-- --- Role A: the served side ---------------------------------------------------

-- Connects again from each "busy" callback until the timer has fired and
-- landed() is true (or 3 s pass): keeping the badge busy while B's burst
-- arrives is also what fills the queue in the emulator.
local function storm_and_timer(landed)
  local storms, fired, t0 = 0, false, sys.millis()
  local function again()
    ble.connect({addr = "02:00:00:00:00:01"}, function(what, reason)
      if what == "close" and reason == "busy" then
        storms = storms + 1
        if not (fired and (landed() or sys.millis() - t0 > 3000)) then again() end
      end
    end)
  end
  timer.after(100, function()
    fired = true
    local late = sys.millis() - t0
    check("A timer fires during a busy-reconnect storm", late < 1000, late .. " ms")
    check("A storm ran", storms >= 2, storms)
    say("INFO " .. storms .. " busy callbacks in " .. late .. " ms")
  end)
  again()
end

local function role_a()
  role = "A"
  local opens, writes, sub_seen, cancel = 0, 0, false, deadline("A finished", 60000)
  local ok, err = ble.serve({service = SVC, chars = {{uuid = RX, write = true}, {uuid = TX, notify = true}}},
    function(what, a, b)
      if what == "open" then
        opens = opens + 1
        if opens == 1 then
          listen(NAME_B, 3, function(heard)
            check("A: B connecting out isn't connectable", not any_connectable(heard), #heard .. " heard")
          end)
        else
          listen(NAME_B, 3, function(heard)
            check("A: B's plain advertisement kept going", #heard > 0 and not any_connectable(heard),
                  #heard .. " heard, connectable " .. tostring(any_connectable(heard)))
            storm_and_timer(function() return writes > 0 end)   -- B's burst comes at 4.5 s
          end)
          local tries, poll = 0, nil
          local stop = deadline("A notify after a lost subscribe", 15000)
          poll = timer.every(200, function()
            tries = tries + 1
            local r, why = ble.notify(TX, "n")
            if r then
              timer.cancel(poll)
              stop()
              say(("INFO writes seen %d of %d, subscribe callback %s, %d tries"):format(writes, WRITES,
                  sub_seen and "seen" or "dropped", tries))
              if sub_seen then
                say("INFO the queue didn't fill: the lost-subscribe case wasn't exercised")
                check("A notify", true)
              else
                check("A notify after a lost subscribe", writes < WRITES, writes .. " writes")
              end
            elseif why ~= "not subscribed" then
              timer.cancel(poll)
              stop()
              check("A notify before subscribe: not subscribed", false, why)
            end
          end)
        end
      elseif what == "write" then
        writes = writes + 1
      elseif what == "subscribe" then
        sub_seen = true
      elseif what == "close" then
        if opens == 1 then
          check("A advertise again", ble.advertise{name = NAME_A, service = SVC, connectable = true} == true)
        else
          cancel()
          done()
        end
      end
    end)
  check("A serve", ok, err)
  check("A advertise", ble.advertise{name = NAME_A, service = SVC, connectable = true} == true)
end

-- --- Role B: the connecting side --------------------------------------------------

local function role_b()
  role = "B"
  check("B advertise connectable", ble.advertise{name = NAME_B, connectable = true} == true)
  local found = deadline("B found A", 20000)
  local opens, link, dev_a = 0, nil, nil
  local function on_link(what, a, b)
    if what == "open" then
      opens = opens + 1
      if opens == 1 then
        refuses("B connectable advertise while linked: busy", "busy", ble.advertise{name = NAME_B, connectable = true})
        listen(NAME_A, 2, function(heard)
          check("B: A, connected to, isn't advertising", #heard == 0, #heard .. " heard")
        end)
        timer.after(4000, function() link:close() end)
      else
        timer.after(4500, function()           -- once A has scanned for us and is storming
          for i = 1, WRITES do link:write(SVC, RX, "w" .. i) end
          check("B subscribe", link:subscribe(SVC, TX) == true)
        end)
      end
    elseif what == "notify" then
      check("B notified", a == TX and b == "n", tostring(a) .. " " .. tostring(b))
      timer.after(4000, function() link:close() end)     -- A's scan for B takes 3 s
    elseif what == "close" then
      if opens == 1 then
        check("B close 1", a == "closed", a)
        check("B advertise plain", ble.advertise{name = NAME_B, connectable = false} == true)
        timer.after(500, function() link = ble.connect(dev_a, on_link) end)
      else
        check("B close 2", a == "closed", a)
        done()
      end
    end
  end
  ble.scan({service = SVC}, function(dev)
    if dev_a then return end
    dev_a = dev
    found()
    link = ble.connect(dev, on_link)
  end)
  deadline("B finished", 60000)
end

function on_key(k)
  if role == nil and k == "left" then role_a() return true end
  if role == nil and k == "right" then role_b() return true end
  return false
end
