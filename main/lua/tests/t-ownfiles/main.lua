-- Regression test (not shipped): files (the app's own folder, 2.3.0) and
-- sys.tick. Put data.bin there first, 1000 bytes, byte i = i % 251:
--   lua recv t-ownfiles files/data.bin 1000 <sha256>
-- The chunk checks the API and prints "t-ownfiles ready", then on_tick counts
-- ticks at sys.tick(16) for 2 s and prints "t-ownfiles ticks <n>" (114 on a badge: 17.5 ms).
local function fails(f, ...)
  local ok, err = pcall(f, ...)
  assert(not ok, "expected an error")
  return tostring(err)
end

assert(files.dir() == "hh/files/t-ownfiles/", files.dir())
assert(files.size("data.bin") == 1000, "size")
assert(files.size("nope.bin") == nil)
local d = files.read("data.bin", 0, 1000)
assert(#d == 1000 and d:byte(1) == 0 and d:byte(252) == 0 and d:byte(1000) == 999 % 251, "read")
assert(#files.read("data.bin", 990, 100) == 10, "read the end")
assert(files.read("data.bin", 1000, 5) == "", "read at the end")
assert(files.read("data.bin", 0x7fffffff, 5) == "", "read far past the end")
local none, why = files.read("nope.bin")
assert(none == nil and why == "not found")
for _, bad in ipairs({"", ".", "..", "../app.json", "a/b", "/x", ".hidden", "x.", "x ", " x", "a\\b",
                      "save/", "save/..", "save/../x", "save/a/b", ("x"):rep(65), "a\0b", "a:b", "a*b"}) do
  assert(fails(files.size, bad):find("a file name"), "refused: " .. bad)
end
assert(fails(files.read, "data.bin", -1, 5):find("offset"))
assert(fails(files.read, "data.bin", 0, 256 * 1024 + 1):find("256 KB"))
-- writes: save/ only, no holes, 1 MB a file
assert(fails(files.write, "data.bin", 0, "x"):find("save/"), "the owner's files are read-only")
files.remove("save/t.sav")
assert(files.write("save/t.sav", 0, "hello"))
assert(files.write("save/t.sav", 5, " world"))
assert(files.read("save/t.sav") == "hello world")
assert(files.write("save/t.sav", 0, "J"))
assert(files.read("save/t.sav") == "Jello world", "overwrite in place")
local n, w = files.write("save/t.sav", 100, "x")
assert(n == nil and w:find("past the end"), "no holes")
n, w = files.write("save/t.sav", 1024 * 1024, "x")
assert(n == nil and w, "a write far past the end")
local seen = {}
for _, f in ipairs(files.list()) do seen[f.name] = f.size end
assert(seen["data.bin"] == 1000 and seen["save/t.sav"] == 11, "list")
assert(fails(files.remove, "data.bin"):find("own saves"))
assert(files.remove("save/t.sav") and files.size("save/t.sav") == nil)
-- sys.tick
assert(sys.tick() == 30, "the default period")
assert(fails(sys.tick, 15):find("16 to 1000"))
assert(fails(sys.tick, 1001):find("16 to 1000"))
assert(sys.tick(16) == 30 and sys.tick() == 16)
print("t-ownfiles ready")

local ticks, t = 0, 0
function on_tick(ms)
  t = t + ms
  ticks = ticks + 1
  if t >= 2000 then
    print("t-ownfiles ticks " .. ticks)
    sys.exit()
  end
end
