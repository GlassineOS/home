-- SPDX-License-Identifier: GPL-2.0-or-later
return {
  render = function(ctx, props)
    local s, err = ctx.read(props.path)
    return { ui.text(s and ("read: " .. s) or ("nil: " .. tostring(err))) }
  end,
}
