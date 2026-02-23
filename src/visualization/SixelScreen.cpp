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
#include <fstream>
#include <sstream>

static struct termios orig_termios;
static const float FOV = 90.0f;
static const float PI = 3.14159265359f;

// --- Terminal management ---

void SixelScreen::enter_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    write(STDOUT_FILENO, "\033[?25l", 6);
    write(STDOUT_FILENO, "\033[?1049h", 8);
    raw_mode_active = true;
}

void SixelScreen::exit_raw_mode() {
    if (!raw_mode_active) return;
    write(STDOUT_FILENO, "\033[?25h", 6);
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
            pixel_width = term_cols * 8;
            pixel_height = term_rows * 16;
        }
    }
    int panel_text_rows = 6 + (int)data.size() * 3;
    int panel_pixel_height = panel_text_rows * (pixel_height / term_rows);
    pixel_height = std::max(100, pixel_height - panel_pixel_height);
    pixel_width = std::min(pixel_width, 2000);
    pixel_height = std::min(pixel_height, 1500);
}

// --- Constructor / Destructor ---

SixelScreen::SixelScreen(const bool& show_structure, const std::string& mode) {
    screen_show_structure = show_structure;
    screen_mode = mode;
}

SixelScreen::~SixelScreen() {
    exit_raw_mode();
    for (Protein* p : data) delete p;
    data.clear();
    if (vectorpointer) {
        // Note: data is already cleared, use a saved size
        delete[] vectorpointer;
        vectorpointer = nullptr;
    }
}

// --- Data setup (mirrors Screen interface) ---

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

    query_terminal_size();
    framebuffer.resize(pixel_width * pixel_height, RGBA{0, 0, 0, 255});
    zbuffer.resize(pixel_width * pixel_height, std::numeric_limits<float>::infinity());
}

// --- Pixel operations ---

void SixelScreen::clear_framebuffer() {
    std::fill(framebuffer.begin(), framebuffer.end(), RGBA{0, 0, 0, 255});
    std::fill(zbuffer.begin(), zbuffer.end(), std::numeric_limits<float>::infinity());
}

void SixelScreen::plot_pixel(int x, int y, float z, RGBA color, float coverage) {
    if (x < 0 || x >= pixel_width || y < 0 || y >= pixel_height) return;
    int idx = y * pixel_width + x;
    if (z > zbuffer[idx] + 0.01f) return;

    float a = (color.a / 255.0f) * coverage;
    if (z < zbuffer[idx]) {
        zbuffer[idx] = z;
        framebuffer[idx].r = (uint8_t)(color.r * a);
        framebuffer[idx].g = (uint8_t)(color.g * a);
        framebuffer[idx].b = (uint8_t)(color.b * a);
        framebuffer[idx].a = (uint8_t)(a * 255);
    } else {
        RGBA& dst = framebuffer[idx];
        dst.r = (uint8_t)std::min(255.0f, dst.r + color.r * a * 0.5f);
        dst.g = (uint8_t)std::min(255.0f, dst.g + color.g * a * 0.5f);
        dst.b = (uint8_t)std::min(255.0f, dst.b + color.b * a * 0.5f);
        dst.a = (uint8_t)std::min(255, (int)dst.a + (int)(a * 128));
    }
}

// --- Anti-aliased line (Xiaolin Wu) ---

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

    float xend = (float)x0;
    float yend = y0 + gradient * (xend - x0);
    float zval = z0;
    int xpxl1 = (int)xend;
    int ypxl1 = (int)yend;
    float frac = yend - ypxl1;

    if (steep) {
        plot_pixel(ypxl1, xpxl1, zval, color, (1.0f - frac) * alpha_scale);
        plot_pixel(ypxl1 + 1, xpxl1, zval, color, frac * alpha_scale);
    } else {
        plot_pixel(xpxl1, ypxl1, zval, color, (1.0f - frac) * alpha_scale);
        plot_pixel(xpxl1, ypxl1 + 1, zval, color, frac * alpha_scale);
    }
    float intery = yend + gradient;
    zval += z_gradient;

    int xpxl2 = x1;

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

// --- Color assignment ---

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

// --- Projection ---

void SixelScreen::project() {
    float fovRads = 1.0f / tanf((FOV / zoom_level) * 0.5f / 180.0f * PI);

    for (size_t ii = 0; ii < data.size(); ii++) {
        Protein* target = data[ii];

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

            for (int i = 0; i < num_atoms; i++) {
                float* pos = chain_atoms[i].get_position();
                float x = pos[0];
                float y = pos[1];
                float z = pos[2] + focal_offset;

                float projX = (x / z) * fovRads + pan_x[ii];
                float projY = (y / z) * fovRads + pan_y[ii];
                int sx = (int)((projX + 1.0f) * 0.5f * pixel_width);
                int sy = (int)((1.0f - projY) * 0.5f * pixel_height);

                float min_z = target->get_scaled_min_z();
                float max_z = target->get_scaled_max_z();
                float zn = (max_z > min_z) ? ((z - focal_offset - min_z) / (max_z - min_z)) : 0.5f;
                zn = std::clamp(zn, 0.0f, 1.0f);
                float depth_alpha = 1.0f - zn * 0.7f;

                RGBA color = get_color_for_point((int)ii, chain_idx, global_atom_idx, total_atoms);
                color.a = (uint8_t)(depth_alpha * 255);

                if (prevSX >= 0) {
                    draw_line_aa(prevSX, prevSY, prevZ, sx, sy, z, color, depth_alpha);
                }

                prevSX = sx; prevSY = sy; prevZ = z;
                global_atom_idx++;
            }
            chain_idx++;
        }
    }
}

// --- Info panel (ANSI text below image) ---

void SixelScreen::draw_info_panel() {
    std::string out;
    out += "\033[0m";

    std::string sep(std::min(term_cols, 80), '-');
    out += sep + "\n";
    out += " WASD:move  XYZ:rotate  R/F:zoom  Q:quit\n";
    out += sep + "\n";

    for (size_t i = 0; i < data.size(); i++) {
        auto* p = data[i];
        int palette_idx = Palettes::UNRAINBOW[i % Palettes::UNRAINBOW.size()];
        const RGBA& c = Palettes::ID2RGBA[palette_idx];
        out += "\033[38;2;" + std::to_string(c.r) + ";" +
               std::to_string(c.g) + ";" + std::to_string(c.b) + "m";
        out += " " + p->get_file_name() + "\033[0m\n";

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

// --- Main draw ---

void SixelScreen::draw_screen() {
    clear_framebuffer();
    project();

    write(STDOUT_FILENO, "\033[H", 3);

    std::string sixel = SixelEncoder::encode(framebuffer, pixel_width, pixel_height);
    write(STDOUT_FILENO, sixel.c_str(), sixel.size());

    draw_info_panel();
}

// --- Input handling ---

bool SixelScreen::handle_input() {
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 0) <= 0) return true;

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
