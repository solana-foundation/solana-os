-- t-svc's service: proves what is nil, counts ticks into storage, echoes
-- the app's messages back, says one notice.
print("t-svc:svc boot, heap " .. sys.memory().used .. " of " .. sys.memory().max)

-- Everything that touches the screen, the radio's channel, Wi-Fi, Bluetooth,
-- or spends or changes !LAMPs and items, or mails another attendee, must be
-- nil in a service.
local forbidden = {
  "ui", "led", "wifi", "ble", "ws", "quest",
  "sys.keys", "sys.exit", "sys.tick",
  "lamps.spend", "lamps.transfer", "lamps.deposit", "lamps.award",
  "items.grant", "items.update", "items.remove",
  "mesh.channel", "mesh.scan", "mesh.pause", "mesh.resume", "mesh.bridge", "mesh.info",
  "service.start", "service.running",
  "mail.send",                        -- only the app on screen mails (LUA_API.md "mail")
  "files.write", "files.remove",      -- ... and writes the owner's files (LUA_API.md "files")
  "load", "dofile", "loadfile",
}
local bad = {}
for _, path in ipairs(forbidden) do
  local v = _G
  for part in path:gmatch("[^.]+") do v = type(v) == "table" and v[part] or nil end
  if v ~= nil then bad[#bad + 1] = path end
end
-- pairs fills a lazy library: a trimmed one must still have nothing more.
for _, lib in ipairs({"lamps", "items", "mesh", "sys"}) do
  for k in pairs(_G[lib]) do
    if (lib .. "." .. k):match("spend") or (lib .. "." .. k):match("grant") or k == "keys" or k == "channel" then
      bad[#bad + 1] = lib .. "." .. k
    end
  end
end
print("t-svc:svc forbidden: " .. (#bad == 0 and "all nil" or ("FAIL " .. table.concat(bad, ", "))))
local allowed = {"timer.every", "storage.set", "fs.read", "json.encode", "crypto.sha256", "solana.encode",
  "lamps.balance", "items.list", "items.all", "mesh.send", "mesh.nearby", "mesh.meet", "mesh.meets",
  "sys.millis", "sys.memory", "badge.id", "service.send", "service.status", "service.notify", "service.stop", "require",
  "mail.list", "mail.get", "files.dir", "files.list", "files.size", "files.read", "profile.me", "profile.get",
  "profile.items", "profile.pages"}
local missing = {}
for _, path in ipairs(allowed) do
  local v = _G
  for part in path:gmatch("[^.]+") do v = type(v) == "table" and v[part] or nil end
  if type(v) ~= "function" then missing[#missing + 1] = path end
end
print("t-svc:svc allowed: " .. (#missing == 0 and "all there" or ("FAIL missing " .. table.concat(missing, ", "))))
-- Timers: 4 at most, never under a second.
local t0 = sys.millis()
timer.after(10, function() print("t-svc:svc timer after " .. (sys.millis() - t0) .. " ms (asked 10)") end)
local ok, err = pcall(function() for _ = 1, 5 do timer.every(5000, function() end) end end)
print("t-svc:svc timers: " .. tostring(err))

local ticks = storage.get("svc_ticks") or 0
local notified = false
function on_tick(ms)
  ticks = ticks + 1
  storage.set("svc_ticks", ticks)
  service.status(("ticks %d, app said %s"):format(ticks, tostring(storage.get("from_app"))))
  if ticks % 10 == 0 then print("t-svc:svc tick " .. ticks .. " (" .. ms .. " ms)") end
  if not notified and ticks >= 3 then
    notified = true
    print("t-svc:svc notify: " .. tostring(service.notify("hello from the background", {led = 0x9945FF})))
    print("t-svc:svc notify again: " .. tostring(service.notify("too soon")))
  end
end

local saw = 0
function on_message(msg)
  saw = saw + 1
  storage.set("svc_saw", saw)
  if msg.n then
    print("t-svc:svc message " .. json.encode(msg) .. " -> " .. tostring(service.send({echo = msg.n, saw = saw})))
  end
end

function on_stop()
  print("t-svc:svc on_stop after " .. ticks .. " ticks, " .. saw .. " messages")
end
