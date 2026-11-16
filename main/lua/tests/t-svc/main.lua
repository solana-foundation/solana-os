-- Regression test (not shipped): a background service's lifecycle, messages
-- both ways, shared storage. Opening it starts the service (the first time the
-- badge asks: SELECT allows). SELECT sends a message, RIGHT reads the
-- service's counter from storage, LEFT stops the service. Lines start "t-svc".
local log = ui.label(8, 8, "t-svc", {size = 12, w = 304})
ui.hint("SELECT send   RIGHT storage   LEFT stop")
local n = 0

local function say(text)
  print("t-svc " .. text)
  log:text(text)
end

say("running before start: " .. tostring(service.running()))
service.start(function(ok, err)
  say("start: " .. tostring(ok) .. " " .. tostring(err) .. ", running " .. tostring(service.running()))
  storage.set("from_app", "hello")
end)

-- Two starts at once: the second one is refused while the first waits.
print("t-svc second start: " .. tostring(select(2, pcall(service.start, function() end))))
-- Too big for a message.
print("t-svc big send: " .. tostring(select(2, service.send({s = ("x"):rep(1100)}))))

function on_service(msg)
  say("from service: " .. json.encode(msg))
end

function on_key(k)
  if k == "select" then
    n = n + 1
    say("send " .. n .. ": " .. tostring(service.send({n = n, t = sys.millis()})))
    -- A flood: only 8 wait.
    local queued = 0
    for i = 1, 20 do
      if service.send({flood = i}) then queued = queued + 1 end
    end
    say("flood queued " .. queued .. " of 20")
    return true
  elseif k == "right" then
    say("storage: svc_ticks=" .. tostring(storage.get("svc_ticks")) .. " svc_saw=" .. tostring(storage.get("svc_saw")))
    return true
  elseif k == "left" then
    say("stop: " .. tostring(service.stop()))
    return true
  end
end
