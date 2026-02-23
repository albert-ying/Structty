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

    void enter_raw_mode();
    void exit_raw_mode();

private:
    int pixel_width = 800;
    int pixel_height = 600;
    int term_cols = 80;
    int term_rows = 24;

    void query_terminal_size();

    std::vector<RGBA> framebuffer;
    std::vector<float> zbuffer;

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

    void project();
    void clear_framebuffer();

    void draw_line_aa(int x0, int y0, float z0,
                      int x1, int y1, float z1,
                      RGBA color, float alpha_scale);

    void plot_pixel(int x, int y, float z, RGBA color, float coverage);

    RGBA get_color_for_point(int protein_idx, int chain_idx, int point_idx, int total_points);

    void draw_info_panel();

    bool raw_mode_active = false;
};
