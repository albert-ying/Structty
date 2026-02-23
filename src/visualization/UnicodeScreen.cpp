#include "UnicodeScreen.hpp"
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
#include <cstdlib>

static struct termios orig_termios;
static const float FOV = 90.0f;
static const float PI = 3.14159265359f;

// --- Terminal management ---

void UnicodeScreen::enter_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    // Hide cursor, alternate screen
    write(STDOUT_FILENO, "\033[?25l", 6);
    write(STDOUT_FILENO, "\033[?1049h", 8);
    // Set terminal background to pywal bg
    std::string set_bg = "\033[48;2;" + std::to_string(bg_color.r) + ";" +
                         std::to_string(bg_color.g) + ";" +
                         std::to_string(bg_color.b) + "m";
    write(STDOUT_FILENO, set_bg.c_str(), set_bg.size());
    // Clear entire screen with bg color
    write(STDOUT_FILENO, "\033[2J", 4);
    raw_mode_active = true;
}

void UnicodeScreen::exit_raw_mode() {
    if (!raw_mode_active) return;
    write(STDOUT_FILENO, "\033[?25h", 6);
    write(STDOUT_FILENO, "\033[?1049l", 8);
    write(STDOUT_FILENO, "\033[0m", 4);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_mode_active = false;
}

void UnicodeScreen::query_terminal_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_cols = ws.ws_col;
        term_rows = ws.ws_row;
    }
    // Reserve bottom rows for protein info
    info_rows = 1 + (int)data.size();
    int render_rows = std::max(4, term_rows - info_rows);
    // Braille: 2 dots wide per cell, 4 dots tall per cell
    buf_width = term_cols * 2;
    buf_height = render_rows * 4;
}

// --- Pywal colors ---

void UnicodeScreen::load_pywal_colors() {
    pywal_colors = {
        {220, 50, 47}, {203, 75, 22}, {181, 137, 0},
        {133, 153, 0}, {42, 161, 152}, {38, 139, 210},
        {108, 113, 196}, {211, 54, 130},
    };
    bg_color = {0, 0, 0};
    fg_color = {197, 195, 196};

    const char* home = getenv("HOME");
    if (!home) return;

    std::string colors_path = std::string(home) + "/.cache/wal/colors";
    std::ifstream file(colors_path);
    if (!file.is_open()) return;

    std::vector<RGB> wal_colors;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] != '#' || line.size() < 7) continue;
        unsigned int r, g, b;
        if (sscanf(line.c_str(), "#%02x%02x%02x", &r, &g, &b) == 3)
            wal_colors.push_back({(uint8_t)r, (uint8_t)g, (uint8_t)b});
    }

    if (wal_colors.size() >= 8) {
        bg_color = wal_colors[0];
        fg_color = wal_colors[7];
        pywal_colors.clear();
        for (int i = 1; i <= 6; i++)
            pywal_colors.push_back(wal_colors[i]);
    }
}

RGB UnicodeScreen::interpolate_color(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    int n = (int)pywal_colors.size();
    if (n == 0) return {255, 255, 255};
    if (n == 1) return pywal_colors[0];

    float idx = t * (n - 1);
    int i = (int)idx;
    float f = idx - i;
    if (i >= n - 1) return pywal_colors[n - 1];

    const RGB& a = pywal_colors[i];
    const RGB& b = pywal_colors[i + 1];
    return {
        (uint8_t)(a.r + (b.r - a.r) * f),
        (uint8_t)(a.g + (b.g - a.g) * f),
        (uint8_t)(a.b + (b.b - a.b) * f),
    };
}

// --- Constructor / Destructor ---

UnicodeScreen::UnicodeScreen(const bool& show_structure, const std::string& mode) {
    screen_show_structure = show_structure;
    screen_mode = mode;
    load_pywal_colors();
}

UnicodeScreen::~UnicodeScreen() {
    exit_raw_mode();
    if (vectorpointer) {
        for (size_t i = 0; i < data.size(); i++) delete[] vectorpointer[i];
        delete[] vectorpointer;
        vectorpointer = nullptr;
    }
    for (Protein* p : data) delete p;
    data.clear();
}

// --- Data setup ---

void UnicodeScreen::set_protein(const std::string& in_file, int ii, const bool& show_structure) {
    Protein* protein = new Protein(in_file, chainVec.at(ii), show_structure);
    data.push_back(protein);
    pan_x.push_back(0.0f);
    pan_y.push_back(0.0f);
}

void UnicodeScreen::set_tmatrix() {
    size_t filenum = data.size();
    vectorpointer = new float*[filenum];
    for (size_t i = 0; i < filenum; i++)
        vectorpointer[i] = new float[3]{0, 0, 0};
}

void UnicodeScreen::set_chainfile(const std::string& chainfile, int filesize) {
    for (int i = 0; i < filesize; i++) chainVec.push_back("-");
    if (chainfile.empty()) return;
    std::ifstream file(chainfile);
    if (!file.is_open()) return;
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

void UnicodeScreen::set_utmatrix(const std::string& utmatrix, bool applyUT) {
    yesUT = !utmatrix.empty();
    const size_t filenum = data.size();
    float** matrixpointer = new float*[filenum];
    for (size_t i = 0; i < filenum; i++) {
        matrixpointer[i] = new float[9];
        for (int j = 0; j < 9; j++)
            matrixpointer[i][j] = (j % 4 == 0) ? 1.f : 0.f;
    }
    if (utmatrix.empty()) { for (size_t i = 0; i < filenum; i++) delete[] matrixpointer[i]; delete[] matrixpointer; return; }
    std::ifstream file(utmatrix);
    if (!file.is_open()) { for (size_t i = 0; i < filenum; i++) delete[] matrixpointer[i]; delete[] matrixpointer; return; }
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

void UnicodeScreen::normalize_proteins(const std::string& utmatrix) {
    const bool hasUT = !utmatrix.empty();
    for (size_t i = 0; i < data.size(); i++)
        data[i]->load_data(vectorpointer[i], yesUT);
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
    framebuffer.resize(buf_width * buf_height, {0, 0, 0, 0.0f, false});
}

// --- Pixel operations ---

void UnicodeScreen::clear_framebuffer() {
    std::fill(framebuffer.begin(), framebuffer.end(), Pixel{0, 0, 0, 0.0f, false});
}

RGB UnicodeScreen::depth_shade(RGB color, float brightness) {
    brightness = std::clamp(brightness, 0.25f, 1.0f);
    return {
        (uint8_t)(color.r * brightness),
        (uint8_t)(color.g * brightness),
        (uint8_t)(color.b * brightness),
    };
}

void UnicodeScreen::plot_pixel(int x, int y, float z, RGB color, float brightness) {
    if (x < 0 || x >= buf_width || y < 0 || y >= buf_height) return;
    int idx = y * buf_width + x;
    if (framebuffer[idx].active && z > framebuffer[idx].depth + 0.01f) return;

    RGB shaded = depth_shade(color, brightness);
    framebuffer[idx] = {shaded.r, shaded.g, shaded.b, z, true};
}

// --- Line drawing (Bresenham with thickness) ---

void UnicodeScreen::draw_line(int x0, int y0, float z0,
                               int x1, int y1, float z1,
                               RGB color, float brightness) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = std::max(abs(dx), abs(dy));
    if (steps == 0) { plot_pixel(x0, y0, z0, color, brightness); return; }

    float xInc = (float)dx / steps;
    float yInc = (float)dy / steps;
    float zInc = (z1 - z0) / steps;

    float x = (float)x0, y = (float)y0, z = z0;

    for (int i = 0; i <= steps; i++) {
        int ix = (int)(x + 0.5f);
        int iy = (int)(y + 0.5f);
        plot_pixel(ix, iy, z, color, brightness);

        // Thicken lines: draw 1px border for visual weight
        plot_pixel(ix + 1, iy, z, color, brightness * 0.6f);
        plot_pixel(ix - 1, iy, z, color, brightness * 0.6f);
        plot_pixel(ix, iy + 1, z, color, brightness * 0.6f);
        plot_pixel(ix, iy - 1, z, color, brightness * 0.6f);

        x += xInc;
        y += yInc;
        z += zInc;
    }
}

// --- Color for backbone position ---

RGB UnicodeScreen::get_color_for_point(int point_idx, int total_points) {
    if (total_points <= 1) return pywal_colors[0];
    float t = (float)point_idx / (total_points - 1);
    return interpolate_color(t);
}

// --- Auto-rotation (around protein centroid) ---

void UnicodeScreen::auto_rotate_step() {
    if (!auto_rotate) return;

    float cosA = cosf(rotation_speed);
    float sinA = sinf(rotation_speed);

    for (auto* protein : data) {
        // Compute centroid directly from current atom positions
        float cx = 0, cy = 0, cz = 0;
        int count = 0;
        for (auto& [chainID, chain_atoms] : protein->get_atoms()) {
            for (Atom& atom : chain_atoms) {
                cx += atom.x;
                cy += atom.y;
                cz += atom.z;
                count++;
            }
        }
        if (count == 0) continue;
        cx /= count;
        cy /= count;
        cz /= count;

        // Y-axis rotation around centroid
        for (auto& [chainID, chain_atoms] : protein->get_atoms()) {
            for (Atom& atom : chain_atoms) {
                float dx = atom.x - cx;
                float dz = atom.z - cz;
                atom.x = cx + dx * cosA + dz * sinA;
                atom.z = cz - dx * sinA + dz * cosA;
            }
        }
    }
}

// --- Projection ---

void UnicodeScreen::project() {
    float fovRads = 1.0f / tanf((FOV / zoom_level) * 0.5f / 180.0f * PI);

    int global_total = 0;
    for (auto* p : data)
        for (const auto& [cid, atoms] : p->get_atoms())
            global_total += p->get_chain_length(cid);

    int global_idx = 0;

    for (size_t ii = 0; ii < data.size(); ii++) {
        Protein* target = data[ii];

        for (const auto& [chainID, chain_atoms] : target->get_atoms()) {
            int num_atoms = target->get_chain_length(chainID);
            if (num_atoms == 0) continue;

            int prevSX = -1, prevSY = -1;
            float prevZ = 0;

            for (int i = 0; i < num_atoms; i++) {
                float* pos = chain_atoms[i].get_position();
                float x = pos[0];
                float y = pos[1];
                float z = pos[2] + focal_offset;

                float projX = (x / z) * fovRads + pan_x[ii];
                float projY = (y / z) * fovRads + pan_y[ii];
                int sx = (int)((projX + 1.0f) * 0.5f * buf_width);
                int sy = (int)((1.0f - projY) * 0.5f * buf_height);

                float min_z = target->get_scaled_min_z();
                float max_z = target->get_scaled_max_z();
                float zn = (max_z > min_z) ? ((z - focal_offset - min_z) / (max_z - min_z)) : 0.5f;
                zn = std::clamp(zn, 0.0f, 1.0f);
                float brightness = 1.0f - zn * 0.65f;

                RGB color = get_color_for_point(global_idx, global_total);

                if (prevSX >= 0)
                    draw_line(prevSX, prevSY, prevZ, sx, sy, z, color, brightness);

                prevSX = sx; prevSY = sy; prevZ = z;
                global_idx++;
            }
        }
    }
}

// --- Braille rendering ---
// Each cell = 2 wide x 4 tall dots
// Braille char = U+2800 + bitmask
// Dot positions:  bit0=(0,0) bit1=(0,1) bit2=(0,2) bit3=(1,0)
//                 bit4=(1,1) bit5=(1,2) bit6=(0,3) bit7=(1,3)

void UnicodeScreen::render_braille() {
    std::string out;
    out.reserve(term_cols * (term_rows - info_rows) * 30);
    out += "\033[H";

    int cell_rows = buf_height / 4;
    int cell_cols = buf_width / 2;

    // Braille dot-to-bit mapping: dot(col, row) -> bit
    // col=0: rows 0,1,2,3 -> bits 0,1,2,6
    // col=1: rows 0,1,2,3 -> bits 3,4,5,7
    static const int dot_bits[2][4] = {
        {0x01, 0x02, 0x04, 0x40},  // col 0: bits 0,1,2,6
        {0x08, 0x10, 0x20, 0x80},  // col 1: bits 3,4,5,7
    };

    for (int cr = 0; cr < cell_rows; cr++) {
        for (int cc = 0; cc < cell_cols; cc++) {
            int pattern = 0;
            // Find frontmost active pixel for color
            float best_depth = std::numeric_limits<float>::infinity();
            RGB best_color = {0, 0, 0};
            bool any_active = false;

            for (int dc = 0; dc < 2; dc++) {
                for (int dr = 0; dr < 4; dr++) {
                    int px = cc * 2 + dc;
                    int py = cr * 4 + dr;
                    if (px >= buf_width || py >= buf_height) continue;
                    int idx = py * buf_width + px;
                    if (framebuffer[idx].active) {
                        pattern |= dot_bits[dc][dr];
                        any_active = true;
                        if (framebuffer[idx].depth < best_depth) {
                            best_depth = framebuffer[idx].depth;
                            best_color = {framebuffer[idx].r, framebuffer[idx].g, framebuffer[idx].b};
                        }
                    }
                }
            }

            if (any_active) {
                // Set fg color + bg color, then braille char
                out += "\033[38;2;";
                out += std::to_string(best_color.r) + ";";
                out += std::to_string(best_color.g) + ";";
                out += std::to_string(best_color.b) + "m";
                out += "\033[48;2;";
                out += std::to_string(bg_color.r) + ";";
                out += std::to_string(bg_color.g) + ";";
                out += std::to_string(bg_color.b) + "m";

                // Encode braille: U+2800 + pattern
                // UTF-8: 0xE2 0xA0+high 0x80+low
                int codepoint = 0x2800 + pattern;
                out += (char)(0xE0 | ((codepoint >> 12) & 0x0F));
                out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                out += (char)(0x80 | (codepoint & 0x3F));
            } else {
                // Empty cell: bg color space
                out += "\033[48;2;";
                out += std::to_string(bg_color.r) + ";";
                out += std::to_string(bg_color.g) + ";";
                out += std::to_string(bg_color.b) + "m ";
            }
        }
        if (cr < cell_rows - 1) out += "\033[0m\n";
    }

    out += "\033[0m";
    write(STDOUT_FILENO, out.c_str(), out.size());
}

// --- Info overlay (protein name + chain info, no controls) ---

void UnicodeScreen::draw_info_overlay() {
    std::string out;
    out += "\033[0m\n";

    for (size_t i = 0; i < data.size(); i++) {
        auto* p = data[i];
        std::string name = p->get_file_name();
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);

        // Protein name in gradient color
        RGB nc = interpolate_color((float)i / std::max(1, (int)data.size() - 1));
        out += "\033[38;2;" + std::to_string(nc.r) + ";" +
               std::to_string(nc.g) + ";" +
               std::to_string(nc.b) + "m";
        out += " " + name;

        // Chain/residue summary in dimmed fg
        out += "\033[38;2;" + std::to_string(fg_color.r * 2 / 3) + ";" +
               std::to_string(fg_color.g * 2 / 3) + ";" +
               std::to_string(fg_color.b * 2 / 3) + "m";

        auto chain_lengths = p->get_chain_length();
        auto residue_counts = p->get_residue_count();
        int total_res = 0, total_chains = 0;
        for (const auto& [cid, len] : chain_lengths) {
            total_chains++;
            auto it = residue_counts.find(cid);
            if (it != residue_counts.end()) total_res += it->second;
        }
        out += "  " + std::to_string(total_chains) + " chain" +
               (total_chains > 1 ? "s" : "") + ", " +
               std::to_string(total_res) + " residues";
        out += "\033[0m";
        if (i < data.size() - 1) out += "\n";
    }

    write(STDOUT_FILENO, out.c_str(), out.size());
}

// --- Main draw ---

void UnicodeScreen::draw_screen() {
    int old_w = buf_width, old_h = buf_height;
    query_terminal_size();
    if (buf_width != old_w || buf_height != old_h)
        framebuffer.resize(buf_width * buf_height);

    auto_rotate_step();
    clear_framebuffer();
    project();
    render_braille();
    draw_info_overlay();
}

// --- Input handling ---

bool UnicodeScreen::handle_input() {
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 0) <= 0) return true;

    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return true;

    float pan_step = 0.05f;

    switch (c) {
        case '0': structNum = -1; break;
        case '1': case '2': case '3': case '4': case '5': case '6':
            if (c - '0' <= (int)data.size()) structNum = c - '1';
            break;
        case 'a': case 'A':
            if (structNum >= 0) pan_x[structNum] -= pan_step;
            else for (auto& px : pan_x) px -= pan_step;
            break;
        case 'd': case 'D':
            if (structNum >= 0) pan_x[structNum] += pan_step;
            else for (auto& px : pan_x) px += pan_step;
            break;
        case 'w': case 'W':
            if (structNum >= 0) pan_y[structNum] += pan_step;
            else for (auto& py : pan_y) py += pan_step;
            break;
        case 's': case 'S':
            if (structNum >= 0) pan_y[structNum] -= pan_step;
            else for (auto& py : pan_y) py -= pan_step;
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
        case ' ':
            auto_rotate = !auto_rotate;
            break;
        case 'q': case 'Q':
            return false;
    }
    return true;
}
