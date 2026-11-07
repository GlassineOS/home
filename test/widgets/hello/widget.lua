-- SPDX-License-Identifier: GPL-2.0-or-later
return {
  render = function(ctx, props)
    print("rendering for", props.who)
    return { ui.title("Hello, " .. props.who), ui.meta("now", { time = ctx.now }) }
  end,
}
