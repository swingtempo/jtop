// Headless render test: includes the app source (renaming its main) and paints
// a fake snapshot that mimics the reference image onto a checkerboard "desktop"
// so alpha/rounded corners are visible. Output: widget_test.png in cwd.
//
// Build with CMake (target: jtop_render_test), or manually:
//   g++ -O2 -o build/jtop_render_test test/render_test.cc $(pkg-config --cflags --libs cairo x11)
//   ./build/jtop_render_test

#define main jtop_app_main // rename the app's entry point while including it...
#include "../src/main.cpp" // statics become visible to this translation unit
#undef main                // ...and restore it for our own test entry below

int main() {
    Snapshot s;
    s.cpuPct = 9;
    s.ramUsedKb = (long)(7.2 * 1048576);
    s.swapUsedKb = 0;
    s.hasSwap = true;
    s.cpuTempC = -1; // unknown in container -> "--"

    GpuInfo g0{5,  2560, 24564, 57};   // GPU 0: light load (like the reference)
    GpuInfo g1{94, 22420, 24564, 88};  // GPU 1: hot -> red value + underline
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
    cairo_destroy(mc);
    cairo_surface_destroy(tmp);

    // checkerboard "desktop" behind the widget (proves real alpha is used)
    const int ox = 30, oy = 30;
    cairo_surface_t* bg =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w + 2 * ox, h + 2 * oy);
    cairo_t* cr = cairo_create(bg);
    for (int y = 0; y < h + 2 * oy; y += 15) {
        for (int x = 0; x < w + 2 * ox; x += 15) {
            bool dk = ((x / 15) + (y / 15)) % 2 == 0;
            cairo_set_source_rgb(cr, dk ? 0.32 : 0.48, dk ? 0.56 : 0.72,
                                 dk ? 0.66 : 0.82);
            cairo_rectangle(cr, x, y, 15, 15);
        }
    }
    cairo_fill(cr);

    // the widget, composited at (ox, oy) — clip so its transparent clear
    // doesn't erase the desktop behind it
    cairo_translate(cr, ox, oy);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_clip(cr);
    paintSnapshot(cr, w, h, true /*argb*/, cols);

#ifdef CAIRO_HAS_PNG_FUNCTIONS
    cairo_status_t st = cairo_surface_write_to_png(bg, "widget_test.png");
#else
    fprintf(stderr, "cairo built without PNG support\n");
    return 1;
#endif
    printf("size %dx%d  png rc=%s\n", w, h,
           st == CAIRO_STATUS_SUCCESS ? "ok" : "FAIL");

    cairo_destroy(cr);
    cairo_surface_destroy(bg);
    return st == CAIRO_STATUS_SUCCESS ? 0 : 1;
}
