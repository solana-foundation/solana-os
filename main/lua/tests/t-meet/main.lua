-- Regression test (not shipped): mesh.meet between two badges running this app
-- (two emulator tabs, or a badge and a bridged emulator). SELECT asks the
-- nearest badge; the other side accepts; RIGHT switches the radio to Wi-Fi and
-- lists what the server holds; LEFT goes back to the mesh. Lines start "t-meet".
local status = ui.label(8, 8, "t-meet: SELECT meets the nearest badge", {size = 12, w = 304})
local log = ui.label(8, 30, "", {size = 12, w = 304})
ui.hint("SELECT meet   RIGHT sync   LEFT mesh")
local n = 0

local function say(text)
  print("t-meet " .. text)
  log:text(text)
end

-- Argument checks: the mistakes an author makes fail in Lua, not on the air.
local function try(name, f)
  local ok, err = pcall(f)
  print(("t-meet %s: %s"):format(name, ok and "FAIL no error" or err))
end
try("bad id", function() mesh.meet("x", {}, function() end) end)
try("no table", function() mesh.meet("7K2Q9M", "x", function() end) end)
try("no cb", function() mesh.meet("7K2Q9M", {}) end)
print("t-meet far: " .. tostring(select(2, mesh.meet("ZZZZZZ", {}, function() end))))

local function nearest()
  for _, p in ipairs(mesh.nearby()) do
    if p.kind == "badge" then return p end
  end
end

local function ask()
  local p = nearest()
  if not p then say("nobody nearby") return end
  n = n + 1
  local big = mesh.meet(p.id, {s = ("x"):rep(120)}, function() end)
  print("t-meet big: " .. tostring(big))
  local ok, err = mesh.meet(p.id, {n = n, hi = "from the asker"}, function(ok, res)
    if ok then
      say(("met %s: %s"):format(res.peer, json.encode(res.data)))
    else
      say("meet failed: " .. res)
    end
  end)
  if not ok then say("meet: " .. err) return end
  print("t-meet busy: " .. tostring(select(2, mesh.meet(p.id, {}, function() end))))
  say("asked " .. p.id)
end

function on_meet(from, data)
  if data.no then
    say("refused " .. from)
    return nil
  end
  say(("accepted %s: %s"):format(from, json.encode(data)))
  return {n = data.n, hi = "from the accepter"}
end

local function sync()
  mesh.pause()
  say("radio to Wi-Fi...")
  local tries = 0
  local id
  id = timer.every(1000, function()
    tries = tries + 1
    if not badge.online() and tries < 20 then return end
    timer.cancel(id)
    mesh.meets(function(ok, res)
      if not ok then say("meets: " .. tostring(res)) return end
      say(("server holds %d, next %s"):format(#res.items, tostring(res.next)))
      for _, m in ipairs(res.items) do
        print(("t-meet #%d %s asked=%s mine=%s theirs=%s bridged=%s at=%d"):format(
          m.id, m.peer, tostring(m.asked), json.encode(m.mine), json.encode(m.theirs), tostring(m.bridged), m.at))
      end
    end)
  end)
end

function on_key(k)
  if k == "select" then ask() return true end
  if k == "right" then sync() return true end
  if k == "left" then mesh.resume() say("radio to the mesh") return true end
  return false
end

timer.every(1000, function()
  local p = nearest()
  status:text(p and ("nearest: " .. p.id .. (p.bridged and " (bridged)" or "")) or "t-meet: nobody nearby")
end)
print("t-meet ready")
