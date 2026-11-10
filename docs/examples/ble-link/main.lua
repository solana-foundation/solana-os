-- Link: two badges talk over one Bluetooth connection.
--
-- One badge hosts: it serves the Nordic UART Service (NUS), a layout many
-- phone apps and libraries know, and advertises it. The other joins: it scans
-- for the service, connects, subscribes to the host's TX characteristic and
-- writes to its RX one. A line typed on either badge, or a key pressed, shows
-- on both. A phone app that speaks NUS can join a hosting badge too.
--
--   RX  6e400002-...  the joiner writes here; the host gets cb("write", RX, data)
--   TX  6e400003-...  the host notifies here; the joiner gets cb("notify", TX, data)
--
-- Permission "ble" (Solana OS 1.0.0). Two emulator tabs (/emulator and
-- /emulator?badge=2) link up too: host on one, join on the other.
-- No pairing, no encryption: anyone in range could join a host, and nothing
-- sent here is private.

local NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
local RX  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
local TX  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
local LINES = 7

local role          -- nil (choosing), "host" or "join"
local link          -- the joiner's link
local peer          -- who we're talking to, for the screen
local ready         -- Bluetooth is on
local found = {}    -- join: hosts heard, {dev, name}
local pick = 1

ui.bg("bg")
ui.label(10, 6, "Link", {size = 20})
local status = ui.label(10, 34, "", {color = "dim", w = 300})
local log = {}
for i = 1, LINES do
  log[i] = ui.label(10, 58 + (i - 1) * 20, "", {w = 300})
end
local lines = {}

-- Anything from the other side is untrusted: printable ASCII only, and short.
local function clean(data)
  return (tostring(data):gsub("[^\32-\126]", "?"):sub(1, 60))
end

local function say(text, color)
  table.insert(lines, 1, {text = text, color = color or "text"})
  lines[LINES + 1] = nil
  for i = 1, LINES do
    log[i]:text(lines[i] and lines[i].text or "")
    log[i]:color(lines[i] and lines[i].color or "text")
  end
end

local function connected() return peer ~= nil end

-- Send a line to the other badge: the host notifies TX, the joiner writes RX.
local function send(text)
  if not connected() then ui.toast("Not linked yet") return end
  local ok, why
  if role == "host" then
    ok, why = ble.notify(TX, text)
  else
    ok, why = link:write(NUS, RX, text, function(done, err)
      if not done then say("Not delivered: " .. err, "orange") end
    end)
  end
  if ok then say("me: " .. text, "green") else ui.toast("Not sent: " .. why) end
end

-- ---------------------------------------------------------------- host
local function host_advertise()
  -- The name goes in the scan response; the 128-bit service leaves room for 6 bytes of data (none here).
  ble.advertise{name = "Link " .. (badge.id() or "badge"), service = NUS, connectable = true}
  status:text("Hosting. Waiting for a badge to join...")
end

local function host()
  role = "host"
  local ok, why = ble.serve({service = NUS, chars = {
    {uuid = RX, write = true},
    {uuid = TX, read = true, notify = true, value = ""},
  }}, function(event, a, b)
    if event == "open" then
      peer = a
      status:text("Linked with " .. a)
      say("joined: " .. a, "aqua")
    elseif event == "subscribe" then                  -- a = TX, b = true (on) or false (off)
      if a == TX and b then say("they're listening", "dim") end
    elseif event == "write" then
      if a == RX then say("them: " .. clean(b), "aqua") end
    elseif event == "close" then
      say("left (" .. a .. ")", "dim")
      peer = nil
      host_advertise()                                -- advertising stops while linked: start it again
    end
  end)
  if not ok then status:text("Can't host: " .. why) return end
  host_advertise()
  ui.hint("SELECT type   arrows send   CANCEL exit")
end

-- ---------------------------------------------------------------- join
local function show_found()
  if #found == 0 then status:text("Looking for a hosting badge...") return end
  local f = found[pick]
  status:text(string.format("%d found. UP/DOWN pick, SELECT join: %s", #found, f.name))
end

local scan_for_hosts      -- declared first: a closed link calls it

local function join(dev)
  ble.scan(false)
  status:text("Joining " .. dev.addr .. "...")
  link = ble.connect(dev, function(event, a, b)
    if event == "open" then
      peer = dev.addr
      status:text("Linked with " .. dev.addr)
      say("linked: " .. dev.addr, "aqua")
      link:subscribe(NUS, TX)                         -- the host's lines arrive as "notify"
      ui.hint("SELECT type   arrows send   CANCEL exit")
    elseif event == "notify" then
      if a == TX then say("them: " .. clean(b), "aqua") end
    elseif event == "close" then
      if peer then say("link ended (" .. a .. ")", "dim") else say("couldn't join (" .. a .. ")", "orange") end
      peer, link = nil, nil
      timer.after(2000, scan_for_hosts)               -- look again in a moment
    end
  end)
end

scan_for_hosts = function()
  found, pick = {}, 1
  ui.hint("UP/DOWN pick   SELECT join   CANCEL exit")
  -- By name too: the Nordic UART service is common, and other people's gadgets advertise it.
  ble.scan({service = NUS, name = "Link ", active = true}, function(dev)
    for _, f in ipairs(found) do
      if f.dev.addr == dev.addr then return end
    end
    if #found < 8 then
      found[#found + 1] = {dev = dev, name = dev.name ~= "" and clean(dev.name) or dev.addr}
      show_found()
    end
  end)
  show_found()
end

-- ---------------------------------------------------------------- keys
local ARROWS = {up = "UP", down = "DOWN", left = "LEFT", right = "RIGHT"}

function on_key(k)
  if k == "back" then return false end                -- CANCEL exits; Bluetooth goes off with the app
  if not ready then return true end
  if not role then
    if k == "up" then host() elseif k == "down" then role = "join" scan_for_hosts() end
    return true
  end
  if connected() then
    if k == "select" then
      ui.input("Say", 40, function(text)
        if text and text ~= "" then send(clean(text)) end
      end)
    elseif ARROWS[k] then
      send("pressed " .. ARROWS[k])
    end
  elseif role == "join" and #found > 0 and not link then
    if k == "up" then pick = (pick - 2) % #found + 1
    elseif k == "down" then pick = pick % #found + 1
    elseif k == "select" then join(found[pick].dev) end
    show_found()
  end
  return true
end

if not ble then
  status:text("This app needs Solana OS 1.0.0 or later")
else
  local ok, why = ble.on()
  ready = ok
  if ok then
    status:text("UP host a link   DOWN join one")
    ui.hint("UP host   DOWN join   CANCEL exit")
  else
    status:text("No Bluetooth: " .. why)
  end
end
