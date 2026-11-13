-- Regression test (not shipped): crypto.sha256, crypto.on_curve, crypto.verify,
-- solana.encode / decode and solana.pda against values from other
-- implementations (Node's SHA-256 and Ed25519, RFC 8032, @solana/web3.js).
-- It runs a part per timer callback: a signature check costs a fifth of a
-- callback's instruction budget, so they don't all fit in one.
local failed = 0
local function hex(s) return (s:gsub(".", function(c) return ("%02x"):format(c:byte()) end)) end
local function bin(h) return (h:gsub("%x%x", function(b) return string.char(tonumber(b, 16)) end)) end
local function check(name, got, want)
  local ok = got == want
  if not ok then failed = failed + 1 end
  print(("t-crypto %s: %s"):format(name, ok and "ok" or ("FAIL got " .. tostring(got))))
end
local function try(name, f)
  local ok, err = pcall(f)
  print(("t-crypto %s: %s"):format(name, ok and "FAIL no error" or err))
  if ok then failed = failed + 1 end
end
local parts = {}
local function part(f) parts[#parts + 1] = f end

-- SHA-256, and points of the curve
part(function()
  check("sha256 empty", hex(crypto.sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
  check("sha256 abc", hex(crypto.sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
  check("sha256 1000 a", hex(crypto.sha256(("a"):rep(1000))),
    "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3")
  check("on_curve base point", crypto.on_curve(bin("58" .. ("66"):rep(31))), true)
  check("on_curve y = 1", crypto.on_curve(bin("01" .. ("00"):rep(31))), true)
  check("on_curve y = 2", crypto.on_curve(bin("02" .. ("00"):rep(31))), false)
  check("on_curve short", crypto.on_curve("abc"), false)
end)

-- Signatures: RFC 8032's tests 1 and 2
local pub1 = bin("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
local sig1 = bin("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
local pub2 = bin("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c")
local sig2 = bin("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00")
part(function()
  check("verify rfc 1", crypto.verify(pub1, "", sig1), true)
  check("verify rfc 2", crypto.verify(pub2, "\x72", sig2), true)
  check("verify other message", crypto.verify(pub2, "\x73", sig2), false)
end)
part(function()
  check("verify other key", crypto.verify(pub1, "\x72", sig2), false)
  check("verify changed signature", crypto.verify(pub2, "\x72", sig2:sub(1, 40) .. "\0" .. sig2:sub(42)), false)
  check("verify short signature", crypto.verify(pub2, "\x72", sig2:sub(1, 63)), false)
  check("verify short key", crypto.verify(pub2:sub(1, 31), "\x72", sig2), false)
end)

-- Small-order keys and R (2.1.0, from review): the neutral point as a key
-- "verified" every message with no private key. Rejected as Solana rejects
-- them (ed25519-dalek's verify_strict); still on the curve.
local Z32 = ("\0"):rep(32)
local NEUTRAL = "\1" .. ("\0"):rep(31)
local ORDER2 = bin("ec" .. ("ff"):rep(30) .. "7f")
local ORDER8 = bin("26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc85")
part(function()
  check("verify neutral key, any message", crypto.verify(NEUTRAL, "anything at all", NEUTRAL .. Z32), false)
  check("verify neutral key, empty message", crypto.verify(NEUTRAL, "", NEUTRAL .. Z32), false)
  check("verify order 2 key", crypto.verify(ORDER2, "hello", ORDER2 .. Z32), false)
  check("verify order 8 key", crypto.verify(ORDER8, "hello", ORDER8 .. Z32), false)
end)
part(function()
  -- y = p + 1: a non-canonical encoding of the neutral point
  check("verify non-canonical neutral key", crypto.verify(bin("ee" .. ("ff"):rep(30) .. "7f"), "hello", NEUTRAL .. Z32), false)
  -- RFC 8032 test 1's key with R the neutral point and s = h * a: passes a plain check
  check("verify small-order R", crypto.verify(pub1, "strict R", bin("01" .. ("00"):rep(31) ..
    "5e56bff9e588f80e628453e25722383f6c32da233c99b1259644a22c175aef00")), false)
  check("verify rfc 1 still", crypto.verify(pub1, "", sig1), true)
  check("on_curve neutral", crypto.on_curve(NEUTRAL), true)
  check("on_curve order 8", crypto.on_curve(ORDER8), true)
end)

-- base58
local SQUADS = "SQDS4ep65T869zMMBKyuUq6aD6EgTu8psMjkvj52pCf"
local squads = solana.decode(SQUADS)
part(function()
  check("decode", hex(squads), "0681c4ce47e22368b8b1555ec887af092efc7efbb66ca3f52fbf68d4ac9cb7a8")
  check("encode", solana.encode(squads), SQUADS)
  local zeros = ("\0"):rep(3) .. ("\7"):rep(29)
  check("encode zeros", solana.encode(zeros), "111KWCfMfKoLt2cZbE5UcrZknqeER1Q4vVBx4Epu58")
  check("decode zeros", solana.decode("111KWCfMfKoLt2cZbE5UcrZknqeER1Q4vVBx4Epu58"), zeros)
  check("encode 64", solana.encode(("\xff"):rep(64)),
    "67rpwLCuS5DGA8KGZXKsVQ7dnPb9goRLoKfgGbLfQg9WoLUgNY77E2jT11fem3coV9nAkguBACzrU1iyZM4B8roQ")
  check("encode 128", #solana.encode(("\xff"):rep(128)), 175)
  check("round trip 128", solana.decode(solana.encode(("\xff"):rep(128))), ("\xff"):rep(128))
  check("encode empty", solana.encode(""), "")
  check("decode empty", solana.decode(""), "")
  local got, why = solana.decode("0OIl")
  check("decode not base58", tostring(got) .. " " .. tostring(why), "nil not base58")
  got, why = solana.decode(("z"):rep(176))
  check("decode too long", tostring(got) .. " " .. tostring(why), "nil too long")
  try("encode 129 bytes", function() solana.encode(("x"):rep(129)) end)
end)

-- Program-derived addresses
local function pda(seeds, program)
  local a, bump = solana.pda(seeds, program)
  return solana.encode(a) .. " " .. bump
end
part(function()
  -- A Squads proposal: "multisig", the multisig, "transaction", its number, "proposal".
  local multisig = bin("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")
  check("pda proposal 1", pda({"multisig", multisig, "transaction", string.pack("<i8", 1), "proposal"}, squads),
    "9qfXCUFoi7oVo4VSNZMbzoxTeB2PWKjxTaYUL2SrLmE7 252")
  check("pda proposal 300", pda({"multisig", multisig, "transaction", string.pack("<i8", 300), "proposal"}, squads),
    "2beX33nyZXw8YL3DQba9aAexgxSSW8p5mKSCFoVTLZwg 253")
  -- An associated token account: the wallet, the token program, the mint.
  check("pda token account", pda({
    solana.decode("Gh9ZwEmdLJ8DscKNTkTqPbNwLNNBjuSzaG9Vp2KGtKJr"),
    solana.decode("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"),
    solana.decode("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"),
  }, solana.decode("ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL")), "GbGfzu5RQsM3PDu3reo7GKSrKWp2uzjHPJieXVDuypc6 254")
  check("pda no seeds", pda({}, squads), "HkYZx2gFvurTDv6t2uaKbTjGNGFD6fC8Unqiih2sT3yN 255")
  check("pda is off the curve", crypto.on_curve(solana.pda({"x"}, squads)), false)
  try("pda 16 seeds", function()
    solana.pda({"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"}, squads)
  end)
  try("pda long seed", function() solana.pda({("x"):rep(33)}, squads) end)
  try("pda seed not a string", function() solana.pda({1}, squads) end)
  try("pda short program", function() solana.pda({}, "abc") end)
end)

local status = ui.label(8, 8, "t-crypto: running")
ui.hint("SELECT exits")
local at = 0
local function run()
  at = at + 1
  if parts[at] then
    parts[at]()
    timer.after(20, run)
    return
  end
  print("t-crypto heap " .. collectgarbage("count"))
  print(("t-crypto done, %d failed"):format(failed))
  status:text(failed == 0 and "t-crypto: all ok" or ("t-crypto: " .. failed .. " failed"))
end
timer.after(20, run)
function on_key(k) if k == "select" then sys.exit() end return true end
