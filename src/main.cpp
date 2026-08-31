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

#include <csignal>
#include <dirent.h>
#include <termios.h>
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
    std::string out;
    char buf[8192];
    while (out.size() < maxBytes) { // chunked so files > one page are read fully
        size_t n = fread(buf, 1, std::min(sizeof(buf), maxBytes - out.size()), f);
        if (n == 0) break;
        out.append(buf, n);
    }
    fclose(f);
    return out;
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

static std::string pciCanon(const std::string& s); // defined below; needed by the nvidia-smi parser

struct GpuConn {
    std::string name;   // drm connector, e.g. "DP-1" / "HDMI-A-2"
    std::string kind;   // human form,      e.g. "DisplayPort", "HDMI"
    bool connected = false;
    std::string mode;   // first listed mode, e.g. "3840x2160@60Hz" (only when connected)
};

struct GpuInfo {
    int  util = -1;         // % or -1
    long vramUsedMiB = -1;  // MiB or -1
    long vramTotalMiB = -1;
    int  tempC = -1;        // °C or -1
    std::string name;       // device name (nvidia-smi / sysfs label) or ""
    std::string pciAddr;    // canonical PCI address, e.g. "0000:65:00.0", or ""
    std::string pciIds;     // vendor:device hex IDs, e.g. "10de:2684", or ""
    std::vector<GpuConn> conns;  // drm display connectors of this GPU
};

static std::vector<GpuInfo> nvidiaGpus() {
    static bool probed = false, ok = false;
    if (!probed) {
        probed = true;
        ok = (system("nvidia-smi -L >/dev/null 2>&1") == 0); // once per run
    }
    std::vector<GpuInfo> out;
    if (!ok) return out;

    const char* cmd = "nvidia-smi --query-gpu=index,name,memory.used,"
                      "memory.total,temperature.gpu,utilization.gpu,pci.bus_id "
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
                case 2: g.name = s;                      break; // display name (may contain spaces)
                case 3: if (num) g.vramUsedMiB = v;      break; // MiB
                case 4: if (num) g.vramTotalMiB = v;     break; // MiB
                case 5: if (num) g.tempC = (int)v;       break; // °C
                case 6: if (num) g.util = (int)v;        break; // utilization.gpu %
                case 7: g.pciAddr = pciCanon(s);         break; // bus id, canonical form
            }
            rest = com ? com + 1 : nullptr;
        }
        if (g.util >= 0 || g.tempC >= 0 || g.vramUsedMiB >= 0) out.push_back(g);
    }
    pclose(p);
    return out;
}

// Canonical PCI address: "00000000:65:00.0" (nvidia-smi style, 8-hex domain)
// and "0000:65:00.0" (sysfs realpath basename) both become "0000:65:00.0",
// so values from the two sources can be compared for equality.
static std::string pciCanon(const std::string& s) {
    if (s.empty()) return "";
    unsigned long dom = 0, bus = 0, devfn = 0; int fn = -1;
    if (sscanf(s.c_str(), "%lx:%lx:%lx.%d", &dom, &bus, &devfn, &fn) < 3) return s;
    char b[24];
    snprintf(b, sizeof(b), "%04lx:%02lx:%02lx.%d", dom, bus, devfn, fn);
    return b;
}

// "0x10de\n" -> "10de" (lowercase hex without prefix); empty stays empty
static std::string hexId(const std::string& raw) {
    std::string t = trim(raw);
    if (t.size() > 2 && (t.compare(0, 2, "0x") == 0 || t.compare(0, 2, "0X") == 0))
        t.erase(0, 2);
    for (auto& c : t) c = tolower((unsigned char)c);
    return t;
}

// connector name prefix -> human kind: DP-1->DisplayPort, HDMI-A-2->HDMI,
// eDP-1/LVDS-1->Internal, VGA-1->VGA, TV-1->TV-out; anything else (DVI-D,...)
// keeps its drm prefix as-is
static std::string connKind(const std::string& nm) {
    size_t d = nm.find('-');
    std::string p = (d == std::string::npos ? nm : nm.substr(0, d));
    for (auto& c : p) c = toupper((unsigned char)c);
    if      (p == "DP")                 return "DisplayPort";
    else if (p == "eDP" || p == "LVDS") return "Internal";
    else if (p == "VGA")                return "VGA";
    else if (!p.empty() && p[0] == 'T' && p.size() >= 2) return "TV-out"; // TV-1 etc.
    return p;
}

// First modeline of <conn>/modes. Old kernels print full modelines
// ("HxV DotMHz h0 h1 HT v0 v1 VT flags..."), newer ones just plain "HxV" lines.
// Returns e.g. "3840x2160@60Hz" when the refresh can be derived, else "HxV",
// or "" when nothing is available (typical for disconnected connectors).
static std::string connFirstMode(const std::string& connPath) {
    std::string raw = readFile(connPath + "/modes", 512);
    size_t eol = raw.find('\n');
    if (eol != std::string::npos) raw.resize(eol); // first line only
    raw = trim(raw);
    int w = 0, h = 0; double mhz = 0.0; long ht = 0, vt = 0;
    // tokens after "HxV": DotMHz hsStart hsEnd HTOTAL vsStart vsEnd VTOTAL
    int n = std::sscanf(raw.c_str(), "%dx%d %lf %*d %*d %ld %*d %*d %ld",
                        &w, &h, &mhz, &ht, &vt);
    if (n < 2 || w <= 0 || h <= 0) return "";
    char b[48];
    if (n >= 5 && ht > 0 && vt > 0 && mhz > 0.0) {
        long hz = (long)(mhz * 1e6 / ((double)ht * (double)vt)); // dot rate / px per frame
        if (hz >= 20 && hz <= 400) { snprintf(b, sizeof b, "%dx%d@%ldHz", w, h, hz); return b; }
    }
    snprintf(b, sizeof b, "%dx%d", w, h);
    return b;
}

// All /sys/class/drm cardN devices with PCI identity and their drm display
// connectors ("card0-DP-1" etc.). Feeds the GPU hover popup on every vendor
// path; writeback/virtual outputs are not real display connections -> skipped.
struct DrmCard {
    int n = -1;
    std::string path;      // /sys/class/drm/cardN
    std::string pciAddr;   // canonical "0000:65:00.0" or ""
    std::string label;     // device/label (amdgpu/intel expose it) or ""
    std::string vendorDev; // PCI IDs, e.g. "10de:2684", or ""
    std::vector<GpuConn> conns;
};

static std::vector<DrmCard> readDrmCards() {
    struct Pending { int n; GpuConn c; }; // connectors seen before their card entry
    std::vector<DrmCard> cards;
    std::vector<Pending> pend;

    if (DIR* d = opendir("/sys/class/drm")) {
        while (struct dirent* e = readdir(d)) {
            const char* nm = e->d_name;
            if (strncmp(nm, "card", 4) != 0 || !isdigit((unsigned char)nm[4])) continue;
            const char* p = nm + 4;
            std::string digits;
            while (*p && isdigit((unsigned char)*p)) { digits += *p; ++p; }
            if (digits.empty()) continue;
            int cn = atoi(digits.c_str());

            if (!*p) { // plain "cardN" -> the GPU device itself
                cards.push_back({cn, std::string("/sys/class/drm/") + nm});
            } else if (*p == '-') { // "card0-DP-1" -> a connector of card 0
                std::string cname = p + 1;
                std::string up = cname; for (auto& ch : up) ch = toupper((unsigned char)ch);
                if (!up.compare(0, 9, "WRITEBACK") || !up.compare(0, 7, "VIRTUAL")) continue;

                GpuConn gc;
                gc.name = cname;
                gc.kind = connKind(cname);
                std::string base = std::string("/sys/class/drm/") + nm;
                gc.connected = (trim(readFile(base + "/status", 64)) == "connected");
                if (gc.connected) gc.mode = connFirstMode(base);
                pend.push_back({cn, std::move(gc)});
            }
        }
        closedir(d);
    }

    for (auto& pc : pend)
        for (auto& card : cards)
            if (card.n == pc.n) { card.conns.push_back(pc.c); break; }

    std::sort(cards.begin(), cards.end(),
              [](const DrmCard& a, const DrmCard& b) { return a.n < b.n; });

    for (auto& c : cards) {
        std::string dev = c.path + "/device";
        char real[PATH_MAX]; // glibc's FORTIFY wrapper requires a PATH_MAX-sized buffer
        if (realpath(dev.c_str(), real)) { // kernel puts relative symlinks here; the
            std::string rp(real);           // basename of the target IS the PCI address
            size_t sl = rp.find_last_of('/');
            c.pciAddr = pciCanon(sl == std::string::npos ? rp : rp.substr(sl + 1));
            std::string vid = hexId(readFile(rp + "/vendor", 64));
            std::string did = hexId(readFile(rp + "/device", 64));
            if (!vid.empty() && !did.empty()) c.vendorDev = vid + ":" + did;
        }
        c.label = trim(readFile(dev + "/label", 256)); // amdgpu/intel; often absent
    }
    for (auto& c : cards)
        std::sort(c.conns.begin(), c.conns.end(),
                  [](const GpuConn& a, const GpuConn& b) { return a.name < b.name; });
    return cards;
}

static void attachMeta(GpuInfo& g, const DrmCard* c) {
    if (!c) return;
    if (g.name.empty() && !c->label.empty())  g.name = c->label;
    if (g.pciAddr.empty())                    g.pciAddr = c->pciAddr;
    if (g.pciIds.empty())                     g.pciIds = c->vendorDev;
    g.conns = c->conns;
}

static std::vector<GpuInfo> sysfsGpus(const std::vector<DrmCard>& cards) {
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
        if (g.util >= 0 || g.vramTotalMiB > 0 || g.tempC >= 0) {
            attachMeta(g, &c); // name / PCI identity / display connectors
            out.push_back(std::move(g));
        }
    }
    return out;
}

static std::vector<GpuInfo> collectGpus() {
    auto cards = readDrmCards(); // PCI metadata + drm connectors of every card
    auto nv = nvidiaGpus();
    if (!nv.empty()) {           // assume all GPUs are NVIDIA when the driver works
        for (auto& g : nv) {
            const DrmCard* m = nullptr;
            for (const auto& c : cards)
                if (!g.pciAddr.empty() && c.pciAddr == g.pciAddr) { m = &c; break; }
            attachMeta(g, m); // display connectors (+ label fallback when smi lacks a name)
        }
        return nv;
    }
    return sysfsGpus(cards);
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

// one indented connector detail line; defined below next to the terminal-table code
// but shared here because --dump prints the same lines as the toggled-on view.
static void connDetail(const GpuConn& cn);

// the CPU/RAM/SWAP summary line (shared by --dump and table mode)
static std::string summaryLine(const Snapshot& s) {
    auto g = [](int v) -> std::string { return v < 0 ? "--" : std::to_string(v); };
    const std::string swap = s.hasSwap ? fmtAlwaysGB(s.swapUsedKb) : "n/a";
    return "CPU " + g(s.cpuPct) + "%   RAM " + fmtSmartKb(s.ramUsedKb) +
           "   SWAP " + swap + "   CPU\xC2\xB0 " + g(s.cpuTempC);
}

static void printDump(const Snapshot& s) {
    auto g = [](int v) -> std::string { return v < 0 ? "--" : std::to_string(v); };
    puts(summaryLine(s).c_str());
    if (s.gpus.empty()) {
        printf("(no GPUs detected)\n");
        return;
    }
    for (size_t i = 0; i < s.gpus.size(); ++i) {
        const auto& gp = s.gpus[i];
        printf("GPU%zu util=%-3s vram=%-6s/%-6s temp=%s°\n",
               i, g(gp.util).c_str(), fmtMiB(gp.vramUsedMiB).c_str(),
               fmtMiB(gp.vramTotalMiB).c_str(), g(gp.tempC).c_str());
        // device / PCI details + display connections (same data as the hover popup)
        if (!gp.name.empty() || !gp.pciAddr.empty() || !gp.pciIds.empty()) {
            std::string m;
            m += gp.name.empty() ? "--" : gp.name;
            if (!gp.pciAddr.empty()) m += "  pci " + gp.pciAddr;
            if (!gp.pciIds.empty())  m += " (" + gp.pciIds + ")";
            printf("      %s\n", m.c_str());
        }
        for (const auto& cn : gp.conns) connDetail(cn);
    }
}

// ---------------------------------------------------------------------------
// terminal view: `--table` mode, and also the automatic fallback when no X
// display is available (e.g. a plain SSH session). One compact line per GPU -
// identity and stats are shown exactly once; the per-connector detail lines
// (same format as --dump) toggle on/off with 'c' in an interactive terminal.
// Column widths are computed from the data itself.
// ---------------------------------------------------------------------------

// display width: everything we print here is made of single-cell characters,
// so each UTF-8 lead byte counts as 1 and continuation bytes as 0 (keeps "65°" aligned)
static size_t dispW(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

struct TCol { const char* title; bool right = false; };

static void printGpuTable(const Snapshot& s, bool showConns) {
    static const TCol kCols[] = {
        {"#", true},   {"DEVICE"},   {"PCI"},       {"UTIL", true},
        {"VRAM", true},{"TEMP", true},{"CONN"},
    };
    constexpr size_t NC = sizeof(kCols) / sizeof(kCols[0]);

    if (s.gpus.empty()) { puts("(no GPUs detected)"); return; }

    std::vector<std::vector<std::string>> rows;
    for (size_t i = 0; i < s.gpus.size(); ++i) {
        const auto& gp = s.gpus[i];
        // PCI identity merged into one field: address [+ vendor:device ids]
        std::string pci = gp.pciAddr.empty() ? "--" : gp.pciAddr;
        if (!gp.pciIds.empty()) pci += (" [" + gp.pciIds + "]");

        // CONN summarizes the connection list as connected/total without
        // printing one line per connector (toggle those with 'c')
        size_t on = 0;
        for (const auto& cn : gp.conns) if (cn.connected) ++on;
        const std::string connSum = gp.conns.empty()
            ? "-" : (std::to_string(on) + "/" + std::to_string(gp.conns.size()));

        rows.push_back({std::to_string(i),
                        gp.name.empty() ? "--" : gp.name,
                        pci,
                        (gp.util < 0) ? "--" : (std::to_string(gp.util) + "%"),
                        fmtMiB(gp.vramUsedMiB) + " / " + fmtMiB(gp.vramTotalMiB),
                        (gp.tempC < 0) ? "--" : (std::to_string(gp.tempC) + "\xC2\xB0"),
                        connSum});
    }

    size_t w[NC];
    for (size_t c = 0; c < NC; ++c) w[c] = std::strlen(kCols[c].title);
    for (const auto& r : rows)
        for (size_t c = 0; c < NC && c < r.size(); ++c)
            w[c] = std::max(w[c], dispW(r[c]));

    const auto line = [&](const std::vector<std::string>& cells) {
        std::string out;
        for (size_t c = 0; c < NC && c < cells.size(); ++c) {
            const size_t dw = dispW(cells[c]);
            if (!out.empty()) out += "  "; // column gap
            if (kCols[c].right) out.append(w[c] - dw, ' '); // right-align the numerics
            out += cells[c];
            if (!kCols[c].right) out.append(w[c] - dw, ' ');
        }
        return out;
    };

    std::vector<std::string> head;
    for (size_t c = 0; c < NC; ++c) head.push_back(kCols[c].title);
    puts(line(head).c_str());

    std::string rule; // separator under the header
    for (size_t c = 0; c < NC; ++c) {
        if (!rule.empty()) rule += "  ";
        rule.append(w[c], '-');
    }
    puts(rule.c_str());

    for (size_t i = 0; i < rows.size(); ++i) {
        puts(line(rows[i]).c_str());
        if (showConns) // detail lines below their GPU, same format as --dump
            for (const auto& cn : s.gpus[i].conns) connDetail(cn);
    }
}

// one indented connector line: name / kind / status (+ mode when connected).
// Shared by --dump and the table view's toggled-on detail lines.
static void connDetail(const GpuConn& cn) {
    const char* st = cn.connected ? "connected" : "off";
    if (!cn.mode.empty())
        printf("      %-10s %-12s %s  %s\n", cn.name.c_str(), cn.kind.c_str(), st, cn.mode.c_str());
    else
        printf("      %-10s %-12s %s\n", cn.name.c_str(), cn.kind.c_str(), st);
}

// one full frame: summary line + table (+ hint footer in an interactive terminal)
static void printTableFrame(const Snapshot& s, bool showConns, bool interactive) {
    if (interactive) fputs("\x1b[H\x1b[2J", stdout); // cursor home + clear screen
    puts(summaryLine(s).c_str());
    printGpuTable(s, showConns);
    if (interactive)
        puts("jtop: terminal mode - c: toggle connection list   q or Ctrl-C: quit");
    fflush(stdout);
}

// poll stdin for a pending key without blocking; returns '\0' when none is
// available. Once stdin hits EOF it flags `closed` so callers don't spin on a
// permanently-readable fd.
static char peekKey(bool& closed) {
    if (closed) return '\0';
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv{0, 0}; // zero timeout = pure poll
    if (select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv) > 0) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 1) return (char)c;
        closed = true; // EOF
    }
    return '\0';
}

// terminal state backup so the tty is restored even when Ctrl-C (SIGINT)
// kills us mid-session instead of our own cleanup path.
static struct termios s_backupTerm{};
static bool s_haveBackup = false;

static void restoreStdin() {
    if (s_haveBackup) tcsetattr(STDIN_FILENO, TCSADRAIN, &s_backupTerm);
}

[[noreturn]] static void onSigint(int sig) { // default action + terminal cleanup
    restoreStdin();
    _exit(128 + sig);
}

// one-shot compact snapshot when stdout is not a tty; in an interactive
// terminal: live re-draw every 5 s (same period as the GUI), waking early on
// keypresses. 'c' toggles the per-GPU connection detail lines, 'q' quits.
static int runTableMode() {
    State s{};
    const bool interactive = (isatty(STDOUT_FILENO) == 1);

    if (!interactive) { // piped/redirected: one compact snapshot and exit
        refresh(s);
        printTableFrame(s.snap, false /* showConns */, false);
        return 0;
    }

    // read keys as soon as they are typed (no Enter needed), without echoing
    // them; remember the original tty state so we can restore it on exit.
    struct termios t{};
    if (tcgetattr(STDIN_FILENO, &t) == 0) {
        s_backupTerm = t;
        s_haveBackup = true;
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSADRAIN, &t);
    }
    signal(SIGINT, onSigint); // keep Ctrl-C working, but restore the tty first

    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::seconds(5); // same period as the GUI
    bool showConns = false;
    bool inClosed = false;

    refresh(s); // first frame immediately (includes a ~300 ms CPU baseline)
    printTableFrame(s.snap, showConns, true);
    auto nextTick = clock::now() + period;

    for (;;) {
        bool toggled = false;
        char k;
        while ((k = peekKey(inClosed)) != '\0') {
            if (k == 'c' || k == 'C')     { showConns = !showConns; toggled = true; }
            else if (k == 'q' || k == 'Q') { restoreStdin(); return 0; }
        }

        const auto now = clock::now();
        if (now >= nextTick) { // data is due for a refresh
            refresh(s);
            while (nextTick <= clock::now()) nextTick += period; // resync if we lagged
        } else if (!toggled) {
            const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                nextTick - now).count();
            struct timeval tv{(long)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
            if (inClosed) { // stdin gone: just wait out the tick
                const struct timespec ts{(long)(ms / 1000), (long)((ms % 1000) * 1000000L)};
                nanosleep(&ts, nullptr);
            } else {
                fd_set rfds; FD_ZERO(&rfds); FD_SET(STDIN_FILENO, &rfds);
                select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, ms > 0 ? &tv : nullptr);
            }
            continue; // a key arrived or the tick hit: re-evaluate at loop top
        }

        printTableFrame(s.snap, showConns, true); // data refreshed and/or list toggled
    }
}

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

struct Col {
    std::string label;  // small grey text on top, e.g. "GPU0"
    std::string value;  // big text below,          e.g. "94%"
    bool hot = false;   // red + underline (as in the reference shot)
};

// x-extent of one column in the last rendered frame; gpu >= 0 marks GPU/VRAM
// columns so pointer motion can be hit-tested against them for hover popups.
struct ColRect {
    double x = 0, w = 0;
    int gpu = -1;
};

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
struct Metrics {
    double labelPx = 19, valuePx = 19.0;
    double padX = 13.0, gap = 22.0, outerPad = 15.0, topPad = 13.0, botPad = 12.5;
    double midGap = 7.0;

    cairo_font_extents_t le{}, ve{}; // font (line) metrics at label / value size

    void prepare(cairo_t* cr) {
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
static std::vector<ColRect> layoutCols(cairo_t* cr, const Metrics& M,
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

struct PopRow {
    std::string text;
    int kind = 0; // 0=title, 1=pci sub-line, 2=connected row, 3=off/dim row
};

static void popupRows(const GpuInfo& g, std::vector<PopRow>& rows) {
    rows.push_back({g.name.empty() ? "(unknown GPU)" : g.name, 0});

    if (!g.pciAddr.empty() || !g.pciIds.empty()) {
        std::string t = "PCI";
        if (!g.pciAddr.empty()) t += " " + g.pciAddr;
        if (!g.pciIds.empty())  t += " (" + g.pciIds + ")";
        rows.push_back({t, 1});
    }

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

struct PopGeom {
    int w = 0, h = 0;
    double padX = 12;
    std::vector<double> base; // text baseline of every row
};

static PopGeom popupGeom(cairo_t* cr, const std::vector<PopRow>& rows) {
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
static void paintPopup(cairo_t* cr, int W, int H, bool argb,
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

// ---------------------------------------------------------------------------

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

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        // no graphics available (e.g. a plain SSH session without forwarding):
        // fall back to the terminal table instead of quitting
        fprintf(stderr, "jtop: cannot open X display (is DISPLAY set?) -> falling back to terminal table\n");
        return runTableMode();
    }
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
