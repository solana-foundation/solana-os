-- Regression test (not shipped): the Lua heap limits. Fills the heap with
-- 1 KB strings until the allocator refuses, catches the error and prints how
-- far it got, then stays on screen so "heap" can be read on the console.
-- Expected: "not enough memory" either at the cap (LUA_HOST_HEAP_MAX, the
-- emulator) or earlier when the badge's heap is down to LUA_HOST_SYS_RESERVE
-- (32 KB); the badge keeps working either way. Measured on 0.5.13 with Wi-Fi
-- joined: ~89 KB of strings, 103 KB peak, 32 KB of system heap left.
local t, n = {}, 0
local ok, err = pcall(function()
  while true do
    n = n + 1
    t[n] = string.rep(string.char(64 + n % 26), 1000)
  end
end)
print(string.format("t-heap: %d KB of strings, then: %s", n, tostring(err)))
print(string.format("t-heap: collectgarbage count %.1f KB", collectgarbage("count")))
ui.label(10, 60, string.format("%d KB of strings held", n))
ui.label(10, 90, "CANCEL exits", {color = "dim"})
