-- Regression test (not shipped): mesh.channel and mesh.scan (LUA_API.md
-- "Choosing a channel"). Run it on two badges (or two emulator tabs): it
-- broadcasts every second, so a scan on the other marks it `app`. SELECT
-- scans 2.4 GHz with a short dwell, UP both bands with the shortest, RIGHT/LEFT
-- step this badge's channel through 1, 6, 11, 36, 149, DOWN goes back to the
-- event's. Lines start "t-chan".
local status = ui.label(8, 8, "", {size = 12, w = 304})
local log = ui.label(8, 30, "", {size = 12, w = 304})
ui.hint("SELECT/UP scan  LEFT/RIGHT channel  DOWN event")
local CHS = {1, 6, 11, 36, 149}
local at = 0

local last
local function show()
  local i = mesh.info()
  local text = ("t-chan ch %d (%s GHz) event %d online %s scanning %s"):format(
    i.channel, i.band, i.event, tostring(i.online), tostring(i.scanning))
  status:text(text)
  if text ~= last then print(text) last = text end
end

local function say(text)
  print("t-chan " .. text)
  log:text(text)
end

-- Argument checks.
local function try(name, f)
  local ok, err = pcall(f)
  print(("t-chan %s: %s"):format(name, ok and "FAIL no error" or err))
end
try("scan no cb", function() mesh.scan({}) end)
try("scan bad band", function() mesh.scan({band = "6"}, function() end) end)
try("scan radar", function() mesh.scan({channels = {52}}, function() end) end)
try("scan both", function() mesh.scan({channels = {1}, band = "5"}, function() end) end)
try("scan dwell", function() mesh.scan({dwell = 100}, function() end) end)
try("channel float", function() mesh.channel(1.5) end)
print("t-chan radar: " .. tostring(select(2, mesh.channel(52))))
print("t-chan 14: " .. tostring(select(2, mesh.channel(14))))
show()

timer.every(1000, function()
  mesh.send("*", {hi = 1})
  show()
end)

local function scan(opts)
  local ok, err = mesh.scan(opts, function(chs)
    local parts = {}
    for _, c in ipairs(chs) do
      if c.badges + c.beacons > 0 then
        parts[#parts + 1] = ("%d:%db/%dc/%da"):format(c.channel, c.badges, c.beacons, c.app)
        for _, p in ipairs(c.peers) do
          print(("t-chan   ch %d %s %s '%s' %d dBm%s"):format(c.channel, p.kind, p.id, p.name, p.rssi, p.app and " app" or ""))
        end
      end
    end
    say(("scanned %d channels: %s"):format(#chs, #parts > 0 and table.concat(parts, " ") or "nothing heard"))
  end)
  if ok then
    say("scanning...")
    print("t-chan again: " .. tostring(select(2, mesh.scan(function() end))))
  else
    say("scan: " .. tostring(err))
  end
end

function on_key(key)
  if key == "select" then
    scan({band = "2.4", dwell = 800})
  elseif key == "up" then
    scan({band = "all", dwell = 500})
  elseif key == "right" or key == "left" then
    at = key == "right" and at % #CHS + 1 or (at - 2) % #CHS + 1
    say(("channel %d: %s"):format(CHS[at], tostring(mesh.channel(CHS[at]))))
  elseif key == "down" then
    say("event channel: " .. tostring(mesh.channel()))
  else
    return false
  end
  show()
  return true
end

function on_mesh(from, msg)
  print("t-chan heard " .. from)
end
