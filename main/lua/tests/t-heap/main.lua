-- Regression test (not shipped): the Lua heap limits of a badge without PSRAM
-- (1.x, or 2.0.0 on a chip without it; with PSRAM see t-psram, since this one's
-- single callback can't fill 6 MB and stops with "Script took too long"). Makes 60 labels first
-- (their LVGL objects live on the badge's heap, ~15 KB, so on a badge the 32 KB
-- reserve is what the app meets, not the 96 KB cap), then fills the Lua heap
-- with 200-byte strings, smaller than the allocator's per-KB check step, until
-- it refuses, catches the error, prints how far it got and stays on screen so
-- "heap" can be read on the console while the strings are held.
-- Expected: "not enough memory" (at the cap in the emulator, whose heap is
-- huge), "heap" never under 32 KB free while the app holds its strings (0.5.13
-- let small allocations creep under the reserve), and a badge that keeps
-- working. The app may instead stop with "The badge is low on memory" when an
-- allocation outside the pcall (the print) is refused at the floor: that is
-- the same guarantee. Measured on 0.5.14 with Wi-Fi joined: see CHANGELOG.md.
for i = 1, 60 do
  ui.label(4 + (i % 6) * 52, 110 + (i // 6) * 11, string.format("label %02d %s", i, string.rep("-", 60)), {size = 14, color = "dim"})
end
local t, n = {}, 0
local ok, err = pcall(function()
  while true do
    n = n + 1
    t[n] = string.rep(string.char(64 + n % 26), 200)
  end
end)
print(string.format("t-heap: %d strings of 200 bytes (%d KB), then: %s", n, n * 200 // 1024, tostring(err)))
ui.label(10, 60, string.format("%d KB held in 200 B strings", n * 200 // 1024))
ui.label(10, 84, "CANCEL exits", {color = "dim"})
