-- Button Box: show the presses of a Bluetooth button you built (../README.md).
--
-- The box serves service ffe0 with one characteristic, ffe1, two bytes:
-- 1 while its button is held (0 when up), then a press counter 0..255.
-- This app scans for ffe0, connects to the first box it finds, reads the value
-- once, subscribes to its changes and shows them. When the box goes away
-- (switched off, out of range) it scans again and reconnects.
--
-- Permission "ble" (Solana OS 1.0.0). Bluetooth is off until ble.on() and goes
-- off when the app exits.

local SERVICE, CHAR = "ffe0", "ffe1"

ui.bg("bg")
ui.label(10, 6, "Button Box", {size = 20})
local status = ui.label(10, 40, "", {color = "dim", w = 300})
local big = ui.label(0, 80, "", {size = 32, w = ui.W, align = "center"})
local count = ui.label(0, 130, "", {w = ui.W, align = "center", color = "dim"})
ui.hint("SELECT reconnect   CANCEL exit")

local link            -- the current link, or nil while scanning
local scanning = false
local retry           -- the timer that scans again after a link ended
local giveup          -- the timer that notices a scan found nothing

-- The box's value. It comes over the air: check it before using it.
local function show(data)
  if type(data) ~= "string" or #data < 2 then return end
  local held, presses = string.byte(data, 1, 2)
  big:text(held == 1 and "PRESSED" or "up")
  big:color(held == 1 and "green" or "text")
  count:text(presses .. " presses")
  if held == 1 then led.fill(0x14, 0xF1, 0x95) else led.off() end
end

local find            -- declared first: the link's callback calls it

local function connect(dev)
  status:text("Connecting to " .. (dev.name ~= "" and dev.name or dev.addr) .. "...")
  link = ble.connect(dev, function(event, a, b)
    if event == "open" then
      status:text("Connected to " .. dev.addr)
      link:read(SERVICE, CHAR, function(ok, data)          -- the state right now
        if ok then show(data) else status:text("Read failed: " .. data) end
      end)
      link:subscribe(SERVICE, CHAR)                         -- then every change
    elseif event == "notify" then
      show(b)                                               -- a = "ffe1", b = the value
    elseif event == "close" then
      link = nil
      led.off()
      big:text("")
      count:text("")
      -- Scan again in a moment ("closed": SELECT hung up to reconnect).
      if a ~= "closed" then status:text("Lost the box (" .. a .. "). Looking again...") end
      retry = timer.after(a == "closed" and 500 or 2000, find)
    end
  end)
end

find = function()
  retry = nil
  if link or scanning then return end
  scanning = true
  status:text("Looking for a button box...")
  -- active: also ask for the name, which the box sends in its scan response.
  ble.scan({service = SERVICE, active = true, seconds = 30}, function(dev)
    if not scanning then return end
    scanning = false
    ble.scan(false)
    connect(dev)
  end)
  -- The scan ends by itself after 30 s. Say so if it found nothing.
  if giveup then timer.cancel(giveup) end
  giveup = timer.after(31000, function()
    giveup = nil
    if scanning then
      scanning = false
      status:text("No box found. SELECT to look again")
    end
  end)
end

function on_key(k)
  if k ~= "select" then return false end
  if not ble or ble.state() ~= "on" then return true end
  if link then
    link:close()                                            -- "close", "closed" follows, then find()
  elseif not retry then
    scanning = false
    ble.scan(false)
    find()
  end
  return true
end

if not ble then
  status:text("This app needs Solana OS 1.0.0 or later")
else
  local ok, why = ble.on()
  if ok then find() else status:text("No Bluetooth: " .. why) end
end
