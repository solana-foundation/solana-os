-- Nearby: who is around, with no connections at all.
--
-- Every badge running this app advertises a few bytes under one service (its
-- team and its score) and scans for the same service. Each advertisement it
-- hears is a row: name, team, score and signal. Nobody connects, so any number
-- of badges can take part, and each sees the others within a second or two.
--
-- Permission "ble" (Solana OS 1.0.0). Two emulator tabs (/emulator and
-- /emulator?badge=2) see each other too.
--
-- An advertisement is open air: anyone can send one with any team and score.
-- Fine for a game of "who's here"; not for anything worth cheating at.

-- Pick your own 128-bit UUID for your app, so that only your app's badges match.
-- With a 128-bit service an advertisement has room for 6 bytes of data.
local SERVICE = "4e5a0001-6b1d-4c7e-9a3f-26b0a7c1d5e2"
local MAGIC = "N"               -- first byte, to skip other apps that reuse the UUID by mistake
local TEAMS = {
  {name = "Purple", color = "purple"}, {name = "Green", color = "green"},
  {name = "Blue", color = "blue"}, {name = "Orange", color = "orange"},
}
local ROWS = 6                  -- rows on screen, the strongest signals first
local GONE_MS = 10000           -- drop a badge not heard for this long

local team = storage.get("team") or 1
local score = storage.get("score") or 0
local seen = {}                 -- addr -> {name, team, score, rssi, at}

ui.bg("bg")
ui.label(10, 6, "Nearby", {size = 20})
local me = ui.label(10, 34, "", {w = 300})
local rows = {}
for i = 1, ROWS do
  rows[i] = ui.label(10, 60 + (i - 1) * 22, "", {size = 12, w = 300})   -- 12 is mono: columns line up
end
local note = ui.label(10, 60 + ROWS * 22, "", {color = "dim", w = 300})
ui.hint("SELECT +1   LEFT/RIGHT team   CANCEL exit")

-- 4 bytes: "N", the team (1 byte), the score (2 bytes, big-endian).
local function advertise()
  local ok, why = ble.advertise{service = SERVICE, data = string.pack(">c1BI2", MAGIC, team, score)}
  if not ok then note:text("Can't advertise: " .. why) end
  me:text("You: " .. TEAMS[team].name .. ", " .. score .. " points")
  me:color(TEAMS[team].color)
end

-- Everything in dev comes over the air: check its size and ranges first.
local function heard(dev)
  if #dev.data ~= 4 or dev.data:sub(1, 1) ~= MAGIC then return end
  local _, t, s = string.unpack(">c1BI2", dev.data)
  if not TEAMS[t] then return end
  seen[dev.addr] = {name = dev.name ~= "" and dev.name or dev.addr, team = t, score = s,
    rssi = dev.rssi, at = sys.millis()}
end

local function redraw()
  local now, list = sys.millis(), {}
  for addr, d in pairs(seen) do
    if now - d.at > GONE_MS then seen[addr] = nil else list[#list + 1] = d end
  end
  table.sort(list, function(a, b) return a.rssi > b.rssi end)
  for i = 1, ROWS do
    local d = list[i]
    if d then
      rows[i]:text(string.format("%-14s %-7s %5d  %d dBm", d.name:sub(1, 14), TEAMS[d.team].name, d.score, d.rssi))
      rows[i]:color(TEAMS[d.team].color)
    else
      rows[i]:text("")
    end
  end
  note:text(#list == 0 and "Nobody yet. Badges running Nearby show up here." or
    (#list > ROWS and (#list - ROWS) .. " more further away" or ""))
end

function on_key(k)
  if k == "select" then
    score = math.min(score + 1, 65535)
  elseif k == "left" or k == "right" then
    team = (team + (k == "right" and 0 or #TEAMS - 2)) % #TEAMS + 1
  else
    return false
  end
  advertise()                   -- call it again to change the data
  return true
end

function on_exit()
  storage.set("team", team)
  storage.set("score", score)
end

if not ble then
  note:text("This app needs Solana OS 1.0.0 or later")
else
  local ok, why = ble.on()
  if not ok then
    note:text("No Bluetooth: " .. why)
  else
    advertise()
    -- active = true also asks for names ("Badge <id>" unless an app sets one).
    ble.scan({service = SERVICE, active = true}, heard)
    timer.every(1000, redraw)
    redraw()
  end
end
