-- Regression test (not shipped): one of several services at once (t-svc,
-- t-svc-a, t-svc-b, t-svc-limits: the fourth is refused, "full").
service.start(function(ok, err) print("t-svc-b start: " .. tostring(ok) .. " " .. tostring(err)) end)
ui.label(8, 8, "t-svc-b: starting its service", {size = 12})
