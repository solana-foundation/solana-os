-- Regression test (not shipped): starts its service while the test makes
-- services.json unwritable. The app's own start runs anyway (cb true); a
-- start nobody on screen asked for is refused (svc-run.ts drives those).
ui.label(8, 8, "t-svc-rec", {size = 12})
service.start(function(ok, err) print("t-svc-rec start: " .. tostring(ok) .. " " .. tostring(err)) end)
