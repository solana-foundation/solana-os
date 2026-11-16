-- Regression test (not shipped): a service answering the mesh while its app
-- is closed. On badge A open it once (its service starts) and close it. On
-- badge B open it: SELECT pings every badge running it ("*"), DOWN floods 40
-- pings at once; pongs print here. Lines start "t-svc-mesh".
local log = ui.label(8, 8, "t-svc-mesh: SELECT ping, DOWN flood", {size = 12, w = 304})
ui.hint("SELECT ping   DOWN flood   RIGHT start service")
local n = 0

local function say(text)
  print("t-svc-mesh " .. text)
  log:text(text)
end

function on_key(k)
  if k == "select" then
    n = n + 1
    say("ping " .. n .. ": " .. tostring(mesh.send("*", {ping = n})))
    return true
  elseif k == "down" then
    -- 4 a second is the app's own limit: the flood spreads over 10 s.
    local sent, i, id = 0, 0, nil
    id = timer.every(260, function()
      i = i + 1
      if mesh.send("*", {ping = 1000 + i}) then sent = sent + 1 end
      if i >= 40 then timer.cancel(id); say("flood sent " .. sent .. " of 40") end
    end)
    return true
  elseif k == "right" then
    service.start(function(ok, err) say("start: " .. tostring(ok) .. " " .. tostring(err)) end)
    return true
  end
end

function on_mesh(from, msg)
  if msg.pong then say(("pong %d from %s (service has %d)"):format(msg.pong, from, msg.seen or -1)) end
end
