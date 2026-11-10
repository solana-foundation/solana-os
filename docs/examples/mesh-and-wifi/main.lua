-- Cheer (mesh): cheer with the badges around you, keep your count on a server.
--
-- SELECT sends a cheer to every badge nearby running this app, over the mesh
-- (ESP-NOW: no Wi-Fi needed). Your cheers are counted on your server with
-- `http`. The two share the badge's one radio, and mesh.info() says how:
--
--   online = true   Wi-Fi is joined on the mesh channel: the mesh and the
--                   server work at the same time. Post whenever you like.
--   online = false  opening this app took the radio from Wi-Fi. To post,
--                   mesh.pause() (Wi-Fi rejoins in a few seconds), post, then
--                   mesh.resume(). Cheers from others don't arrive meanwhile.
--
-- Permissions "mesh" and "http", hosts ["cheer.example.com"]: the server side
-- is ../wifi-room/server.ts (POST /score). mesh.info is Solana OS 1.0.0 and
-- mesh.pause 0.5.15; on older badges the app still runs and syncs when it can.

local HOST = "https://cheer.example.com"
local SYNC_EVERY = 20             -- cheers between automatic syncs
local WAIT_S = 20                 -- how long to wait for Wi-Fi after mesh.pause()
local MAX_BATCH = 1000            -- cheers per request: the server takes no more

-- Kept in one storage key, written in one go, so a batch is never half saved.
local saved = storage.get("cheer") or {}
local mine = saved.mine or 0                   -- all my cheers
local unsynced = saved.unsynced or 0           -- not in a batch yet
local batch = saved.batch                      -- {id = "...", n = cheers}: sent, not acknowledged yet
local heard = 0                                -- cheers from others since the app opened
local last_send = -1000
local syncing = false

ui.bg("bg")
ui.label(10, 6, "Cheer", {size = 20})
local radio = ui.label(10, 34, "", {w = 300})
local radio2 = ui.label(10, 52, "", {color = "dim", w = 300})
local big = ui.label(0, 84, "", {size = 32, w = ui.W, align = "center", color = "green"})
local small = ui.label(0, 128, "", {w = ui.W, align = "center", color = "dim"})
local last = ui.label(0, 150, "", {w = ui.W, align = "center"})
ui.hint("SELECT cheer   RIGHT sync   CANCEL exit")

-- Cheers not on the server yet: the batch on its way and the rest.
local function waiting() return unsynced + (batch and batch.n or 0) end

-- What the radio is doing, for the player. Older builds have no mesh.info.
local function info()
  if mesh.info then return mesh.info() end
  return {online = badge.online(), paused = false}
end

local function show()
  local i = info()
  local where = i.channel and string.format("channel %d, %s GHz", i.channel, i.band) or "the mesh channel"
  if i.paused then
    radio:text("Syncing: Wi-Fi has the radio")
    radio:color("yellow")
    radio2:text("Cheers from others wait until it's done")
  elseif i.online then
    radio:text("On the mesh and online")
    radio:color("green")
    radio2:text(where .. ": cheers sync as you go")
  else
    radio:text("On the mesh, offline")
    radio:color("orange")
    radio2:text(where .. ". RIGHT pauses the mesh to sync")
  end
  big:text(tostring(mine))
  small:text(waiting() > 0 and (waiting() .. " not synced yet") or "all synced")
end

local function save()
  storage.set("cheer", {mine = mine, unsynced = unsynced, batch = batch})
end

-- 16 random hex digits (math.random is seeded per launch).
local function new_id()
  local r = math.random
  return string.format("%04x%04x%04x%04x", r(0, 0xFFFF), r(0, 0xFFFF), r(0, 0xFFFF), r(0, 0xFFFF))
end

-- POST the cheers in batches of at most MAX_BATCH. A batch keeps its id until the server
-- answers with that id, so when an answer is lost the next try sends the same batch again
-- and the server counts it once. The server learns the badge and player from the signed
-- headers, so the body only says which batch and how many.
local function post(done, synced)
  synced = synced or 0
  if not batch then
    if unsynced == 0 then
      if synced > 0 then ui.toast("Synced " .. synced .. " cheers") end
      return done()
    end
    batch = {id = new_id(), n = math.min(unsynced, MAX_BATCH)}
    unsynced = unsynced - batch.n
    save()                                  -- before sending: the id has to outlive the app
  end
  http.post(HOST .. "/score", {batch = batch.id, cheers = batch.n}, function(ok, res)
    local m = ok and res.status == 200 and json.decode(res.body)
    if type(m) == "table" and m.batch == batch.id then
      local n = batch.n
      batch = nil                           -- acknowledged: now it's safe to forget
      save()
      post(done, synced + n)                -- the next batch, if there is one
    else
      ui.toast("Not synced: " .. (ok and ("server said " .. res.status) or res))
      done()
    end
  end)
end

local function sync()
  if syncing or waiting() == 0 then return end
  local i = info()
  if i.online then                          -- the network is on the mesh channel, or Wi-Fi is free anyway
    syncing = true
    post(function() syncing = false show() end)
    return
  end
  if not mesh.pause then                    -- before 0.5.15: wait for the app to close
    ui.toast("Offline: syncs next time you're online")
    return
  end
  -- Hand the radio to Wi-Fi, wait for it to rejoin, post, take the radio back.
  syncing = true
  mesh.pause()
  show()
  local waited = 0
  local t
  t = timer.every(1000, function()
    waited = waited + 1
    if badge.online() then
      timer.cancel(t)
      post(function() mesh.resume() syncing = false show() end)
    elseif waited >= WAIT_S then
      timer.cancel(t)
      mesh.resume()
      syncing = false
      ui.toast("No Wi-Fi: will sync later")
      show()
    end
  end)
end

function on_key(k)
  if k == "right" then sync() return true end
  if k ~= "select" then return false end
  if sys.millis() - last_send < 500 then return true end   -- the mesh takes 4 messages a second
  last_send = sys.millis()
  mesh.send("*", {c = 1})
  mine = mine + 1
  unsynced = unsynced + 1
  save()
  led.fill(0x14, 0xF1, 0x95)
  timer.after(80, led.off)
  if waiting() >= SYNC_EVERY and info().online then sync() end  -- only automatic when it costs nothing
  show()
  return true
end

-- Anything can arrive over the radio: check the message before using it.
function on_mesh(from, msg)
  if type(msg) ~= "table" or msg.c ~= 1 then return end
  heard = heard + 1
  last:text(from .. " cheered (" .. heard .. " heard)")
end

timer.every(1000, show)                     -- online can change: Wi-Fi joins, drops, a hello moves the channel
show()
if waiting() > 0 and info().online then sync() end
