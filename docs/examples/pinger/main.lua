-- Pinger: SELECT pings every badge running it nearby. Its service (service.lua)
-- answers pings and counts who pinged, also while this app is closed.
local title = ui.label(8, 8, "Pinger", {size = 20})
local info = ui.label(8, 40, "Starting the service...", {size = 14, w = 304})
local log = ui.label(8, 70, "", {size = 12, w = 304})
ui.hint("SELECT ping   LEFT stop service")

local function show()
  local seen = storage.get("seen") or {}
  local n = 0
  for _ in pairs(seen) do n = n + 1 end
  info:text(("%d badges pinged you. Service: %s"):format(n, service.running() and "on" or "off"))
end

service.start(function(ok, err)
  if not ok then log:text("The service didn't start: " .. err) end
  show()
end)

function on_key(k)
  if k == "select" then
    local ok, err = mesh.send("*", {ping = sys.millis()})
    log:text(ok and "Ping sent" or ("Not sent: " .. err))
    return true
  elseif k == "left" then
    service.stop()
    timer.after(1500, show)
    return true
  end
end

-- Pongs come to the app (the service answers pings, not pongs).
function on_mesh(from, msg)
  if msg.pong then log:text("Pong from " .. from) end
end

-- The service tells the open app when someone pinged.
function on_service(msg)
  if msg.pinged then show() end
end
