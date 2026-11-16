-- Themes: your badge at a glance, in a look you pick. Twelve looks, each a
-- full screen drawn from its design (a bundled picture) with the badge's own
-- details over it: the owner, the badge ID, whether it is online, the battery.
-- LEFT and RIGHT change the look; the choice is kept. No permissions.
--
-- An app can only draw its own screen, so a look lasts while Themes is open.

local W, H = ui.W, ui.H

-- ---------------------------------------------------------------------------
-- What the looks show.

local data = {owner = "", id = "", net = "", power = "", online = false}

local function read()
  data.owner = badge.owner() or "Not linked yet"
  data.id = badge.id() or "------"
  data.online = badge.online()
  data.net = data.online and "Online" or "Offline"
  local b = sys.battery()
  data.power = b and (b .. "%") or "USB"
end

-- Text cut to n characters, so a long name never runs into the art.
local function fit(s, n)
  if #s <= n then return s end
  return s:sub(1, n - 1) .. "."
end

-- ---------------------------------------------------------------------------
-- The looks. `art` is the picture; each field is one label over it:
--   {what, x, y, size, colour, [w = width, align = "right"], [max = characters],
--    [up = true | low = true], [short = true: On / Off for the "net" field],
--    [on = colour when online, for the "net" field]}
-- `what` is a key of `data`, or text in quotes to draw as it is.

local function pearl(art, ink, dim, warn, ok)
  return {art = art, fields = {
    {"'My badge", 22, 13, 12, dim},
    {"owner", 22, 36, 20, ink, max = 15},
    {"id", 22, 82, 12, dim},
    {"power", 114, 83, 12, dim, w = 42, align = "center"},
    {"net", 30, 172, 12, warn, on = ok},
  }}
end

local looks = {
  {name = "Lumen", art = "art/lumen.png", fields = pearl(nil, 0xFFFFFF, 0xAEB4C4, 0xFFC79A, 0x8FEBD8).fields},
  {name = "Pearl", art = "art/pearl.png", fields = pearl(nil, 0x1F1B2E, 0x5A5370, 0x9A3C00, 0x10804E).fields},
  {name = "Black pearl", art = "art/black_pearl.png", fields = pearl(nil, 0xFFFFFF, 0xC2C0D2, 0xFFD6B8, 0x8FEBD8).fields},

  {name = "Icon grid", art = "art/icon_grid.png", fields = {}},

  {name = "Grouped list", art = "art/grouped_list.png", fields = {
    {"owner", 66, 11, 14, 0xFFFFFF, max = 24},
    {"id", 66, 30, 12, 0xC4C8D6},
    {"power", 120, 30, 12, 0xC4C8D6},
    {"net", 202, 66, 12, 0x9A3C00, on = 0x10804E, w = 80, align = "right"},
  }},

  {name = "Dial", art = "art/dial.png", fields = {
    {"power", 196, 18, 12, 0xD0D0D8, w = 42, align = "center"},
    {"id", 82, 40, 32, 0xFFFFFF},
    {"'Badge ID", 82, 88, 12, 0xB4B4BE},
    {"owner", 82, 106, 12, 0xFFFFFF, max = 21},
    {"net", 119, 149, 12, 0xFFB870, on = 0x34D17A},
  }},

  {name = "Control tiles", art = "art/control_tiles.png", fields = {
    {"owner", 173, 14, 12, 0xD8DAE6, max = 11},
    {"id", 173, 34, 20, 0xFFFFFF},
  }},

  {name = "Terminal", art = "art/terminal.png", fields = {
    {"'solana-os", 23, 5, 12, 0x3DFFA2},
    {"net", 224, 5, 12, 0xFFB866, on = 0x3DFFA2, low = true, short = true},
    {"owner", 116, 46, 12, 0x020503, max = 17},
    {"id", 240, 46, 12, 0x020503, w = 50, align = "right"},
    {"net", 116, 64, 12, 0xFFB866, on = 0x3DFFA2, low = true},
    {"power", 240, 64, 12, 0x2BB273, w = 50, align = "right"},
  }},

  {name = "E-paper", art = "art/epaper.png", fields = {
    {"net", 178, 5, 12, 0x141412, w = 116, align = "right", up = true},
    {"owner", 25, 46, 14, 0x141412, max = 15},
    {"power", 25, 92, 32, 0x141412},
    {"'POWER", 25, 134, 12, 0x141412},
    {"id", 91, 180, 12, 0x141412, w = 50, align = "right"},
    {"net", 206, 39, 12, 0xECE8DD, w = 80, align = "right", up = true},
  }},

  {name = "Bold blocks", art = "art/bold_blocks.png", fields = {
    {"net", 216, 5, 12, 0x111111, w = 50, align = "center", up = true, short = true},
    {"id", 105, 33, 12, 0x111111, w = 42, align = "center"},
    {"power", 33, 52, 32, 0xFFFFFF},
    {"'POWER", 33, 91, 12, 0xFFFFFF},
    {"owner", 33, 110, 12, 0xFFFFFF, max = 15},
    {"net", 232, 55, 12, 0x111111, w = 52, align = "center", up = true, short = true},
  }},

  {name = "Monster RPG", art = "art/monster_rpg.png", fields = {
    {"id", 100, 13, 12, 0xFFFFFF, w = 80, align = "right"},
    {"owner", 70, 33, 12, 0x23233A, w = 110, align = "right", up = true, max = 15},
    {"'POWER", 32, 68, 12, 0x5A5A70},
    {"power", 100, 60, 20, 0x23233A, w = 80, align = "right"},
    {"net", 100, 91, 12, 0xB83A16, on = 0x2E7D4F, w = 80, align = "right", up = true, short = true},
    {"'WHAT WILL YOU DO?", 34, 169, 12, 0x23233A},
  }},

  {name = "Streaming", art = "art/streaming.png", fields = {
    {"owner", 14, 75, 12, 0x57D98A, max = 11},
    {"power", 96, 76, 12, 0xE6E6EA, w = 28, align = "center"},
  }},
}

-- ---------------------------------------------------------------------------

local current = storage.get("look") or 1
if type(current) ~= "number" or not looks[current] then current = 1 end
local labels = {}                   -- { {label, field}, ... } of the look on screen

local function value(f)
  local what = f[1]
  if what:sub(1, 1) == "'" then return what:sub(2) end
  local s = data[what]
  if f.short then s = data.online and "On" or "Off" end
  if f.up then s = s:upper() elseif f.low then s = s:lower() end
  if f.max then s = fit(s, f.max) end
  return s
end

local function colour(f)
  if f[1] == "net" and f.on and data.online then return f.on end
  return f[5]
end

local function show(i)
  current = i
  local look = looks[i]
  ui.clear()
  labels = {}
  ui.bg(0x000000)
  ui.image(0, 0, look.art)
  for _, f in ipairs(look.fields) do
    local l = ui.label(f[2], f[3], value(f), {size = f[4], color = colour(f), w = f.w, align = f.align})
    labels[#labels + 1] = {l, f}
  end
end

local function refresh()
  read()
  for _, p in ipairs(labels) do
    p[1]:text(value(p[2]))
    p[1]:color(colour(p[2]))
  end
end

read()
show(current)
ui.toast(looks[current].name .. "   LEFT RIGHT change the look")
timer.every(2000, refresh)

function on_key(k, rep)
  if k == "left" or k == "right" then
    local n = current + (k == "right" and 1 or -1)
    if n < 1 then n = #looks elseif n > #looks then n = 1 end
    show(n)
    storage.set("look", n)
    ui.toast(looks[n].name .. "  " .. n .. "/" .. #looks)
    return true
  elseif k == "select" then
    ui.toast(looks[current].name .. "  " .. current .. "/" .. #looks)
    return true
  end
  return false                      -- CANCEL exits
end
