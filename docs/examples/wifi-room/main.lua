-- Cheer (room): the same game as ../mesh-and-wifi, through your server only.
--
-- For rooms where the radios aren't an option: the access points aren't on
-- the mesh channel, badges are spread across the venue, emulators play too, or
-- the organisers want everything through the server. Every badge opens one
-- WebSocket to the app's own host (`ws`, permission "http"); SELECT sends a
-- cheer, and the server tells everyone in the room the total.
--
-- Hosts ["cheer.example.com"]: the server is server.ts beside this file.
-- While the socket is open the badge isn't connected to the badge server:
-- other server calls (lamps, items, http, ...) answer "socket open". So make
-- them before ws.open, or close the socket first.

local URL = "wss://cheer.example.com/rooms/"
local ROOMS = {"lobby", "stage", "hall-b"}

local room = storage.get("room") or 1
if not ROOMS[room] then room = 1 end
local sock
local retry_ms = 2000             -- wait before reconnecting; doubles up to 30 s
local last_send = -1000

ui.bg("bg")
ui.label(10, 6, "Cheer", {size = 20})
local where = ui.label(10, 34, "", {w = 300})
local status = ui.label(10, 52, "", {color = "dim", w = 300})
local big = ui.label(0, 84, "", {size = 32, w = ui.W, align = "center", color = "green"})
local here = ui.label(0, 128, "", {w = ui.W, align = "center", color = "dim"})
local last = ui.label(0, 150, "", {w = ui.W, align = "center"})
ui.hint("SELECT cheer   LEFT/RIGHT room   CANCEL exit")

local connect                     -- declared first: the socket's callback calls it
local switching = false           -- closing the socket to change rooms
local wait                        -- the one timer that connects later (retry, Wi-Fi, room change)

-- Connect in `ms`, replacing any connect already waiting: one timer, never a pile.
local function connect_in(ms)
  if wait then timer.cancel(wait) end
  wait = timer.after(ms, function() wait = nil connect() end)
end

local function on_event(event, data)
  if event == "open" then
    retry_ms = 2000
    status:text("Connected")
  elseif event == "message" then
    -- From your own server, but check it anyway: a bad message shouldn't stop the app.
    local m = json.decode(data)
    if type(m) ~= "table" or type(m.total) ~= "number" then return end
    big:text(tostring(m.total))
    here:text((m.here or 0) .. " in " .. ROOMS[room])
    if type(m.last) == "string" then last:text(m.last:sub(1, 8) .. " cheered") end
  elseif event == "close" then
    sock = nil
    if switching then                               -- we closed it to change rooms: open the new one
      switching = false
      connect_in(1000)
      return
    end
    status:text("Closed (" .. data .. "). Again in " .. retry_ms // 1000 .. " s")
    connect_in(retry_ms)
    retry_ms = math.min(retry_ms * 2, 30000)        -- 20 opens a minute at most
  end
end

connect = function()
  if sock then return end
  where:text("Room: " .. ROOMS[room])
  if not badge.online() then
    status:text("Waiting for Wi-Fi...")
    connect_in(3000)
    return
  end
  status:text("Connecting...")
  sock = ws.open(URL .. ROOMS[room], on_event)
end

function on_key(k)
  if k == "select" then
    if not sock or sock:state() ~= "open" then ui.toast("Not connected") return true end
    if sys.millis() - last_send < 250 then return true end   -- the server takes 4 a second
    last_send = sys.millis()
    local ok, why = sock:send({c = 1})
    if not ok then ui.toast(why) end
    return true
  elseif k == "left" or k == "right" then
    room = (room + (k == "right" and 0 or #ROOMS - 2)) % #ROOMS + 1
    storage.set("room", room)
    big:text("")
    last:text("")
    where:text("Room: " .. ROOMS[room])
    if sock then
      if not switching then
        switching = true
        sock:close()                                -- "close", "closed" follows, then connect_in()
      end
    else
      connect_in(1000)                              -- a second after the last press: opens count, 20 a minute
    end
    return true
  end
  return false
end

-- Before anything else: below Solana OS 2.0.0 a secure connection needs room to
-- start (56 KB of the badge's heap free), so open the socket while the app is
-- still small and load big data after it's up. (From 2.0.0 it takes PSRAM.)
connect()
