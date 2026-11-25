-- Regression test (not shipped), Solana OS 2.5.0: canvas parts, mirrors and
-- turns, changed-area redraws, ui.frame, on_key_up, collectgarbage, sys.budget,
-- the main chunk's budget and ui.load's errors. tests/play-run.ts drives it in
-- the emulator and reads the screen; on a badge: lua run t-play, then the keys
-- the runner presses (see play-run.ts) and a look at the screen.
local function say(fmt, ...) print("t-play " .. string.format(fmt, ...)) end
local function err(f)
  local ok, e = pcall(f)
  return ok and "no error" or tostring(e)
end

-- The main chunk has 2 000 000 instructions: 600 000 here, then still room.
local n = 0
for i = 1, 300000 do n = n + i end
say("main budget left %d", sys.budget())

-- collectgarbage: silent where it used to raise.
say("gc %s %s %s %s %s", tostring(collectgarbage()), tostring(collectgarbage("collect")),
    tostring(collectgarbage("step")), tostring(collectgarbage("isrunning")),
    type(collectgarbage("count")))
say("gc bad: %s", err(function() collectgarbage("setpause", 100) end))

-- ui.load's errors name the problem.
say("load empty path: %s", err(function() ui.load("") end))
say("load empty rows: %s", err(function() ui.load({""}) end))
say("load no rows: %s", err(function() ui.load({}) end))

-- A 4 x 3 image of twelve colours, distinct in RGB565.
local chars, pal = "ABCDEFGHIJKL", {}
for i = 1, 12 do pal[chars:sub(i, i)] = (i * 16 + 8) << 16 | (255 - i * 16) << 8 | (i * 8) end
local sheet = ui.load({"ABCD", "EFGH", "IJKL"}, {palette = pal})
local ref = ui.canvas(0, 0, 4, 3)
ref:show(false)
ref:image(sheet, 0, 0)

-- Every turn and mirror of the 3 x 2 part at (1, 1), against a forward mapping.
local work = ui.canvas(0, 0, 4, 4)
work:show(false)
local sx, sy, sw, sh = 1, 1, 3, 2
local bad, cases = 0, 0
for _, rot in ipairs({0, 90, 180, 270, -90, 450}) do
  for _, fx in ipairs({false, true}) do
    for _, fy in ipairs({false, true}) do
      work:clear()
      work:image(sheet, 0, 0, sx, sy, sw, sh, {rotate = rot, flip_x = fx, flip_y = fy})
      local r = rot % 360
      for b = 0, sh - 1 do
        for a = 0, sw - 1 do
          local a2, b2 = fx and sw - 1 - a or a, fy and sh - 1 - b or b
          local u, v
          if r == 0 then u, v = a2, b2
          elseif r == 90 then u, v = sh - 1 - b2, a2
          elseif r == 180 then u, v = sw - 1 - a2, sh - 1 - b2
          else u, v = b2, sw - 1 - a2 end
          if work:get(u, v) ~= ref:get(sx + a, sy + b) then bad = bad + 1 end
        end
      end
      -- Nothing outside the result.
      local dw = (r == 90 or r == 270) and sh or sw
      local dh = (r == 90 or r == 270) and sw or sh
      for v = 0, 3 do for u = 0, 3 do
        if (u >= dw or v >= dh) and work:get(u, v) then bad = bad + 1 end
      end end
      cases = cases + 1
    end
  end
end
say("turns %d cases, %d wrong", cases, bad)
work:clear()
work:image(sheet, -1, -1, 0, 0, 4, 3, {rotate = 90})   -- clipped at the top-left
say("turn clipped: %s", tostring(work:get(0, 0) == ref:get(1, 1)))   -- (u,v)=(1,1) of the 3 x 4 result
-- A canvas removed by the options' __index mid-call: an error, not a write to it.
local victim = ui.canvas(0, 0, 4, 4)
local trap = setmetatable({}, {__index = function() victim:remove() end})
say("removed mid-call: %s", err(function() victim:image(sheet, 0, 0, trap) end))
say("part outside: %s", err(function() work:image(sheet, 0, 0, 3, 0, 2, 2) end))
say("bad rotate: %s", err(function() work:image(sheet, 0, 0, {rotate = 45}) end))

-- ui.frame: a hidden red canvas to the screen at 2x (on SELECT: by then the
-- app area's first redraw, which would paint over it, is done).
local fr = ui.canvas(0, 0, 20, 10, {color = 0xFF0000})
fr:show(false)
local function frames()
  say("frame shown: %s", tostring(ui.frame(fr, 10, 150, 2)))
  say("frame outside: %s", err(function() ui.frame(fr, 300, 0, 2) end))
  say("frame image: %s", tostring(ui.frame(sheet, 120, 150, 4)))
end

-- A visible canvas the runner reads off the screen.
local scr = ui.canvas(200, 40, 20, 20, {scale = 4, color = 0x000000})
local step = 0
local function draw()
  step = step + 1
  if step == 1 then
    scr:pixel(2, 2, 0x00FF00)
  elseif step == 2 then
    scr:rect(15, 15, 3, 3, 0x0000FF)
  elseif step == 3 then
    scr:image(sheet, 6, 6, 0, 0, 2, 1, {flip_x = true})   -- B then A
  end
  say("drew %d", step)
end

local ticks, first_tick = 0, nil
function on_tick(ms)
  ticks = ticks + 1
  if not first_tick then
    first_tick = sys.budget()
    say("tick budget %d", first_tick)
  end
end

function on_key(k, rep)
  say("key %s%s", k, rep and " repeat" or "")
  if k == "right" and not rep then draw() end
  if k == "select" then frames() end
  if k == "left" and not rep then
    ui.confirm("A dialog over the frame", function(yes) say("confirm %s", tostring(yes)) end)
    say("frame under dialog: %s", tostring(ui.frame(fr, 10, 150, 2)))
  end
  if k == "up" and not rep then sys.exit() end
  return true
end

function on_key_up(k, ms)
  say("up %s %d", k, ms)
end

say("ready")
