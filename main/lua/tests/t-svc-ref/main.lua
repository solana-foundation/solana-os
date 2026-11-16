-- Regression test (not shipped): the app holds its shared storage while its
-- service fails before it ever loaded storage (service.lua doesn't compile).
-- The failed service must not release the app's reference: the app's set()
-- after it has to reach data.json. Prints "t-svc-ref b set".
ui.label(8, 8, "t-svc-ref", {size = 12})
storage.set("a", 1)
service.start(function(ok, err)
  print("t-svc-ref start: " .. tostring(ok) .. " " .. tostring(err))
  storage.set("b", 2)
  print("t-svc-ref b set")
end)
