-- Regression test (not shipped): solana.pda charges the callback's budget
-- for every bump it tries (2.1.0, from review). The seed "s239751" with the
-- Squads program needs 18 tries (bump 238), found ahead of time, as anyone
-- can. Part 1 times two such calls (2 x 18 x 5 000 = 180 000 instructions,
-- inside one callback's 200 000) and prints the time per bump. Part 2 makes
-- ten in one callback: before the fix that was 10 x 10 000 instructions and
-- ran 180 hashes and curve tests under the LVGL lock; now the budget stops it
-- after 40 tries with "Script took too long" and the app ends.
local SQUADS = solana.decode("SQDS4ep65T869zMMBKyuUq6aD6EgTu8psMjkvj52pCf")
local SEEDS = {"s239751"}
ui.label(8, 8, "t-pda: running")
timer.after(50, function()
  local t0 = sys.millis()
  local _, bump = solana.pda(SEEDS, SQUADS)
  solana.pda(SEEDS, SQUADS)
  local ms = sys.millis() - t0
  print(("t-pda part 1: bump %d (%d tries), 2 calls %d ms, %.2f ms a try"):format(bump, 256 - bump, ms, ms / (2 * (256 - bump))))
  timer.after(50, function()
    print("t-pda part 2: ten calls in one callback")
    local t1 = sys.millis()
    for _ = 1, 10 do solana.pda(SEEDS, SQUADS) end
    print(("t-pda part 2: FAIL not stopped: 180 tries in %d ms in one callback"):format(sys.millis() - t1))
  end)
end)
function on_key(k) if k == "select" then sys.exit() end return true end
