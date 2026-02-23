# Sixel Rendering Mode Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a `--sixel` flag that renders protein structures as pixel-level Sixel graphics for terminals like iTerm2, matching terminal pixel resolution with anti-aliased lines and depth shading.

**Architecture:** Bypass ncurses entirely in Sixel mode. Reuse existing Protein/Atom data pipeline. New SixelScreen class projects atoms at pixel resolution into an RGBA buffer, then SixelEncoder converts to Sixel escape sequences written to stdout. Raw termios for keyboard input.

**Tech Stack:** C++17, POSIX termios/ioctl, Sixel protocol (DCS sequences), no new external dependencies.

---

### Task 1: Add --sixel flag to Parameters

**Files:**
- Modify: `src/structure/Parameters.hpp:14` (add member)
- Modify: `src/structure/Parameters.cpp:111-113` (add parsing)

**Step 1: Add sixel member and accessor to Parameters.hpp**

In `src/structure/Parameters.hpp`, add after line 15 (`bool predict_structure = false;`):

```cpp
bool sixel = false;
```

Add accessor in the public section after `check_arg_okay()` (after line 73):

```cpp
bool get_sixel(){
    return sixel;
}
```

**Step 2: Add CLI parsing in Parameters.cpp**

In `src/structure/Parameters.cpp`, add a new `else if` branch after the `-p`/`--predict` handler (after line 116):

```cpp
else if (!strcmp(argv[i], "--sixel")) {
    sixel = true;
}
```

Also add to `print_help()` (after line 24):

```cpp
std::cout<<"--, --sixel:\n\trender using Sixel graphics (requires Sixel-capable terminal like iTerm2)"<<std::endl;
```

And in `print_args()` add after line 161:

```cpp
cout << "  sixel: " << sixel << endl;
```

**Step 3: Build and verify flag is accepted**

Run:
```bash
cd /tmp/Structty/build && cmake .. -DCURSES_INCLUDE_PATH=/opt/homebrew/opt/ncurses/include -DCURSES_LIBRARY=/opt/homebrew/opt/ncurses/lib/libncursesw.dylib -DCMAKE_EXE_LINKER_FLAGS="-L/opt/homebrew/opt/ncurses/lib -lncursesw -Wl,-rpath,/opt/homebrew/opt/ncurses/lib" -DCMAKE_CXX_FLAGS="-I/opt/homebrew/opt/ncurses/include" && make -j10
```

Then test:
```bash
./StrucTTY ../example/1mh1.pdb --sixel 2>&1 | head -5
```

Expected: Parameters printout shows `sixel: 1`. The program will still enter ncurses mode (we haven't branched yet).

**Step 4: Commit**

```bash
git add src/structure/Parameters.hpp src/structure/Parameters.cpp
git commit -m "feat: add --sixel CLI flag to Parameters"
```

---

### Task 2: Create SixelEncoder

**Files:**
- Create: `src/visualization/SixelEncoder.hpp`
- Create: `src/visualization/SixelEncoder.cpp`

**Step 1: Create SixelEncoder.hpp**

Create `src/visualization/SixelEncoder.hpp`:

```cpp
#pragma once
#include "Palette.hpp"
#include <vector>
#include <string>
#include <cstdint>

class SixelEncoder {
public:
    // Encode RGBA pixel buffer to Sixel escape sequence string
    // pixels: row-major RGBA buffer, width*height elements
    // bg_r, bg_g, bg_b: background color (pixels with a==0 use this)
    static std::string encode(const std::vector<RGBA>& pixels,
                              int width, int height,
                              uint8_t bg_r = 0, uint8_t bg_g = 0, uint8_t bg_b = 0);

private:
    // Build palette from Palettes::ID2RGBA (256 entries)
    // Returns palette definition string: "#0;2;r;g;b#1;2;r;g;b..."
    static std::string build_palette();

    // Map an RGBA pixel to nearest palette index
    // Uses simple nearest-neighbor in RGB space
    static int nearest_palette_color(uint8_t r, uint8_t g, uint8_t b);

    // Encode one band (6 rows) of pixels
    // band_y: top row of this band
    // palette_pixels: pre-quantized palette index per pixel (width*height)
    static void encode_band(std::string& out,
                            const std::vector<int>& palette_pixels,
                            int width, int height, int band_y);
};
```

**Step 2: Create SixelEncoder.cpp**

Create `src/visualization/SixelEncoder.cpp`:

```cpp
#include "SixelEncoder.hpp"
#include <cmath>
#include <algorithm>
#include <unordered_set>

std::string SixelEncoder::build_palette() {
    std::string pal;
    pal.reserve(256 * 20);
    for (int i = 0; i < 256; i++) {
        const RGBA& c = Palettes::ID2RGBA[i];
        // Sixel palette uses 0-100 range
        int r = (int)(c.r * 100 / 255);
        int g = (int)(c.g * 100 / 255);
        int b = (int)(c.b * 100 / 255);
        pal += "#" + std::to_string(i) + ";2;" +
               std::to_string(r) + ";" +
               std::to_string(g) + ";" +
               std::to_string(b);
    }
    return pal;
}

int SixelEncoder::nearest_palette_color(uint8_t r, uint8_t g, uint8_t b) {
    // Fast path: check 6x6x6 color cube (indices 16-231)
    // Each axis: 0, 95, 135, 175, 215, 255
    static const int levels[6] = {0, 95, 135, 175, 215, 255};

    auto nearest_level = [&](uint8_t v) -> int {
        int best = 0;
        int best_dist = abs(v - levels[0]);
        for (int i = 1; i < 6; i++) {
            int d = abs(v - levels[i]);
            if (d < best_dist) { best_dist = d; best = i; }
        }
        return best;
    };

    int ri = nearest_level(r);
    int gi = nearest_level(g);
    int bi = nearest_level(b);
    return 16 + ri * 36 + gi * 6 + bi;
}

void SixelEncoder::encode_band(std::string& out,
                                const std::vector<int>& palette_pixels,
                                int width, int height, int band_y) {
    // Collect which colors appear in this band
    std::unordered_set<int> active_colors;
    for (int row = band_y; row < std::min(band_y + 6, height); row++) {
        for (int x = 0; x < width; x++) {
            int c = palette_pixels[row * width + x];
            if (c >= 0) active_colors.insert(c);
        }
    }

    bool first_color = true;
    for (int color : active_colors) {
        if (!first_color) {
            out += '$'; // carriage return within band
        }
        first_color = false;

        out += '#';
        out += std::to_string(color);

        // Build sixel data for this color across the band
        int run_char = -1;
        int run_len = 0;

        auto flush_run = [&]() {
            if (run_len <= 0) return;
            if (run_len <= 3) {
                for (int i = 0; i < run_len; i++) out += (char)run_char;
            } else {
                out += '!';
                out += std::to_string(run_len);
                out += (char)run_char;
            }
        };

        for (int x = 0; x < width; x++) {
            // Build 6-bit value for this column
            int sixel_val = 0;
            for (int bit = 0; bit < 6; bit++) {
                int row = band_y + bit;
                if (row < height) {
                    int c = palette_pixels[row * width + x];
                    if (c == color) {
                        sixel_val |= (1 << bit);
                    }
                }
            }
            int ch = sixel_val + 63; // Sixel encoding offset

            if (ch == run_char) {
                run_len++;
            } else {
                flush_run();
                run_char = ch;
                run_len = 1;
            }
        }
        flush_run();
    }

    out += '-'; // next band (newline in Sixel)
}

std::string SixelEncoder::encode(const std::vector<RGBA>& pixels,
                                  int width, int height,
                                  uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    // Quantize all pixels to palette indices
    std::vector<int> palette_pixels(width * height, -1);
    for (int i = 0; i < width * height; i++) {
        const RGBA& px = pixels[i];
        if (px.a < 16) continue; // transparent -> background (skip)

        // Alpha-blend over background
        float a = px.a / 255.0f;
        uint8_t r = (uint8_t)(px.r * a + bg_r * (1.0f - a));
        uint8_t g = (uint8_t)(px.g * a + bg_g * (1.0f - a));
        uint8_t b = (uint8_t)(px.b * a + bg_b * (1.0f - a));
        palette_pixels[i] = nearest_palette_color(r, g, b);
    }

    // Build output
    std::string out;
    out.reserve(width * height); // rough estimate

    // DCS intro: ESC P 0;1;q  (0=aspect 1:1, 1=no bg erase)
    out += "\033P0;1;q";

    // Palette definitions
    out += build_palette();

    // Encode bands
    for (int band_y = 0; band_y < height; band_y += 6) {
        encode_band(out, palette_pixels, width, height, band_y);
    }

    // DCS terminator: ESC backslash
    out += "\033\\";

    return out;
}
```

**Step 3: Verify it compiles**

```bash
cd /tmp/Structty/build && make -j10
```

Expected: clean compile (GLOB picks up new files automatically via `src/CMakeLists.txt`).

**Step 4: Commit**

```bash
git add src/visualization/SixelEncoder.hpp src/visualization/SixelEncoder.cpp
git commit -m "feat: add SixelEncoder for RGBA-to-Sixel conversion"
```

---

### Task 3: Create SixelScreen

**Files:**
- Create: `src/visualization/SixelScreen.hpp`
- Create: `src/visualization/SixelScreen.cpp`

**Step 1: Create SixelScreen.hpp**

Create `src/visualization/SixelScreen.hpp`:

```cpp
#pragma once
#include "Protein.hpp"
#include "Atom.hpp"
#include "RenderPoint.hpp"
#include "Palette.hpp"
#include "SixelEncoder.hpp"
#include <vector>
#include <string>
#include <cmath>
#include <map>

class SixelScreen {
public:
    SixelScreen(const bool& show_structure, const std::string& mode);
    ~SixelScreen();

    void set_protein(const std::string& in_file, int ii, const bool& show_structure);
    void normalize_proteins(const std::string& utmatrix);
    void set_tmatrix();
    void set_utmatrix(const std::string& utmatrix, bool onlyU);
    void set_chainfile(const std::string& chainfile, int filesize);

    void draw_screen();
    bool handle_input();

    // Terminal raw mode setup/teardown
    void enter_raw_mode();
    void exit_raw_mode();

private:
    // Terminal pixel dimensions
    int pixel_width = 800;
    int pixel_height = 600;
    int term_cols = 80;
    int term_rows = 24;

    void query_terminal_size();

    // Pixel buffer (RGBA)
    std::vector<RGBA> framebuffer;

    // Depth buffer
    std::vector<float> zbuffer;

    // Scene data (mirrors Screen's interface)
    std::vector<Protein*> data;
    std::vector<float> pan_x;
    std::vector<float> pan_y;
    std::vector<std::string> chainVec;
    float** vectorpointer = nullptr;
    bool yesUT = false;

    BoundingBox global_bb;
    std::string screen_mode;
    bool screen_show_structure;
    int structNum = -1;
    float zoom_level = 2.0f;
    float focal_offset = 5.0f;

    // Projection
    void project();
    void clear_framebuffer();

    // Anti-aliased line drawing (Xiaolin Wu)
    void draw_line_aa(int x0, int y0, float z0,
                      int x1, int y1, float z1,
                      RGBA color, float alpha_scale);

    // Set a pixel with z-test and alpha compositing
    void plot_pixel(int x, int y, float z, RGBA color, float coverage);

    // Color assignment
    RGBA get_color_for_point(int protein_idx, int chain_idx, int point_idx, int total_points);

    // Info panel (text below Sixel image)
    void draw_info_panel();

    // Saved terminal state
    bool raw_mode_active = false;
};
```

**Step 2: Create SixelScreen.cpp**

Create `src/visualization/SixelScreen.cpp`. This is the largest file. Key sections:

```cpp
#include "SixelScreen.hpp"
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <limits>
#include <iostream>

static struct termios orig_termios;
static const float FOV = 90.0f;
static const float PI = 3.14159265359f;

// ─── Terminal management ─────────────────────────────────

void SixelScreen::enter_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    // Hide cursor
    write(STDOUT_FILENO, "\033[?25l", 6);
    // Switch to alternate screen
    write(STDOUT_FILENO, "\033[?1049h", 8);
    raw_mode_active = true;
}

void SixelScreen::exit_raw_mode() {
    if (!raw_mode_active) return;
    // Show cursor
    write(STDOUT_FILENO, "\033[?25h", 6);
    // Switch back from alternate screen
    write(STDOUT_FILENO, "\033[?1049l", 8);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_mode_active = false;
}

void SixelScreen::query_terminal_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_cols = ws.ws_col;
        term_rows = ws.ws_row;
        if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
            pixel_width = ws.ws_xpixel;
            pixel_height = ws.ws_ypixel;
        } else {
            // Fallback: assume ~8px per col, ~16px per row
            pixel_width = term_cols * 8;
            pixel_height = term_rows * 16;
        }
    }
    // Reserve bottom rows for info panel text
    int panel_text_rows = 6 + (int)data.size() * 3;
    int panel_pixel_height = panel_text_rows * (pixel_height / term_rows);
    pixel_height = std::max(100, pixel_height - panel_pixel_height);

    // Ensure dimensions are reasonable
    pixel_width = std::min(pixel_width, 2000);
    pixel_height = std::min(pixel_height, 1500);
}

// ─── Constructor / Destructor ────────────────────────────

SixelScreen::SixelScreen(const bool& show_structure, const std::string& mode) {
    screen_show_structure = show_structure;
    screen_mode = mode;
}

SixelScreen::~SixelScreen() {
    exit_raw_mode();
    for (Protein* p : data) delete p;
    data.clear();
    if (vectorpointer) {
        for (size_t i = 0; i < data.size(); i++) delete[] vectorpointer[i];
        delete[] vectorpointer;
    }
}

// ─── Data setup (mirrors Screen interface) ───────────────

void SixelScreen::set_protein(const std::string& in_file, int ii, const bool& show_structure) {
    Protein* protein = new Protein(in_file, chainVec.at(ii), show_structure);
    data.push_back(protein);
    pan_x.push_back(0.0f);
    pan_y.push_back(0.0f);
}

void SixelScreen::set_tmatrix() {
    size_t filenum = data.size();
    vectorpointer = new float*[filenum];
    for (size_t i = 0; i < filenum; i++) {
        vectorpointer[i] = new float[3]{0, 0, 0};
    }
}

void SixelScreen::set_chainfile(const std::string& chainfile, int filesize) {
    for (size_t i = 0; i < (size_t)filesize; i++) chainVec.push_back("-");
    if (chainfile.empty()) return;
    std::ifstream file(chainfile);
    if (!file.is_open()) { std::cerr << "Failed to open chainfile\n"; return; }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        int index; std::string chainlist;
        iss >> index >> chainlist;
        if (index >= filesize) continue;
        chainVec[index] = chainlist;
    }
}

void SixelScreen::set_utmatrix(const std::string& utmatrix, bool applyUT) {
    yesUT = !utmatrix.empty();
    const size_t filenum = data.size();
    float** matrixpointer = new float*[filenum];
    for (size_t i = 0; i < filenum; i++) {
        matrixpointer[i] = new float[9];
        for (int j = 0; j < 9; j++)
            matrixpointer[i][j] = (j % 4 == 0) ? 1.f : 0.f;
    }
    if (utmatrix.empty()) return;
    std::ifstream file(utmatrix);
    if (!file.is_open()) { std::cerr << "Failed to open utmatrix file\n"; return; }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        int index; std::string mat9Str, mat3Str;
        iss >> index >> mat9Str >> mat3Str;
        if (index < 0 || index >= (int)filenum) continue;
        { std::istringstream mss(mat9Str); std::string val; int c = 0;
          while (std::getline(mss, val, ',') && c < 9) matrixpointer[index][c++] = std::stof(val); }
        { std::istringstream mss(mat3Str); std::string val; int c = 0;
          while (std::getline(mss, val, ',') && c < 3) vectorpointer[index][c++] = std::stof(val); }
    }
    if (applyUT) {
        for (size_t i = 0; i < filenum; i++) {
            data[i]->do_naive_rotation(matrixpointer[i]);
            data[i]->do_shift(vectorpointer[i]);
        }
    }
    for (size_t i = 0; i < filenum; i++) delete[] matrixpointer[i];
    delete[] matrixpointer;
}

void SixelScreen::normalize_proteins(const std::string& utmatrix) {
    const bool hasUT = !utmatrix.empty();
    for (size_t i = 0; i < data.size(); i++) {
        data[i]->load_data(vectorpointer[i], yesUT);
    }
    if (hasUT) set_utmatrix(utmatrix, true);

    global_bb = BoundingBox();
    for (auto* p : data) { p->set_bounding_box(); global_bb = global_bb + p->get_bounding_box(); }

    float max_ext = std::max({global_bb.max_x - global_bb.min_x,
                              global_bb.max_y - global_bb.min_y,
                              global_bb.max_z - global_bb.min_z});
    float scale = (max_ext > 0.f) ? (2.0f / max_ext) : 1.0f;

    if (hasUT) {
        float gx = 0.5f * (global_bb.min_x + global_bb.max_x);
        float gy = 0.5f * (global_bb.min_y + global_bb.max_y);
        float gz = 0.5f * (global_bb.min_z + global_bb.max_z);
        float global_shift[3] = {-gx, -gy, -gz};
        for (auto* p : data) { p->set_scale(scale); p->do_shift(global_shift); p->do_scale(scale); }
    } else {
        for (auto* p : data) {
            float cs[3] = {-p->cx, -p->cy, -p->cz};
            p->set_scale(scale); p->do_shift(cs); p->do_scale(scale);
        }
    }

    // Now query terminal size (after data is loaded so we know panel height)
    query_terminal_size();
    framebuffer.resize(pixel_width * pixel_height, RGBA{0, 0, 0, 255});
    zbuffer.resize(pixel_width * pixel_height, std::numeric_limits<float>::infinity());
}

// ─── Pixel operations ────────────────────────────────────

void SixelScreen::clear_framebuffer() {
    std::fill(framebuffer.begin(), framebuffer.end(), RGBA{0, 0, 0, 255});
    std::fill(zbuffer.begin(), zbuffer.end(), std::numeric_limits<float>::infinity());
}

void SixelScreen::plot_pixel(int x, int y, float z, RGBA color, float coverage) {
    if (x < 0 || x >= pixel_width || y < 0 || y >= pixel_height) return;
    int idx = y * pixel_width + x;
    if (z > zbuffer[idx] + 0.01f) return; // behind existing pixel

    // Alpha from depth + coverage
    float a = (color.a / 255.0f) * coverage;
    if (z < zbuffer[idx]) {
        // Closer: replace
        zbuffer[idx] = z;
        framebuffer[idx].r = (uint8_t)(color.r * a);
        framebuffer[idx].g = (uint8_t)(color.g * a);
        framebuffer[idx].b = (uint8_t)(color.b * a);
        framebuffer[idx].a = (uint8_t)(a * 255);
    } else {
        // Same depth: blend (for AA)
        RGBA& dst = framebuffer[idx];
        dst.r = (uint8_t)std::min(255.0f, dst.r + color.r * a * 0.5f);
        dst.g = (uint8_t)std::min(255.0f, dst.g + color.g * a * 0.5f);
        dst.b = (uint8_t)std::min(255.0f, dst.b + color.b * a * 0.5f);
        dst.a = (uint8_t)std::min(255, (int)dst.a + (int)(a * 128));
    }
}

// ─── Anti-aliased line (Xiaolin Wu) ──────────────────────

void SixelScreen::draw_line_aa(int x0, int y0, float z0,
                                int x1, int y1, float z1,
                                RGBA color, float alpha_scale) {
    bool steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); std::swap(z0, z1); }

    float dx = (float)(x1 - x0);
    float dy = (float)(y1 - y0);
    float dz = z1 - z0;
    float gradient = (dx == 0.0f) ? 1.0f : dy / dx;
    float z_gradient = (dx == 0.0f) ? 0.0f : dz / dx;

    // First endpoint
    float xend = (float)x0;
    float yend = y0 + gradient * (xend - x0);
    float zval = z0;
    float xgap = 1.0f;
    int xpxl1 = (int)xend;
    int ypxl1 = (int)yend;
    float frac = yend - ypxl1;

    if (steep) {
        plot_pixel(ypxl1, xpxl1, zval, color, (1.0f - frac) * xgap * alpha_scale);
        plot_pixel(ypxl1 + 1, xpxl1, zval, color, frac * xgap * alpha_scale);
    } else {
        plot_pixel(xpxl1, ypxl1, zval, color, (1.0f - frac) * xgap * alpha_scale);
        plot_pixel(xpxl1, ypxl1 + 1, zval, color, frac * xgap * alpha_scale);
    }
    float intery = yend + gradient;
    zval += z_gradient;

    // Second endpoint
    xend = (float)x1;
    yend = y1 + gradient * (xend - x1);
    xgap = 1.0f;
    int xpxl2 = (int)xend;
    int ypxl2 = (int)yend;

    // Main loop
    for (int x = xpxl1 + 1; x < xpxl2; x++) {
        int iy = (int)intery;
        float f = intery - iy;
        if (steep) {
            plot_pixel(iy, x, zval, color, (1.0f - f) * alpha_scale);
            plot_pixel(iy + 1, x, zval, color, f * alpha_scale);
        } else {
            plot_pixel(x, iy, zval, color, (1.0f - f) * alpha_scale);
            plot_pixel(x, iy + 1, zval, color, f * alpha_scale);
        }
        intery += gradient;
        zval += z_gradient;
    }
}

// ─── Color assignment ────────────────────────────────────

RGBA SixelScreen::get_color_for_point(int protein_idx, int chain_idx,
                                       int point_idx, int total_points) {
    int palette_idx = 0;
    if (screen_mode == "protein") {
        palette_idx = Palettes::UNRAINBOW[protein_idx % Palettes::UNRAINBOW.size()];
    } else if (screen_mode == "chain") {
        int ci = (protein_idx * 10 + chain_idx) % Palettes::UNRAINBOW.size();
        palette_idx = Palettes::UNRAINBOW[ci];
    } else if (screen_mode == "rainbow") {
        int num_colors = Palettes::RAINBOW.size();
        int ci = (total_points > 0) ? (point_idx * num_colors / total_points) : 0;
        ci = std::min(ci, num_colors - 1);
        palette_idx = Palettes::RAINBOW[ci];
    }
    return Palettes::ID2RGBA[palette_idx];
}

// ─── Projection ──────────────────────────────────────────

void SixelScreen::project() {
    float fovRads = 1.0f / tanf((FOV / zoom_level) * 0.5f / 180.0f * PI);

    for (size_t ii = 0; ii < data.size(); ii++) {
        Protein* target = data[ii];

        // Count total atoms for rainbow mode
        int total_atoms = 0;
        for (const auto& [cid, atoms] : target->get_atoms())
            total_atoms += target->get_chain_length(cid);

        int global_atom_idx = 0;
        int chain_idx = 0;

        for (const auto& [chainID, chain_atoms] : target->get_atoms()) {
            if (chain_atoms.empty()) { chain_idx++; continue; }
            int num_atoms = target->get_chain_length(chainID);

            int prevSX = -1, prevSY = -1;
            float prevZ = 0;
            RGBA prevColor{0,0,0,0};

            for (int i = 0; i < num_atoms; i++) {
                float* pos = chain_atoms[i].get_position();
                float x = pos[0];
                float y = pos[1];
                float z = pos[2] + focal_offset;

                float projX = (x / z) * fovRads + pan_x[ii];
                float projY = (y / z) * fovRads + pan_y[ii];
                int sx = (int)((projX + 1.0f) * 0.5f * pixel_width);
                int sy = (int)((1.0f - projY) * 0.5f * pixel_height);

                // Depth-based alpha (closer = brighter)
                float min_z = target->get_scaled_min_z();
                float max_z = target->get_scaled_max_z();
                float zn = (max_z > min_z) ? ((z - focal_offset - min_z) / (max_z - min_z)) : 0.5f;
                zn = std::clamp(zn, 0.0f, 1.0f);
                float depth_alpha = 1.0f - zn * 0.7f; // near=1.0, far=0.3

                RGBA color = get_color_for_point((int)ii, chain_idx, global_atom_idx, total_atoms);
                color.a = (uint8_t)(depth_alpha * 255);

                if (prevSX >= 0) {
                    draw_line_aa(prevSX, prevSY, prevZ, sx, sy, z, color, depth_alpha);
                }

                prevSX = sx; prevSY = sy; prevZ = z; prevColor = color;
                global_atom_idx++;
            }
            chain_idx++;
        }
    }
}

// ─── Info panel (ANSI text below image) ──────────────────

void SixelScreen::draw_info_panel() {
    // Move cursor below the Sixel image
    // Use ANSI escapes for colored text
    std::string out;
    out += "\033[0m"; // reset

    // Separator
    std::string sep(std::min(term_cols, 80), '-');
    out += sep + "\n";

    // Controls
    out += " WASD:move  XYZ:rotate  R/F:zoom  C:screenshot  Q:quit\n";
    out += sep + "\n";

    // File info
    for (size_t i = 0; i < data.size(); i++) {
        auto* p = data[i];
        // Get color for this protein
        int palette_idx = Palettes::UNRAINBOW[i % Palettes::UNRAINBOW.size()];
        const RGBA& c = Palettes::ID2RGBA[palette_idx];
        // ANSI 24-bit color
        out += "\033[38;2;" + std::to_string(c.r) + ";" +
               std::to_string(c.g) + ";" + std::to_string(c.b) + "m";
        out += " " + p->get_file_name() + "\033[0m\n";

        // Chain info
        auto chain_lengths = p->get_chain_length();
        auto residue_counts = p->get_residue_count();
        int count = 0;
        out += "  ";
        for (const auto& [cid, len] : chain_lengths) {
            int res = 0;
            auto it = residue_counts.find(cid);
            if (it != residue_counts.end()) res = it->second;
            out += cid + ":" + std::to_string(res) + "(" + std::to_string(len) + ") ";
            count++;
            if (count % 6 == 0) out += "\n  ";
        }
        out += "\n";
    }

    write(STDOUT_FILENO, out.c_str(), out.size());
}

// ─── Main draw ───────────────────────────────────────────

void SixelScreen::draw_screen() {
    clear_framebuffer();
    project();

    // Cursor home
    write(STDOUT_FILENO, "\033[H", 3);

    // Encode and write Sixel
    std::string sixel = SixelEncoder::encode(framebuffer, pixel_width, pixel_height);
    write(STDOUT_FILENO, sixel.c_str(), sixel.size());

    // Info panel
    draw_info_panel();
}

// ─── Input handling ──────────────────────────────────────

bool SixelScreen::handle_input() {
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 0) <= 0) return true; // no input

    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return true;

    auto pan_step_x = 2.0f * 4.0f / pixel_width * 50.0f;
    auto pan_step_y = 2.0f * 2.0f / pixel_height * 50.0f;

    switch (c) {
        case '0': structNum = -1; break;
        case '1': case '2': case '3': case '4': case '5': case '6':
            if (c - '0' <= (int)data.size()) structNum = c - '1';
            break;
        case 'a': case 'A':
            if (structNum >= 0) pan_x[structNum] -= pan_step_x;
            else for (auto& px : pan_x) px -= pan_step_x;
            break;
        case 'd': case 'D':
            if (structNum >= 0) pan_x[structNum] += pan_step_x;
            else for (auto& px : pan_x) px += pan_step_x;
            break;
        case 'w': case 'W':
            if (structNum >= 0) pan_y[structNum] += pan_step_y;
            else for (auto& py : pan_y) py += pan_step_y;
            break;
        case 's': case 'S':
            if (structNum >= 0) pan_y[structNum] -= pan_step_y;
            else for (auto& py : pan_y) py -= pan_step_y;
            break;
        case 'x': case 'X':
            if (structNum >= 0) data[structNum]->set_rotate(1, 0, 0);
            else for (auto* p : data) p->set_rotate(1, 0, 0);
            break;
        case 'y': case 'Y':
            if (structNum >= 0) data[structNum]->set_rotate(0, 1, 0);
            else for (auto* p : data) p->set_rotate(0, 1, 0);
            break;
        case 'z': case 'Z':
            if (structNum >= 0) data[structNum]->set_rotate(0, 0, 1);
            else for (auto* p : data) p->set_rotate(0, 0, 1);
            break;
        case 'r': case 'R':
            if (zoom_level + 0.3f < 15.0f) zoom_level += 0.3f;
            break;
        case 'f': case 'F':
            if (zoom_level - 0.3f > 0.5f) zoom_level -= 0.3f;
            break;
        case 'q': case 'Q':
            return false;
    }
    return true;
}
```

**Step 3: Build and verify compilation**

```bash
cd /tmp/Structty/build && make -j10
```

Expected: clean compile.

**Step 4: Commit**

```bash
git add src/visualization/SixelScreen.hpp src/visualization/SixelScreen.cpp
git commit -m "feat: add SixelScreen with pixel-level Sixel rendering"
```

---

### Task 4: Wire up main.cpp to branch on --sixel

**Files:**
- Modify: `main.cpp`

**Step 1: Add SixelScreen include and branching logic**

Replace the contents of `main.cpp` with:

```cpp
#include <iostream>
#include <ncurses.h>
#include <unistd.h>
#include "Protein.hpp"
#include "Parameters.hpp"
#include "Screen.hpp"
#include "SixelScreen.hpp"

int main(int argc, char* argv[]) {
    Parameters params(argc, argv);

    if (!params.check_arg_okay()) {
        return -1;
    }
    params.print_args();

    if (params.get_sixel()) {
        // ── Sixel rendering path (no ncurses) ──
        SixelScreen screen(params.get_show_structure(), params.get_mode());
        screen.set_chainfile(params.get_chainfile(), params.get_in_file().size());
        for (int i = 0; i < (int)params.get_in_file().size(); i++) {
            screen.set_protein(params.get_in_file(i), i, params.get_show_structure());
        }
        screen.set_tmatrix();

        if (params.get_utmatrix() != "") {
            screen.set_utmatrix(params.get_utmatrix(), 0);
        }
        screen.normalize_proteins(params.get_utmatrix());

        screen.enter_raw_mode();
        bool run = true;
        while (run) {
            screen.draw_screen();
            run = screen.handle_input();
            usleep(33000); // ~30 FPS
        }
        screen.exit_raw_mode();

    } else {
        // ── Original ncurses path (unchanged) ──
        initscr();
        cbreak();
        noecho();

        Screen screen(params.get_width(), params.get_height(), params.get_show_structure(), params.get_mode(), params.get_depthcharacter());
        screen.set_chainfile(params.get_chainfile(), params.get_in_file().size());
        for (int i = 0; i < (int)params.get_in_file().size(); i++) {
            screen.set_protein(params.get_in_file(i), i, params.get_show_structure());
        }
        screen.set_tmatrix();

        if (params.get_utmatrix() != "") {
            screen.set_utmatrix(params.get_utmatrix(), 0);
        }
        screen.normalize_proteins(params.get_utmatrix());

        bool run = true;
        while (run) {
            screen.draw_screen();
            run = screen.handle_input();
            usleep(100);
        }

        endwin();
    }
    return 0;
}
```

**Step 2: Build**

```bash
cd /tmp/Structty/build && make -j10
```

Expected: clean compile, binary links both paths.

**Step 3: Test the ASCII path still works**

```bash
timeout 2 ./StrucTTY ../example/1mh1.pdb -s --mode rainbow 2>&1 | head -3
```

Expected: renders normally (exit 124 from timeout).

**Step 4: Test the Sixel path launches**

```bash
timeout 2 ./StrucTTY ../example/1mh1.pdb --sixel --mode rainbow 2>&1 | head -3
```

Expected: outputs Sixel escape sequences (starts with `\033P`). Won't render visually unless in iTerm2, but shouldn't crash.

**Step 5: Commit**

```bash
git add main.cpp
git commit -m "feat: wire --sixel flag to SixelScreen in main.cpp"
```

---

### Task 5: Test end-to-end in iTerm2

**Step 1: Run interactively in iTerm2**

Open iTerm2 and run:

```bash
/tmp/Structty/build/StrucTTY /tmp/1EMA_gfp.pdb --sixel -s --mode rainbow
```

**Verify:**
- Protein renders as pixel-level image (not ASCII)
- Rotation with Y/X/Z keys works smoothly
- Pan with WASD works
- Zoom with R/F works
- Q quits cleanly
- Terminal is restored after exit (cursor visible, echo on)

**Step 2: Test multi-protein**

```bash
/tmp/Structty/build/StrucTTY /tmp/1EMA_gfp.pdb /tmp/4INS_insulin.pdb --sixel -m chain -s
```

**Step 3: Test original ASCII mode is unaffected**

```bash
/tmp/Structty/build/StrucTTY /tmp/1EMA_gfp.pdb -s --mode rainbow
```

Verify: same ASCII rendering as before, no regressions.

**Step 4: Commit any fixes from testing**

```bash
git add -A && git commit -m "fix: address issues found during Sixel end-to-end testing"
```

---

### Task 6: Push to fork and create PR

**Step 1: Add fork remote and push**

```bash
cd /tmp/Structty
git remote add fork https://github.com/albert-ying/Structty.git
git push fork main
```

**Step 2: (Optional) Create PR to upstream**

```bash
gh pr create --repo sooyoung-cha/Structty --title "feat: add Sixel rendering mode for pixel-level visualization" --body "..."
```
