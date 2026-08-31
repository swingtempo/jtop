// jtop terminal presentation (see terminal.h). All output goes to stdout;
// nothing here touches X11 or cairo, so it also runs in plain SSH sessions.
#include "terminal.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

// one indented connector line: name / kind / status (+ mode when connected).
// Shared by --dump and the table view's toggled-on detail lines.
static void connDetail(const GpuConn& cn) {
    const char* st = cn.connected ? "connected" : "off";
    if (!cn.mode.empty())
        printf("      %-10s %-12s %s  %s\n", cn.name.c_str(), cn.kind.c_str(), st, cn.mode.c_str());
    else
        printf("      %-10s %-12s %s\n", cn.name.c_str(), cn.kind.c_str(), st);
}



// the CPU/RAM/SWAP summary line (shared by --dump and table mode)
static std::string summaryLine(const Snapshot& s) {
    auto g = [](int v) -> std::string { return v < 0 ? "--" : std::to_string(v); };
    const std::string swap = s.hasSwap ? fmtAlwaysGB(s.swapUsedKb) : "n/a";
    return "CPU " + g(s.cpuPct) + "%   RAM " + fmtSmartKb(s.ramUsedKb) +
           "   SWAP " + swap + "   CPU\xC2\xB0 " + g(s.cpuTempC);
}

void printDump(const Snapshot& s) {
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
int runTableMode() {
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
