-- Regression test (not shipped): the app and its service write different keys of
-- their one shared storage at the same time (LUA_API.md "service": both see each
-- other's writes). Each writes 200 keys of its own; none may be lost. On a badge
-- the service's task is preempted mid-write by the app's, the race the shared
-- copy's version check exists for. Prints "t-svc-store a=<n> s=<n>".
ui.label(8, 8, "t-svc-store", {size = 12})
service.start(function(ok, err) print("t-svc-store start: " .. tostring(ok) .. " " .. tostring(err)) end)
local i = 0
local function count(prefix)
  local n = 0
  for k = 1, 200 do if storage.get(prefix .. k) == k then n = n + 1 end end
  return n
end
function on_tick(ms)
  i = i + 1
  if i <= 200 then storage.set("a" .. i, i) end
  if i % 100 == 0 then print(("t-svc-store a=%d s=%d"):format(count("a"), count("s"))) end
end
-- On a badge (not in the emulator, whose tasks take turns) this also races the
-- shared copy's commits; t-svc-final races the two last saves (hardware, see its
-- header). Both are run with tools/badge.py-style drivers: lua recv the files
-- (bundled ones by their bare path, e.g. "service.lua"), lua run, key select once.
