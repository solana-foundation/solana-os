-- Regression test (not shipped): Lua's mail module (Mailbox, Solana OS 2.3.0).
-- At launch it checks the arguments mail.send refuses, lists this app's mail on
-- the badge, and, when storage holds {"to": "<badge id>"} (the console's
-- `lua put`, or a test writing data.json), sends one there. SELECT sends to that
-- badge or the nearest one; on_mail prints what arrives. Lines start "t-mail".
local status = ui.label(8, 8, "t-mail: SELECT mails a badge", {size = 12, w = 304})
local log = ui.label(8, 30, "", {size = 12, w = 304})
ui.hint("SELECT send   DOWN list")
local n = 0

local function say(text)
  print("t-mail " .. text)
  log:text(text)
end

local function check(name, ...)
  local ok, err = mail.send(...)
  print(("t-mail %s: %s %s"):format(name, tostring(ok), tostring(err)))
end
check("bad id", "x", "hi")
check("emu id", "eZZZZ1", "")
check("long title", "7K2Q9M", ("t"):rep(33))
check("newline", "7K2Q9M", "ok", "two\nlines")
check("long text", "7K2Q9M", "ok", ("w"):rep(201))
check("big data", "7K2Q9M", "ok", "", {s = ("d"):rep(130)})
check("accent", "7K2Q9M", "caf\195\169")
print("t-mail no table: " .. tostring(pcall(mail.send, "7K2Q9M", "ok", "", "data")))
print("t-mail get nothing: " .. tostring(mail.get(999999999)))

local function list()
  local all = mail.list()
  say(("%d in the mailbox"):format(#all))
  for _, m in ipairs(all) do
    local full = mail.get(m.id)
    print(("t-mail item %d from %s by %s read %s: %s | %s | %s"):format(m.id, m.from, tostring(m.by), tostring(m.read),
      m.title, full and full.text or "?", full and full.data and json.encode(full.data) or "-"))
  end
end

local function send(to)
  n = n + 1
  local ok, err = mail.send(to, "t-mail #" .. n, "Sent by the t-mail test", {n = n})
  say(("send to %s: %s %s"):format(to, tostring(ok), tostring(err)))
  local again, why = mail.send(to, "too soon")
  print("t-mail rate: " .. tostring(again) .. " " .. tostring(why))
end

function on_mail(m)
  say(("on_mail %d from %s: %s (%s) data %s"):format(m.id, m.from, m.title, m.text,
    m.data and json.encode(m.data) or "-"))
end

function on_key(k)
  if k == "select" then
    local to = storage.get("to")
    if not to then
      for _, p in ipairs(mesh.nearby()) do
        if p.kind == "badge" then to = p.id break end
      end
    end
    if to then send(to) else say("nobody to mail") end
    return true
  end
  if k == "down" then list() return true end
  return false
end

list()
local to = storage.get("to")
if to then send(to) end
status:text("t-mail: ready")
