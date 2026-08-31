// jtop - tiny GPU/CPU status widget for Linux (X11)
//
// A small borderless desktop window showing, in one compact row:
//   CPU % | RAM | SWAP | CPU °  |  per-GPU: util %, VRAM used, temp °C
//
// Data sources (all local, no network):
//   * /proc/stat          -> aggregate CPU usage (delta between samples)
//   * /proc/meminfo       -> RAM and swap usage
//   * sysfs hwmon/thermal -> CPU temperature (coretemp/k10temp/x86_pkg_temp...)
//   * nvidia-smi          -> NVIDIA GPU util / VRAM / temp (if present)
//   * /sys/class/drm      -> amdgpu/radeon/nvidia fallback via sysfs
//
// Dependencies: libX11 + cairo only. No Qt/GTK/SDL.
//
// Interaction:
//   left mouse button  : drag to move the widget
//   right mouse button : quit
//   hover a GPU column : popup with device name, PCI info and display connections
//   auto refresh       : every 5 seconds while running
//
// CLI modes: --dump prints one full snapshot to stdout; --table shows compact
// per-GPU lines in the terminal (connection list toggled with 'c'; also used
// automatically when no X display is available, e.g. over plain SSH).
//
// Source layout: this file only parses arguments and dispatches between the
// three modules below; system.{h,cc} reads the machine, terminal.{h,cc} renders
// text output (stdout), gui.{h,cc} renders the X11 widget. The headless render
// test links against them directly.

#include <cstdio>
#include <cstring>

#include "gui.h"
#include "system.h"
#include "terminal.h"

int main(int argc, char** argv) {
    // --dump: print one snapshot to stdout and exit (handy for testing/headless)
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
        State s{};
        refresh(s);
        printDump(s.snap);
        return 0;
    }

    // --table: GPU/device/connection info as a terminal table, no X involved
    if (argc > 1 && std::strcmp(argv[1], "--table") == 0)
        return runTableMode();

    int rc = runWidget();
    if (rc < 0) {
        // No graphics available (e.g. a plain SSH session without forwarding):
        // fall back to the terminal table instead of quitting.
        fprintf(stderr, "jtop: cannot open X display (is DISPLAY set?) -> falling back to terminal table\n");
        return runTableMode();
    }
    return rc;
}
