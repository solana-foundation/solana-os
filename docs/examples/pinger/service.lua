-- Pinger's service: answers pings and remembers who pinged, in storage the
-- app reads. No screen here: it tells the owner with a toast and the
-- status line in Services.
local seen = storage.get("seen") or {}
local count = 0
for _ in pairs(seen) do count = count + 1 end
service.status(count .. " badges pinged you")

function on_mesh(from, msg)
  if not msg.ping then return end
  mesh.send(from, {pong = msg.ping})
  if not seen[from] then
    seen[from] = sys.time() or true
    count = count + 1
    storage.set("seen", seen)
    service.status(count .. " badges pinged you")
    service.notify("Ping from " .. from)   -- at most one every 10 s; the rest are dropped
  end
  service.send({pinged = from})            -- reaches the app only while it's open
end

function on_stop()
  storage.set("seen", seen)
end
