# hypr-oled-saver

`hypr-oled-saver` is an OLED-friendly Hyprland plugin screensaver.

It renders a black compositor overlay and animated live previews of open
Hyprland windows. Large, dim previews follow staggered accelerating Bezier
crossing paths so the display stays mostly black while cycling through real
desktop state.

This is an idle visual, not a lock screen. Use `hyprlock` for locking.

## Build

```sh
nix build
```

For local development:

```sh
direnv allow
cmake -S . -B build
cmake --build build
```

## Hyprland

Load the built plugin from Hyprland, then use the dispatcher:

```conf
exec-once = hyprctl plugin load /path/to/hypr-oled-saver.so

bind = SUPER, O, hyproledsaver, toggle
```

Dispatcher actions:

```sh
hyprctl dispatch hyproledsaver start
hyprctl dispatch hyproledsaver stop
hyprctl dispatch hyproledsaver toggle
hyprctl dispatch hyproledsaver activate
hyprctl dispatch hyproledsaver deactivate
hyprctl dispatch hyproledsaver present
hyprctl dispatch hyproledsaver dismiss
```

If your Hyprland config layer has trouble passing dispatcher arguments, the
plugin also registers no-argument dispatchers:

```sh
hyprctl dispatch hyproledsaverstart
hyprctl dispatch hyproledsaverstop
hyprctl dispatch hyproledsavertoggle
hyprctl dispatch hyproledsaveractivate
hyprctl dispatch hyproledsaverdeactivate
hyprctl dispatch hyproledsaverpresent
hyprctl dispatch hyproledsaverdismiss
```

Lua-based Hyprland configs can call the plugin directly:

```lua
hl.plugin.hyproledsaver.set_active(true)
hl.plugin.hyproledsaver.set_active(false)
hl.plugin.hyproledsaver.is_active()
hl.plugin.hyproledsaver.activate()
hl.plugin.hyproledsaver.deactivate()
hl.plugin.hyproledsaver.present()
hl.plugin.hyproledsaver.dismiss()
hl.plugin.hyproledsaver.start()
hl.plugin.hyproledsaver.stop()
hl.plugin.hyproledsaver.toggle()
```

Example Lua bind:

```lua
bind(main_mod .. " + O", function()
  if hl.plugin and hl.plugin.hyproledsaver then
    hl.plugin.hyproledsaver.toggle()
  end
end)
```

Config values use the `hyproledsaver` plugin namespace:

```conf
plugin:hyproledsaver:background = rgba(000000ff)
plugin:hyproledsaver:border_color = rgba(46c7d822)
plugin:hyproledsaver:border_size = 2
plugin:hyproledsaver:opacity = 0.34
plugin:hyproledsaver:max_visible = 2
plugin:hyproledsaver:target_window_area = 0.34
plugin:hyproledsaver:min_window_scale = 0.55
plugin:hyproledsaver:max_window_scale = 0.94
plugin:hyproledsaver:path_duration_ms_min = 35000
plugin:hyproledsaver:path_duration_ms_max = 70000
plugin:hyproledsaver:start_stagger_ms_min = 2500
plugin:hyproledsaver:start_stagger_ms_max = 9000
plugin:hyproledsaver:curve_strength = 0.28
plugin:hyproledsaver:path_accel_power = 1.35
plugin:hyproledsaver:dismiss_on_activity = 1
plugin:hyproledsaver:activity_grace_ms = 500
```

## Hypridle

The plugin intentionally does not own idle policy. Use the plugin API for
manual state changes and `hypridle` for idle/resume triggers. Keeping idle
timing in `hypridle` avoids duplicating lock/DPMS/inhibitor logic inside the
plugin.

Manual activation still dismisses on activity by default. The plugin listens for
keyboard, pointer, and touch input while active, ignores activity for
`activity_grace_ms` after activation, then dismisses itself and consumes the
wake input.

For development, `Escape` and right click always dismiss immediately, bypassing
the activation grace period.

```conf
listener {
    timeout = 300
    on-timeout = hyprctl dispatch hyproledsaverstart
    on-resume = hyprctl dispatch hyproledsaverstop
}

listener {
    timeout = 600
    on-timeout = hyprctl dispatch dpms off
    on-resume = hyprctl dispatch dpms on
}
```

## Notes

- Uses Hyprland's compositor internals to snapshot window contents.
- Cycles large previews along staggered, non-straight Bezier crossing paths.
- Keeps the background pure black for OLED friendliness.
- The old standalone GTK layer-shell prototype is kept in `src/standalone.cpp`
  for reference, but the primary build artifact is now the Hyprland plugin.
