// Headless render test: links against the app's modules (gui.h exposes the
// layout/paint API) and paints a fake snapshot that mimics the reference image
// onto a checkerboard "desktop" so alpha/rounded corners are visible. Also
// renders the GPU hover popup below the first GPU column.
// Output: widget_test.png in cwd.
//
// Build with CMake (target: jtop_render_test), or manually:
//   g++ -O2 -o build/jtop_render_test test/render_test.cc src/gui.cc src/system.cc $(pkg-config --cflags --libs cairo x11)
//   ./build/jtop_render_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/gui.h"
#include "../src/system.h"

int main() {
    Snapshot s;
    s.cpuPct = 9;
    s.ramUsedKb = (long)(7.2 * 1048576);
    s.swapUsedKb = 0;
    s.hasSwap = true;
    s.cpuTempC = -1; // unknown in container -> "--"

    GpuInfo g0{5,  2560, 24564, 57};   // GPU 0: light load (like the reference)
    GpuInfo g1{94, 22420, 24564, 88};  // GPU 1: hot -> red value + underline

    // GPU metadata feeding the hover popup (what readDrmCards()/nvidia-smi yield)
    g0.name = "NVIDIA GeForce RTX 3090";
    g0.pciAddr = "0000:65:00.0";
    g0.pciIds = "10de:2280";
    g0.pcieGen = 4; g0.pcieMaxGen = 4; g0.pcieWidth = 16; // "Gen4 x16" (a real 3090 in a Gen4 slot)
    g0.conns.push_back({"DP-1",     "DisplayPort", true,  "3840x2160@60Hz"});
    g0.conns.push_back({"HDMI-A-1", "HDMI",        false, ""});

    s.gpus.push_back(g0);
    s.gpus.push_back(g1);

    auto cols = buildColumns(s);

    // measure with the same code path the real widget uses
    Metrics M;
    cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t* mc = cairo_create(tmp);
    M.prepare(mc);
    int w = M.width(mc, cols);
    int h = M.height();

    // column hit areas (hover popup anchors below the first GPU column) and
    // popup content/size for GPU 0
    auto rects = layoutCols(mc, M, cols);
    std::vector<PopRow> prows;
    popupRows(s.gpus[0], prows);
    PopGeom pg = popupGeom(mc, prows);

    cairo_destroy(mc);
    cairo_surface_destroy(tmp);

    // checkerboard "desktop" behind everything (proves real alpha is used)
    const int ox = 30, oy = 30, popGap = 12;
    const int px_ = ox + (int)std::ceil(rects[3].x);   // root coords of popup left edge
    const int py_ = oy + h + popGap;                   // just below the widget
    const int CW = std::max(ox + w, px_ + pg.w) + ox;
    const int CH = py_ + pg.h + oy;
    cairo_surface_t* bg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CW, CH);
    cairo_t* cr = cairo_create(bg);
    for (int y = 0; y < CH; y += 15) {
        for (int x = 0; x < CW; x += 15) {
            bool dk = ((x / 15) + (y / 15)) % 2 == 0;
            cairo_set_source_rgb(cr, dk ? 0.32 : 0.48, dk ? 0.56 : 0.72,
                                 dk ? 0.66 : 0.82);
            cairo_rectangle(cr, x, y, 15, 15);
        }
    }
    cairo_fill(cr);

    // the widget, composited at (ox, oy) — clip so its transparent clear
    // doesn't erase the desktop behind it
    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_clip(cr);
    paintSnapshot(cr, w, h, true /*argb*/, cols);
    cairo_restore(cr);

    // the GPU hover popup, composited at (px_, py_)
    cairo_save(cr);
    cairo_translate(cr, px_, py_);
    cairo_rectangle(cr, 0, 0, pg.w, pg.h);
    cairo_clip(cr);
    paintPopup(cr, pg.w, pg.h, true /*argb*/, prows);
    cairo_restore(cr);

#ifdef CAIRO_HAS_PNG_FUNCTIONS
    cairo_status_t st = cairo_surface_write_to_png(bg, "widget_test.png");
#else
    fprintf(stderr, "cairo built without PNG support\n");
    return 1;
#endif
    printf("size %dx%d  popup %dx%d  png rc=%s\n", w, h, pg.w, pg.h,
           st == CAIRO_STATUS_SUCCESS ? "ok" : "FAIL");

    cairo_destroy(cr);
    cairo_surface_destroy(bg);
    return st == CAIRO_STATUS_SUCCESS ? 0 : 1;
}
