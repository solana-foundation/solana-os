-- C vs Lua benchmark, the Lua side (../README.md). ../c/main.c is the same app
-- in C: the same menu, phases and console lines, so run.py drives both, and
-- each workload mirrors ../c/kernels.c step by step so the checksums match.
-- Plain, reasonably careful Lua: locals, arrays instead of small tables, no
-- tricks a Lua programmer wouldn't use. Times are sys.millis() (whole ms;
-- summed over many callbacks the rounding evens out), printed as us.

local SAFE_MS = 40           -- a frame drawn in one callback if it takes less (the budget is ~65 ms)
local verifying = false

-- Mandelbrot: 320 x 240, -2..1 x -1.2..1.2, at most 32 iterations ----------------

local MW, MH, MITER = 320, 240, 32

local function mandel_row(y)  -- integers, 12 fractional bits (// floors like C's >>)
  local ci = -4915 + (y * 9830) // MH
  local sum = 0
  for x = 0, MW - 1 do
    local cr = -8192 + (x * 12288) // MW
    local zr, zi, n = 0, 0, 0
    while n < MITER do
      local zr2, zi2 = (zr * zr) // 4096, (zi * zi) // 4096
      if zr2 + zi2 > 16384 then break end
      zi = (zr * zi) // 2048 + ci
      zr = zr2 - zi2 + cr
      n = n + 1
    end
    sum = sum + n
  end
  return sum
end

local function mandelf_row(y)  -- floats: Lua's numbers are 32-bit floats on the badge
  local ci = -1.2 + y * (2.4 / MH)
  local sum = 0
  for x = 0, MW - 1 do
    local cr = -2.0 + x * (3.0 / MW)
    local zr, zi, n = 0.0, 0.0, 0
    while n < MITER do
      local zr2, zi2 = zr * zr, zi * zi
      if zr2 + zi2 > 4.0 then break end
      zi = 2.0 * zr * zi + ci
      zr = zr2 - zi2 + cr
      n = n + 1
    end
    sum = sum + n
  end
  return sum
end

-- Sieve up to 4096 ----------------------------------------------------------------

local SN = 4096
local flags = {}
local function sieve()
  for i = 2, SN do flags[i] = true end
  local i = 2
  while i * i <= SN do
    if flags[i] then
      for j = i * i, SN, i do flags[j] = false end
    end
    i = i + 1
  end
  local count = 0
  for k = 2, SN do if flags[k] then count = count + 1 end end
  return count
end

-- SHA-256, a 64-byte block at a time (32-bit integers wrap, >> is logical) -------

local K = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
}
local SHA_MSG = 4096
local sha_w, sha_h, w = {}, {}, {}

local function sha_init(h)
  h[1], h[2], h[3], h[4] = 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a
  h[5], h[6], h[7], h[8] = 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
end

local function sha_block(h, m, off)  -- m[off + 1 .. off + 16]
  for i = 1, 16 do w[i] = m[off + i] end
  for i = 17, 64 do
    local x, y = w[i - 15], w[i - 2]
    local s0 = ((x >> 7) | (x << 25)) ~ ((x >> 18) | (x << 14)) ~ (x >> 3)
    local s1 = ((y >> 17) | (y << 15)) ~ ((y >> 19) | (y << 13)) ~ (y >> 10)
    w[i] = w[i - 16] + s0 + w[i - 7] + s1
  end
  local a, b, c, d, e, f, g, hh = h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]
  for i = 1, 64 do
    local S1 = ((e >> 6) | (e << 26)) ~ ((e >> 11) | (e << 21)) ~ ((e >> 25) | (e << 7))
    local ch = (e & f) ~ (~e & g)
    local t1 = hh + S1 + ch + K[i] + w[i]
    local S0 = ((a >> 2) | (a << 30)) ~ ((a >> 13) | (a << 19)) ~ ((a >> 22) | (a << 10))
    local maj = (a & b) ~ (a & c) ~ (b & c)
    hh, g, f, e, d, c, b, a = g, f, e, d + t1, c, b, a, t1 + S0 + maj
  end
  h[1], h[2], h[3], h[4] = h[1] + a, h[2] + b, h[3] + c, h[4] + d
  h[5], h[6], h[7], h[8] = h[5] + e, h[6] + f, h[7] + g, h[8] + hh
end

local pad = {0x80000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, SHA_MSG * 8}
local function sha_unit(i)  -- 64 blocks, then the padding block returns the digest's first word
  local b = i % 65
  if b == 0 then sha_init(sha_h) end
  if b < 64 then
    sha_block(sha_h, sha_w, b * 16)
    return 0
  end
  sha_block(sha_h, pad, 0)
  return sha_h[1]
end

local sha_parts = {}
local function shaapi_unit(i)  -- the badge's own SHA-256 (crypto.sha256) of 1 KB
  local d = crypto.sha256(sha_parts[i % 4 + 1])
  local b1, b2, b3, b4 = d:byte(1, 4)
  return (b1 << 24) | (b2 << 16) | (b3 << 8) | b4
end

-- A big buffer: 256 K integers worked on 16 K at a time ----------------------------

local BUF, SEG = 256 * 1024, 16 * 1024
local buf_a, buf_b

local function buffers()  -- a coroutine: filling 512 K entries takes several callbacks
  buf_a, buf_b = table.create(BUF), table.create(BUF)
  for i = 0, BUF - 1 do
    buf_a[i + 1] = i ~ 0x12345678
    buf_b[i + 1] = 0
    if i % 8192 == 8191 then coroutine.yield() end
  end
end

local function segsum(t, seg)
  local s = 0
  for i = seg + 1, seg + SEG do s = s + t[i] end
  return s
end

local function fill_unit(i)
  local seg = (i % (BUF // SEG)) * SEG
  local a = buf_a
  for k = 0, SEG - 1 do a[seg + k + 1] = (k + seg) ~ 0x5A5A5A5A end
  return verifying and segsum(a, seg) or 0
end

local function copy_unit(i)
  local seg = (i % (BUF // SEG)) * SEG
  table.move(buf_a, seg + 1, seg + SEG, seg + 1, buf_b)
  return verifying and segsum(buf_b, seg) or 0
end

-- Drawing: the frame pixel by pixel on a ui.canvas (the best a Lua app has) -------

local canvas, pixel, fw, fh

local function draw_setup(cw, chh, scale)
  fw, fh = cw, chh
  canvas = ui.canvas(0, 0, cw, chh, {scale = scale, color = 0x000000})
  pixel = canvas.pixel  -- the method looked up once
end

local function draw_unit(i)
  local y, t = i % fh, i // fh
  local c, px = canvas, pixel
  local sum = 0
  local g = ((y << 1) + t) & 63
  for x = 0, fw - 1 do
    local r, b = (x + t) & 31, ((x ~ y) >> 3) & 31
    sum = sum + ((r << 11) | (g << 5) | b)
    px(c, x, y, (r << 19) | (g << 10) | (b << 3))
  end
  return sum
end

-- API-heavy ---------------------------------------------------------------------------

local labels = {}
local function labels_setup()
  for i = 0, 15 do
    labels[i + 1] = ui.label(10 + (i % 4) * 75, 20 + (i // 4) * 30, "0", {size = 14})
  end
end
local function labels_unit(i)
  labels[i % 16 + 1]:text(tostring(i))
  return 0
end

local list, items_a, items_b = nil, {}, {}
local function list_setup()
  for i = 1, 20 do
    items_a[i] = "Item " .. i
    items_b[i] = "Row " .. i
  end
  list = ui.list(0, 0, ui.W, 180, items_a)
end
local function list_unit(i)
  list:items(i & 1 == 1 and items_b or items_a)
  return 0
end

-- A game tick: 256 particles moved, bounced and drawn 2 x 2 on a 160 x 108 canvas at 2x

local PN, PW, PH = 256, 160, 108
local XMAX, YMAX = (PW - 2) << 8, (PH - 2) << 8
local PCOL = {0x9945FF, 0x14F195, 0xEB54BC, 0xB6B0FF, 0x64A8F2, 0x80ECFF, 0xACFF9B, 0xE8F180}
local ppx, ppy, pvx, pvy = {}, {}, {}, {}

local function game_reset()
  for i = 0, PN - 1 do
    ppx[i + 1] = ((i * 97) % (PW - 2)) << 8
    ppy[i + 1] = ((i * 53) % (PH - 8)) << 8
    pvx[i + 1] = ((i * 29) % 200) - 100
    pvy[i + 1] = 0
  end
end

local function game_unit(i)
  local c = canvas
  c:clear(0x000010)
  local rect = c.rect
  for p = 1, PN do
    local x, y, vx, vy = ppx[p], ppy[p], pvx[p], pvy[p] + 6
    x, y = x + vx, y + vy
    if x < 0 then x, vx = -x, -vx
    elseif x > XMAX then x, vx = 2 * XMAX - x, -vx end
    if y > YMAX then
      y = 2 * YMAX - y
      vy = -((vy * 15) >> 4)
      if vy > -96 then vy = -(400 + (((p - 1) * 37) & 127)) end
    end
    ppx[p], ppy[p], pvx[p], pvy[p] = x, y, vx, vy
    if y >= 0 then rect(c, x >> 8, y >> 8, 2, 2, PCOL[(p - 1) % 8 + 1]) end
  end
  if verifying and i == 99 then
    local s = 0
    for p = 1, PN do s = s + ppx[p] + ppy[p] * 3 + pvy[p] end
    return s
  end
  return 0
end

-- name, units checked, units a frame, setup (may yield), reset, unit, most units a callback while checking
local wls = {
  -- rows in a scattered order (as main.c): the middle rows cost many times the edges
  {"mandel", MH, 0, nil, nil, function(i) return mandel_row(i * 97 % MH) end},
  {"mandelf", MH, 0, nil, nil, function(i) return mandelf_row(i * 97 % MH) end},
  {"mandeld"},  -- C only: Lua has no 64-bit floats on the badge
  {"sieve", 4, 0, nil, nil, sieve},
  {"sha", 65, 0, function()
    for i = 0, SHA_MSG // 4 - 1 do
      local v = 0
      for j = 0, 3 do v = (v << 8) | (((i * 4 + j) * 7 + 3) & 255) end
      sha_w[i + 1] = v
    end
  end, nil, sha_unit},
  {"shaapi", 4, 0, function()
    local bytes = {}
    for i = 0, SHA_MSG - 1 do bytes[i + 1] = string.char((i * 7 + 3) & 255) end
    local s = table.concat(bytes)
    for k = 0, 3 do sha_parts[k + 1] = s:sub(k * 1024 + 1, k * 1024 + 1024) end
  end, nil, shaapi_unit},
  {"fill", BUF // SEG, 0, buffers, nil, fill_unit, 1},  -- 1: one unit a callback while checking
  {"copy", BUF // SEG, 0, buffers, nil, copy_unit, 1},
  {"draw", 216, 216, function() draw_setup(320, 216, 1) end, nil, draw_unit},
  {"draw80", 54, 54, function() draw_setup(80, 54, 4) end, nil, draw_unit},
  {"labels", 0, 0, labels_setup, nil, labels_unit},
  {"list", 0, 0, list_setup, nil, list_unit},
  {"game", 100, 1, function() draw_setup(PW, PH, 2) end, game_reset, game_unit},
}
local limits = {"alloc", "stack", "ui"}
local NWL = #wls
local NMENU = NWL + #limits

-- Limits ------------------------------------------------------------------------------

local hoard, hoarded = {}, 0
local function alloc_tick()  -- 64 KB strings until the app is stopped
  for k = 1, 8 do
    hoard[#hoard + 1] = string.rep(string.char(65 + (#hoard % 26)), 65536)
    hoarded = hoarded + 65536
  end
  print(string.format("ALLOC lang=lua got=%d used=%d max=%d", hoarded, sys.memory().used, sys.memory().max))
end

local function rec(n)
  if n <= 0 then return 0 end
  return (rec(n - 1) ~ n) + 1
end
local depth = 100
local function stack_tick()  -- deeper each callback; pcall catches Lua's own stack error
  local t0 = sys.millis()
  local ok, err = pcall(rec, depth)
  if not ok then
    print(string.format("STACK lang=lua fail=%d err=%s", depth, tostring(err)))
    return true
  end
  print(string.format("STACK lang=lua ok=%d us=%d", depth, (sys.millis() - t0) * 1000))
  depth = depth + depth // 4
end

local function ui_limit()
  ui.clear()
  local n = 0
  while true do
    local ok, err = pcall(ui.label, 0, (n % 15) * 14, "x")
    if not ok then
      print(string.format("UI lang=lua labels=%d err=%s", n, tostring(err)))
      return
    end
    n = n + 1
  end
end

-- The runner ------------------------------------------------------------------------

local phase, sel, cur, ramp, lim = "menu", 0, 1, false, 0
local cursor
local n_cal, unit_ms, chunk, done, chk, units, work, t_run, n_ramp = 1, 1, 1, 0, 0, 0, 0, 0, 1
local setup_co
local max_ms = 0  -- the longest callback of work so far

local function menu_name(i) return i < NWL and wls[i + 1][1] or limits[i - NWL + 1] end
local function menu_x(i) return i < 8 and 24 or 174 end
local function menu_y(i) return 24 + (i % 8) * 20 end

local function run_units(n)  -- n units from `done`; their time in ms
  local unit = wls[cur][6]
  local t0 = sys.millis()
  local s = chk
  for _ = 1, n do
    s = s + unit(done)
    done = done + 1
  end
  chk = s
  local dt = sys.millis() - t0
  if dt > max_ms then max_ms = dt end
  return dt
end

local function restart()
  done, chk = 0, 0
  local reset = wls[cur][5]
  if reset then reset() end
end

local function finish()
  print("DONE wl=" .. menu_name(cur - 1))
  sys.exit()
  phase = "menu"
end

local function start(i, with_ramp)
  cur, ramp = i + 1, with_ramp
  ui.clear()
  ui.label(4, 4, menu_name(i), {size = 12, color = "dim"})
  if i >= NWL then
    lim, phase = i - NWL + 1, "limit"
    return
  end
  if not wls[cur][2] then
    print("SKIP wl=" .. wls[cur][1] .. " why=no_64bit_floats")
    finish()
    return
  end
  phase, max_ms = "setup", 0
  setup_co = wls[cur][4] and coroutine.create(wls[cur][4])
end

local function begin_run()
  verifying = false
  restart()
  t_run, units, work = sys.millis(), 0, 0
  phase = "run"
end

function on_tick(ms)
  local w = wls[cur]
  if phase == "setup" then
    if setup_co and coroutine.status(setup_co) ~= "dead" then
      local ok, err = coroutine.resume(setup_co)
      if not ok then error(err) end
      if coroutine.status(setup_co) ~= "dead" then return end
    end
    restart()
    n_cal, phase = 1, "cal"
  elseif phase == "cal" then
    local dt = run_units(n_cal)
    if dt < 8 and n_cal < (1 << 20) then
      n_cal = n_cal * 2
      return
    end
    unit_ms = math.max(dt, 0.5) / n_cal
    chunk = math.max(1, math.floor(15 / unit_ms))
    if w[3] > 0 then
      chunk = w[3] * unit_ms <= SAFE_MS and w[3] or math.max(1, math.floor(SAFE_MS / unit_ms))
    end
    if w[2] > 0 then
      restart()
      verifying, phase = true, "check"
    else
      begin_run()
    end
  elseif phase == "check" then
    run_units(math.min(w[7] or chunk, chunk, w[2] - done))
    if done == w[2] then
      print(string.format("CHK wl=%s chk=%08x", w[1], chk))
      begin_run()
    end
  elseif phase == "run" then
    work = work + run_units(chunk)
    units = units + chunk
    if sys.millis() - t_run >= 3000 then
      local wall = sys.millis() - t_run
      print(string.format("BENCH wl=%s lang=lua units=%d us=%d wall=%d frames=%d blit=0", w[1], units, work * 1000,
        wall, w[3] > 0 and units // w[3] or 0))
      unit_ms = math.max(work, 1) / units
      if ramp then
        n_ramp, phase = math.max(1, math.floor(20 / unit_ms)), "ramp"
      else
        phase = "probe"
      end
    end
  elseif phase == "probe" then
    -- The biggest callback of the run (at least the run's chunk, and about
    -- 20 ms): `lua mem` reads its instruction count.
    n_ramp = math.max(chunk, math.floor(20 / unit_ms))
    work = run_units(n_ramp)
    phase = "probed"
  elseif phase == "probed" then
    print(string.format("PROBE wl=%s n=%d us=%d max_us=%d", w[1], n_ramp, work * 1000, max_ms * 1000))
    finish()
  elseif phase == "ramp" then
    local dt = run_units(n_ramp)
    print(string.format("FIT wl=%s n=%d us=%d", w[1], n_ramp, dt * 1000))
    n_ramp = n_ramp + math.max(1, n_ramp // 16)
  elseif phase == "limit" then
    if lim == 1 then
      alloc_tick()
    elseif lim == 2 then
      if stack_tick() then finish() end
    else
      ui_limit()
      finish()
    end
  end
end

function on_key(key)
  if key == "back" then return false end
  if phase ~= "menu" then return true end
  if key == "up" and sel > 0 then
    sel = sel - 1
  elseif key == "down" and sel < NMENU - 1 then
    sel = sel + 1
  elseif key == "select" or key == "right" then
    start(sel, key == "right")
    return true
  end
  cursor:pos(menu_x(sel) - 14, menu_y(sel))
  return true
end

ui.label(4, 2, "C vs Lua: the Lua app", {size = 12, color = "green"})
local names = {}
for i = 0, NMENU - 1 do
  ui.label(menu_x(i), menu_y(i), menu_name(i), {size = 14})
  names[#names + 1] = menu_name(i)
end
cursor = ui.label(menu_x(0) - 14, menu_y(0), ">", {size = 14, color = "green"})
ui.hint("SELECT run   RIGHT run + ramp   CANCEL exit")
print("MENU " .. table.concat(names, ","))
print("READY lang=lua ms=" .. sys.millis())
