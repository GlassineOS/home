-- SPDX-License-Identifier: GPL-2.0-or-later
return {
  render = function(ctx, props)
    local names = { "os", "io", "require", "load", "loadstring", "dofile", "loadfile", "debug", "coroutine",
      "collectgarbage", "getmetatable", "setmetatable", "rawget", "rawset", "_G", "package", "xpcall" }
    local out = {}
    for _, n in ipairs(names) do
      if _ENV[n] ~= nil then out[#out + 1] = n end
    end
    if string.dump ~= nil then out[#out + 1] = "string.dump" end
    if ("").dump ~= nil then out[#out + 1] = "(\"\").dump" end
    if math.random ~= nil then out[#out + 1] = "math.random" end
    return { ui.text("reachable: " .. table.concat(out, " ")) }
  end,
}
