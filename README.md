# hypr-oled-saver

`hypr-oled-saver` is a small OLED-friendly layer-shell screensaver for Hyprland.
It renders mostly black surfaces, a dim drifting clock, and moving window glyphs
derived from the open Hyprland clients across workspaces.

This is an idle visual, not a lock screen. Use `hyprlock` for locking.

## Running

```sh
nix run github:colonelpanic8/hypr-oled-saver
```

For local development:

```sh
direnv allow
cmake -S . -B build
cmake --build build
./build/hypr-oled-saver
```

## Hypridle

Use a wrapper that starts the process on timeout and kills it on resume, or wire
it into an existing screensaver script.

```conf
listener {
    timeout = 300
    on-timeout = hypr-oled-saver
    on-resume = pkill -x hypr-oled-saver
}

listener {
    timeout = 600
    on-timeout = hyprctl dispatch dpms off
    on-resume = hyprctl dispatch dpms on
}
```

## Notes

- Uses `gtk-layer-shell` to cover each monitor with an overlay-layer surface.
- Queries `hyprctl -j monitors` and `hyprctl -j clients`.
- A standalone Wayland layer-shell client can render Hyprland window metadata, but
  it cannot render real window contents from inactive workspaces. Real live
  thumbnails require a compositor-side Hyprland plugin/render-pass design.
- Falls back to synthetic particles outside Hyprland.
- Keeps brightness low by design; the default background is pure black.
