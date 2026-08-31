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
//   auto refresh       : every 5 seconds while running

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>

#include <dirent.h>
#include <sys/select.h>
#include <unistd.h>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// small filesystem helpers
// ---------------------------------------------------------------------------

static std::string readFile(const std::string& path, size_t maxBytes = 65536) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return std::string(buf, n < maxBytes ? n : maxBytes);
}

// returns -1 when missing or unparseable
static long readLong(const std::string& path) {
    std::string s = readFile(path, 256);
    if (s.empty()) return -1;
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    return (end == s.c_str()) ? -1 : v;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// /proc/stat -> aggregate CPU usage %
// ---------------------------------------------------------------------------

struct CpuTimes {
    unsigned long v[7] = {0}; // user nice system idle iowait irq softirq steal
};

static bool readCpuTimes(CpuTimes& c) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return false;
    char line[256];
    bool ok = (fgets(line, sizeof(line), f) != nullptr);
    fclose(f);
    if (!ok) return false;
    unsigned long a, b, d, e, g, h, i;
    int n = sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu", &a, &b, &d, &e, &g, &h, &i);
    if (n < 6) return false; // user nice system idle iowait irq [softirq]
    c.v[0] = a; c.v[1] = b; c.v[2] = d; c.v[3] = e; c.v[4] = g;
    if (n >= 7) { c.v[5] = h; c.v[6] = i; } // irq, softirq
    return true;
}

static unsigned long cpuTotal(const CpuTimes& c) {
    unsigned long t = 0;
    for (int i = 0; i < 7; ++i) t += c.v[i];
    return t;
}
static unsigned long cpuIdle(const CpuTimes& c) { return c.v[3] + c.v[4]; }

// usage % between two samples, or -1 if not computable yet
static int cpuPercentBetween(const CpuTimes& prev, const CpuTimes& cur) {
    long dt = (long)(cpuTotal(cur) - cpuTotal(prev));
    if (dt <= 0) return -1;
    long di = (long)(cpuIdle(cur) - cpuIdle(prev));
    double pct = 100.0 * (dt - di) / (double)dt;
    int p = (int)std::lround(pct);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

// ---------------------------------------------------------------------------
// /proc/meminfo -> RAM / swap usage (kB)
// ---------------------------------------------------------------------------

struct MemInfo {
    long kbRamTotal = 0, kbRamAvail = 0;
    long kbSwapTotal = 0, kbSwapFree = 0;
};

static void readMemInfo(MemInfo& m) {
    std::string data = readFile("/proc/meminfo");
    size_t pos = 0;
    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) break;
        std::string line = data.substr(pos, eol - pos);
        pos = eol + 1;

        char key[32];
        long val = 0;
        if (sscanf(line.c_str(), " %31[^:]:%ld", key, &val) != 2) continue;
        std::string k = trim(key);
        if      (k == "MemTotal")     m.kbRamTotal  = val;
        else if (k == "MemAvailable") m.kbRamAvail  = val;
        else if (k == "SwapTotal")    m.kbSwapTotal = val;
        else if (k == "SwapFree")     m.kbSwapFree  = val;
    }
}

static long ramUsedKb(const MemInfo& m) {
    return m.kbRamTotal > 0 ? m.kbRamTotal - m.kbRamAvail : 0;
}
static long swapUsedKb(const MemInfo& m) {
    return m.kbSwapTotal > 0 ? m.kbSwapTotal - m.kbSwapFree : 0;
}

// ---------------------------------------------------------------------------
// formatting helpers
// ---------------------------------------------------------------------------

// e.g. "7.2G" / "512M"
static std::string fmtSmartKb(long kb) {
    char b[32];
    double g = kb / 1048576.0;
    if (g >= 1.0) { snprintf(b, sizeof(b), "%.1fG", g); return b; }
    long mib = kb / 1024;
    snprintf(b, sizeof(b), "%ldM", mib);
    return b;
}

// always GiB with one decimal: "0.0G" (matches the reference look for SWAP)
static std::string fmtAlwaysGB(long kb) {
    char b[32];
    snprintf(b, sizeof(b), "%.1fG", kb / 1048576.0);
    return b;
}

static std::string fmtMiB(long mib) {
    if (mib < 0) return "--";
    char b[32];
    double g = mib / 1024.0;
    if (g >= 1.0) { snprintf(b, sizeof(b), "%.1fG", g); return b; }
    snprintf(b, sizeof(b), "%ldM", mib);
    return b;
}

// ---------------------------------------------------------------------------
// CPU temperature via sysfs hwmon / thermal zones
// ---------------------------------------------------------------------------

static int chipMaxTemp(const std::string& chipDir) {
    int best = -1;
    for (int i = 1; i <= 24; ++i) {
        long mdeg = readLong(chipDir + "/temp" + std::to_string(i) + "_input");
        if (mdeg > 0 && mdeg < 300000) best = std::max(best, (int)(mdeg / 1000));
    }
    return best;
}

static int cpuTempC() {
    // 1) prefer a dedicated CPU hwmon chip (coretemp/k10temp/zenpower/...)
    int prioBest = 99, val = -1;
    if (DIR* d = opendir("/sys/class/hwmon")) {
        while (struct dirent* e = readdir(d)) {
            std::string dir = std::string("/sys/class/hwmon/") + e->d_name;
            std::string name = trim(readFile(dir + "/name", 64));
            int p = 99;
            if (name == "coretemp") p = 0;
            else if (name.find("cpu") != std::string::npos || name == "k10temp" ||
                     name == "zenpower") p = 1;
            if (p < prioBest) {
                int t = chipMaxTemp(dir);
                if (t >= 0) { prioBest = p; val = t; }
            }
        }
        closedir(d);
    }
    if (val >= 0) return val;

    // 2) fallback: ACPI thermal zones whose type mentions cpu / pkg_temp
    int best = -1;
    if (DIR* d = opendir("/sys/class/thermal")) {
        while (struct dirent* e = readdir(d)) {
            std::string zone = std::string("/sys/class/thermal/") + e->d_name;
            DIR* zt = opendir(zone.c_str());
            if (!zt) continue;
            closedir(zt);

            std::string tl = trim(readFile(zone + "/type", 64));
            for (auto& c : tl) c = tolower(c);
            bool match = (tl.find("pkg_temp") != std::string::npos) || (tl.rfind("cpu", 0) == 0);
            if (!match) continue;

            long mdeg = readLong(zone + "/temp");
            if (mdeg > 0 && mdeg < 300000) best = std::max(best, (int)(mdeg / 1000));
        }
        closedir(d);
    }
    return best; // -1 -> unknown, shown as "--"
}

// ---------------------------------------------------------------------------
// GPUs: NVIDIA via nvidia-smi, otherwise amdgpu/radeon/nvidia via sysfs
// ---------------------------------------------------------------------------

struct GpuInfo {
    int  util = -1;         // % or -1
    long vramUsedMiB = -1;  // MiB or -1
    long vramTotalMiB = -1;
    int  tempC = -1;        // °C or -1
};

static std::vector<GpuInfo> nvidiaGpus() {
    static bool probed = false, ok = false;
    if (!probed) {
        probed = true;
        ok = (system("nvidia-smi -L >/dev/null 2>&1") == 0); // once per run
    }
    std::vector<GpuInfo> out;
    if (!ok) return out;

    const char* cmd = "nvidia-smi --query-gpu=index,utilization.gpu,"
                      "memory.used,memory.total,temperature.gpu "
                      "--format=csv,noheader,nounits 2>/dev/null";
    FILE* p = popen(cmd, "r");
    if (!p) return out;

    char line[1024];
    while (fgets(line, sizeof(line), p)) {
        GpuInfo g;
        int f = 0;
        for (char* rest = line; rest && *rest; ) { // manual split on ','
            char* com = strchr(rest, ',');
            if (com) *com = '\0';
            std::string s = trim(rest);
            ++f;
            char* end = nullptr;
            long v = strtol(s.c_str(), &end, 10);
            bool num = (end != s.c_str()); // rejects "[N/A]" etc.
            switch (f) {
                case 2: if (num) g.util = (int)v;       break; // utilization.gpu %
                case 3: if (num) g.vramUsedMiB = v;     break; // MiB
                case 4: if (num) g.vramTotalMiB = v;    break; // MiB
                case 5: if (num) g.tempC = (int)v;      break; // °C
            }
            rest = com ? com + 1 : nullptr;
        }
        if (g.util >= 0 || g.tempC >= 0 || g.vramUsedMiB >= 0) out.push_back(g);
    }
    pclose(p);
    return out;
}

static std::vector<GpuInfo> sysfsGpus() {
    struct Card { int n; std::string path; };
    std::vector<Card> cards;
    if (DIR* d = opendir("/sys/class/drm")) {
        while (struct dirent* e = readdir(d)) {
            const char* nm = e->d_name;
            if (strncmp(nm, "card", 4) != 0 || !isdigit((unsigned char)nm[4])) continue;
            cards.push_back({atoi(nm + 4), std::string("/sys/class/drm/") + nm});
        }
        closedir(d);
    }
    std::sort(cards.begin(), cards.end(),
              [](const Card& a, const Card& b) { return a.n < b.n; });

    std::vector<GpuInfo> out;
    for (auto& c : cards) {
        std::string dev = c.path + "/device";
        GpuInfo g;

        long u = readLong(dev + "/gpu_busy_percent"); // amdgpu/radeon
        if (u >= 0 && u <= 100) g.util = (int)u;

        long vu = readLong(dev + "/mem_info_vram_used");  // bytes
        long vt = readLong(dev + "/mem_info_vram_total");
        if (vu > 0)       g.vramUsedMiB  = vu >> 20;
        if (vt > (1L << 30)) g.vramTotalMiB = vt >> 20;

        // temperature: any hwmon chip linked from this PCI device
        if (DIR* hd = opendir((dev + "/hwmon").c_str())) {
            while (struct dirent* e2 = readdir(hd)) {
                // NOTE: glibc's FORTIFY wrapper requires a PATH_MAX-sized buffer
                char real[PATH_MAX];
                // kernel puts *relative* symlinks here; realpath() resolves them
                if (!realpath((dev + "/hwmon/" + e2->d_name).c_str(), real))
                    continue;

                std::string buf(real);
                std::string name = trim(readFile(buf + "/name", 64));
                for (auto& ch : name) ch = tolower(ch);
                if (name.find("amdgpu") == std::string::npos && name.find("radeon") == std::string::npos &&
                    name.find("nvidia") == std::string::npos)
                    continue;

                g.tempC = std::max(g.tempC, chipMaxTemp(buf));
            }
            closedir(hd);
        }

        // only report cards that actually expose something GPU-like
        if (g.util >= 0 || g.vramTotalMiB > 0 || g.tempC >= 0) out.push_back(g);
    }
    return out;
}

static std::vector<GpuInfo> collectGpus() {
    auto nv = nvidiaGpus();
    if (!nv.empty()) return nv; // assume all GPUs are NVIDIA when the driver works
    return sysfsGpus();
}

// ---------------------------------------------------------------------------
// snapshot of everything we display
// ---------------------------------------------------------------------------

struct Snapshot {
    int cpuPct = -1;              // or -1 (unknown)
    long ramUsedKb = 0;
    long swapUsedKb = 0;
    bool hasSwap = false;
    int cpuTempC = -1;
    std::vector<GpuInfo> gpus;
};

struct State {
    CpuTimes prevCpu{};
    bool havePrev = false;
    Snapshot snap{};
};

static void refresh(State& s) {
    Snapshot sn;

    // CPU % : delta since previous snapshot (quick 300 ms baseline at startup)
    CpuTimes cur{};
    if (readCpuTimes(cur)) {
        int pct = -1;
        if (!s.havePrev) {
            usleep(300 * 1000);
            CpuTimes c2;
            if (readCpuTimes(c2)) pct = cpuPercentBetween(cur, c2);
            cur = c2; // keep the newest sample as baseline
        } else {
            pct = cpuPercentBetween(s.prevCpu, cur);
        }
        sn.cpuPct = pct;
    }
    s.prevCpu = cur;
    s.havePrev = true;

    MemInfo mi{};
    readMemInfo(mi);
    sn.ramUsedKb  = ramUsedKb(mi);
    sn.swapUsedKb = swapUsedKb(mi);
    sn.hasSwap    = (mi.kbSwapTotal > 0);

    sn.cpuTempC = cpuTempC();
    sn.gpus     = collectGpus();

    s.snap = std::move(sn);
}

static void printDump(const Snapshot& s) {
    auto g = [](int v) -> std::string { return v < 0 ? "--" : std::to_string(v); };
    printf("CPU %s%%   RAM %s   SWAP %s",
           (s.cpuPct < 0 ? "--" : std::to_string(s.cpuPct)).c_str(),
           fmtSmartKb(s.ramUsedKb).c_str(),
           s.hasSwap ? fmtAlwaysGB(s.swapUsedKb).c_str() : "n/a");
    printf("   CPU° %s\n", g(s.cpuTempC).c_str());
    if (s.gpus.empty()) {
        printf("(no GPUs detected)\n");
        return;
    }
    for (size_t i = 0; i < s.gpus.size(); ++i) {
        const auto& gp = s.gpus[i];
        printf("GPU%zu util=%-3s vram=%-6s/%-6s temp=%s°\n",
               i, g(gp.util).c_str(), fmtMiB(gp.vramUsedMiB).c_str(),
               fmtMiB(gp.vramTotalMiB).c_str(), g(gp.tempC).c_str());
    }
}

// ---------------------------------------------------------------------------
// window / drawing (X11 + cairo)
// ---------------------------------------------------------------------------

struct Col {
    std::string label;  // small grey text on top, e.g. "GPU 0"
    std::string value;  // big text below,          e.g. "94%"
    bool hot = false;   // red + underline (as in the reference shot)
};

struct View {
    Display* dpy = nullptr;
    int screen = 0, rootW = 0, rootH = 0;
    Window win = None;
    cairo_surface_t* surf = nullptr;
    Visual* visual = nullptr;
    bool argb = false;

    bool dragging = false;
    int dragDX = 0, dragDY = 0;

    int winW = 240, winH = 68;
};

// layout metrics (single source of truth for sizing + painting)
struct Metrics {
    double labelPx = 10.5, valuePx = 19.0;
    double padX = 13.0, gap = 22.0, outerPad = 15.0, topPad = 13.0, botPad = 12.5;
    double midGap = 7.0;

    cairo_font_extents_t le{}, ve{}; // font (line) metrics at label / value size

    void prepare(cairo_t* cr) {
        cairo_select_font_face(cr, "DejaVu Sans Mono", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, labelPx);
        cairo_font_extents(cr, &le);
        cairo_set_font_size(cr, valuePx);
        cairo_font_extents(cr, &ve);
    }

    double labelBase() const { return topPad + le.ascent; }
    double valueBase() const { return topPad + le.ascent + le.descent + midGap + ve.ascent; }
    int height() const {
        return (int)std::ceil(topPad + le.ascent + le.descent + midGap +
                              ve.ascent + ve.descent + botPad);
    }

    double colWidth(cairo_t* cr, const Col& c) const {
        cairo_text_extents_t el, ev;
        cairo_set_font_size(cr, labelPx);
        cairo_text_extents(cr, c.label.c_str(), &el);
        cairo_set_font_size(cr, valuePx);
        cairo_text_extents(cr, c.value.c_str(), &ev);
        return std::max(el.width, ev.width) + 2 * padX;
    }

    int width(cairo_t* cr, const std::vector<Col>& cols) const {
        int w = (int)std::ceil(2 * outerPad);
        for (const auto& c : cols) w += (int)std::ceil(colWidth(cr, c));
        if (cols.size() > 1) w += (int)(cols.size() - 1) * gap;
        return w;
    }
};

static void roundRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static std::vector<Col> buildColumns(const Snapshot& s) {
    auto tempCol = [](const std::string& label, int t) -> Col {
        return {label, (t < 0 ? "--" : (std::to_string(t) + "\xC2\xB0")), t >= 90}; // ° UTF-8
    };

    std::vector<Col> cols;
    cols.push_back({"CPU",  s.cpuPct < 0 ? "--" : (std::to_string(s.cpuPct) + "%"),
                    s.cpuPct >= 90});
    cols.push_back({"RAM", fmtSmartKb(s.ramUsedKb), false});
    cols.push_back({"SWAP", s.hasSwap ? fmtAlwaysGB(s.swapUsedKb) : "--", false});
    cols.push_back(tempCol("CPU \xC2\xB0", s.cpuTempC));

    for (size_t i = 0; i < s.gpus.size(); ++i)
        cols.push_back({"GPU " + std::to_string(i),
                        s.gpus[i].util < 0 ? "--" : (std::to_string(s.gpus[i].util) + "%"),
                        s.gpus[i].util >= 90});
    for (size_t i = 0; i < s.gpus.size(); ++i) {
        const auto& gp = s.gpus[i];
        bool hotVram = false;
        if (gp.vramTotalMiB > 0 && gp.vramUsedMiB >= 0)
            hotVram = (double)gp.vramUsedMiB / gp.vramTotalMiB >= 0.95;
        cols.push_back({"VRAM " + std::to_string(i), fmtMiB(gp.vramUsedMiB), hotVram});
    }
    for (size_t i = 0; i < s.gpus.size(); ++i)
        cols.push_back(tempCol("GPU \xC2\xB0 " + std::to_string(i), s.gpus[i].tempC));
    return cols;
}

// paints one full widget frame onto `cr` (X window or a plain image surface)
static void paintSnapshot(cairo_t* cr, int W, int H, bool argb,
                          const std::vector<Col>& cols) {
    Metrics M;
    M.prepare(cr);

    if (argb) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0); // transparent base -> rounded corners
    } else {
        cairo_set_source_rgb(cr, 0.043, 0.059, 0.078);
    }
    cairo_paint(cr);

    roundRect(cr, 0.5, 0.5, W - 1, H - 1, 11.0);
    if (argb) cairo_set_source_rgba(cr, 0.043, 0.059, 0.078, 0.94);
    else      cairo_set_source_rgb(cr, 0.043, 0.059, 0.078);
    cairo_fill(cr);

    roundRect(cr, 0.5, 0.5, W - 1, H - 1, 11.0);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08); // hairline border
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    const double labelBase = M.labelBase();
    const double valueBase = M.valueBase();

    double x = M.outerPad;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i > 0) { // subtle separator in the gap between columns
            double sx = x - M.gap / 2.0 + 0.5;
            cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
            cairo_move_to(cr, sx, 16);
            cairo_line_to(cr, sx, H - 16);
            cairo_stroke(cr);
        }

        // label
        cairo_set_font_size(cr, M.labelPx);
        cairo_text_extents_t el;
        cairo_text_extents(cr, cols[i].label.c_str(), &el);
        cairo_set_source_rgb(cr, 0.49, 0.53, 0.58); // #7D8794
        cairo_move_to(cr, x + M.padX, labelBase);
        cairo_show_text(cr, cols[i].label.c_str());

        // value
        cairo_set_font_size(cr, M.valuePx);
        cairo_text_extents_t ev;
        cairo_text_extents(cr, cols[i].value.c_str(), &ev);
        if (cols[i].hot) cairo_set_source_rgb(cr, 1.0, 0.30, 0.29); // red like reference
        else             cairo_set_source_rgb(cr, 0.93, 0.945, 0.96);
        cairo_move_to(cr, x + M.padX, valueBase);
        cairo_show_text(cr, cols[i].value.c_str());

        if (cols[i].hot) { // small underline under hot values (as in the shot)
            double uw = std::min(ev.width, 2 * M.padX + 8.0);
            cairo_rectangle(cr, x + M.padX, valueBase + 5.0, uw, 2.0);
            cairo_fill(cr);
        }

        x += M.colWidth(cr, cols[i]);
        if (i + 1 < cols.size()) x += M.gap;
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

    XStoreName(d, v.win, "jtop");
}

static int createWindow(View& v) {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "jtop: cannot open X display (is DISPLAY set?)\n");
        return 1;
    }
    v.dpy = dpy;
    v.screen = DefaultScreen(dpy);
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

    int w0 = 240, h0 = 68;

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

    long valuemask = CWOverrideRedirect | CWBackPixel | CWBorderPixel |
                     CWEventMask | CWColormap;
    XSetWindowAttributes attr{};
    attr.override_redirect = True; // no WM decorations -> true desktop widget
    attr.background_pixel  = bg.pixel;
    attr.border_pixel      = BlackPixel(dpy, v.screen);
    attr.colormap          = cmap;
    attr.event_mask        = ExposureMask | ButtonPressMask | PointerMotionMask;

    Window win = XCreateWindow(
        dpy, RootWindow(dpy, v.screen),
        v.rootW - w0 - 24, v.rootH - h0 - 24, // bottom-right corner
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

                case MotionNotify:
                    if (v.dragging) {
                        int nx = ev.xmotion.x_root - v.dragDX;
                        int ny = ev.xmotion.y_root - v.dragDY;
                        nx = std::max(0, std::min(nx, v.rootW  - v.winW));
                        ny = std::max(0, std::min(ny, v.rootH - v.winH));
                        XMoveWindow(v.dpy, v.win, nx, ny);
                    }
                    break;

                case ButtonRelease:
                    if (v.dragging) {
                        v.dragging = false;
                        XUngrabPointer(v.dpy, CurrentTime);
                    }
                    break;

                case Expose:
                    render(v, s.snap); // full redraw is cheap at this size
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

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // --dump: print one snapshot to stdout and exit (handy for testing/headless)
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
        State s{};
        refresh(s);
        printDump(s.snap);
        return 0;
    }

    View v{};
    int rc = createWindow(v);
    if (rc != 0) return rc;

    State s{};
    refresh(s);      // initial snapshot (includes a quick CPU baseline sample)
    render(v, s.snap);
    XMapWindow(v.dpy, v.win); // paint first, map second -> no background flash

    runEventLoop(v, s);

    cairo_surface_destroy(v.surf);
    XDestroyWindow(v.dpy, v.win);
    XCloseDisplay(v.dpy);
    return 0;
}
