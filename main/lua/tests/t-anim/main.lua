-- Regression test (not shipped): ui.anim (GIFs played), GIFs as still images,
-- loop options, play/stop, errors, memory. SELECT exits.
local function try(name, f)
  local ok, err = pcall(f)
  print(("t-anim %s: %s"):format(name, ok and "FAIL no error" or err))
end
local function kb() return ("%.1f KB"):format(collectgarbage("count")) end

print("t-anim start, heap " .. kb())
ui.bg("bg")
local ball = ui.anim(4, 4, "ball.gif", {scale = 3})          -- loops forever (the file says so)
print("t-anim ball, heap " .. kb())
local spin = ui.anim(110, 4, "spin.gif", {scale = 2})
print("t-anim spin, heap " .. kb())
local once = ui.anim(4, 90, "once.gif", {scale = 2})          -- no loop in the file: plays once
local twice = ui.anim(100, 90, "once.gif", {loop = 2})        -- twice, then stops
local still = ui.image(200, 90, "ball.gif")                   -- a GIF's first frame, still
local w, h = ui.load("ball.gif"):size()
print(("t-anim load gif %dx%d"):format(w, h))
local c = ui.canvas(4, 120, 40, 16, {scale = 3, color = "panel"})
c:image("ball.gif", 0, -6)
c:image("once.gif", 0, 8)

try("missing", function() ui.anim(0, 0, "nope.gif") end)
try("png", function() ui.anim(0, 0, "notgif.png") end)
try("bad loop", function() ui.anim(0, 0, "ball.gif", {loop = 0}) end)
try("stop on image", function() spin.stop(still) end)

local t, checked = 0, false
function on_tick(ms)
  t = t + ms
  if t > 2500 and not checked then
    checked = true
    print(("t-anim after 2.5 s: ball %s, once %s, twice %s, heap %s"):format(
      tostring(ball:playing()), tostring(once:playing()), tostring(twice:playing()), kb()))
    spin:stop()
    once:play()      -- from the start again
    print("t-anim spin stopped " .. tostring(spin:playing()) .. ", once again " .. tostring(once:playing()))
  end
end
ui.hint("SELECT exits")
print("t-anim done, heap " .. kb())
function on_key(k) if k == "select" then sys.exit() end return true end
