-- Regression test (not shipped): a hostile service. Each key picks how it
-- misbehaves, (re)starts it, and the app keeps ticking a counter on screen to
-- show the badge stays responsive. `svc` on the console shows how each ended.
--   UP loop (one callback over 100 000 instructions)   DOWN alloc (over 1 MB)
--   LEFT pattern (pathological string.find)           RIGHT minute (2 000 000 a minute)
--   SELECT stopper (on_stop never returns)             (open again) spam (floods on_service)
--   (data.json {"mode":"rep"} before opening) work in C past a second of wall-clock time
local log = ui.label(8, 8, "t-svc-limits", {size = 12, w = 304})
local tick = ui.label(8, 200, "", {size = 12})
ui.hint("UP loop DOWN alloc LEFT pattern RIGHT minute SEL stopper")
local frames, got = 0, 0
local modes = {up = "loop", down = "alloc", left = "pattern", right = "minute", select = "stopper"}

local function say(text)
  print("t-svc-limits " .. text)
  log:text(text)
end

local function start(mode)
  local ok, err = pcall(service.start, function(ok, err) say(mode .. ": start " .. tostring(ok) .. " " .. tostring(err)) end)
  if not ok then say(mode .. ": " .. err) end
end

local function go(mode)
  storage.set("mode", mode)
  if not service.running() then start(mode) return end
  service.stop()
  timer.after(1500, function() start(mode) end)
end

function on_key(k)
  if modes[k] then go(modes[k]) return true end
end

function on_service(msg) got = got + 1 end

function on_tick(ms)
  frames = frames + 1
  if frames % 33 == 0 then
    tick:text(("app alive: %d frames, %d messages from the service, running %s"):format(frames, got,
      tostring(service.running())))
  end
end

go(storage.get("mode") or "spam")
