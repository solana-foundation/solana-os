-- After the top-level chunk the service counts as running: only then can the
-- app's service.send reach it, so "ready" goes from a timer.
timer.after(50, function() service.send("ready") end)
-- Saved at once (the first periodic flush), so the pad below, set within the
-- same second, gets no periodic flush: its first write is the last save.
storage.set("t", (storage.get("t") or 0) + 1)
function on_message(msg)
  if msg ~= "go" then return end
  local n = storage.get("n") or 0
  if n % 2 == 0 then
    storage.set("s", n)                 -- while the app's last save is on the card
  else
    -- 0-25 ms of counting (about 1 500 loop turns a ms on a badge): the write
    -- below then starts at a different moment each round.
    for _ = 1, ((n * 7) % 26) * 1500 do end
    storage.set("pad", string.rep(string.char(65 + n % 26), 14000))
  end
  service.stop()
end
