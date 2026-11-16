-- t-svc-mesh's service: answers every ping it hears, counts them.
local seen = 0
function on_mesh(from, msg)
  if msg.ping then
    seen = seen + 1
    local ok, err = mesh.send(from, {pong = msg.ping, seen = seen})
    print(("t-svc-mesh:svc ping %d from %s -> %s %s"):format(msg.ping, from, tostring(ok), tostring(err)))
    service.status(seen .. " pings answered")
  end
end
function on_meet(from, data)
  return {svc = true}
end
