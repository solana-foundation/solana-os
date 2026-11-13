-- Regression test (not shipped): the Lua heap in PSRAM (2.0.0). Makes 124 UI
-- objects first (the cap is 128; their LVGL objects live in SRAM), then fills
-- the Lua heap with 64 KB strings, four per timer callback (one callback's
-- 200 000 instructions can't fill 6 MB), until the allocator refuses. Then it
-- prints how far it got and holds the strings, so "heap" and "lua mem" can be
-- read on the console meanwhile.
-- Expected on a badge with PSRAM: "not enough memory" just under 6 MB (the
-- cap), "psram" never under 512 KB free, SRAM where it was before the fill,
-- and a badge that keeps working. Without PSRAM (1.x limits): under 96 KB.
local m = sys.memory()
print(string.format("t-psram: sys.memory() used %d, max %d", m.used, m.max))
for i = 1, 120 do
  ui.label(4 + (i % 8) * 39, 120 + (i // 8) * 6, string.format("%03d", i), {size = 12, color = "dim"})
end
local status = ui.label(10, 30, "Filling the heap...")
local held = ui.label(10, 54, "")
ui.label(10, 84, "CANCEL exits", {color = "dim"})

local t, n, stopped = {}, 0, false
local CHUNK = 64 * 1024

local fill
fill = timer.every(50, function()
  if stopped then return end
  local ok, err = pcall(function()
    for _ = 1, 4 do
      n = n + 1
      t[n] = string.rep(string.char(65 + n % 26), CHUNK)
    end
  end)
  if not ok then
    stopped = true
    timer.cancel(fill)
    n = #t
    local kb = n * CHUNK // 1024
    print(string.format("t-psram: %d strings of 64 KB (%d KB), then: %s; sys.memory() used %d", n, kb,
                        tostring(err), sys.memory().used))
    status:text("Stopped: " .. tostring(err))
    held:text(string.format("%d KB held in 64 KB strings", kb))
  else
    held:text(string.format("%d KB so far", n * CHUNK // 1024))
  end
end)
