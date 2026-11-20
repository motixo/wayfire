# Wayfire desktop fork

A fork of [Wayfire](https://github.com/WayfireWM/wayfire) with built-in plugins for a desktop that mixes tiled and floating windows.

## What this fork adds

- **Window appearance:** `panel-decor` draws themed title bars for windows that use server-side decorations. It reads the Desktop Settings palette and can use Tabler button icons. `rounded-windows` clips content to the configured corner radius and draws a palette-aware active border that hides in fullscreen.
- **Floating windows:** `window-sizes` remembers resized windows and opens them at their saved size, centered in the work area.
- **Tiling and workspaces:** Simple Tile can use a Dwindle layout and swap tiles by dragging their title bars. Optional workspace clipping keeps windows inside their own workspace. `tile-focus` brings tiles into focus as the pointer enters them and uses a fade when they open.
- **Dock previews:** `dock-snapshot` captures live and minimized window previews for a dock over Wayfire IPC.
- **Rendering:** The fork adjusts adaptive repaint timing, adds a presented-frame counter to `wayfire/render-metrics`, and changes how blur, workspace transitions, and window maximization are rendered.

## Build and configure

Install the [upstream build dependencies](https://github.com/WayfireWM/wayfire#dependencies) and `json-c`. Install `librsvg` for SVG button icons. Rounded corners and dock previews use GLES. Dock previews also require image I/O support in the build.

```sh
git submodule update --init subprojects/wf-config subprojects/wf-json subprojects/wf-touch subprojects/wf-utils
meson setup build -Duse_system_wlroots=enabled -Duse_system_wfconfig=disabled
meson compile -C build
```

This example uses system wlroots 0.20. In `wayfire.ini`, replace `decoration` with `panel-decor` in `[core] plugins`. Add `rounded-windows`, `window-sizes`, `simple-tile` with `tile-focus`, or `ipc` with `dock-snapshot` as needed. Use the unversioned names when migrating from the standalone builds.

Set `layout_mode = dwindle` in `[simple-tile]` for Dwindle and `isolate_workspaces = true` in `[core]` for workspace clipping.

[Upstream configuration guide](https://github.com/WayfireWM/wayfire/wiki/Configuration) · [MIT license](LICENSE)
