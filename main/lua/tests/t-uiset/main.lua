-- Regression test (not shipped): UI objects and their text live in SRAM, not
-- the Lua heap (2.0.0), so every call that makes UI objects checks it leaves
-- the badge 32 KB of SRAM (2.0.1). This one checks the calls that GROW what
-- objects already hold (2.1.0, from review): it makes 60 empty labels and an
-- empty list, then lists and rects until the badge refuses more (or the
-- 128-object cap), so SRAM sits near the floor, then fills the labels with 200
-- bytes each (12 KB, the text cap), the list with long rows, the hint and a
-- toast.
-- Expected (2.1.0): the fills stop with "...: the badge is low on memory", and
-- the console's "heap" shows "sram: ... min" (SRAM malloc can take) at 32768
-- or more while the app holds its UI. Measured on a prototype with Wi-Fi off:
-- 40 312 (before the fix every fill went through and it fell to 92).
local labels, made = {}, {lists = 0, rects = 0}
for i = 1, 60 do
  labels[i] = ui.label(2 + (i % 10) * 31, 2 + (i // 10) * 12, "", {size = 12})
end
local list = ui.list(0, 90, 160, 108, {})
local ok, err = pcall(function()
  while true do ui.list(0, 0, 320, 216, {}); made.lists = made.lists + 1 end
end)
print("t-uiset: lists until refused: " .. made.lists .. ", then: " .. tostring(err))
ok, err = pcall(function()
  while true do ui.rect(0, 0, 1, 1); made.rects = made.rects + 1 end
end)
print("t-uiset: rects until refused: " .. made.rects .. ", then: " .. tostring(err))

local function try(what, f)
  local n, ok, err = 0, true, nil
  ok, err = pcall(function() n = f() end)
  print(string.format("t-uiset: %s: %s%s", what, ok and "all done" or "refused: ", ok and "" or tostring(err)))
  return ok
end
local filled = 0
try("label:text x60", function()
  for i = 1, 60 do labels[i]:text(string.rep(string.char(65 + i % 26), 200)); filled = i end
  return filled
end)
print("t-uiset: labels filled: " .. filled)
try("list:items", function()
  local items = {}
  for i = 1, 50 do
    items[i] = {title = string.rep("T", 39), sub = string.rep("s", 55), right = string.rep("r", 19)}
  end
  list:items(items)
end)
try("ui.hint", function() ui.hint(string.rep("h", 200)) end)
try("ui.toast", function() ui.toast(string.rep("t", 200)) end)
print("t-uiset: done; read heap now. CANCEL exits")
function on_key(k) if k == "back" then sys.exit() end return true end
