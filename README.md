# jtop

A very lightweight Linux desktop widget (X11) that shows system stats in one
compact row, styled like a mini taskbar panel:

```
 CPU   RAM    SWAP   CPU ° | GPU 0   GPU 1   VRAM 0   VRAM 1  | GPU ° 0  GPU ° 1
 9%   7.2G   0.0G    45°   |  5%    94%(red)  2.5G     21.9G  |   57°      88°
```

- **CPU**: aggregate usage % (all cores, from `/proc/stat`)
- **RAM**: used memory (`MemTotal - MemAvailable`, from `/proc/meminfo`)
- **SWAP**: swap in use (always shown as `x.xG`, e.g. `0.0G`)
- **CPU °**: package temperature (sysfs hwmon `coretemp`/`k10temp`/... or ACPI thermal zone)
- **Per GPU** (all of them are enumerated automatically):
  - utilization %
  - VRAM used
  - temperature °C

Hot values turn red with a small underline (GPU util ≥ 90 %, temp ≥ 90 °C,
VRAM ≥ 95 % full), just like the reference shot.

Data refreshes **every 5 seconds** while the app is running. Left-drag moves
the widget; right-click quits it. It sits in the top-right corner as a
borderless always-on-top "dock" window (no taskbar entry).

## GPU support

| Vendor | Source                                   | util % | VRAM   | temp |
|--------|------------------------------------------|:------:|:------:|:----:|
| NVIDIA | `nvidia-smi`                             | ✔      | ✔      | ✔    |
| AMD (amdgpu/radeon) | `/sys/class/drm/cardN/device`  | ✔      | ✔      | ✔    |
| fallback       | same sysfs paths (e.g. nvidia temp only) | –      | –/✔    | ✔    |

Intel integrated GPUs don't expose a busy counter in mainline sysfs, so an
iGPU-only machine may show no GPU columns — that's expected.

## Build

Dependencies (Ubuntu):

```sh
sudo apt install build-essential cmake pkg-config libx11-dev libcairo2-dev fonts-dejavu-core
```

Then:

```sh
cmake -S . -B build
cmake --build build
./build/jtop                # start the widget
./build/jtop_render_test    # (optional) headless render test -> widget_test.png
```

No CMake? One-liner also works:

```sh
g++ -O2 src/main.cpp -o jtop $(pkg-config --cflags --libs x11 cairo)
./jtop
```

## Test without a display (headless CI/container)

```sh
./build/jtop --dump     # prints one snapshot to stdout and exits
```

Example:

```
CPU 4%   RAM 7.2G   SWAP 0.0G   CPU° 51
GPU0 util=94  vram=6.1G   /23.6G   temp=88°
GPU1 util=5   vram=2.5G   /21.9G   temp=57°
```

## DPI / scaling

The widget scales its fonts, paddings and decorations to match the session's
UI scale. Detection order (first hit wins):

1. `JTOP_SCALE` env var — manual override, e.g. `JTOP_SCALE=2 ./jtop`
2. `GDK_DPI_X` / `GDK_DPI_Y` (dots per inch; 96 = no scaling)
3. `GDK_SCALE` integer factor
4. GNOME settings: `scaling-factor` x `text-scaling-factor`
   (`org.gnome.desktop.interface`) — i.e. follows the normal display-scale UI setting
5. Physical screen DPI from X (only honored above ~145 dpi, clamped to 2x)

On a standard unscaled desktop this resolves to 1.0 and the layout is
exactly the original pixel design.

## Notes

- Only `libX11` + `cairo` are linked — no Qt, GTK or SDL at runtime.
- CPU usage is a delta between the last two 5 s samples (a quick ~300 ms
  baseline sample is taken once at startup so the first frame already has a %).
- Unknown values render as `--` (e.g. no swap partition, missing temp sensor,
  or an iGPU that exposes no sysfs counters).
- The window is `override-redirect` (a true widget): the window manager won't
  decorate it; with a compositor (GNOME/Mutter default) you get rounded,
  semi-transparent corners. Without one it's a plain dark rectangle — same data.
- NVIDIA GPU stats are collected by spawning `nvidia-smi` once per refresh
  (~5 s). If the driver is broken/unloaded the sysfs fallback is used instead.
