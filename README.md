# hypr-oled-saver

`hypr-oled-saver` is an OLED-friendly Hyprland plugin screensaver.

It renders a black compositor overlay and animated live previews of open
Hyprland windows. Previews start in a non-overlapping grid, then bounce around
the monitor and collide with each other.

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
```

If your Hyprland config layer has trouble passing dispatcher arguments, the
plugin also registers no-argument dispatchers:

```sh
hyprctl dispatch hyproledsaverstart
hyprctl dispatch hyproledsaverstop
hyprctl dispatch hyproledsavertoggle
```

Lua-based Hyprland configs can call the plugin directly:

```lua
hl.plugin.hyproledsaver.start()
hl.plugin.hyproledsaver.stop()
hl.plugin.hyproledsaver.toggle()
```

Config values use the `hyproledsaver` plugin namespace:

```conf
plugin:hyproledsaver:background = rgba(000000ff)
plugin:hyproledsaver:border_color = rgba(46c7d822)
plugin:hyproledsaver:border_size = 2
plugin:hyproledsaver:margin = 64
plugin:hyproledsaver:gap = 36
plugin:hyproledsaver:speed = 85.0
plugin:hyproledsaver:opacity = 0.82
```

## Hypridle

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
- Starts previews in a non-overlapping grid before allowing full-screen motion.
- Keeps the background pure black for OLED friendliness.
- The old standalone GTK layer-shell prototype is kept in `src/standalone.cpp`
  for reference, but the primary build artifact is now the Hyprland plugin.
