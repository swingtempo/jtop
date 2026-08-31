// jtop system layer: /proc, sysfs and nvidia-smi/rocm-smi reads feeding
// Snapshot. Public surface in system.h; everything else below is file-local
// unless declared there (readFile / trim / fmt* / refresh).
#include "system.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// small filesystem helpers
// ---------------------------------------------------------------------------

std::string readFile(const std::string& path, size_t maxBytes) {
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

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// /proc/stat -> aggregate CPU usage %
// ---------------------------------------------------------------------------


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
std::string fmtSmartKb(long kb) {
    char b[32];
    double g = kb / 1048576.0;
    if (g >= 1.0) { snprintf(b, sizeof(b), "%.1fG", g); return b; }
    long mib = kb / 1024;
    snprintf(b, sizeof(b), "%ldM", mib);
    return b;
}

// always GiB with one decimal: "0.0G" (matches the reference look for SWAP)
std::string fmtAlwaysGB(long kb) {
    char b[32];
    snprintf(b, sizeof(b), "%.1fG", kb / 1048576.0);
    return b;
}

std::string fmtMiB(long mib) {
    if (mib < 0) return "--";
    char b[32];
    double g = mib / 1024.0;
    if (g >= 1.0) { snprintf(b, sizeof(b), "%.1fG", g); return b; }
    snprintf(b, sizeof(b), "%ldM", mib);
    return b;
}

// compact PCIe link description: "Gen4 x16"; running below the max supported
// generation is shown as "Gen3/4 x16". Empty when unknown.
std::string pcieLabel(const GpuInfo& g) {
    if (g.pcieGen < 0) return "";
    std::string s = "Gen" + std::to_string(g.pcieGen);
    if (g.pcieMaxGen > g.pcieGen) s += "/" + std::to_string(g.pcieMaxGen); // downgraded link
    if (g.pcieWidth > 0)          s += " x" + std::to_string(g.pcieWidth);
    return s;
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
    int pcieGen = -1, pcieMaxGen = -1, pcieWidth = -1; // PCIe link state, or -1
};

// per-lane speed -> PCIe generation from the spec table: Gen1..Gen6 =
// 2.5 / 5 / 8 / 16 / 32 / 64 GT/s (half a step of tolerance covers rounding)
static int gtToGen(double gts) {
    static const double kGt[6] = {2.5, 5.0, 8.0, 16.0, 32.0, 64.0};
    for (int gen = 0; gen < 6; ++gen)
        if (std::abs(gts - kGt[gen]) < 0.5) return gen + 1;
    return -1;
}

// PCIe link state of one PCI device, from the kernel's own reporting:
//   current_link_speed: "32.0 GT/s PCIe", sometimes with the max supported
//                       value appended like "8.0 GT/s cap 16.0 GT/s"
//   current_link_width: a plain lane count ("16")
// Non-PCIe devices don't expose these files at all -> fields stay -1.
static void readPcieLink(const std::string& devPath, DrmCard& c) {
    const std::string spd = trim(readFile(devPath + "/current_link_speed", 64));
    if (!spd.empty() && spd.find("GT/s") != std::string::npos) {
        c.pcieGen = gtToGen(strtod(spd.c_str(), nullptr));
        size_t capPos = spd.find("cap ");
        if (capPos != std::string::npos) { // "... GT/s cap 16.0 GT/s"
            const char* p2 = spd.c_str() + capPos + 4;
            while (*p2 && !isdigit((unsigned char)*p2)) ++p2; // skip to the number
            c.pcieMaxGen = gtToGen(strtod(p2, nullptr));
        }
    }
    long w = readLong(devPath + "/current_link_width");
    if (w >= 1 && w <= 64) c.pcieWidth = (int)w;
}

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
            readPcieLink(rp, c);
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
    // PCIe link state only comes from sysfs (works for every vendor)
    if (g.pcieGen < 0)      g.pcieGen    = c->pcieGen;
    if (g.pcieMaxGen < 0)   g.pcieMaxGen = c->pcieMaxGen;
    if (g.pcieWidth < 0)    g.pcieWidth  = c->pcieWidth;
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



void refresh(State& s) {
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
