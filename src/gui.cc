// jtop GUI: X11 window management, UI-scale (DPI) detection, cairo painting of
// the widget row and the GPU hover popup, plus the event loop. Public surface
// in gui.h; everything else here is file-local.
#include "gui.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// UI scale (DPI): one factor applied to every layout constant below so that
// text, padding and decorations all grow together on scaled desktops.
// Detection order, first hit wins:
//   1. JTOP_SCALE env var      - manual override, any float > 0
//   2. GDK_DPI_X / GDK_DPI_Y   - dots per inch, factor = value / 96
//   3. GDK_SCALE               - integer factor >= 1
//   4. monitors.xml            - Mutter's saved display configuration: per-monitor
//                                fractional scale of the monitor under the pointer,
//                                from the <configuration> block matching the live
//                                xrandr outputs (only exists when a custom/saved
//                                GNOME layout is active)
//   5. GNOME settings          - gsettings scaling-factor * text-scaling-factor
//                                (0 == "auto" -> counts as 1; the whole step is
//                                 skipped if gsettings is missing or fails)
//   6. physical X screen DPI   - avg dpi / 96 clamped to [1,2]; returns 1.0 when
//                                the reported size implies < ~145 dpi so plain
//                                unscaled desktops are not over-scaled
// The result is finally clamped to [1.0, 3.0]. Unscaled sessions resolve to
// exactly 1.0 (pixel-identical to pre-scaling behavior).
// ---------------------------------------------------------------------------

static double g_uiScale = 1.0; // set once by detectUiScale() in main()

// parse a non-empty env var as double; false when unset/empty/not-a-number
static bool envToDouble(const char* name, double& out) {
    const char* s = getenv(name);
    if (!s || !*s) return false;
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return false;
    out = v;
    return true;
}

// run `gsettings get <key>` and parse the printed value as a double.
// Returns false (=> caller skips this source) when gsettings is missing,
// the key does not exist or nothing numeric can be parsed. Note that some
// GLib versions prefix the value with its type ("uint32 0").
static bool gnomeSettingDouble(const char* key, double& out) {
    std::string cmd = "gsettings get ";
    cmd += key;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[128];
    std::string line;
    if (fgets(buf, sizeof(buf), p)) line = trim(std::string(buf));
    int rc = pclose(p);
    if (rc != 0 || line.empty()) return false;

    auto parseTok = [&](const std::string& t) -> bool {
        char* e = nullptr;
        double v = strtod(t.c_str(), &e);
        if (t.empty() || e == t.c_str() || *e != '\0') return false;
        out = v;
        return true;
    };
    size_t sp = line.find(' ');
    if (sp != std::string::npos) { // "type value" -> try the value first
        if (parseTok(trim(line.substr(sp + 1)))) return true;
        if (parseTok(trim(line.substr(0, sp)))) return true;
    } else if (parseTok(line)) {
        return true;
    }
    return false;
}

// monitors.xml lookup (defined further down next to monitorRects(), whose
// output names it reuses for configuration-block matching)
static double monitorsXmlScale(const std::string& activeConnector);

static double detectUiScale(Display* dpy, const std::string& activeConnector) {
    const auto clamp = [](double x) { return std::max(1.0, std::min(x, 3.0)); };
    double v;

    // 1) manual override (documented in README)
    if (envToDouble("JTOP_SCALE", v) && v > 0.0) return clamp(v);

    // 2) GDK dots-per-inch env vars (96 dpi == factor 1.0)
    if (envToDouble("GDK_DPI_X", v) && v > 0.0) return clamp(v / 96.0);
    if (envToDouble("GDK_DPI_Y", v) && v > 0.0) return clamp(v / 96.0);

    // 3) GDK integer scaling factor
    const char* gs = getenv("GDK_SCALE");
    if (gs && *gs) { int f = atoi(gs); if (f >= 1) return clamp((double)f); }

    // 4) Mutter's saved display configuration (monitors.xml): the per-monitor
    //    scale of the monitor under the pointer. This is what carries GNOME's
    //    fractional scaling when nothing above knows it.
    double mxs = monitorsXmlScale(activeConnector);
    if (mxs > 0.0) return clamp(mxs);

    // 5) GNOME interface settings (both keys must succeed, else fall through)
    double sf = 0.0, tsf = 0.0;
    if (gnomeSettingDouble("org.gnome.desktop.interface scaling-factor", sf) &&
        gnomeSettingDouble("org.gnome.desktop.interface text-scaling-factor", tsf)) {
        if (sf < 1.0) sf = 1.0; // uint32 0 == "automatic" -> unscaled base
        return clamp(sf * tsf);
    }

    // 6) physical screen DPI reported by the X server
    double dpiX = -1.0, dpiY = -1.0;
    if (dpy) {
        int scr = DefaultScreen(dpy);
        long mmw = DisplayWidthMM(dpy, scr), mmt = DisplayHeightMM(dpy, scr);
        if (mmw > 0) dpiX = 25.4 * DisplayWidth(dpy, scr) / (double)mmw;
        if (mmt > 0) dpiY = 25.4 * DisplayHeight(dpy, scr) / (double)mmt;
    }
    double avg = -1.0;
    if (dpiX > 0 && dpiY > 0)      avg = (dpiX + dpiY) / 2.0;
    else if (dpiX > 0 || dpiY > 0) avg = dpiX > 0 ? dpiX : dpiY;
    if (avg < 145.0) return 1.0; // unknown or small physical size -> no over-scaling
    return clamp(std::min(2.0, avg / 96.0));
}

// ---------------------------------------------------------------------------
// window / drawing (X11 + cairo)
// ---------------------------------------------------------------------------


// x-extent of one column in the last rendered frame; gpu >= 0 marks GPU/VRAM
// columns so pointer motion can be hit-tested against them for hover popups.

struct View {
    Display* dpy = nullptr;
    int screen = 0, rootW = 0, rootH = 0;
    Window root = None, win = None;
    cairo_surface_t* surf = nullptr;
    Visual* visual = nullptr;
    bool argb = false;

    Colormap cmap = None; // shared by the main and popup window
    int depth = 24;
    unsigned long bgPixel = 0;

    bool dragging = false;
    int dragDX = 0, dragDY = 0;

    std::vector<ColRect> colRects; // per-column hit areas (last render)
    int mx = -1, my = -1;          // last pointer pos in window coords (-1: none)

    // GPU hover popup state
    Window popWin = None;
    cairo_surface_t* popSurf = nullptr;
    int popW = 0, popH = 0;
    bool popShown = false;
    int popGpu = -1;              // which GPU index the shown popup describes

    int winW = 240, winH = 68;
};

// layout metrics (single source of truth for sizing + painting)

static void roundRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

std::vector<Col> buildColumns(const Snapshot& s) {
    std::vector<Col> cols;
    cols.push_back({"CPU",  s.cpuPct < 0 ? "--" : (std::to_string(s.cpuPct) + "% " + std::to_string(s.cpuTempC) + "\xC2\xB0"),
                    s.cpuPct >= 90 || s.cpuTempC >= 90});
    cols.push_back({"RAM", fmtSmartKb(s.ramUsedKb), false});
    cols.push_back({"SWAP", s.hasSwap ? fmtAlwaysGB(s.swapUsedKb) : "--", false});

    for (size_t i = 0; i < s.gpus.size(); ++i)
        cols.push_back({"GPU" + std::to_string(i),
                        (s.gpus[i].util < 0 ? "--" : (std::to_string(s.gpus[i].util) + "% ") + std::to_string(s.gpus[i].tempC) + "\xC2\xB0"),
                        s.gpus[i].util >= 90 || s.gpus[i].tempC >= 90});
    for (size_t i = 0; i < s.gpus.size(); ++i) {
        const auto& gp = s.gpus[i];
        bool hotVram = false;
        if (gp.vramTotalMiB > 0 && gp.vramUsedMiB >= 0)
            hotVram = (double)gp.vramUsedMiB / gp.vramTotalMiB >= 0.95;
        cols.push_back({"VRAM" + std::to_string(i), fmtMiB(gp.vramUsedMiB), hotVram});
    }
    return cols;
}

// x-extent of every column under the current layout. Painting and pointer
// hit-testing both use this, so what you see is exactly what you can point at.
std::vector<ColRect> layoutCols(cairo_t* cr, const Metrics& M,
                                       const std::vector<Col>& cols) {
    std::vector<ColRect> out;
    double x = M.outerPad;
    for (size_t i = 0; i < cols.size(); ++i) {
        ColRect r{x, M.colWidth(cr, cols[i]), -1};
        out.push_back(r);
        x += r.w;
        if (i + 1 < cols.size()) x += M.gap;
    }
    return out;
}

// paints one full widget frame onto `cr` (X window or a plain image surface)
void paintSnapshot(cairo_t* cr, int W, int H, bool argb,
                          const std::vector<Col>& cols) {
    Metrics M;
    M.prepare(cr);

    if (argb) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0); // transparent base -> rounded corners
    } else {
        cairo_set_source_rgb(cr, 0.043, 0.059, 0.078);
    }
    cairo_paint(cr);

    const double rad = 11.0 * g_uiScale;   // corner radius scales with the UI
    roundRect(cr, 0.5, 0.5, W - 1, H - 1, rad);
    if (argb) cairo_set_source_rgba(cr, 0.043, 0.059, 0.078, 0.94);
    else      cairo_set_source_rgb(cr, 0.043, 0.059, 0.078);
    cairo_fill(cr);

    roundRect(cr, 0.5, 0.5, W - 1, H - 1, rad);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08); // hairline border (>= 1 px)
    cairo_set_line_width(cr, std::max(1.0, 1.0 * g_uiScale));
    cairo_stroke(cr);

    const double labelBase = M.labelBase();
    const double valueBase = M.valueBase();

    std::vector<ColRect> rects = layoutCols(cr, M, cols);
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i > 0) { // subtle separator in the gap between columns
            double sx = rects[i].x - M.gap / 2.0 + 0.5;
            cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
            const double sepIn = 16.0 * g_uiScale; // vertical insets scale too
            cairo_move_to(cr, sx, sepIn);
            cairo_line_to(cr, sx, H - sepIn);
            cairo_stroke(cr);
        }

        const Col& c = cols[i];
        double x = rects[i].x;

        // label
        cairo_set_font_size(cr, M.labelPx);
        cairo_text_extents_t el;
        cairo_text_extents(cr, c.label.c_str(), &el);
        cairo_set_source_rgb(cr, 0.49, 0.53, 0.58); // #7D8794
        cairo_move_to(cr, x + M.padX, labelBase);
        cairo_show_text(cr, c.label.c_str());

        // value
        cairo_set_font_size(cr, M.valuePx);
        cairo_text_extents_t ev;
        cairo_text_extents(cr, c.value.c_str(), &ev);
        if (c.hot) cairo_set_source_rgb(cr, 1.0, 0.30, 0.29); // red like reference
        else       cairo_set_source_rgb(cr, 0.93, 0.945, 0.96);
        cairo_move_to(cr, x + M.padX, valueBase);
        cairo_show_text(cr, c.value.c_str());

        if (c.hot) { // small underline under hot values (as in the shot)
            double uw = std::min(ev.width, 2 * M.padX + 8.0 * g_uiScale);
            cairo_rectangle(cr, x + M.padX, valueBase + 5.0 * g_uiScale, uw,
                            2.0 * g_uiScale);
            cairo_fill(cr);
        }
    }
}

static void render(View& v, const Snapshot& s) {
    Metrics M;

    auto cols = buildColumns(s);

    // --- compute needed size on a scratch surface (font metrics only) -----
    cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t* mc = cairo_create(tmp);
    M.prepare(mc);
    int w = M.width(mc, cols);
    int h = M.height();

    // remember per-column hit areas; GPU/VRAM columns map back to their index
    // (column order: CPU RAM SWAP | GPU0..GPU(n-1) | VRAM0..VRAM(n-1))
    v.colRects = layoutCols(mc, M, cols);
    size_t ng = s.gpus.size();
    for (size_t i = 3; i < v.colRects.size() && i - 3 <= 2 * ng; ++i) {
        long gi = (long)i - 3;
        if (gi >= 0 && gi < (long)ng)         v.colRects[i].gpu = (int)gi;
        else if (ng > 0 && gi < 2 * (long)ng) v.colRects[i].gpu = (int)(gi - ng);
    }

    cairo_destroy(mc);
    cairo_surface_destroy(tmp);

    // --- resize window if the column set changed (e.g. GPU hot-plug) ------
    if (w != v.winW || h != v.winH) {
        XResizeWindow(v.dpy, v.win, w, h);
        cairo_xlib_surface_set_size(v.surf, w, h);
        v.winW = w;
        v.winH = h;
    }

    // --- paint ------------------------------------------------------------
    cairo_t* cr = cairo_create(v.surf);
    paintSnapshot(cr, v.winW, v.winH, v.argb, cols);
    cairo_surface_flush(v.surf);
    cairo_destroy(cr);
    XFlush(v.dpy);
}

// ---------------------------------------------------------------------------
// window creation / event loop
// ---------------------------------------------------------------------------

static void setEwmhHints(View& v) {
    Display* d = v.dpy;

    Atom a_type = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False);
    Atom a_dock = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", False); // stay on top, no taskbar entry
    XChangeProperty(d, v.win, a_type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&a_dock, 1);

    // keep GNOME client-side-decoration frame extents at zero
    Atom gfe = XInternAtom(d, "_GTK_FRAME_EXTENTS", False);
    long ext[4] = {0, 0, 0, 0};
    XChangeProperty(d, v.win, gfe, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)ext, 4);

    // ask to stay above regular windows (compositor/WMs may honor this)
    Atom a_state = XInternAtom(d, "_NET_WM_STATE", False);
    Atom a_above = XInternAtom(d, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(d, v.win, a_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&a_above, 1);

    XStoreName(d, v.win, "jtop");
}

struct OutRect { int x, y, w, h; bool primary; std::string name; };

// Ask the `xrandr` CLI for connected outputs (avoids linking libXrandr).
// On Mutter/GNOME X11 all monitors form ONE merged root window, so "top
// right of the root" can land on a monitor the user isn't even looking at.
static std::vector<OutRect> monitorRects() {
    static const char* kCmd = "xrandr -q 2>/dev/null";
    std::vector<OutRect> out;
    FILE* f = popen(kCmd, "r");
    if (!f) return out; // xrandr missing -> caller falls back to whole root
    char buf[512];
    std::string line;
    while (fgets(buf, sizeof buf, f)) {
        line = buf;
        if (line.empty() || !isalpha((unsigned char)line[0])) continue;
        size_t p = line.find(" connected"); // "disconnected" has no leading space
        if (p == std::string::npos) continue;
        const char* tok = line.c_str() + p + 10; // skip past " connected"
        while (*tok == ' ') ++tok;
        OutRect r{0, 0, 0, 0, false};
        size_t sp = line.find_first_of(" \t"); // output name is the line prefix
        if (sp != std::string::npos) r.name = trim(line.substr(0, sp));
        if (!std::strncmp(tok, "primary ", 8)) { r.primary = true; tok += 8; }
        int x = 0, y = 0, w = 0, h = 0;
        // geometry token looks like 6144x3456+0+1874 (rotation already accounted for)
        if (std::sscanf(tok, "%dx%d+%d+%d", &w, &h, &x, &y) == 4) {
            r.w = w; r.h = h; r.x = x; r.y = y;
            out.push_back(r);
        }
    }
    pclose(f);
    return out;
}

// The connected output whose rectangle contains the pointer (XQueryPointer on
// the given root window); falls back to the primary monitor, then the first.
static const OutRect* pointerMonitor(Display* dpy, Window root,
                                     const std::vector<OutRect>& rects) {
    if (!dpy || rects.empty()) return nullptr;
    Window rr = None, cr = None;
    int xr = 0, yr = 0, xd = 0, yd = 0; unsigned mask = 0;
    if (XQueryPointer(dpy, root, &rr, &cr, &xr, &yr, &xd, &yd, &mask))
        for (const auto& r : rects)
            if (xr >= r.x && xr < r.x + r.w && yr >= r.y && yr < r.y + r.h) return &r;
    for (const auto& r : rects) if (r.primary) return &r; // no pointer match -> primary
    return &rects[0];
}

// Mutter's saved display configuration: $XDG_CONFIG_HOME/monitors.xml (else
// ~/.config/monitors.xml) holds ONE <configuration> block per distinct hardware
// state GNOME has seen, each with a <scale> per logicalmonitor. Connector names
// get renamed across reboots (DP-1 -> DP-2 ...), so stale blocks must be
// filtered out: pick the block whose connectors best match the outputs xrandr
// reports as connected right now (exact set match beats partial overlap, then
// most matches) and return its scale for activeConnector (case-sensitive).
// Falls back to the value all entries of that block share. Returns 0 ("not
// found / ambiguous") when the file is missing, no live output appears in any
// block, or no unambiguous scale can be derived. Parsed with strstr/substr only.
static double monitorsXmlScale(const std::string& activeConnector) {
    // path: $XDG_CONFIG_HOME/monitors.xml else ~/.config/monitors.xml
    std::string path;
    if (const char* x = getenv("XDG_CONFIG_HOME")) {
        if (*x) path = std::string(x) + "/monitors.xml";
    } else if (const char* h = getenv("HOME")) {
        if (*h) path = std::string(h) + "/.config/monitors.xml";
    }
    if (path.empty()) return 0.0;

    std::string data = readFile(path, 1 << 20); // few KB in practice; room to grow
    if (data.empty()) return 0.0;               // no saved config -> fall through

    // currently-connected outputs (names come from xrandr via monitorRects)
    std::vector<std::string> live;
    for (const auto& r : monitorRects()) live.push_back(r.name);
    if (live.empty()) return 0.0;               // cannot match any block

    // trimmed inner text of <tag>...</tag> within seg, "" when absent
    const auto tagContent = [](const std::string& seg, const char* openTag) -> std::string {
        size_t a = seg.find(openTag);
        if (a == std::string::npos) return "";
        a += strlen(openTag);
        std::string closeTag = std::string("</") + (openTag + 1); // "<x>" -> "</x>"
        size_t b = seg.find(closeTag, a);
        if (b == std::string::npos) return "";
        return trim(seg.substr(a, b - a));
    };

    struct Entry { std::string conn; double scale; };
    const char* openCfg  = "<configuration>";
    const char* closeCfg = "</configuration>";
    const char* openLm   = "<logicalmonitor>";
    const char* closeLm  = "</logicalmonitor>";

    int bestMatch = 0, bestScore = -1;
    std::vector<Entry> bestEntries;

    size_t pos = 0;
    while (true) {
        size_t b0 = data.find(openCfg, pos);
        if (b0 == std::string::npos) break;
        size_t e0 = data.find(closeCfg, b0 + strlen(openCfg));
        if (e0 == std::string::npos) break; // malformed tail -> ignore the rest
        std::string block = data.substr(b0, e0 - b0 + strlen(closeCfg));
        pos = e0 + strlen(closeCfg);

        std::vector<Entry> entries;
        size_t p2 = 0;
        while (true) {
            size_t m0 = block.find(openLm, p2);
            if (m0 == std::string::npos) break;
            size_t m1 = block.find(closeLm, m0 + strlen(openLm));
            size_t len = (m1 == std::string::npos) ? std::string::npos : m1 - m0;
            if (m1 == std::string::npos) p2 = block.size(); // unterminated tail element
            else                          p2 = m1 + strlen(closeLm);
            std::string el = block.substr(m0, len);

            double sc = 0.0; // per-monitor scale (stays 0 when the tag is absent)
            {
                std::string t = tagContent(el, "<scale>");
                if (!t.empty()) {
                    char* endp = nullptr;
                    double dv = strtod(t.c_str(), &endp);
                    if (endp != t.c_str() && *endp == '\0') sc = dv; // strict number
                }
            }
            std::string conn = tagContent(el, "<connector>");
            if (!conn.empty()) entries.push_back({conn, sc});
        }

        int match = 0; // how many live outputs appear in this block
        for (const auto& en : entries)
            for (const auto& n : live)
                if (en.conn == n) { ++match; break; } // count each output once
        bool exact = (entries.size() == live.size() && match == (int)live.size());

        int score = 2 * match + (exact ? 1 : 0); // exact set beats partial overlap
        if (score > bestScore) {
            bestScore = score;
            bestMatch = match;
            bestEntries = std::move(entries);
        }
    }
    if (bestMatch <= 0) return 0.0;              // nothing live in any saved block

    for (const auto& en : bestEntries)
        if (!activeConnector.empty() && en.conn == activeConnector && en.scale > 0.0)
            return en.scale;

    // no per-monitor hit: use the value when every scaled entry agrees on it
    double common = -1.0;
    for (const auto& en : bestEntries) {
        if (en.scale <= 0.0) continue;           // unscaled monitor -> no info
        if (common < 0.0)      common = en.scale;
        else if (en.scale != common) return 0.0; // mixed scales -> ambiguous
    }
    return common > 0.0 ? common : 0.0;
}

// Move the (still unmapped) widget to the top-right of the monitor that
// currently holds the pointer, i.e. where the user ran the app and is looking.
static void placeTopRight(View& v) {
    const int margin = 24;
    auto rects = monitorRects();

    const OutRect* mon = pointerMonitor(v.dpy, v.root, rects);

    int x, y;
    if (mon) {
        x = mon->x + mon->w - v.winW - margin; // top-right corner of that monitor
        y = mon->y + margin;
        // clamp: keep it on the monitor even if the widget is bigger than it
        x = std::max(mon->x, std::min(x, mon->x + mon->w - v.winW));
        y = std::max(mon->y, std::min(y, mon->y + mon->h - v.winH));
    } else {
        x = v.rootW - v.winW - margin; // top-right of the whole root
        y = margin;
    }
    XMoveWindow(v.dpy, v.win, x, y);
}

static int createWindow(View& v, Display* dpy) {
    if (!dpy) {
        fprintf(stderr, "jtop: cannot open X display (is DISPLAY set?)\n");
        return 1;
    }
    v.dpy = dpy;
    v.screen = DefaultScreen(dpy);
    v.root = RootWindow(dpy, v.screen);
    v.rootW = DisplayWidth(dpy, v.screen);
    v.rootH = DisplayHeight(dpy, v.screen);

    // ARGB (32-bit) visual when available -> real rounded corners/alpha
    XVisualInfo vi{};
    int depth = DefaultDepth(dpy, v.screen);
    if (XMatchVisualInfo(dpy, v.screen, 32, TrueColor, &vi)) {
        v.argb = true;
        v.visual = vi.visual;
        depth = 32;
    } else {
        v.visual = DefaultVisual(dpy, v.screen);
    }
    v.depth = depth; // the popup window (created later) needs it too

    // scaled placeholder so the pre-resize state is sensible too
    int w0 = (int)(240 * g_uiScale), h0 = (int)(68 * g_uiScale);

    // Colormap for the window's OWN visual. Passing CWColormap without a
    // colormap that matches v.visual makes X reject the CreateWindow with
    // BadMatch (colormaps must belong to the same visual as their window).
    Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, v.screen),
                                    v.visual, AllocNone);
    if (cmap == None) {
        fprintf(stderr, "jtop: failed to create colormap\n");
        XCloseDisplay(dpy);
        return 1;
    }

    // opaque dark background (shown before first paint / without a compositor)
    XColor bg{};
    bg.red   = (short)(0.043 * 65535);
    bg.green = (short)(0.059 * 65535);
    bg.blue  = (short)(0.078 * 65535);
    XAllocColor(dpy, cmap, &bg);

    v.cmap = cmap;   // the popup window shares it (same visual)
    v.bgPixel = bg.pixel;

    long valuemask = CWOverrideRedirect | CWBackPixel | CWBorderPixel |
                     CWEventMask | CWColormap;
    XSetWindowAttributes attr{};
    attr.override_redirect = True; // no WM decorations -> true desktop widget
    attr.background_pixel  = bg.pixel;
    attr.border_pixel      = BlackPixel(dpy, v.screen);
    attr.colormap          = cmap;
    attr.event_mask        = ExposureMask | ButtonPressMask | PointerMotionMask |
                             LeaveWindowMask; // hide the GPU popup on pointer leave

    Window win = XCreateWindow(
        dpy, RootWindow(dpy, v.screen),
        v.rootW - w0 - 24, 24,                // top-right corner (re-placed before map)
        w0, h0, 0, depth, InputOutput, v.visual, valuemask, &attr);
    if (win == None) {
        fprintf(stderr, "jtop: failed to create window\n");
        XCloseDisplay(dpy);
        return 1;
    }

    v.win = win;
    setEwmhHints(v);

    v.surf = cairo_xlib_surface_create(dpy, win, v.visual, w0, h0);
    if (cairo_surface_status(v.surf) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "jtop: failed to create cairo surface\n");
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return 1;
    }
    // NOTE: window is mapped by the caller AFTER the first paint (no flash)
    return 0;
}

// ---------------------------------------------------------------------------
// GPU hover popup: a small always-on-top card with the device name, PCI info
// and the display connections (DP/HDMI/...) of one GPU.
// ---------------------------------------------------------------------------


void popupRows(const GpuInfo& g, std::vector<PopRow>& rows) {
    rows.push_back({g.name.empty() ? "(unknown GPU)" : g.name, 0});

    if (!g.pciAddr.empty() || !g.pciIds.empty()) {
        std::string t = "PCI";
        if (!g.pciAddr.empty()) t += " " + g.pciAddr;
        if (!g.pciIds.empty())  t += " (" + g.pciIds + ")";
        rows.push_back({t, 1});
    }

    const std::string pcie = pcieLabel(g); // e.g. "Gen4 x16"
    if (!pcie.empty()) rows.push_back({"PCIe " + pcie, 1});

    if (g.conns.empty()) {
        rows.push_back({"no display connections", 3});
        return;
    }

    // align the name/kind columns of all connector rows (monospace grid)
    int wN = 0, wK = 0;
    for (const auto& c : g.conns) {
        wN = std::max(wN, (int)c.name.size());
        wK = std::max(wK, (int)c.kind.size());
    }
    char buf[160];
    for (const auto& c : g.conns) {
        const std::string tail = c.connected ? (!c.mode.empty() ? c.mode
                                                                : std::string("--"))
                                             : "off";
        snprintf(buf, sizeof buf, "%-*s  %-*s  %s",
                 wN, c.name.c_str(), wK, c.kind.c_str(), tail.c_str());
        rows.push_back({buf, c.connected ? 2 : 3});
    }
}


PopGeom popupGeom(cairo_t* cr, const std::vector<PopRow>& rows) {
    Metrics M;
    M.prepare(cr); // applies the UI scale + label/value font metrics

    const double sc   = g_uiScale;
    const double padX = 12 * sc, pt = 10 * sc, pb = 11 * sc;
    const double gapT = 9 * sc, gapR = 6 * sc; // after title / between other rows

    PopGeom g;
    g.padX = padX;
    double y = pt, maxw = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool title = (rows[i].kind == 0);
        cairo_text_extents_t te;
        cairo_set_font_size(cr, title ? M.valuePx : M.labelPx);
        cairo_text_extents(cr, rows[i].text.c_str(), &te);
        maxw = std::max(maxw, te.width);
        g.base.push_back(y + (title ? M.ve.ascent : M.le.ascent));
        y += (title ? M.ve.ascent + M.ve.descent : M.le.ascent + M.le.descent)
           + ((i + 1 < rows.size()) ? (title ? gapT : gapR) : 0);
    }
    g.w = (int)std::ceil(maxw + 2 * padX);
    g.h = (int)std::ceil(y + pb);
    return g;
}

// same dark rounded-card look as the main panel, a touch more opaque since it
// floats in front of everything
void paintPopup(cairo_t* cr, int W, int H, bool argb,
                       const std::vector<PopRow>& rows) {
    Metrics M;
    M.prepare(cr);
    PopGeom g = popupGeom(cr, rows); // per-row baselines at the current scale

    if (argb) cairo_set_source_rgba(cr, 0, 0, 0, 0);
    else      cairo_set_source_rgb(cr, 0.043, 0.059, 0.078);
    cairo_paint(cr);

    const double rad = 9 * g_uiScale;
    roundRect(cr, 0.5, 0.5, W - 1, H - 1, rad);
    if (argb) cairo_set_source_rgba(cr, 0.043, 0.059, 0.078, 0.97);
    else      cairo_set_source_rgb(cr, 0.043, 0.059, 0.078);
    cairo_fill(cr);

    roundRect(cr, 0.5, 0.5, W - 1, H - 1, rad);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10); // hairline border
    cairo_set_line_width(cr, std::max(1.0, 1.0 * g_uiScale));
    cairo_stroke(cr);

    for (size_t i = 0; i < rows.size(); ++i) {
        const bool title = (rows[i].kind == 0);
        cairo_set_font_size(cr, title ? M.valuePx : M.labelPx);
        if      (title)             cairo_set_source_rgb(cr, 0.93, 0.945, 0.96); // device name
        else if (rows[i].kind == 1) cairo_set_source_rgb(cr, 0.49, 0.53, 0.58); // pci info
        else if (rows[i].kind == 2) cairo_set_source_rgb(cr, 0.76, 0.80, 0.86); // connected
        else                        cairo_set_source_rgb(cr, 0.40, 0.435, 0.49); // off / dim
        cairo_move_to(cr, g.padX, g.base[i]);
        cairo_show_text(cr, rows[i].text.c_str());
    }
}

// separate small borderless window for the popup; shares main's visual +
// colormap. Non-fatal when unavailable -> hovering simply does nothing.
static void createPopup(View& v) {
    long valuemask = CWOverrideRedirect | CWBackPixel | CWBorderPixel |
                     CWEventMask | CWColormap;
    XSetWindowAttributes attr{};
    attr.override_redirect = True; // no WM decorations, like the main widget
    attr.background_pixel  = v.bgPixel;
    attr.border_pixel      = BlackPixel(v.dpy, v.screen);
    attr.colormap          = v.cmap;
    attr.event_mask        = ExposureMask; // repaint on expose

    int w0 = (int)(320 * g_uiScale), h0 = (int)(150 * g_uiScale);
    Window win = XCreateWindow(v.dpy, v.root, 0, 0, w0, h0, 0, v.depth,
                               InputOutput, v.visual, valuemask, &attr);
    if (win == None) {
        fprintf(stderr, "jtop: failed to create popup window (hover disabled)\n");
        return;
    }
    XStoreName(v.dpy, win, "jtop-gpu-info");

    // stay above regular windows, like the main panel does
    Atom a_state = XInternAtom(v.dpy, "_NET_WM_STATE", False);
    Atom a_above = XInternAtom(v.dpy, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(v.dpy, win, a_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&a_above, 1);

    v.popWin = win;
    cairo_surface_t* ps = cairo_xlib_surface_create(v.dpy, win, v.visual, w0, h0);
    if (cairo_surface_status(ps) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "jtop: failed to create popup surface (hover disabled)\n");
        cairo_surface_destroy(ps);
        XDestroyWindow(v.dpy, win);
        v.popWin = None;
        return;
    }
    v.popSurf = ps;
}

// GPU index under the last pointer position, or -1 (also when over CPU/RAM/...)
static int hoverGpuIndex(const View& v) {
    if (v.mx < 0 || v.my < 0 || v.my >= v.winH) return -1;
    for (const auto& r : v.colRects)
        if (r.gpu >= 0 && v.mx >= r.x && v.mx < r.x + r.w) return r.gpu;
    return -1;
}

static void hidePopup(View& v) {
    if (!v.popShown || v.popWin == None) { v.popShown = false; return; }
    XUnmapWindow(v.dpy, v.popWin);
    v.popShown = false;
    XFlush(v.dpy);
}

// (re)build the popup content of the current GPU and position it just below
// the widget at the hovered column (above when there is no room underneath)
static void refreshPopup(View& v, const State& s) {
    if (v.popWin == None || !v.popSurf) return;
    int g = v.popGpu;
    if (g < 0 || g >= (int)s.snap.gpus.size()) { hidePopup(v); return; }

    std::vector<PopRow> rows;
    popupRows(s.snap.gpus[g], rows);

    cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t* mc = cairo_create(tmp);
    PopGeom pg = popupGeom(mc, rows);
    cairo_destroy(mc);
    cairo_surface_destroy(tmp);

    if (pg.w != v.popW || pg.h != v.popH) {
        XResizeWindow(v.dpy, v.popWin, pg.w, pg.h);
        cairo_xlib_surface_set_size(v.popSurf, pg.w, pg.h);
        v.popW = pg.w;
        v.popH = pg.h;
    }

    Window child = None; int wx0 = 0, wy0 = 0;
    XTranslateCoordinates(v.dpy, v.win, v.root, 0, 0, &wx0, &wy0, &child);
    double colX = -1; // first rect of this GPU is its "GPU i" column
    for (const auto& r : v.colRects)
        if (r.gpu == g) { colX = r.x; break; }

    const int gap = (int)(8 * g_uiScale);
    int x = wx0 + (int)(colX < 0 ? 0 : colX);
    int y = wy0 + v.winH + gap;
    if (y + pg.h > v.rootH - 2 && wy0 - pg.h - gap >= 0) y = wy0 - pg.h - gap; // flip up
    x = std::max(2, std::min(x, v.rootW - pg.w - 2));
    y = std::max(2, std::min(y, v.rootH - pg.h - 2));
    XMoveWindow(v.dpy, v.popWin, x, y);

    cairo_t* cr = cairo_create(v.popSurf);
    paintPopup(cr, pg.w, pg.h, v.argb, rows);
    cairo_surface_flush(v.popSurf);
    cairo_destroy(cr);
}

static void showPopupAt(View& v, const State& s, int g) {
    v.popGpu = g;
    refreshPopup(v, s);
    if (!v.popShown && v.popWin != None) {
        XMapWindow(v.dpy, v.popWin);
        v.popShown = true;
        XFlush(v.dpy);
    }
}

static int runEventLoop(View& v, State& s) {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::milliseconds(5000); // refresh every 5 s

    auto nextTick = clock::now() + period; // first render already done by caller
    int fd = ConnectionNumber(v.dpy);

    for (;;) {
        // -- X events ------------------------------------------------------
        while (XPending(v.dpy)) {
            XEvent ev{};
            XNextEvent(v.dpy, &ev);
            switch (ev.type) {
                case ButtonPress:
                    if (ev.xbutton.button == 3) return 0; // right click -> quit
                    if (ev.xbutton.button == 1) {
                        hidePopup(v); // no hover popup while dragging
                        Window child = None;
                        int wx = 0, wy = 0;
                        XTranslateCoordinates(v.dpy, v.win, RootWindow(v.dpy, v.screen),
                                              0, 0, &wx, &wy, &child); // window origin on root
                        v.dragging = true;
                        v.dragDX = ev.xbutton.x_root - wx;
                        v.dragDY = ev.xbutton.y_root - wy;
                        XGrabPointer(v.dpy, v.win, True,
                                     ButtonPressMask | PointerMotionMask |
                                     ButtonReleaseMask,
                                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                    }
                    break;

                case MotionNotify: {
                    if (v.dragging) {
                        int nx = ev.xmotion.x_root - v.dragDX;
                        int ny = ev.xmotion.y_root - v.dragDY;
                        nx = std::max(0, std::min(nx, v.rootW  - v.winW));
                        ny = std::max(0, std::min(ny, v.rootH - v.winH));
                        XMoveWindow(v.dpy, v.win, nx, ny);
                    } else { // GPU hover -> popup (state change only redraws)
                        v.mx = ev.xmotion.x;
                        v.my = ev.xmotion.y;
                        int g = hoverGpuIndex(v);
                        if (g < 0) hidePopup(v);
                        else if (!v.popShown || v.popGpu != g) showPopupAt(v, s, g);
                    }
                    break;
                }

                case ButtonRelease:
                    if (v.dragging) {
                        v.dragging = false;
                        XUngrabPointer(v.dpy, CurrentTime);
                    }
                    break;

                case LeaveNotify: // pointer left the widget -> no hover popup
                    hidePopup(v);
                    break;

                case Expose:
                    if (v.popWin != None && ev.xexpose.window == v.popWin) {
                        if (v.popShown) refreshPopup(v, s); // repaint popup content
                    } else {
                        render(v, s.snap); // full redraw is cheap at this size
                    }
                    break;

                default:
                    break;
            }
        }

        // -- 5 second refresh timer -----------------------------------------
        auto now = clock::now();
        if (now >= nextTick) {
            refresh(s);
            render(v, s.snap);
            if (v.popShown && !v.dragging) refreshPopup(v, s); // data may have changed
            while (nextTick <= clock::now()) nextTick += period; // resync if we lagged
        } else {
            long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          nextTick - now).count();
            struct timeval tv{(long)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            select(fd + 1, &rfds, nullptr, nullptr, ms > 0 ? &tv : nullptr);
        }
    }
}

// Metrics::prepare lives out-of-line (see gui.h): it applies the session UI
// scale, which is file-local to this translation unit.
void Metrics::prepare(cairo_t* cr) {
    // apply the session UI scale to every layout constant (the font sizes
    // are set from labelPx/valuePx below, so text scales along with them)
    labelPx  *= g_uiScale;  valuePx  *= g_uiScale;
    padX     *= g_uiScale;  gap      *= g_uiScale;
    outerPad *= g_uiScale;  topPad   *= g_uiScale;
    botPad   *= g_uiScale;  midGap   *= g_uiScale;

    cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, labelPx);
    cairo_font_extents(cr, &le);
    cairo_set_font_size(cr, valuePx);
    cairo_font_extents(cr, &ve);
}

// ---------------------------------------------------------------------------
// GUI entry point: display -> UI scale -> windows -> event loop.
// ---------------------------------------------------------------------------

int runWidget() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return -1; // no graphics: main() falls back to the terminal table

    // Which connected output sits under the pointer? Needed before any window
    // exists so monitors.xml's per-monitor scale can be picked for it.
    auto startupRects = monitorRects();
    const OutRect* ptrMon =
        pointerMonitor(dpy, RootWindow(dpy, DefaultScreen(dpy)), startupRects);
    std::string activeConn = (ptrMon && !ptrMon->name.empty()) ? ptrMon->name : "";

    // Resolve the session UI scale once, before any window/geometry decision.
    g_uiScale = detectUiScale(dpy, activeConn);

    View v{};
    int rc = createWindow(v, dpy);
    if (rc != 0) return rc;
    createPopup(v); // GPU hover popup window (non-fatal when unavailable)

    State s{};
    refresh(s);      // initial snapshot (includes a quick CPU baseline sample)
    render(v, s.snap);
    placeTopRight(v);             // land on the monitor the user is looking at
    XMapWindow(v.dpy, v.win);     // paint first, map second -> no background flash

    runEventLoop(v, s);

    if (v.popSurf) cairo_surface_destroy(v.popSurf);
    if (v.popWin != None) XDestroyWindow(v.dpy, v.popWin);
    cairo_surface_destroy(v.surf);
    XDestroyWindow(v.dpy, v.win);
    XCloseDisplay(v.dpy);
    return 0;
}
