-- SPDX-License-Identifier: GPL-2.0-or-later
-- describes what ctx.load_lua gives back, keys sorted
local function show(v, depth)
  if type(v) ~= "table" then
    if v ~= v then return "nan" end
    if v == math.huge then return "inf" end
    if v == -math.huge then return "-inf" end
    return tostring(v)
  end
  if depth > 2 then return "{...}" end
  local keys = {}
  for k in pairs(v) do keys[#keys + 1] = tostring(k) end
  table.sort(keys)
  local parts = {}
  for _, k in ipairs(keys) do
    local x = v[k]
    if x == nil then x = v[tonumber(k)] end
    parts[#parts + 1] = k .. "=" .. (x == v and "<self>" or show(x, depth + 1))
  end
  return "{" .. table.concat(parts, ",") .. "}"
end
return {
  render = function(ctx, props)
    local v, err = ctx.load_lua("~/data/" .. props.file)
    if err then return { ui.text("error: " .. err) } end
    return { ui.text(show(v, 0)) }
  end,
}
