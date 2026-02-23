#pragma once
#include "Protein.hpp"
#include "Atom.hpp"
#include "RenderPoint.hpp"
#include "Palette.hpp"
#include <vector>
#include <string>
#include <cmath>
#include <map>
#include <cstdint>

struct RGB {
    uint8_t r, g, b;
};

struct Pixel {
    uint8_t r, g, b;
    float depth;
    bool active;
};

class UnicodeScreen {
public:
    UnicodeScreen(const bool& show_structure, const std::string& mode);
    ~UnicodeScreen();

    void set_protein(const std::string& in_file, int ii, const bool& show_structure);
    void normalize_proteins(const std::string& utmatrix);
    void set_tmatrix();
    void set_utmatrix(const std::string& utmatrix, bool onlyU);
    void set_chainfile(const std::string& chainfile, int filesize);

    void draw_screen();
    bool handle_input();

    void enter_raw_mode();
    void exit_raw_mode();

private:
    int term_cols = 80;
    int term_rows = 24;
    int info_rows = 3;
    int buf_width;   // = term_cols * 2 (braille: 2 dots per cell width)
    int buf_height;  // = (term_rows - info_rows) * 4 (braille: 4 dots per cell height)

    void query_terminal_size();

    std::vector<Pixel> framebuffer;

    // Pywal colors
    std::vector<RGB> pywal_colors;
    RGB bg_color;
    RGB fg_color;
    void load_pywal_colors();
    RGB interpolate_color(float t);

    // Data
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
    float zoom_level = 2.8f;
    float focal_offset = 5.0f;

    // Auto-rotation
    bool auto_rotate = true;
    float rotation_speed = 0.02f;

    void auto_rotate_step();
    void project();
    void clear_framebuffer();

    void draw_line(int x0, int y0, float z0,
                   int x1, int y1, float z1,
                   RGB color, float brightness);

    void plot_pixel(int x, int y, float z, RGB color, float brightness);

    RGB depth_shade(RGB color, float brightness);
    RGB get_color_for_point(int point_idx, int total_points);

    void render_braille();
    void draw_info_overlay();

    bool raw_mode_active = false;
};
