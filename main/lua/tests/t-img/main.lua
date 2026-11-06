-- Regression test (not shipped): ui.load / ui.image / ui.canvas, their limits
-- and errors, and memory. SELECT exits.
local function try(name, f)
  local ok, err = pcall(f)
  print(("t-img %s: %s"):format(name, ok and "FAIL no error" or err))
end
local function kb() return ("%.1f KB"):format(collectgarbage("count")) end

print("t-img start, heap " .. kb())
ui.bg("bg")

-- A PNG at 1x and 2x (one load, two views).
local disc = ui.load("disc.png")
local w, h = disc:size()
print(("t-img disc %dx%d, heap %s"):format(w, h, kb()))
ui.image(4, 4, disc)
local big = ui.image(56, 4, disc, {scale = 2})

-- Pixel art, default palette and a custom one.
local heart = ui.load({
  ".mm.mm.",
  "mmmmmmm",
  "mmmmmmm",
  ".mmmmm.",
  "..mmm..",
  "...m...",
})
ui.image(160, 4, heart, {scale = 6})
ui.image(210, 4, {"rgr", "g.g", "rgr"}, {scale = 8, palette = {r = 0xFF0000, g = "green"}})

-- A canvas: 80 x 40 pixels shown 3x (240 x 120 on screen, 6.4 KB).
local c = ui.canvas(4, 100, 80, 36, {scale = 3, color = "panel"})
c:rect(0, 30, 80, 6, "purple")
c:line(0, 0, 79, 29, "aqua")
c:line(79, 0, 0, 29, "yellow")
for i = 0, 79, 4 do c:pixel(i, 2, "text") end
local ship = ui.load("ship.png")
c:image(ship, 34, 12)
print("t-img get " .. tostring(c:get(0, 31)) .. " " .. tostring(c:get(-1, 0)) .. " " .. tostring(c:get(0, 0)))
print("t-img canvas, heap " .. kb())

-- Errors
try("missing", function() ui.load("nope.png") end)
try("not png", function() ui.load("main.txt") end)
try("bad char", function() ui.load({"..x.."}) end)
try("bad scale", function() ui.image(0, 0, heart, {scale = 17}) end)
try("too big", function() ui.canvas(0, 0, 200, 200) end)
try("bad src", function() ui.image(0, 0, 42) end)
try("set on canvas", function() big.set(c, heart) end)
try("draw on image", function() c.rect(big, 0, 0, 1, 1, "green") end)
-- Far off the canvas costs only the visible part (clipped, no "too long").
c:line(-2000000000, -1000000000, 2000000000, 1000000000, "lime")
c:line(-2000000000, 5000, 2000000000, 5000, "lime")
print("t-img long lines ok")

-- Swap and rescale a shown image.
big:set(heart)
big:scale(4)
big:set(disc)
big:scale(2)

-- Animation: the ship flies across the canvas.
local x, frames, t0 = 0, 0, sys.ms and sys.ms() or 0
function on_tick(ms)
  c:rect(0, 8, 80, 22, "panel")
  c:image(ship, x, 12)
  x = (x + 1) % 80
  frames = frames + 1
  if frames == 100 then print("t-img 100 frames, heap " .. kb()) end
end
ui.label(100, 160, "t-img done", {size = 12})
ui.hint("SELECT exits")
print("t-img done, heap " .. kb())
function on_key(k) if k == "select" then sys.exit() end return true end
