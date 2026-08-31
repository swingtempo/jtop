// jtop system layer: snapshot types and everything read from /proc, sysfs and
// nvidia-smi/rocm-smi. Pure data acquisition - no X11, no cairo, no output.
#ifndef JTOP_SYSTEM_H_
#define JTOP_SYSTEM_H_

#include <string>
#include <vector>

struct CpuTimes {
    unsigned long v[7] = {0}; // user nice system idle iowait irq softirq steal
};
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
    int pcieGen = -1;      // current PCIe link generation (1..6), or -1 if unknown
    int pcieMaxGen = -1;   // max supported generation, or -1 if unknown
    int pcieWidth = -1;    // lanes in use, e.g. 16, or -1 if unknown
};
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

// small file/text utilities shared across modules
std::string readFile(const std::string& path, size_t maxBytes = 65536); // "" on failure
std::string trim(const std::string& s);                                 // strip surrounding whitespace

// human-readable memory sizes (shared by the GUI and the terminal views)
std::string fmtSmartKb(long kb);   // best-fit unit, e.g. "7.2G" / "512M"
std::string fmtAlwaysGB(long kb);  // GB with one decimal, e.g. "1.4G"
std::string fmtMiB(long mib);      // MiB values coming from the GPU tools

// compact PCIe link description: "Gen4 x16"; when running below the max
// supported generation: "Gen3/4 x16". Empty string when unknown.
std::string pcieLabel(const GpuInfo& g);

// take a full snapshot (the first call builds its own ~300 ms CPU baseline)
void refresh(State& s);

#endif  // JTOP_SYSTEM_H_
