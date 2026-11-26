/*
 * GuitarMidi-LV2 - X11/Cairo UI
 *
        const double row_bottom = chart_y + (FILTER_COUNT - filter) * cell_h;
 * Uses raw Xlib + Cairo, implements the LV2UI X11 interface with
 * the ui:idleInterface extension (no toolkit event loop needed).
 *
            draw_text(cr, chart_x - 7, row_bottom - cell_h * 0.5 + 3,
                      label, 8, COL_TEXT_DIM, false, true);
 */

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>
#include <lv2/ui/ui.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#define GUITARMIDI_URI "http://github.com/geraldmwangi/GuitarMidi-LV2"
#define GUITARMIDI_UI_URI GUITARMIDI_URI "#ui"

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
static const int UI_W = 1000;
static const int UI_H = 460;
static const int DEBUG_W = 800;
static const int DEBUG_H = 560;
static const size_t NOTE_COUNT = 37;
static const size_t FILTER_COUNT = 148;
static const size_t HARMONICS_PER_NOTE = FILTER_COUNT / NOTE_COUNT;
static const int MIDI_NOTE_OFFSET = 40;
static const size_t FILTER_WAVEFORM_SAMPLES = 64;
static const size_t NOTE_PLOT_COUNT = 4;
static const size_t PLOT_COUNT = NOTE_PLOT_COUNT + 1;
static const size_t TOOLBAR_BUTTON_COUNT = PLOT_COUNT + 1;
static const size_t HISTORY_SIZE = 960;
static const size_t DEFAULT_VISIBLE_COLUMNS = 240;
static const size_t MIN_VISIBLE_COLUMNS = 30;
static const size_t MIN_FILTER_VISIBLE_SAMPLES = 16;
static const double DEBUG_UPDATES_PER_SECOND = 20.0;
static const size_t FILTER_SAMPLE_RATE = 1280;
static const size_t FILTER_HISTORY_SIZE = FILTER_SAMPLE_RATE * 48;
static const size_t DEFAULT_FILTER_VISIBLE_SAMPLES = FILTER_SAMPLE_RATE * 12;
static const double PLOT_BUTTON_WIDTH = 96.0;
static const double PLOT_BUTTON_GAP = 6.0;
static const uint32_t SPLIT_NOTE_PORT = 15;

// Input level meter (bottom of the INPUT panel). Fed by the two floats the
// debug sender appends to its packet, so it updates at the packet rate.
static const size_t METER_SEGMENTS = 12;
static const double METER_SEG_WIDTH = 6.0;
static const double METER_SEG_GAP = 2.0;
static const double METER_HEIGHT = 9.0;
static const double METER_MIN_DB = -60.0;
static const double METER_PEAK_DECAY_DB = 1.5;   // per debug packet
static const float  METER_CLIP_DB = -0.5f;       // peak that trips the latch
static const int    METER_CLIP_LATCH_PACKETS = 20;

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------
struct Color {
    double r, g, b;
};

static inline void set_color(cairo_t* cr, Color c, double a = 1.0)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
}

static Color note_color(size_t note)
{
    const double hue = std::fmod(note * 0.6180339887498949, 1.0) * 6.0;
    const double saturation = 0.82;
    const double lightness = 0.62;
    const double chroma = (1.0 - std::fabs(2.0 * lightness - 1.0)) * saturation;
    const double secondary = chroma * (1.0 - std::fabs(std::fmod(hue, 2.0) - 1.0));
    const double offset = lightness - chroma * 0.5;
    if (hue < 1.0) return {chroma + offset, secondary + offset, offset};
    if (hue < 2.0) return {secondary + offset, chroma + offset, offset};
    if (hue < 3.0) return {offset, chroma + offset, secondary + offset};
    if (hue < 4.0) return {offset, secondary + offset, chroma + offset};
    if (hue < 5.0) return {secondary + offset, offset, chroma + offset};
    return {chroma + offset, offset, secondary + offset};
}

// Palette (modern dark)
static const Color COL_BG_TOP     = {0.086, 0.094, 0.118}; // #161e1e
static const Color COL_BG_BOT     = {0.055, 0.059, 0.075};
static const Color COL_PANEL      = {0.118, 0.129, 0.161};
static const Color COL_PANEL_EDGE = {0.20, 0.22, 0.27};
static const Color COL_TEXT       = {0.90, 0.91, 0.93};
static const Color COL_TEXT_DIM   = {0.55, 0.58, 0.64};
static const Color COL_ACCENT     = {0.00, 0.78, 0.60};  // teal/green
static const Color COL_ACCENT2    = {1.00, 0.62, 0.20};  // orange (offset group)
static const Color COL_ACCENT3    = {0.35, 0.62, 1.00};  // blue (input group)
static const Color COL_TRACK      = {0.22, 0.24, 0.29};
static const Color COL_CLIP       = {0.95, 0.27, 0.25};  // red (meter clip)
static const Color COL_KNOB_FACE  = {0.16, 0.175, 0.215};
static const Color COL_KNOB_EDGE  = {0.30, 0.33, 0.39};

// ---------------------------------------------------------------------------
// Knob widget model
// ---------------------------------------------------------------------------
struct Knob {
    uint32_t    port;       // LV2 port index
    const char* label;
    const char* unit;       // may be ""

    const char* tooltip; //the description of the parameter
    float       min, max, def;
    int         decimals;   // display precision
    Color       accent;
    // geometry (computed in layout)
    double cx, cy, radius;
    // state
    float value;
    // optional enumeration labels indexed from min; nullptr = numeric display
    const char* const* names = nullptr;
    // toggles are drawn as buttons and flip on click instead of dragging
    bool toggle = false;
};

// A rectangle in UI coordinates; x1/y1 are exclusive. Used to accumulate the
// region that has to be repainted in the next idle frame.
struct Rect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    void clear() { x0 = y0 = x1 = y1 = 0.0; }

    void add(double x, double y, double w, double h)
    {
        if (w <= 0.0 || h <= 0.0)
            return;
        if (empty()) {
            x0 = x;
            y0 = y;
            x1 = x + w;
            y1 = y + h;
            return;
        }
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x + w);
        y1 = std::max(y1, y + h);
    }
};

struct DebugPlot {
    Window window = 0;
    cairo_surface_t* surface = nullptr;
    cairo_surface_t* backbuffer = nullptr;
    int width = DEBUG_W;
    int height = DEBUG_H;
    bool visible = false;
    bool dirty = false;
    size_t next_column = 0;
    size_t columns_filled = 0;
    size_t visible_columns = DEFAULT_VISIBLE_COLUMNS;
    size_t row_count = NOTE_COUNT;
    std::array<std::array<float, FILTER_COUNT>, HISTORY_SIZE> history = {};
    std::vector<std::array<float, FILTER_COUNT>> filter_history;
    std::vector<float> filter_column_peaks;
};

struct GuitarMidiUI {
    // LV2 plumbing
    LV2UI_Write_Function write;
    LV2UI_Controller     controller;
    uint32_t debug_port;
    LV2_URID event_transfer;
    LV2_URID vector_type;
    LV2_URID float_type;
    Atom wm_delete;

    // X11
    Display* dpy;
    Window   parent;
    Window   win;
    int      screen;
    bool     mapped;

    // Cairo
    cairo_surface_t* surface;      // the X window, only ever painted from the backbuffer
    cairo_surface_t* backbuffer;   // persistent image the whole UI is drawn into
    cairo_font_options_t* font_options; // the window surface's text rendering options
    int      win_w;
    int      win_h;

    // Widgets
    std::vector<Knob> knobs;
    DebugPlot plots[PLOT_COUNT];

    // Interaction state
    int    drag_knob;     // index into knobs, -1 = none
    double drag_start_y;
    float  drag_start_val;
    bool   fine_drag;
    int    hover_knob;

    // Input level meter state (updated by the debug packet handler)
    float  meter_rms_db = static_cast<float>(METER_MIN_DB);
    float  meter_peak_db = static_cast<float>(METER_MIN_DB);
    int    meter_clip_latch = 0;
    bool   meter_has_data = false;
    // Bounds the meter last painted (set by draw_input_meter, updated by
    // layout_knobs); a debug packet dirties only this strip.
    Rect   meter_bounds;

    bool dirty;           // something has to be drawn
    Rect dirty_rect;      // what, in UI coordinates
    std::string save_status;
};

// ---------------------------------------------------------------------------
// Knob definitions (ports 2..9 first row, 12..18/21..23 second row)
// ---------------------------------------------------------------------------
static const char* const SCALE_NAMES[] = {
    "Off", "Major", "Natural Minor", "Harmonic Minor", "Melodic Minor",
    "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian",
    "Major Pentatonic", "Minor Pentatonic", "Blues", "Chromatic",
    "Whole Tone", "Diminished", "Hungarian Minor", "Phrygian Dominant"
};
static const char* const PITCH_CLASS_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
static const char* const ON_OFF_NAMES[] = {"Off", "On"};

static void init_knobs(GuitarMidiUI* ui)
{
    ui->knobs = {
        // port, label, unit,tooltip, min, max, default, decimals, accent
        {2, "Input Gain",   "dB","Add gain to the input signal prior to processing", -20.0f, 40.0f,  28.0f,  1, COL_ACCENT3, 0, 0, 0, 28.0f},
        {3, "Expressivity", "dB", "The range of the attack velocity . For low values any attack strength of the string produces max velocity, passed to the synth", -3.0f, 20.0f,  3.0f,  1, COL_ACCENT3, 0, 0, 0, 3.0f},
        // onset knobs
        {4, "Smoothing",    "",   "Timewise smoothing of the note begin to avoid jitter. High values produce smoother predictions at the expense of latency",   0.0f,  0.9f,  0.3f,  2, COL_ACCENT,  0, 0, 0, 0.3f},
        {6, "Confidence",   "",   "The confidence level of the note beginning. Use to filter out false notes. High values lead to more conservative note detection",   0.0f,  0.99f, 0.95f, 2, COL_ACCENT,  0, 0, 0, 0.95f},
        {8, "Energy",       "dB", "The energy threshold for note detection. Lower values make the detector more sensitive to quieter notes", -20.0f,  3.0f,  -18.0f,  1, COL_ACCENT,  0, 0, 0, -18.0f},

        // offset knobs
        {5, "Smoothing",    "",   "Timewise smoothing of the note ends to avoid jitter. Lower values lead to notes being released more quickly",   0.0f,  0.9f,  0.3f,  2, COL_ACCENT2, 0, 0, 0, 0.3f},
        {7, "Confidence",   "",   "The confidence level of the note ending. Lower values make the notes linger on",   0.0f,  0.99f, 0.3f, 2, COL_ACCENT2, 0, 0, 0, 0.3f},
        {9, "Energy",       "dB", "The energy threshold for note detection. Lower values make the notes linger on, especially when chords change fast", -20.0f,  3.0f, -18.0f, 1, COL_ACCENT2, 0, 0, 0, -18.0f},

        // note filter knobs
        {12, "Scale",      "",   "Keep the sent notes inside this scale. Off passes every detected note", 0.0f, 17.0f, 0.0f, 0, COL_ACCENT, 0, 0, 0, 0.0f, SCALE_NAMES},
        {13, "Scale Root", "",   "Root note of the selected scale", 0.0f, 11.0f, 0.0f, 0, COL_ACCENT, 0, 0, 0, 0.0f, PITCH_CLASS_NAMES},
        {14, "Transpose",  "st", "Shift every sent note by this many semitones", -24.0f, 24.0f, 0.0f, 0, COL_ACCENT, 0, 0, 0, 0.0f},

        // routing knobs
        {16, "Low Ch",     "ch", "MIDI channel for notes below the split note", 1.0f, 16.0f, 1.0f, 0, COL_ACCENT2, 0, 0, 0, 1.0f},
        {15, "Split Note", "",   "First note of the high side. Notes below it use the low channel; 0 turns splitting off", 0.0f, 127.0f, 0.0f, 0, COL_ACCENT2, 0, 0, 0, 0.0f},
        {17, "High Ch",    "ch", "MIDI channel for notes at or above the split note", 1.0f, 16.0f, 1.0f, 0, COL_ACCENT2, 0, 0, 0, 1.0f},
        {21, "MPE Output", "",   "Give every sounding note its own MPE member channel with pitch bend", 0.0f, 1.0f, 0.0f, 0, COL_ACCENT2, 0, 0, 0, 0.0f, ON_OFF_NAMES, true},
        {23, "One Per String", "", "Allow only one note per guitar string and drop harmonic ghosts that one string cannot play", 0.0f, 1.0f, 0.0f, 0, COL_ACCENT2, 0, 0, 0, 0.0f, ON_OFF_NAMES, true},

        // tracking knobs
        {18, "Tuning",     "st", "Guitar tuning offset: notes are reported this many semitones lower for a detuned guitar", -12.0f, 12.0f, 0.0f, 0, COL_ACCENT3, 0, 0, 0, 0.0f},
        {22, "Bend Range", "st", "Pitch bend range in semitones used for tracked bends; 0 disables bend output", 0.0f, 24.0f, 2.0f, 0, COL_ACCENT3, 0, 0, 0, 2.0f},
    };
    for (auto& k : ui->knobs)
        k.value = k.def;
}



// Panel geometry ------------------------------------------------------------
struct Panel {
    const char* title;
    Color       accent;
    double x, y, w, h;
    int first_knob, num_knobs;
};

// One row entry: title, accent, relative width and the knobs it owns.
struct PanelSpec {
    const char* title;
    Color       accent;
    double      weight;
    int         first_knob, num_knobs;
};

static const double HEADER_H = 64;
static const double PANEL_MARGIN = 16;
static const double PANEL_GAP = 12;
static const double CONTENT_TOP = HEADER_H + 12;
static const double ROW_GAP = 24;            // the divider sits in the middle
static const double FOOTER_SPACE = 26;       // room for the hint line
static const double KNOB_CENTER_OFFSET = 98; // below the panel top
static const double KNOB_RADIUS = 30;
static const double TOGGLE_W = 64;
static const double TOGGLE_H = 28;

// INPUT: 2 knobs; ONSET: 3 knobs; OFFSET: 3 knobs (unchanged widths)
static const PanelSpec ROW_TOP[3] = {
    {"INPUT",       COL_ACCENT3, 2, 0, 2},
    {"NOTE ONSET",  COL_ACCENT,  3, 2, 3},
    {"NOTE OFFSET", COL_ACCENT2, 3, 5, 3},
};

// FILTER: 3 knobs; ROUTING: 5 knobs; TRACKING: 2 knobs (equal cell widths)
static const PanelSpec ROW_BOTTOM[3] = {
    {"NOTE FILTER", COL_ACCENT,  3,  8, 3},
    {"ROUTING",     COL_ACCENT2, 5, 11, 5},
    {"TRACKING",    COL_ACCENT3, 2, 16, 2},
};

static double panel_row_height()
{
    return (UI_H - FOOTER_SPACE - CONTENT_TOP - ROW_GAP) * 0.5;
}

static double divider_y()
{
    return CONTENT_TOP + panel_row_height() + ROW_GAP * 0.5;
}

static void layout_panel_row(const PanelSpec specs[3], double top, double height,
                             Panel out[3])
{
    double total_weight = 0;
    for (int i = 0; i < 3; ++i)
        total_weight += specs[i].weight;

    const double total_w = UI_W - 2 * PANEL_MARGIN - 2 * PANEL_GAP;
    double x = PANEL_MARGIN;
    for (int i = 0; i < 3; ++i) {
        const double w = total_w * specs[i].weight / total_weight;
        out[i] = {specs[i].title, specs[i].accent, x, top, w, height,
                  specs[i].first_knob, specs[i].num_knobs};
        x += w + PANEL_GAP;
    }
}

static void layout_panels(Panel rows[2][3])
{
    const double h = panel_row_height();
    layout_panel_row(ROW_TOP, CONTENT_TOP, h, rows[0]);
    layout_panel_row(ROW_BOTTOM, CONTENT_TOP + h + ROW_GAP, h, rows[1]);
}

// Everything the input meter paints, in UI coordinates: caption and readout on
// the segment baseline, the segment track and the peak tick's 2 px overhang.
// draw_input_meter fills this in; layout_knobs seeds it too, so a debug packet
// that arrives before the first frame still dirties the right strip.
static Rect meter_bounds_for(const Panel& pn)
{
    const double inner_x = pn.x + 14.0;
    const double inner_right = pn.x + pn.w - 14.0;
    const double meter_y = pn.y + pn.h - 6.0 - METER_HEIGHT;
    return {inner_x - 2.0, meter_y - 4.0, inner_right + 2.0,
            meter_y + METER_HEIGHT + 4.0};
}

// Dirty-region bookkeeping. A debug packet only touches the meter strip; the
// rare events (expose, hover, drag, toggle, status text) repaint everything.
static void mark_dirty_all(GuitarMidiUI* ui)
{
    ui->dirty = true;
    ui->dirty_rect.clear();
    ui->dirty_rect.add(0.0, 0.0, ui->win_w, ui->win_h);
}

static void mark_meter_dirty(GuitarMidiUI* ui)
{
    ui->dirty = true;
    const Rect& r = ui->meter_bounds;
    ui->dirty_rect.add(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}

static void layout_knobs(GuitarMidiUI* ui)
{
    Panel rows[2][3];
    layout_panels(rows);

    for (int r = 0; r < 2; ++r) {
        for (int p = 0; p < 3; ++p) {
            const Panel& pn = rows[r][p];
            const double cell_w = pn.w / pn.num_knobs;
            for (int i = 0; i < pn.num_knobs; ++i) {
                Knob& k = ui->knobs[pn.first_knob + i];
                k.cx = pn.x + cell_w * (i + 0.5);
                k.cy = pn.y + KNOB_CENTER_OFFSET;
                k.radius = KNOB_RADIUS;
            }
        }
    }

    // The meter lives at the bottom of the INPUT panel (first top-row panel).
    ui->meter_bounds = meter_bounds_for(rows[0][0]);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}


// Draw the tooptip for a knob
static void draw_tooltip(cairo_t* cr, const Knob& k){
    cairo_select_font_face(cr,"sans-serif",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);

    cairo_set_font_size(cr,11);
    cairo_text_extents_t ext;
    cairo_text_extents(cr,k.tooltip,&ext);

    const double padding_x=10;
    const double padding_y=7;

    const double width=ext.width+padding_x*2;
    const double height=24;

    double x=k.cx-width/2;
    double y=k.cy-k.radius-height-20;
    // Bottom-row knobs get the tooltip below the value, where it cannot cover
    // the group title.
    if (k.cy > divider_y())
        y = k.cy + k.radius + 26;

    if (x < 6)
        x = 6;
    if (x + width > UI_W - 6)
        x = UI_W - width - 6;
    if (y < 6)
        y = 6;

    set_color(cr,COL_PANEL_EDGE,0.98);
    rounded_rect(cr,x,y,width,height,5);
    cairo_fill(cr);
    
    //show the text
    set_color(cr,COL_TEXT);
    cairo_move_to(cr,x+padding_x,y+padding_y+height/2);
    cairo_show_text(cr,k.tooltip);

}
static void draw_text(cairo_t* cr, double x, double y, const char* text,
                      double size, Color col, bool bold, bool center)
{
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    set_color(cr, col);
    if (center) {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, text, &ext);
        x -= ext.width / 2 + ext.x_bearing;
    }
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
}

static void format_value(const Knob& k, char* buf, size_t n)
{
    // Enumeration knobs display the name of the closest step.
    const int index = static_cast<int>(std::lround(k.value)) -
                      static_cast<int>(std::lround(k.min));
    if (k.names && index >= 0 && static_cast<float>(index) <= k.max - k.min) {
        snprintf(buf, n, "%s", k.names[index]);
        return;
    }
    // Split Note uses 0 as its "disabled" value rather than a note number.
    if (k.port == SPLIT_NOTE_PORT && std::lround(k.value) == 0) {
        snprintf(buf, n, "Off");
        return;
    }
    if (k.unit[0])
        snprintf(buf, n, "%.*f %s", k.decimals, k.value, k.unit);
    else
        snprintf(buf, n, "%.*f", k.decimals, k.value);
}

// Draw horizontally centred text, shrinking the font if needed to fit max_w.
static void draw_text_fitted(cairo_t* cr, double x, double y, const char* text,
                             double size, double min_size, double max_w,
                             Color col, bool bold)
{
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    if (ext.width > max_w && ext.width > 0.0) {
        cairo_set_font_size(cr, std::max(min_size, size * max_w / ext.width));
        cairo_text_extents(cr, text, &ext);
    }
    set_color(cr, col);
    cairo_move_to(cr, x - ext.width * 0.5 - ext.x_bearing, y);
    cairo_show_text(cr, text);
}

// Draw text right-aligned at x.
static void draw_text_right(cairo_t* cr, double x, double y, const char* text,
                            double size, Color col, bool bold)
{
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    set_color(cr, col);
    cairo_move_to(cr, x - ext.width - ext.x_bearing, y);
    cairo_show_text(cr, text);
}

// Apply one debug packet to the meter: new RMS level, decaying peak hold and
// the clip latch (a peak at or above -0.5 dBFS holds "CLIP" for ~1 s).
static void update_input_meter(GuitarMidiUI* ui, float rms_db, float peak_db)
{
    if (!std::isfinite(rms_db))
        rms_db = static_cast<float>(METER_MIN_DB);
    if (!std::isfinite(peak_db))
        peak_db = static_cast<float>(METER_MIN_DB);

    ui->meter_rms_db = rms_db;
    ui->meter_peak_db = std::max(peak_db,
        ui->meter_peak_db - static_cast<float>(METER_PEAK_DECAY_DB));
    if (ui->meter_peak_db < METER_MIN_DB)
        ui->meter_peak_db = static_cast<float>(METER_MIN_DB);

    if (peak_db >= METER_CLIP_DB)
        ui->meter_clip_latch = METER_CLIP_LATCH_PACKETS;
    else if (ui->meter_clip_latch > 0)
        --ui->meter_clip_latch;

    ui->meter_has_data = true;
    // Only the meter strip changed: peak-hold decay and the clip latch stay
    // inside it, so packets must not force a full-window repaint.
    mark_meter_dirty(ui);
}

// Zone colour of one segment: dim blue, green, amber, red (clip).
static void meter_segment_source(cairo_t* cr, size_t segment, bool lit)
{
    if (!lit) {
        set_color(cr, COL_TRACK, 0.55);
        return;
    }
    if (segment < 4)
        set_color(cr, COL_ACCENT3, 0.55);   // dim blue: quiet input
    else if (segment < 9)
        set_color(cr, COL_ACCENT);
    else if (segment < 11)
        set_color(cr, COL_ACCENT2);
    else
        set_color(cr, COL_CLIP);
}

static void draw_input_meter(cairo_t* cr, const Panel& pn, GuitarMidiUI& ui)
{
    // Record the strip this paints so the event loop can repaint just it.
    ui.meter_bounds = meter_bounds_for(pn);

    const double inner_x = pn.x + 14.0;
    const double inner_right = pn.x + pn.w - 14.0;
    const double meter_y = pn.y + pn.h - 6.0 - METER_HEIGHT;
    const double baseline = meter_y + METER_HEIGHT - 1.0;
    const double db_span = -METER_MIN_DB;   // 60 dB from left edge to 0 dBFS

    // Fixed caption/readout reservations: the segment track must not move as
    // the dB digits (or "CLIP") change width from packet to packet.
    static const double CAPTION_W = 34.0;
    static const double READOUT_W = 50.0;
    const double track_x = inner_x + CAPTION_W;
    const double track_area = (inner_right - READOUT_W) - track_x;
    // Fill the available width, but never go below the compact segment width.
    const double seg_w = std::max(METER_SEG_WIDTH,
        (track_area - (METER_SEGMENTS - 1) * METER_SEG_GAP) / METER_SEGMENTS);
    const double pitch = seg_w + METER_SEG_GAP;
    const double track_w = METER_SEGMENTS * seg_w + (METER_SEGMENTS - 1) * METER_SEG_GAP;

    const bool clipping = ui.meter_clip_latch > 0;
    char readout[16];
    if (clipping)
        snprintf(readout, sizeof(readout), "CLIP");
    else {
        const double clamped = std::max(METER_MIN_DB,
            std::min(0.0, static_cast<double>(ui.meter_rms_db)));
        snprintf(readout, sizeof(readout), "%d dBFS", static_cast<int>(std::lround(clamped)));
    }

    // Caption left, readout right, the segment track filling the middle.
    draw_text(cr, inner_x, baseline, "INPUT", 9, COL_TEXT_DIM, true, false);
    draw_text_right(cr, inner_right, baseline, readout, 9,
                    clipping ? COL_CLIP : COL_TEXT_DIM, clipping);

    // Dark track behind the segments.
    set_color(cr, COL_BG_BOT, 0.9);
    rounded_rect(cr, track_x - 2.0, meter_y - 1.0, track_w + 4.0,
                 METER_HEIGHT + 2.0, 2.0);
    cairo_fill(cr);

    const double level = std::max(METER_MIN_DB,
        std::min(0.0, static_cast<double>(ui.meter_rms_db)));
    int lit = static_cast<int>(std::floor((level - METER_MIN_DB) / (db_span / METER_SEGMENTS))) + 1;
    if (level <= METER_MIN_DB || !ui.meter_has_data)
        lit = 0;
    lit = std::min<int>(lit, static_cast<int>(METER_SEGMENTS));

    for (size_t segment = 0; segment < METER_SEGMENTS; ++segment) {
        const bool on = static_cast<int>(segment) < lit;
        meter_segment_source(cr, segment, on);
        rounded_rect(cr, track_x + segment * pitch, meter_y,
                     seg_w, METER_HEIGHT, 1.0);
        cairo_fill(cr);
    }

    // Peak-hold tick, 2 px wide, spanning slightly past the segments.
    if (ui.meter_has_data && ui.meter_peak_db > METER_MIN_DB) {
        const double peak_norm =
            (std::min(0.0, static_cast<double>(ui.meter_peak_db)) - METER_MIN_DB) / db_span;
        const double peak_x = track_x + peak_norm * track_w;
        set_color(cr, COL_TEXT, 0.85);
        cairo_rectangle(cr, peak_x - 1.0, meter_y - 2.0, 2.0, METER_HEIGHT + 4.0);
        cairo_fill(cr);
    }
}

static void draw_toggle(cairo_t* cr, const Knob& k, bool hovered)
{
    const bool on = k.value > 0.5f;
    const double x = k.cx - TOGGLE_W * 0.5;
    const double y = k.cy - TOGGLE_H * 0.5;

    draw_text(cr, k.cx, k.cy - KNOB_RADIUS - 12, k.label, 12, COL_TEXT, false, true);

    set_color(cr, COL_PANEL);
    rounded_rect(cr, x, y, TOGGLE_W, TOGGLE_H, 4);
    cairo_fill(cr);

    set_color(cr, k.accent, hovered ? 1.0 : 0.75);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, TOGGLE_W - 1, TOGGLE_H - 1, 4);
    cairo_stroke(cr);

    draw_text(cr, k.cx, k.cy + 4, on ? "On" : "Off", 11,
              on ? k.accent : COL_TEXT_DIM, true, true);
}

static void draw_knob(cairo_t* cr, const Knob& k, bool hovered)
{
    cairo_new_path(cr); // avoid connecting lines from previous text/current point
    if (k.toggle) {
        draw_toggle(cr, k, hovered);
        return;
    }

    const double a_start = 0.75 * M_PI;  // 135 deg
    const double a_range = 1.5 * M_PI;   // 270 deg sweep
    const double norm = (k.value - k.min) / (k.max - k.min);
    const double a_val = a_start + norm * a_range;

    // Track arc
    cairo_set_line_width(cr, 5.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    set_color(cr, COL_TRACK);
    cairo_arc(cr, k.cx, k.cy, k.radius, a_start, a_start + a_range);
    cairo_stroke(cr);

    // Value arc (glow underlay + main)
    set_color(cr, k.accent, 0.25);
    cairo_set_line_width(cr, 9.0);
    cairo_arc(cr, k.cx, k.cy, k.radius, a_start, a_val);
    cairo_stroke(cr);

    set_color(cr, k.accent);
    cairo_set_line_width(cr, 5.0);
    cairo_arc(cr, k.cx, k.cy, k.radius, a_start, a_val);
    cairo_stroke(cr);

    // Knob face (radial-ish shading)
    const double fr = k.radius - 9;
    cairo_pattern_t* grad = cairo_pattern_create_linear(
        k.cx, k.cy - fr, k.cx, k.cy + fr);
    cairo_pattern_add_color_stop_rgb(grad, 0,
        COL_KNOB_FACE.r + 0.05, COL_KNOB_FACE.g + 0.05, COL_KNOB_FACE.b + 0.05);
    cairo_pattern_add_color_stop_rgb(grad, 1,
        COL_KNOB_FACE.r - 0.02, COL_KNOB_FACE.g - 0.02, COL_KNOB_FACE.b - 0.02);
    cairo_set_source(cr, grad);
    cairo_arc(cr, k.cx, k.cy, fr, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    set_color(cr, hovered ? k.accent : COL_KNOB_EDGE, hovered ? 0.8 : 1.0);
    cairo_set_line_width(cr, 1.2);
    cairo_arc(cr, k.cx, k.cy, fr, 0, 2 * M_PI);
    cairo_stroke(cr);

    // Pointer
    const double px1 = k.cx + cos(a_val) * (fr * 0.35);
    const double py1 = k.cy + sin(a_val) * (fr * 0.35);
    const double px2 = k.cx + cos(a_val) * (fr * 0.85);
    const double py2 = k.cy + sin(a_val) * (fr * 0.85);
    set_color(cr, k.accent);
    cairo_set_line_width(cr, 3.0);
    cairo_move_to(cr, px1, py1);
    cairo_line_to(cr, px2, py2);
    cairo_stroke(cr);

    // Label above, value below
    draw_text(cr, k.cx, k.cy - k.radius - 12, k.label, 12, COL_TEXT, false, true);

    char buf[48];
    format_value(k, buf, sizeof(buf));
    draw_text_fitted(cr, k.cx, k.cy + k.radius + 20, buf, 12, 9, 84,
                     hovered ? COL_TEXT : COL_TEXT_DIM, hovered);
}

static void draw_panel(cairo_t* cr, const Panel& pn)
{
    set_color(cr, COL_PANEL);
    rounded_rect(cr, pn.x, pn.y, pn.w, pn.h, 10);
    cairo_fill(cr);

    set_color(cr, COL_PANEL_EDGE, 0.6);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, pn.x + 0.5, pn.y + 0.5, pn.w - 1, pn.h - 1, 10);
    cairo_stroke(cr);

    // Panel title with accent tick
    set_color(cr, pn.accent);
    rounded_rect(cr, pn.x + 14, pn.y + 14, 4, 14, 2);
    cairo_fill(cr);
    draw_text(cr, pn.x + 26, pn.y + 26, pn.title, 13, COL_TEXT, true, false);
}

// (Re)create the persistent image the UI is rendered into, filled with the
// background colour so nothing uninitialised is ever blitted.
static void create_backbuffer(GuitarMidiUI* ui, int width, int height)
{
    if (ui->backbuffer)
        cairo_surface_destroy(ui->backbuffer);
    ui->backbuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(ui->backbuffer) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(ui->backbuffer);
        ui->backbuffer = nullptr;
        return;
    }
    cairo_t* cr = cairo_create(ui->backbuffer);
    cairo_set_source_rgb(cr, COL_BG_BOT.r, COL_BG_BOT.g, COL_BG_BOT.b);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(ui->backbuffer);
}

// Repaint the given UI-space region: render the whole scene into the
// backbuffer clipped to it, then blit only that region to the window. Clipping
// (instead of skipping widgets) keeps the z-order of a full redraw, so a
// widget that overlaps the region -- e.g. a knob value above the meter -- is
// repainted before the meter draws over it.
static void draw_ui(GuitarMidiUI* ui, const Rect& dirty)
{
    if (!ui->surface || ui->win_w <= 0 || ui->win_h <= 0)
        return;

    // Round the region out to whole pixels and keep it inside the window: the
    // clip and the blit have to cover exactly the same pixels.
    const double x0 = std::max(0.0, std::floor(dirty.x0));
    const double y0 = std::max(0.0, std::floor(dirty.y0));
    const double x1 = std::min<double>(ui->win_w, std::ceil(dirty.x1));
    const double y1 = std::min<double>(ui->win_h, std::ceil(dirty.y1));
    if (x1 <= x0 || y1 <= y0)
        return;
    const double w = x1 - x0;
    const double h = y1 - y0;

    // Without a backbuffer (image allocation failed) draw straight to the
    // window; the dirty region is still exact, so no repaint is skipped.
    const bool buffered = ui->backbuffer != nullptr;
    cairo_surface_t* target = buffered ? ui->backbuffer : ui->surface;

    cairo_t* cr = cairo_create(target);
    cairo_rectangle(cr, x0, y0, w, h);
    cairo_clip(cr);
    // Render as the window surface would: the image surface has other
    // hinting/subpixel defaults, which would move glyph edges and (through
    // hint metrics) the text extents measured for centring.
    if (ui->font_options)
        cairo_set_font_options(cr, ui->font_options);

    // Background gradient
    cairo_pattern_t* bg = cairo_pattern_create_linear(0, 0, 0, UI_H);
    cairo_pattern_add_color_stop_rgb(bg, 0, COL_BG_TOP.r, COL_BG_TOP.g, COL_BG_TOP.b);
    cairo_pattern_add_color_stop_rgb(bg, 1, COL_BG_BOT.r, COL_BG_BOT.g, COL_BG_BOT.b);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    // Header ---------------------------------------------------------------
    // subtle accent strip
    set_color(cr, COL_ACCENT, 0.9);
    cairo_rectangle(cr, 0, 0, UI_W, 3);
    cairo_fill(cr);

    draw_text(cr, 20, 38, "GUITAR", 24, COL_TEXT, true, false);
    cairo_text_extents_t ext;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 24);
    cairo_text_extents(cr, "GUITAR", &ext);
    draw_text(cr, 20 + ext.width + 8, 38, "MIDI", 24, COL_ACCENT, true, false);

    draw_text(cr, 20, 55, "Neural guitar-to-MIDI converter", 11, COL_TEXT_DIM, false, false);

    const double button_y = 15;
    const double button_w = PLOT_BUTTON_WIDTH;
    const double button_h = 30;
    const double button_gap = PLOT_BUTTON_GAP;
    const double button_start = UI_W - 16 - TOOLBAR_BUTTON_COUNT * button_w -
        (TOOLBAR_BUTTON_COUNT - 1) * button_gap;
    const Color button_colors[TOOLBAR_BUTTON_COUNT] = {
        COL_ACCENT, COL_ACCENT2, COL_ACCENT, COL_ACCENT2, COL_ACCENT3, COL_TEXT_DIM
    };
    const char* button_labels[TOOLBAR_BUTTON_COUNT] = {
        "CONF SMOOTH", "ENERGY SMOOTH", "CONF RAW", "ENERGY RAW", "FILTER OUT", "SAVE PLOTS"
    };
    for (size_t i = 0; i < TOOLBAR_BUTTON_COUNT; ++i) {
        const double x = button_start + i * (button_w + button_gap);
        set_color(cr, COL_PANEL);
        rounded_rect(cr, x, button_y, button_w, button_h, 4);
        cairo_fill(cr);
        set_color(cr, button_colors[i], 0.75);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, x + 0.5, button_y + 0.5,
                     button_w - 1, button_h - 1, 4);
        cairo_stroke(cr);
        draw_text(cr, x + button_w / 2, button_y + 19,
                  button_labels[i], 10, COL_TEXT, true, true);
    }

    // Panels -----------------------------------------------------------------
    Panel rows[2][3];
    layout_panels(rows);
    for (int r = 0; r < 2; ++r)
        for (int p = 0; p < 3; ++p)
            draw_panel(cr, rows[r][p]);

    // Divider between the two control rows
    set_color(cr, COL_PANEL_EDGE, 0.6);
    cairo_set_line_width(cr, 1.0);
    const double line_y = std::floor(divider_y()) + 0.5;
    cairo_move_to(cr, PANEL_MARGIN, line_y);
    cairo_line_to(cr, UI_W - PANEL_MARGIN, line_y);
    cairo_stroke(cr);

    // Knobs
    for (size_t i = 0; i < ui->knobs.size(); ++i)
        draw_knob(cr, ui->knobs[i], (int)i == ui->hover_knob);

    // Live input level meter at the bottom of the INPUT panel
    draw_input_meter(cr, rows[0][0], *ui);

    if(ui->hover_knob>=0)
        draw_tooltip(cr,ui->knobs[ui->hover_knob]);
    // Footer hint (centered)
    const char* footer = ui->save_status.empty()
        ? "drag: adjust    shift+drag: fine    double-click: reset    scroll: step"
        : ui->save_status.c_str();
    draw_text(cr, UI_W / 2.0, UI_H - 6, footer, 10,
              COL_TEXT_DIM, false, true);

    cairo_destroy(cr);
    cairo_surface_flush(target);

    if (!buffered)
        return;

    // Blit just the repainted region to the window.
    cairo_t* wcr = cairo_create(ui->surface);
    cairo_rectangle(wcr, x0, y0, w, h);
    cairo_clip(wcr);
    cairo_set_source_surface(wcr, target, 0.0, 0.0);
    cairo_paint(wcr);
    cairo_destroy(wcr);
    cairo_surface_flush(ui->surface);
}

static void open_plot_window(GuitarMidiUI* ui, int plot_index)
{
    DebugPlot& plot = ui->plots[plot_index];
    if (!plot.window) {
        plot.window = XCreateSimpleWindow(ui->dpy, RootWindow(ui->dpy, ui->screen),
            80 + plot_index * 50, 80 + plot_index * 50, DEBUG_W, DEBUG_H,
            0, BlackPixel(ui->dpy, ui->screen), BlackPixel(ui->dpy, ui->screen));
        plot.row_count = plot_index == static_cast<int>(NOTE_PLOT_COUNT)
            ? FILTER_COUNT : NOTE_COUNT;
        if (plot_index == static_cast<int>(NOTE_PLOT_COUNT))
            plot.visible_columns = DEFAULT_FILTER_VISIBLE_SAMPLES;
        XSelectInput(ui->dpy, plot.window,
                 ExposureMask | StructureNotifyMask | ButtonPressMask);
        static const char* window_titles[PLOT_COUNT] = {
            "GuitarMidi Smoothed Note Confidences",
            "GuitarMidi Smoothed Note Energies",
            "GuitarMidi Momentary Note Confidences",
            "GuitarMidi Momentary Note Energies",
            "GuitarMidi Filter Bank Outputs"
        };
        XStoreName(ui->dpy, plot.window, window_titles[plot_index]);
        XSetWMProtocols(ui->dpy, plot.window, &ui->wm_delete, 1);
        XSizeHints hints = {};
        hints.flags = PMinSize;
        hints.min_width = 500;
        hints.min_height = 320;
        XSetWMNormalHints(ui->dpy, plot.window, &hints);
        plot.surface = cairo_xlib_surface_create(ui->dpy, plot.window,
            DefaultVisual(ui->dpy, ui->screen), plot.width, plot.height);
        plot.backbuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
            plot.width, plot.height);
    }
    plot.visible = true;
    plot.dirty = true;
    XMapRaised(ui->dpy, plot.window);
    XFlush(ui->dpy);
}

static void draw_filter_waveforms(cairo_t* cr, const DebugPlot& plot,
                                  double chart_x, double chart_y,
                                  double chart_w, double chart_h)
{
    const size_t visible_samples = std::min(plot.columns_filled, plot.visible_columns);
    const size_t first_column = (plot.next_column + FILTER_HISTORY_SIZE - visible_samples) % FILTER_HISTORY_SIZE;
    const double cell_h = chart_h / FILTER_COUNT;
    float amplitude_limit = 0.0f;
    for (size_t column = 0; column < visible_samples; ++column)
        amplitude_limit = std::max(amplitude_limit,
            plot.filter_column_peaks[(first_column + column) % FILTER_HISTORY_SIZE]);
    if (amplitude_limit < 1.0e-12f)
        amplitude_limit = 1.0f;

    for (size_t filter = 0; filter < FILTER_COUNT; ++filter) {
        const double row_bottom = chart_y + (FILTER_COUNT - filter) * cell_h;
        const Color trace_color = note_color(filter / HARMONICS_PER_NOTE);
        if (filter % HARMONICS_PER_NOTE == 0) {
            static const char* pitch_names[] = {
                "C", "C#", "D", "D#", "E", "F",
                "F#", "G", "G#", "A", "A#", "B"
            };
            const int midi_note = MIDI_NOTE_OFFSET + static_cast<int>(filter / HARMONICS_PER_NOTE);
            char label[16];
            snprintf(label, sizeof(label), "%s%d", pitch_names[midi_note % 12], midi_note / 12 - 1);
            const double group_center = chart_y +
                (FILTER_COUNT - filter - HARMONICS_PER_NOTE * 0.5) * cell_h;
            draw_text(cr, chart_x * 0.5, group_center + 3,
                      label, 8, trace_color, true, true);
            draw_text(cr, chart_x + chart_w + chart_x * 0.5, group_center + 3,
                      label, 8, trace_color, true, true);
        }

        set_color(cr, COL_TRACK, 0.65);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, chart_x, row_bottom);
        cairo_line_to(cr, chart_x + chart_w, row_bottom);
        cairo_stroke(cr);

        cairo_new_path(cr);
        if (visible_samples <= static_cast<size_t>(chart_w)) {
            for (size_t point = 0; point < visible_samples; ++point) {
                const size_t column = (first_column + point) % FILTER_HISTORY_SIZE;
                const double x = chart_x + (visible_samples > 1
                    ? static_cast<double>(point) * chart_w / (visible_samples - 1) : chart_w);
                const double y = row_bottom - std::min(1.0f,
                    plot.filter_history[column][filter] / amplitude_limit) * cell_h * 0.9;
                if (point == 0)
                    cairo_move_to(cr, x, y);
                else
                    cairo_line_to(cr, x, y);
            }
            cairo_set_line_width(cr, 1.0);
            set_color(cr, trace_color);
            cairo_stroke(cr);
        } else {
            const size_t pixel_count = static_cast<size_t>(chart_w);
            for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
                const size_t begin = pixel * visible_samples / pixel_count;
                const size_t end = std::max(begin + 1,
                    (pixel + 1) * visible_samples / pixel_count);
                float minimum = std::numeric_limits<float>::max();
                float maximum = 0.0f;
                for (size_t point = begin; point < std::min(end, visible_samples); ++point) {
                    const size_t column = (first_column + point) % FILTER_HISTORY_SIZE;
                    const float value = plot.filter_history[column][filter];
                    minimum = std::min(minimum, value);
                    maximum = std::max(maximum, value);
                }
                const double x = chart_x + pixel;
                const double y_top = row_bottom - std::min(1.0f, maximum / amplitude_limit) * cell_h * 0.9;
                const double y_bottom = row_bottom - std::min(1.0f, minimum / amplitude_limit) * cell_h * 0.9;
                cairo_move_to(cr, x, y_top);
                cairo_line_to(cr, x, y_bottom);
            }
            set_color(cr, trace_color);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        }
    }
}

static void render_debug_plot(GuitarMidiUI* ui, int plot_index, cairo_t* cr)
{
    DebugPlot& plot = ui->plots[plot_index];
    cairo_set_source_rgb(cr, COL_BG_BOT.r, COL_BG_BOT.g, COL_BG_BOT.b);
    cairo_paint(cr);

    static const char* titles[PLOT_COUNT] = {
        "SMOOTHED CONFIDENCE", "SMOOTHED ENERGY",
        "MOMENTARY CONFIDENCE", "MOMENTARY ENERGY", "FILTER WAVEFORMS"
    };
    const bool is_confidence = plot_index == 0 || plot_index == 2;
    const char* title = titles[plot_index];
    const Color accent = plot_index == static_cast<int>(NOTE_PLOT_COUNT)
        ? COL_ACCENT3 : (is_confidence ? COL_ACCENT : COL_ACCENT2);
    draw_text(cr, 20, 30, title, 16, COL_TEXT, true, false);
    char duration[32];
    if (plot_index == static_cast<int>(NOTE_PLOT_COUNT))
        snprintf(duration, sizeof(duration), "%zu samples / %.2f ms",
                 plot.visible_columns,
                 plot.visible_columns * 1000.0 / FILTER_SAMPLE_RATE);
    else
        snprintf(duration, sizeof(duration), "%.1f s", plot.visible_columns / DEBUG_UPDATES_PER_SECOND);
    draw_text(cr, 190, 30, duration, 11, COL_TEXT_DIM, false, false);
    draw_text(cr, plot.width - 55, 29,
              plot_index == static_cast<int>(NOTE_PLOT_COUNT) ? "start" : "older",
              10, COL_TEXT_DIM, false, false);
    draw_text(cr, plot.width - 55, 43,
              plot_index == static_cast<int>(NOTE_PLOT_COUNT) ? "end" : "newer",
              10, COL_TEXT_DIM, false, false);

    const double label_w = 58;
    const double chart_x = label_w;
    const double chart_y = 56;
    const double chart_w = plot.width - 2 * label_w;
    const double chart_h = plot.height - chart_y - 18;
    if (plot_index == static_cast<int>(NOTE_PLOT_COUNT)) {
        draw_filter_waveforms(cr, plot, chart_x, chart_y, chart_w, chart_h);
    } else {
        const double cell_w = chart_w / plot.visible_columns;
        const double cell_h = chart_h / NOTE_COUNT;
        const char* pitch_names[] = {"C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B"};
        const size_t visible_count = std::min(plot.columns_filled, plot.visible_columns);
        const size_t first_column = (plot.next_column + HISTORY_SIZE - visible_count) % HISTORY_SIZE;
        for (size_t note = 0; note < NOTE_COUNT; ++note) {
            const int midi_note = 40 + static_cast<int>(note);
            char label[16];
            snprintf(label, sizeof(label), "%s%d", pitch_names[midi_note % 12], midi_note / 12 - 1);
            const double y = chart_y + (plot.row_count - 1 - note) * cell_h;
            const Color note_accent = note_color(note);
            draw_text(cr, chart_x * 0.5, y + cell_h * 0.78,
                      label, 8, note_accent, true, true);
            draw_text(cr, chart_x + chart_w + chart_x * 0.5, y + cell_h * 0.78,
                      label, 8, note_accent, true, true);

            for (size_t column = 0; column < visible_count; ++column) {
                float value = plot.history[(first_column + column) % HISTORY_SIZE][note];
                value = std::max(0.0f, value);
                if (is_confidence)
                    value = std::min(1.0f, value);
                else
                    value = static_cast<float>(std::min(1.0, std::log1p(value) / std::log(101.0)));

                const double x = chart_x + (plot.visible_columns - visible_count + column) * cell_w;
                set_color(cr, COL_TRACK);
                cairo_rectangle(cr, x, y, cell_w + 0.3, cell_h - 0.7);
                cairo_fill(cr);
                set_color(cr, accent, 0.12 + value * 0.88);
                cairo_rectangle(cr, x, y, cell_w + 0.3, cell_h - 0.7);
                cairo_fill(cr);
            }
        }
    }

}

static void draw_debug_plot(GuitarMidiUI* ui, int plot_index)
{
    DebugPlot& plot = ui->plots[plot_index];
    if (!plot.surface || !plot.backbuffer || !plot.visible)
        return;

    cairo_t* cr = cairo_create(plot.backbuffer);
    render_debug_plot(ui, plot_index, cr);
    cairo_destroy(cr);
    cairo_surface_flush(plot.backbuffer);
    cairo_t* window_cr = cairo_create(plot.surface);
    cairo_set_source_surface(window_cr, plot.backbuffer, 0, 0);
    cairo_paint(window_cr);
    cairo_destroy(window_cr);
    cairo_surface_flush(plot.surface);
}

static void save_plot_snapshots(GuitarMidiUI* ui)
{
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        ui->save_status = "Save failed: HOME is not set";
        mark_dirty_all(ui);
        return;
    }

    const std::filesystem::path output_directory =
        std::filesystem::path(home) / "Pictures" / "GuitarMidiSnapshots";
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error) {
        ui->save_status = "Save failed: " + error.message();
        mark_dirty_all(ui);
        return;
    }

    static const char* file_stems[PLOT_COUNT] = {
        "smoothed-confidence", "smoothed-energy", "momentary-confidence",
        "momentary-energy", "filter-waveforms"
    };
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (size_t i = 0; i < PLOT_COUNT; ++i) {
        DebugPlot& plot = ui->plots[i];
        cairo_surface_t* image = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, plot.width, plot.height);
        if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
            const std::string message = cairo_status_to_string(cairo_surface_status(image));
            cairo_surface_destroy(image);
            ui->save_status = "Save failed: " + message;
            mark_dirty_all(ui);
            return;
        }

        cairo_t* cr = cairo_create(image);
        render_debug_plot(ui, static_cast<int>(i), cr);
        cairo_destroy(cr);
        cairo_surface_flush(image);

        const std::filesystem::path file = output_directory /
            (std::string(file_stems[i]) + "-" + std::to_string(timestamp) + ".png");
        const cairo_status_t status = cairo_surface_write_to_png(image, file.c_str());
        cairo_surface_destroy(image);
        if (status != CAIRO_STATUS_SUCCESS) {
            ui->save_status = "Save failed: " + std::string(cairo_status_to_string(status));
            mark_dirty_all(ui);
            return;
        }
    }

    ui->save_status = "Saved 5 PNGs to ~/Pictures/GuitarMidiSnapshots";
    mark_dirty_all(ui);
}

static void handle_plot_event(GuitarMidiUI* ui, int plot_index, XEvent* ev)
{
    DebugPlot& plot = ui->plots[plot_index];
    if (ev->type == ConfigureNotify &&
        (plot.width != ev->xconfigure.width || plot.height != ev->xconfigure.height)) {
        plot.width = ev->xconfigure.width;
        plot.height = ev->xconfigure.height;
        cairo_xlib_surface_set_size(plot.surface, plot.width, plot.height);
        cairo_surface_destroy(plot.backbuffer);
        plot.backbuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
            plot.width, plot.height);
        plot.dirty = true;
    } else if (ev->type == Expose && ev->xexpose.count == 0) {
        plot.dirty = true;
    } else if (ev->type == ButtonPress &&
               (ev->xbutton.button == Button4 || ev->xbutton.button == Button5)) {
            const size_t new_zoom = ev->xbutton.button == Button4
                ? std::max(plot_index == static_cast<int>(NOTE_PLOT_COUNT)
                               ? MIN_FILTER_VISIBLE_SAMPLES : MIN_VISIBLE_COLUMNS,
                           plot.visible_columns / 2)
                : std::min(plot_index == static_cast<int>(NOTE_PLOT_COUNT)
                               ? FILTER_HISTORY_SIZE : HISTORY_SIZE,
                           plot.visible_columns * 2);
        if (new_zoom != plot.visible_columns) {
            plot.visible_columns = new_zoom;
            plot.dirty = true;
        }
    } else if (ev->type == ClientMessage &&
               static_cast<Atom>(ev->xclient.data.l[0]) == ui->wm_delete) {
        plot.visible = false;
        XUnmapWindow(ui->dpy, plot.window);
    }
}

// ---------------------------------------------------------------------------
// Interaction helpers
// ---------------------------------------------------------------------------
static int knob_at(GuitarMidiUI* ui, double x, double y)
{
    for (size_t i = 0; i < ui->knobs.size(); ++i) {
        const Knob& k = ui->knobs[i];
        if (k.toggle) {
            if (std::fabs(x - k.cx) <= TOGGLE_W * 0.5 + 4 &&
                std::fabs(y - k.cy) <= TOGGLE_H * 0.5 + 4)
                return (int)i;
            continue;
        }
        const double dx = x - k.cx;
        const double dy = y - k.cy;
        if (sqrt(dx * dx + dy * dy) <= k.radius + 6)
            return (int)i;
    }
    return -1;
}

static void set_knob_value(GuitarMidiUI* ui, int idx, float v, bool notify)
{
    Knob& k = ui->knobs[idx];
    if (v < k.min) v = k.min;
    if (v > k.max) v = k.max;
    // Step-1 parameters (decimals == 0) snap to their integer positions.
    if (k.decimals == 0)
        v = std::round(v);
    if (v == k.value)
        return;
    k.value = v;
    mark_dirty_all(ui);
    if (notify && ui->write)
        ui->write(ui->controller, k.port, sizeof(float), 0, &k.value);
}

// ---------------------------------------------------------------------------
// X11 event handling
// ---------------------------------------------------------------------------
static void handle_event(GuitarMidiUI* ui, XEvent* ev)
{
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0)
            mark_dirty_all(ui);
        break;

    case ConfigureNotify: {
        const int w = ev->xconfigure.width;
        const int h = ev->xconfigure.height;
        if (w != ui->win_w || h != ui->win_h) {
            ui->win_w = w;
            ui->win_h = h;
            cairo_xlib_surface_set_size(ui->surface, w, h);
            create_backbuffer(ui, w, h);
            mark_dirty_all(ui);
        }
        break;
    }

    case ButtonPress: {
        if (ev->xbutton.button == Button1 && ev->xbutton.y >= 15 && ev->xbutton.y <= 45) {
            const double button_w = PLOT_BUTTON_WIDTH;
            const double button_gap = PLOT_BUTTON_GAP;
            const double button_start = UI_W - 16 - TOOLBAR_BUTTON_COUNT * button_w -
                (TOOLBAR_BUTTON_COUNT - 1) * button_gap;
            for (size_t i = 0; i < TOOLBAR_BUTTON_COUNT; ++i) {
                const double x = button_start + i * (button_w + button_gap);
                if (ev->xbutton.x >= x && ev->xbutton.x < x + button_w) {
                    if (i == PLOT_COUNT)
                        save_plot_snapshots(ui);
                    else
                        open_plot_window(ui, static_cast<int>(i));
                    break;
                }
            }
            if (ev->xbutton.x >= button_start && ev->xbutton.x < UI_W - 16)
                break;
        }
        const int idx = knob_at(ui, ev->xbutton.x, ev->xbutton.y);
        if (idx < 0)
            break;
        if (ui->knobs[idx].toggle) {
            // Toggles flip on click and never start a drag; the wheel is ignored.
            if (ev->xbutton.button == Button1)
                set_knob_value(ui, idx,
                               ui->knobs[idx].value > 0.5f ? 0.0f : 1.0f, true);
            break;
        }
        if (ev->xbutton.button == Button1) {
            // Double-click detection via time
            static Time last_click = 0;
            static int  last_idx = -1;
            if (last_idx == idx && ev->xbutton.time - last_click < 350) {
                set_knob_value(ui, idx, ui->knobs[idx].def, true);
                last_click = 0;
                last_idx = -1;
                break;
            }
            last_click = ev->xbutton.time;
            last_idx = idx;

            ui->drag_knob = idx;
            ui->drag_start_y = ev->xbutton.y;
            ui->drag_start_val = ui->knobs[idx].value;
            ui->fine_drag = (ev->xbutton.state & ShiftMask) != 0;
        } else if (ev->xbutton.button == Button4 || ev->xbutton.button == Button5) {
            // Scroll wheel
            const Knob& k = ui->knobs[idx];
            const float range = k.max - k.min;
            float step = k.decimals == 0
                ? 1.0f
                : range / ((ev->xbutton.state & ShiftMask) ? 200.0f : 40.0f);
            if (ev->xbutton.button == Button5)
                step = -step;
            set_knob_value(ui, idx, k.value + step, true);
        }
        break;
    }

    case ButtonRelease:
        if (ev->xbutton.button == Button1)
            ui->drag_knob = -1;
        break;

    case MotionNotify: {
        // Compress motion events
        while (XCheckTypedWindowEvent(ui->dpy, ui->win, MotionNotify, ev))
            ;
        if (ui->drag_knob >= 0) {
            const Knob& k = ui->knobs[ui->drag_knob];
            const double sensitivity = (ev->xmotion.state & ShiftMask) ? 1000.0 : 200.0;
            const double dy = ui->drag_start_y - ev->xmotion.y;
            const float range = k.max - k.min;
            set_knob_value(ui, ui->drag_knob,
                           ui->drag_start_val + (float)(dy / sensitivity) * range,
                           true);
        } else {
            const int h = knob_at(ui, ev->xmotion.x, ev->xmotion.y);
            if (h != ui->hover_knob) {
                ui->hover_knob = h;
                mark_dirty_all(ui);
            }
        }
        break;
    }

    case LeaveNotify:
        if (ui->hover_knob != -1 && ui->drag_knob < 0) {
            ui->hover_knob = -1;
            mark_dirty_all(ui);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// LV2UI interface
// ---------------------------------------------------------------------------
static LV2UI_Handle instantiate(const LV2UI_Descriptor*   descriptor,
                                const char*               plugin_uri,
                                const char*               bundle_path,
                                LV2UI_Write_Function      write_function,
                                LV2UI_Controller          controller,
                                LV2UI_Widget*             widget,
                                const LV2_Feature* const* features)
{
    (void)descriptor;
    (void)bundle_path;

    if (strcmp(plugin_uri, GUITARMIDI_URI) != 0)
        return nullptr;

    Window parent = 0;
    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_UI__parent))
            parent = (Window)(uintptr_t)features[i]->data;
    }
    if (!parent) {
        fprintf(stderr, "guitarmidi_ui: host does not provide ui:parent\n");
        return nullptr;
    }

    LV2_URID_Map* map = nullptr;
    LV2UI_Port_Map* port_map = nullptr;
    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map))
            map = (LV2_URID_Map*)features[i]->data;
        else if (!strcmp(features[i]->URI, LV2_UI__portMap))
            port_map = (LV2UI_Port_Map*)features[i]->data;
    }
    if (!map || !port_map)
        return nullptr;

    GuitarMidiUI* ui = new GuitarMidiUI();
    ui->write = write_function;
    ui->controller = controller;
    ui->plots[NOTE_PLOT_COUNT].visible_columns = DEFAULT_FILTER_VISIBLE_SAMPLES;
    ui->debug_port = port_map->port_index(port_map->handle, "debugdata");
    ui->event_transfer = map->map(map->handle, LV2_ATOM__eventTransfer);
    ui->vector_type = map->map(map->handle, LV2_ATOM__Vector);
    ui->float_type = map->map(map->handle, LV2_ATOM__Float);
    if (ui->debug_port == LV2UI_INVALID_PORT_INDEX) {
        delete ui;
        return nullptr;
    }
    ui->drag_knob = -1;
    ui->hover_knob = -1;
    ui->dirty = false;
    ui->mapped = false;

    ui->dpy = XOpenDisplay(nullptr);
    if (!ui->dpy) {
        delete ui;
        return nullptr;
    }
    ui->screen = DefaultScreen(ui->dpy);
    ui->parent = parent;
    ui->wm_delete = XInternAtom(ui->dpy, "WM_DELETE_WINDOW", False);

    XSetWindowAttributes attr = {};
    attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | LeaveWindowMask | StructureNotifyMask;
    attr.background_pixel = BlackPixel(ui->dpy, ui->screen);

    ui->win = XCreateWindow(ui->dpy, parent, 0, 0, UI_W, UI_H, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWEventMask | CWBackPixel, &attr);

    // Fixed size hints (many hosts honour these for resize behaviour)
    XSizeHints hints = {};
    hints.flags = PMinSize | PMaxSize | PBaseSize;
    hints.min_width = hints.max_width = hints.base_width = UI_W;
    hints.min_height = hints.max_height = hints.base_height = UI_H;
    XSetWMNormalHints(ui->dpy, ui->win, &hints);

    XMapRaised(ui->dpy, ui->win);
    XFlush(ui->dpy);

    ui->surface = cairo_xlib_surface_create(ui->dpy, ui->win,
                                            DefaultVisual(ui->dpy, ui->screen),
                                            UI_W, UI_H);

    // The window is only ever painted by blitting the persistent backbuffer,
    // created here and again on resize; first frame is a full repaint.
    ui->win_w = UI_W;
    ui->win_h = UI_H;
    ui->font_options = cairo_font_options_create();
    cairo_surface_get_font_options(ui->surface, ui->font_options);
    create_backbuffer(ui, ui->win_w, ui->win_h);

    init_knobs(ui);
    layout_knobs(ui);
    mark_dirty_all(ui);

    *widget = (LV2UI_Widget)(uintptr_t)ui->win;
    return (LV2UI_Handle)ui;
}

static void cleanup(LV2UI_Handle handle)
{
    GuitarMidiUI* ui = (GuitarMidiUI*)handle;
    if (ui->surface)
        cairo_surface_destroy(ui->surface);
    if (ui->backbuffer)
        cairo_surface_destroy(ui->backbuffer);
    if (ui->font_options)
        cairo_font_options_destroy(ui->font_options);
    for (auto& plot : ui->plots) {
        if (plot.surface)
            cairo_surface_destroy(plot.surface);
        if (plot.backbuffer)
            cairo_surface_destroy(plot.backbuffer);
        if (plot.window)
            XDestroyWindow(ui->dpy, plot.window);
    }
    if (ui->dpy) {
        XDestroyWindow(ui->dpy, ui->win);
        XCloseDisplay(ui->dpy);
    }
    delete ui;
}

static void port_event(LV2UI_Handle handle,
                       uint32_t     port_index,
                       uint32_t     buffer_size,
                       uint32_t     format,
                       const void*  buffer)
{
    GuitarMidiUI* ui = (GuitarMidiUI*)handle;
    if (port_index == ui->debug_port && format == ui->event_transfer &&
        buffer_size >= sizeof(LV2_Atom_Vector_Body) +
            (NOTE_COUNT * NOTE_PLOT_COUNT + FILTER_COUNT * FILTER_WAVEFORM_SAMPLES + 1) * sizeof(float)) {
        const auto* atom = (const LV2_Atom*)buffer;
        if (atom->type != ui->vector_type ||
            atom->size < sizeof(LV2_Atom_Vector_Body) +
                (NOTE_COUNT * NOTE_PLOT_COUNT + FILTER_COUNT * FILTER_WAVEFORM_SAMPLES + 1) * sizeof(float) ||
            atom->size > buffer_size)
            return;
        const auto* vector = (const LV2_Atom_Vector*)atom;
        if (vector->body.child_size != sizeof(float) || vector->body.child_type != ui->float_type)
            return;
        const auto* values = (const float*)((const uint8_t*)vector + sizeof(LV2_Atom_Vector));
        // Older senders stop after the debug tick; the meter floats are
        // appended, so only read them when the packet actually carries them.
        const size_t base_floats =
            NOTE_COUNT * NOTE_PLOT_COUNT + FILTER_COUNT * FILTER_WAVEFORM_SAMPLES + 1;
        const size_t packet_floats =
            (atom->size - sizeof(LV2_Atom_Vector_Body)) / sizeof(float);
        for (size_t plot_index = 0; plot_index < PLOT_COUNT; ++plot_index) {
            DebugPlot& plot = ui->plots[plot_index];
            if (plot_index < NOTE_PLOT_COUNT) {
                const size_t value_offset = plot_index * NOTE_COUNT;
                for (size_t row = 0; row < NOTE_COUNT; ++row)
                    plot.history[plot.next_column][row] = values[value_offset + row];
                plot.next_column = (plot.next_column + 1) % HISTORY_SIZE;
                plot.columns_filled = std::min(HISTORY_SIZE, plot.columns_filled + 1);
            } else {
                if (plot.filter_history.empty()) {
                    plot.filter_history.resize(FILTER_HISTORY_SIZE);
                    plot.filter_column_peaks.resize(FILTER_HISTORY_SIZE);
                }
                const size_t value_offset = NOTE_PLOT_COUNT * NOTE_COUNT;
                for (size_t sample = 0; sample < FILTER_WAVEFORM_SAMPLES; ++sample) {
                    float column_peak = 0.0f;
                    for (size_t filter = 0; filter < FILTER_COUNT; ++filter)
                    {
                        const float value = values[value_offset +
                            filter * FILTER_WAVEFORM_SAMPLES + sample];
                        plot.filter_history[plot.next_column][filter] = value;
                        column_peak = std::max(column_peak, std::fabs(value));
                    }
                    plot.filter_column_peaks[plot.next_column] = column_peak;
                    plot.next_column = (plot.next_column + 1) % FILTER_HISTORY_SIZE;
                    plot.columns_filled = std::min(FILTER_HISTORY_SIZE, plot.columns_filled + 1);
                }
            }
            plot.dirty = plot.visible;
        }
        // Input meter: rms dB then peak dB, appended after the debug tick.
        if (packet_floats >= base_floats + 2)
            update_input_meter(ui, values[base_floats], values[base_floats + 1]);
        return;
    }
    if (format != 0 || buffer_size != sizeof(float))
        return;

    const float v = *static_cast<const float*>(buffer);
    for (size_t i = 0; i < ui->knobs.size(); ++i) {
        if (ui->knobs[i].port != port_index)
            continue;
        // While the user drags this knob, the gesture owns the value; applying
        // host echoes mid-drag would fight the pointer.
        if (ui->drag_knob == static_cast<int>(i))
            return;
        // Never echo the value back: this notification came from the host.
        set_knob_value(ui, static_cast<int>(i), v, false);
        return;
    }
}

// ui:idleInterface ----------------------------------------------------------
static int ui_idle(LV2UI_Handle handle)
{
    GuitarMidiUI* ui = (GuitarMidiUI*)handle;

    XEvent ev;
    while (XPending(ui->dpy) > 0) {
        XNextEvent(ui->dpy, &ev);
        if (ev.xany.window == ui->win) {
            handle_event(ui, &ev);
        } else {
            for (size_t i = 0; i < PLOT_COUNT; ++i) {
                if (ev.xany.window == ui->plots[i].window) {
                    handle_plot_event(ui, static_cast<int>(i), &ev);
                    break;
                }
            }
        }
    }

    if (ui->dirty) {
        draw_ui(ui, ui->dirty_rect);
        ui->dirty_rect.clear();
        XFlush(ui->dpy);
        ui->dirty = false;
    }
    for (size_t i = 0; i < PLOT_COUNT; ++i) {
        if (ui->plots[i].dirty) {
            draw_debug_plot(ui, static_cast<int>(i));
            XFlush(ui->dpy);
            ui->plots[i].dirty = false;
        }
    }
    return 0;
}

static const LV2UI_Idle_Interface idle_iface = {ui_idle};

static const void* extension_data(const char* uri)
{
    if (!strcmp(uri, LV2_UI__idleInterface))
        return &idle_iface;
    return nullptr;
}

static const LV2UI_Descriptor descriptor = {
    GUITARMIDI_UI_URI,
    instantiate,
    cleanup,
    port_event,
    extension_data
};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
