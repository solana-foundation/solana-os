local n = 0
function on_tick(ms)
  for k = 1, 40 do
    n = n + 1
    if n <= 200 then storage.set("s" .. n, n) end
  end
end
