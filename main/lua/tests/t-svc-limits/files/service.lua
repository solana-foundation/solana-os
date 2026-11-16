-- t-svc-limits's service: misbehaves as storage "mode" says.
local mode = storage.get("mode") or "spam"
print("t-svc-limits:svc mode " .. mode)
local t0 = sys.millis()
local keep = {}

function on_tick()
  if mode == "loop" then
    print("t-svc-limits:svc looping")
    while true do end
  elseif mode == "alloc" then
    for i = 1, 1e9 do keep[#keep + 1] = ("x"):rep(1000) .. i end
  elseif mode == "rep" then
    -- Few instructions, much work in C: the second of wall-clock time stops it.
    while true do local big = ("x"):rep(400000) end
  elseif mode == "pattern" then
    local s = ("a"):rep(5000)
    string.find(s, ("a*"):rep(20) .. "b")
  elseif mode == "minute" then
    -- ~90 000 instructions each second: under the per-callback cap, over 2 000 000 a minute.
    local x = 0
    for i = 1, 30000 do x = x + i end
    service.status(("burning, %d s"):format((sys.millis() - t0) // 1000))
  elseif mode == "spam" then
    local sent = 0
    for i = 1, 50 do if service.send({i = i}) then sent = sent + 1 end end
    service.status("spammed " .. sent)
  end
end

function on_stop()
  if mode == "stopper" then
    print("t-svc-limits:svc on_stop: looping forever")
    while true do end
  end
  print("t-svc-limits:svc on_stop after " .. (sys.millis() - t0) .. " ms")
end
