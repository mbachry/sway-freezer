# sway-freezer

Save battery by freezing greedy apps when they are not focused in sway.

## Install

Install `meson` and `jansson`, `glib` and `liburing` development
packages. Build with:

```
meson setup build
ninja -C build
```

## Running

Use `swaymsg -t get_tree` to note down app ids of runaway apps. Pass
the app ids to the freezer:

```
./build/sway-freezer emacs org.mozilla.firefox
```

Or use this systemd unit:

```
[Unit]
Description=sway freezer

[Service]
ExecStart=%h/.local/bin/sway-freezer emacs org.mozilla.firefox

[Install]
WantedBy=sway-session.target
```

Install with `systemctl --user enable --now sway-freezer.service`.
