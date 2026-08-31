// jtop GUI module: the X11 window and all cairo painting of the widget row and
// the GPU hover popup. The layout/paint functions are public because the
// headless render test drives them on plain image surfaces; runWidget() is the
// normal application entry point (see main.cpp). Everything that manages X11
// windows/state stays file-local in gui.cc.
#ifndef JTOP_GUI_H_
#define JTOP_GUI_H_

#include <cairo/cairo.h>

#include "system.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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

// layout metrics (single source of truth for sizing + painting)
struct Metrics {
    double labelPx = 19, valuePx = 19.0;
    double padX = 13.0, gap = 22.0, outerPad = 15.0, topPad = 13.0, botPad = 12.5;
    double midGap = 7.0;

    cairo_font_extents_t le{}, ve{}; // font (line) metrics at label / value size

    void prepare(cairo_t* cr); // defined in gui.cc: applies the session UI scale

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

struct PopRow {
    std::string text;
    int kind = 0; // 0=title, 1=pci sub-line, 2=connected row, 3=off/dim row
};

struct PopGeom {
    int w = 0, h = 0;
    double padX = 12;
    std::vector<double> base; // text baseline of every row
};

// snapshot -> widget columns (CPU/RAM/SWAP/CPU degrees block + two per GPU)
std::vector<Col> buildColumns(const Snapshot& s);

// x-extent of every column at the current metrics (hover hit areas)
std::vector<ColRect> layoutCols(cairo_t* cr, const Metrics& M, const std::vector<Col>& cols);

// paint one full widget frame onto `cr` (X window or a plain image surface)
void paintSnapshot(cairo_t* cr, int W, int H, bool argb, const std::vector<Col>& cols);

// GPU hover popup: content rows + measured geometry, then the painting itself
void    popupRows(const GpuInfo& g, std::vector<PopRow>& rows);
PopGeom popupGeom(cairo_t* cr, const std::vector<PopRow>& rows);
void    paintPopup(cairo_t* cr, int W, int H, bool argb, const std::vector<PopRow>& rows);

// entry point: opens the display, builds the windows and runs until closed.
// Returns 0 on normal exit; -1 when no X display could be opened at all, so
// the caller can fall back to terminal mode.
int runWidget();

#endif  // JTOP_GUI_H_
