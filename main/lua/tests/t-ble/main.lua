-- Regression test (not shipped): ble between two badges running this app (two
-- emulator tabs, a badge and an emulator can't: the emulator's air is the
-- browser; or two badges). It depends only on docs/LUA_API.md "ble".
--   LEFT   role A: serve a service and advertise it, connectable
--   RIGHT  role B: scan for it, connect, read, write, subscribe, close; then
--          a connect that nobody answers ("unreachable", 10 s)
--   UP     hold: A advertises again / B connects and waits, so that switching
--          the other badge off (or closing its tab) shows "lost"
-- ble.notify sets the value even when it can't push it, so A only notifies "hello"
-- before B has read it. At launch both run the checks that need no peer (off, limits, errors).
-- Every line starts "t-ble": "PASS <step>", "FAIL <step>: <why>", "DONE ...".
local SVC = "b1e26001-0bad-4e5c-9a11-50c1a7a0b1e5"
local RX = "b1e26002-0bad-4e5c-9a11-50c1a7a0b1e5"   -- write only
local TX = "b1e26003-0bad-4e5c-9a11-50c1a7a0b1e5"   -- read + notify, "hello"
local RO = "b1e26004-0bad-4e5c-9a11-50c1a7a0b1e5"   -- read only, "ro"
local NAME = "t-ble A"

local status = ui.label(8, 8, "t-ble: LEFT serve (A), RIGHT connect (B)", {size = 12, w = 304})
local log = ui.label(8, 30, "", {size = 12, w = 304})
ui.hint("LEFT A   RIGHT B   UP hold")
local passed, failed, role = 0, 0, nil

local function say(text)
  print("t-ble " .. text)
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

-- f must raise an error containing want.
local function raises(step, want, f)
  local ok, err = pcall(f)
  check(step, not ok and tostring(err):find(want, 1, true), ok and "no error" or err)
end

-- f must return nil and why.
local function refuses(step, why, ...)
  local r, w = ...
  check(step, r == nil and w == why, ("got %s, %s"):format(tostring(r), tostring(w)))
end

local function done()
  say(("DONE %s: %d passed, %d failed"):format(role or "-", passed, failed))
end

local function deadline(step, ms)
  local fired = false
  local id = timer.after(ms, function()
    fired = true
    check(step, false, "timed out")
    done()
  end)
  return function() if not fired then timer.cancel(id) end end
end

-- --- Without a peer --------------------------------------------------------

local function nop() end

check("off at launch", ble.state() == "off", ble.state())
check("no address while off", ble.address() == nil, ble.address())
ble.off()                                                   -- harmless while off
raises("advertise while off", "Bluetooth is off", function() ble.advertise{name = "x"} end)
raises("scan while off", "Bluetooth is off", function() ble.scan({}, nop) end)
raises("connect while off", "Bluetooth is off", function() ble.connect({addr = "02:00:00:00:00:01"}, nop) end)
raises("serve while off", "Bluetooth is off", function() ble.serve({service = "ffe0", chars = {{uuid = "ffe1"}}}, nop) end)
raises("notify while off", "Bluetooth is off", function() ble.notify("ffe1", "x") end)
raises("drop while off", "Bluetooth is off", function() ble.drop() end)

local on, why = ble.on()
if not check("on", on, why) then done() return end
check("state on", ble.state() == "on", ble.state())
check("on again", ble.on() == true)
local me = ble.address()
check("address", type(me) == "string" and me:match("^%x%x:%x%x:%x%x:%x%x:%x%x:%x%x$"), me)

raises("bad UUID", "UUID", function() ble.scan({service = "xyz"}, nop) end)
raises("bad UUID 128", "UUID", function() ble.advertise{service = "6e400001-b5a3-f393-e0a9-e50e24dcca9"} end)
raises("bad address", "addr", function() ble.connect({addr = "nope"}, nop) end)
refuses("name too big", "too big", ble.advertise{name = ("n"):rep(30)})
refuses("data too big", "too big", ble.advertise{data = ("d"):rep(25)})
refuses("data too big, 16-bit service", "too big", ble.advertise{service = "ffe0", data = ("d"):rep(21)})
refuses("data too big, 128-bit service", "too big", ble.advertise{service = SVC, data = ("d"):rep(7)})
check("24 bytes of data", ble.advertise{data = ("d"):rep(24)} == true)
check("advertise off", ble.advertise(false) == true)
raises("interval range", "interval", function() ble.advertise{interval = 5} end)
raises("set over 244 bytes", "244", function() ble.set(TX, ("x"):rep(245)) end)
raises("set before serve", "not a served characteristic", function() ble.set(TX, "x") end)
raises("a fifth characteristic", "1 to 4", function()
  ble.serve({service = SVC, chars = {{uuid = "ff01"}, {uuid = "ff02"}, {uuid = "ff03"}, {uuid = "ff04"}, {uuid = "ff05"}}}, nop)
end)
raises("value over 244 bytes", "244", function()
  ble.serve({service = SVC, chars = {{uuid = RX, write = true, value = ("v"):rep(245)}}}, nop)
end)
check("scan and stop", ble.scan({seconds = 1}, nop) == true and ble.scan(false) == true)

-- --- Role A: the served side ---------------------------------------------------

local a_writes, a_cancel = {}, nil

local function role_a()
  role = "A"
  status:text("t-ble A: serving " .. NAME)
  local ok, err = ble.serve({service = SVC, chars = {
    {uuid = RX, write = true},
    {uuid = TX, read = true, notify = true, value = "hello"},
    {uuid = RO, read = true, value = "ro"},
  }}, function(what, a, b)
    if what == "open" then
      check("A open", type(a) == "string" and a:match("^%x%x:"), a)
      refuses("A notify before subscribe", "not subscribed", ble.notify(TX, "hello"))
      refuses("A serve again", "already serving", ble.serve({service = SVC, chars = {{uuid = RX}}}, nop))
      local busy = ble.connect({addr = "02:00:00:00:00:01"}, function(w, r)
        check("A connect while connected: busy", w == "close" and r == "busy", tostring(w) .. " " .. tostring(r))
      end)
      check("A busy link state", busy:state() == "closed", busy:state())
    elseif what == "write" then
      a_writes[#a_writes + 1] = b
      check("A write " .. #a_writes, a == RX and (b == "with response" or b == "no response"), tostring(a) .. " " .. tostring(b))
    elseif what == "subscribe" then
      check("A subscribe", a == TX and b == true, tostring(a) .. " " .. tostring(b))
      if b then
        check("A notify", ble.notify(TX, "ping 1") == true)
        raises("A notify over 244 bytes", "244", function() ble.notify(TX, ("x"):rep(245)) end)
      end
    elseif what == "close" then
      check("A close: peer closed", a == "peer closed", a)
      refuses("A notify after close", "not open", ble.notify(TX, "late"))
      check("A writes in order", a_writes[1] == "with response" and a_writes[2] == "no response",
            table.concat(a_writes, ","))
      if a_cancel then a_cancel() end
      done()
    end
  end)
  check("A serve", ok, err)
  refuses("A notify with nobody connected", "not open", ble.notify(TX, "x"))
  check("A set", ble.set(TX, "hello") == true)
  check("A advertise", ble.advertise{name = NAME, service = SVC, data = "\1\2", connectable = true} == true)
  a_cancel = deadline("A finished", 60000)
end

-- --- Role B: the connecting side --------------------------------------------------

local function unreachable()
  local cancel = deadline("B unreachable", 15000)
  ble.connect({addr = "02:00:00:00:00:01", random = false}, function(what, reason)
    cancel()
    check("B unreachable", what == "close" and reason == "unreachable", tostring(what) .. " " .. tostring(reason))
    done()
  end)
end

local function role_b()
  role = "B"
  status:text("t-ble B: scanning for " .. NAME)
  local cancel = deadline("B found A", 20000)
  local link
  ble.scan({service = SVC, active = true}, function(dev)
    if link then return end
    cancel()
    ble.scan(false)
    check("B found A", dev.name == NAME, dev.name)
    check("B dev fields", dev.services[1] == SVC and dev.data == "\1\2" and dev.company == 0xffff and dev.connectable == true
          and type(dev.rssi) == "number" and type(dev.random) == "boolean" and dev.addr:match("^%x%x:"),
          json.encode(dev))
    local opened = deadline("B open", 12000)
    local reads = 0
    link = ble.connect(dev, function(what, a, b)
      if what == "open" then
        opened()
        check("B open", link:state() == "open", link:state())
        check("B read queued", link:read(SVC, TX, function(ok, v)
          check("B read initial value", ok and v == "hello", tostring(ok) .. " " .. tostring(v))
        end) == true)
        link:read(SVC, "ffff", function(ok, r)
          check("B no such characteristic", not ok and r == "no such characteristic", tostring(ok) .. " " .. tostring(r))
        end)
        link:read(SVC, RX, function(ok, r)
          check("B read write-only: not allowed", not ok and r == "not allowed", tostring(ok) .. " " .. tostring(r))
        end)
        link:read(SVC, RO, function(ok, v)
          check("B read RO", ok and v == "ro", tostring(ok) .. " " .. tostring(v))
          raises("B write over 244 bytes", "244", function() link:write(SVC, RX, ("x"):rep(245)) end)
          raises("B bad UUID", "UUID", function() link:read(SVC, "zz", nop) end)
          link:write(SVC, RX, "with response", function(ok2, err)
            check("B write with response", ok2, err)
            check("B write without response", link:write(SVC, RX, "no response") == true)
            check("B subscribe", link:subscribe(SVC, TX) == true)
          end)
        end)
        refuses("B fifth op: busy", "busy", link:read(SVC, TX, nop))
      elseif what == "notify" then
        check("B notify", a == TX and b == "ping 1", tostring(a) .. " " .. tostring(b))
        link:close()
        check("B state after close", link:state() == "closed", link:state())
        refuses("B read after close", "not open", link:read(SVC, TX, nop))
      elseif what == "close" then
        check("B close: closed", a == "closed", a)
        refuses("B stale link", "not open", link:read(SVC, TX, nop))
        unreachable()
      end
    end)
    check("B connecting", link:state() == "connecting", link:state())
    refuses("B read before open", "not open", link:read(SVC, TX, nop))
    ble.connect(dev, function(what, reason)
      check("B second connect: busy", what == "close" and reason == "busy", tostring(what) .. " " .. tostring(reason))
    end)
  end)
end

-- --- Hold: for "lost" -----------------------------------------------------------

local function hold()
  if role == "A" then
    check("A advertise again", ble.advertise{name = NAME, service = SVC, connectable = true} == true)
    return
  end
  role = role or "B"
  ble.scan({service = SVC}, function(dev)
    ble.scan(false)
    local link
    link = ble.connect(dev, function(what, a)
      if what == "open" then
        say("hold: open, now take the other badge away")
      elseif what == "close" then
        check("B hold close: lost", a == "lost", a)
        done()
      end
    end)
  end)
end

function on_key(k)
  if role == nil and k == "left" then role_a() return true end
  if role == nil and k == "right" then role_b() return true end
  if k == "up" then hold() return true end
  return false
end
