// jtop terminal presentation: the --dump snapshot text and the live table view
// that doubles as the automatic fallback when no X display is available.
#ifndef JTOP_TERMINAL_H_
#define JTOP_TERMINAL_H_

#include "system.h"

// one-shot human/machine readable snapshot (used by --dump)
void printDump(const Snapshot& s);

// interactive tty: live re-draw every 5 s, 'c' toggles the per-GPU connection
// detail lines, 'q'/Ctrl-C quits and restores the terminal. Non-tty stdout:
// prints one compact table and returns immediately. Returns a process exit code.
int runTableMode();

#endif  // JTOP_TERMINAL_H_
