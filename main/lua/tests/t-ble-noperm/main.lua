-- Regression test (not shipped): an app without the "ble" permission gets the
-- same Lua error as other modules. Lines start "t-ble-noperm".
for _, name in ipairs({"on", "off", "state", "address", "advertise", "scan", "serve"}) do
  local ok, err = pcall(ble[name], {})
  local want = 'ble: add the "ble" permission to app.json'
  print(("t-ble-noperm %s %s%s"):format((not ok and tostring(err):find(want, 1, true)) and "PASS" or "FAIL",
    name, ok and " (no error)" or ""))
end
ui.label(8, 8, "t-ble-noperm: see the console", {size = 12})
