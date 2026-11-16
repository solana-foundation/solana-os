-- A service that does almost nothing: what one costs at rest.
local n = 0
function on_tick() n = n + 1; service.status("up " .. n .. " s") end
