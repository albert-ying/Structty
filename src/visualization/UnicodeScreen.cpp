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
    write(STDOUT_FILENO, "\033[?25l", 6);
    write(STDOUT_FILENO, "\033[?1049h", 8);
    std::string set_bg = "\033[48;2;" + std::to_string(bg_color.r) + ";" +
                         std::to_string(bg_color.g) + ";" +
                         std::to_string(bg_color.b) + "m";
    write(STDOUT_FILENO, set_bg.c_str(), set_bg.size());
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
    info_rows = 1 + (int)data.size();
    int render_rows = std::max(4, term_rows - info_rows);
    buf_width = term_cols * 2;
    buf_height = render_rows * 4;
}

// --- Colors ---

static RGB boost_color(RGB c, RGB bg) {
    // Ensure color is visible against bg by guaranteeing minimum contrast
    // Compute perceived luminance difference
    float c_lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    float bg_lum = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
    float diff = std::abs(c_lum - bg_lum);

    if (diff < 80.0f) {
        // Too close to bg — boost away from it
        float scale = (bg_lum < 128.0f) ? 1.6f : 0.6f;
        return {
            (uint8_t)std::clamp((int)(c.r * scale), 0, 255),
            (uint8_t)std::clamp((int)(c.g * scale), 0, 255),
            (uint8_t)std::clamp((int)(c.b * scale), 0, 255),
        };
    }
    return c;
}

void UnicodeScreen::load_colors() {
    // Fallback
    rainbow_colors = {
        {255, 70, 70}, {255, 140, 50}, {255, 210, 60}, {80, 220, 80},
        {50, 200, 220}, {80, 120, 255}, {160, 80, 255}, {255, 80, 180},
    };
    bg_color = {18, 18, 24};
    fg_color = {180, 180, 180};

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

    if (wal_colors.size() >= 16) {
        bg_color = wal_colors[0];
        fg_color = wal_colors[7];
        // Use all pywal accent colors (1-6 + 8 for contrast)
        // Boost each to ensure visibility against bg
        rainbow_colors.clear();
        // color8 first (typically most saturated/distinct)
        rainbow_colors.push_back(boost_color(wal_colors[8], bg_color));
        for (int i = 1; i <= 6; i++)
            rainbow_colors.push_back(boost_color(wal_colors[i], bg_color));
    } else if (wal_colors.size() >= 8) {
        bg_color = wal_colors[0];
        fg_color = wal_colors[7];
        rainbow_colors.clear();
        for (int i = 1; i <= 6; i++)
            rainbow_colors.push_back(boost_color(wal_colors[i], bg_color));
    }
}

RGB UnicodeScreen::interpolate_color(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    int n = (int)rainbow_colors.size();
    if (n == 0) return {255, 255, 255};
    if (n == 1) return rainbow_colors[0];

    float idx = t * (n - 1);
    int i = (int)idx;
    float f = idx - i;
    if (i >= n - 1) return rainbow_colors[n - 1];

    const RGB& a = rainbow_colors[i];
    const RGB& b = rainbow_colors[i + 1];
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
    load_colors();
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
    brightness = std::clamp(brightness, 0.45f, 1.0f);
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

// --- Drawing primitives ---

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
        plot_pixel(ix + 1, iy, z, color, brightness * 0.7f);
        plot_pixel(ix - 1, iy, z, color, brightness * 0.7f);
        plot_pixel(ix, iy + 1, z, color, brightness * 0.7f);
        plot_pixel(ix, iy - 1, z, color, brightness * 0.7f);
        x += xInc; y += yInc; z += zInc;
    }
}

void UnicodeScreen::draw_filled_circle(int cx, int cy, float z, int radius,
                                        RGB color, float brightness) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            float dist = sqrtf((float)(dx * dx + dy * dy));
            if (dist <= radius) {
                // Fade at edges for smooth look
                float edge = 1.0f - std::max(0.0f, (dist - radius + 1.5f) / 1.5f);
                plot_pixel(cx + dx, cy + dy, z, color, brightness * edge);
            }
        }
    }
}

// --- Color ---

RGB UnicodeScreen::get_color_for_point(int point_idx, int total_points) {
    if (total_points <= 1) return rainbow_colors[0];
    float t = (float)point_idx / (total_points - 1);
    return interpolate_color(t);
}

// --- Auto-rotation (around atom centroid) ---

void UnicodeScreen::auto_rotate_step() {
    if (!auto_rotate) return;

    float cosA = cosf(rotation_speed);
    float sinA = sinf(rotation_speed);

    for (auto* protein : data) {
        float cx = 0, cy = 0, cz = 0;
        int count = 0;
        for (auto& [chainID, chain_atoms] : protein->get_atoms()) {
            for (Atom& atom : chain_atoms) {
                cx += atom.x; cy += atom.y; cz += atom.z;
                count++;
            }
        }
        if (count == 0) continue;
        cx /= count; cy /= count; cz /= count;

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

// --- Projection helpers ---

struct ProjAtom {
    int sx, sy;
    float z;
    float brightness;
    RGB color;
};

static void project_atoms(std::vector<Protein*>& data,
                           std::vector<float>& pan_x,
                           float zoom_level, float focal_offset,
                           std::vector<float>& pan_y,
                           int buf_width, int buf_height,
                           std::vector<std::vector<ProjAtom>>& chains_out,
                           int& global_total) {
    global_total = 0;
    for (auto* p : data)
        for (const auto& [cid, atoms] : p->get_atoms())
            global_total += p->get_chain_length(cid);

    float fovRads = 1.0f / tanf((FOV / zoom_level) * 0.5f / 180.0f * PI);
    int global_idx = 0;

    for (size_t ii = 0; ii < data.size(); ii++) {
        Protein* target = data[ii];
        for (const auto& [chainID, chain_atoms] : target->get_atoms()) {
            int num_atoms = target->get_chain_length(chainID);
            if (num_atoms == 0) continue;

            std::vector<ProjAtom> chain;
            for (int i = 0; i < num_atoms; i++) {
                float* pos = chain_atoms[i].get_position();
                float x = pos[0], y = pos[1];
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

                chain.push_back({sx, sy, z, brightness, {0, 0, 0}});
                global_idx++;
            }
            chains_out.push_back(std::move(chain));
        }
    }
}

// --- View: Backbone ---

void UnicodeScreen::project_backbone() {
    std::vector<std::vector<ProjAtom>> chains;
    int global_total;
    project_atoms(data, pan_x, zoom_level, focal_offset, pan_y,
                  buf_width, buf_height, chains, global_total);

    int idx = 0;
    for (auto& chain : chains) {
        for (size_t i = 0; i < chain.size(); i++) {
            RGB color = get_color_for_point(idx, global_total);
            if (i > 0) {
                draw_line(chain[i-1].sx, chain[i-1].sy, chain[i-1].z,
                          chain[i].sx, chain[i].sy, chain[i].z,
                          color, chain[i].brightness);
            }
            idx++;
        }
    }
}

// --- View: Surface Grid (wireframe mesh) ---

void UnicodeScreen::project_grid() {
    std::vector<std::vector<ProjAtom>> chains;
    int global_total;
    project_atoms(data, pan_x, zoom_level, focal_offset, pan_y,
                  buf_width, buf_height, chains, global_total);

    // Flatten all projected atoms with their colors
    struct FlatAtom {
        int sx, sy;
        float z, brightness;
        float x3d, y3d, z3d;  // 3D position for neighbor search
        RGB color;
    };
    std::vector<FlatAtom> all_atoms;

    // Collect 3D positions alongside projected positions
    int global_idx = 0;
    for (size_t ii = 0; ii < data.size(); ii++) {
        Protein* target = data[ii];
        int chain_offset = 0;
        for (const auto& [chainID, chain_atoms] : target->get_atoms()) {
            int num_atoms = target->get_chain_length(chainID);
            for (int i = 0; i < num_atoms; i++) {
                float* pos = chain_atoms[i].get_position();
                // Find the matching projected atom
                if (chain_offset < (int)chains.size() && i < (int)chains[chain_offset].size()) {
                    auto& pa = chains[chain_offset][i];
                    RGB color = get_color_for_point(global_idx, global_total);
                    all_atoms.push_back({pa.sx, pa.sy, pa.z, pa.brightness,
                                         pos[0], pos[1], pos[2], color});
                }
                global_idx++;
            }
            chain_offset++;
        }
    }

    // Compute average sequential distance for threshold
    float avg_dist = 0;
    int dist_count = 0;
    for (size_t i = 1; i < all_atoms.size(); i++) {
        float dx = all_atoms[i].x3d - all_atoms[i-1].x3d;
        float dy = all_atoms[i].y3d - all_atoms[i-1].y3d;
        float dz = all_atoms[i].z3d - all_atoms[i-1].z3d;
        float d = sqrtf(dx*dx + dy*dy + dz*dz);
        if (d > 0.001f && d < 0.5f) {  // reasonable sequential distance
            avg_dist += d;
            dist_count++;
        }
    }
    float threshold = (dist_count > 0) ? (avg_dist / dist_count) * 2.5f : 0.15f;

    int n = (int)all_atoms.size();

    // Draw backbone connections (sequential)
    global_idx = 0;
    for (auto& chain : chains) {
        for (size_t i = 1; i < chain.size(); i++) {
            int ai = global_idx + (int)i - 1;
            int bi = global_idx + (int)i;
            if (ai >= 0 && ai < n && bi < n) {
                RGB color = all_atoms[bi].color;
                float br = (all_atoms[ai].brightness + all_atoms[bi].brightness) * 0.5f;
                draw_line(all_atoms[ai].sx, all_atoms[ai].sy, all_atoms[ai].z,
                          all_atoms[bi].sx, all_atoms[bi].sy, all_atoms[bi].z,
                          color, br);
            }
        }
        global_idx += (int)chain.size();
    }

    // Draw cross-connections (nearby non-sequential atoms)
    for (int i = 0; i < n; i++) {
        for (int j = i + 3; j < n; j++) {  // skip immediate neighbors
            float dx = all_atoms[i].x3d - all_atoms[j].x3d;
            float dy = all_atoms[i].y3d - all_atoms[j].y3d;
            float dz = all_atoms[i].z3d - all_atoms[j].z3d;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist < threshold) {
                RGB color = all_atoms[j].color;
                float br = (all_atoms[i].brightness + all_atoms[j].brightness) * 0.5f;
                // Thinner cross-links (draw without thickness expansion)
                int ddx = all_atoms[j].sx - all_atoms[i].sx;
                int ddy = all_atoms[j].sy - all_atoms[i].sy;
                int steps = std::max(abs(ddx), abs(ddy));
                if (steps == 0) continue;
                float xInc = (float)ddx / steps;
                float yInc = (float)ddy / steps;
                float zInc = (all_atoms[j].z - all_atoms[i].z) / steps;
                float px = (float)all_atoms[i].sx, py = (float)all_atoms[i].sy;
                float pz = all_atoms[i].z;
                for (int s = 0; s <= steps; s++) {
                    plot_pixel((int)(px + 0.5f), (int)(py + 0.5f), pz, color, br * 0.7f);
                    px += xInc; py += yInc; pz += zInc;
                }
            }
        }
    }

    // Draw dots at atom positions
    for (int i = 0; i < n; i++)
        draw_filled_circle(all_atoms[i].sx, all_atoms[i].sy, all_atoms[i].z,
                           1, all_atoms[i].color, all_atoms[i].brightness);
}

// --- View: Surface ---

void UnicodeScreen::project_surface() {
    std::vector<std::vector<ProjAtom>> chains;
    int global_total;
    project_atoms(data, pan_x, zoom_level, focal_offset, pan_y,
                  buf_width, buf_height, chains, global_total);

    // Larger radius circles that overlap to form surface
    // Scale radius by depth (closer = bigger)
    int idx = 0;
    for (auto& chain : chains) {
        for (auto& a : chain) {
            RGB color = get_color_for_point(idx, global_total);
            int radius = (int)(3.0f + a.brightness * 3.0f);  // 3-6 dots
            draw_filled_circle(a.sx, a.sy, a.z, radius, color, a.brightness);
            idx++;
        }
    }
}

// --- Braille rendering ---

void UnicodeScreen::render_braille() {
    std::string out;
    out.reserve(term_cols * (term_rows - info_rows) * 30);
    out += "\033[H";

    int cell_rows = buf_height / 4;
    int cell_cols = buf_width / 2;

    static const int dot_bits[2][4] = {
        {0x01, 0x02, 0x04, 0x40},
        {0x08, 0x10, 0x20, 0x80},
    };

    for (int cr = 0; cr < cell_rows; cr++) {
        for (int cc = 0; cc < cell_cols; cc++) {
            int pattern = 0;
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
                out += "\033[38;2;";
                out += std::to_string(best_color.r) + ";";
                out += std::to_string(best_color.g) + ";";
                out += std::to_string(best_color.b) + "m";
                out += "\033[48;2;";
                out += std::to_string(bg_color.r) + ";";
                out += std::to_string(bg_color.g) + ";";
                out += std::to_string(bg_color.b) + "m";
                int codepoint = 0x2800 + pattern;
                out += (char)(0xE0 | ((codepoint >> 12) & 0x0F));
                out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                out += (char)(0x80 | (codepoint & 0x3F));
            } else {
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

// --- View mode name ---

const char* UnicodeScreen::view_mode_name() {
    switch (view_mode) {
        case ViewMode::BACKBONE: return "backbone";
        case ViewMode::GRID:    return "grid";
        case ViewMode::SURFACE: return "surface";
    }
    return "unknown";
}

// --- Info overlay ---

void UnicodeScreen::draw_info_overlay() {
    std::string out;
    out += "\033[0m\n";

    for (size_t i = 0; i < data.size(); i++) {
        auto* p = data[i];
        std::string name = p->get_file_name();
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);

        RGB nc = interpolate_color((float)i / std::max(1, (int)data.size() - 1));
        out += "\033[38;2;" + std::to_string(nc.r) + ";" +
               std::to_string(nc.g) + ";" +
               std::to_string(nc.b) + "m";
        out += " " + name;

        // Dimmed summary
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

        // View mode indicator (dimmer)
        out += "  \033[38;2;" + std::to_string(fg_color.r / 3) + ";" +
               std::to_string(fg_color.g / 3) + ";" +
               std::to_string(fg_color.b / 3) + "m";
        out += "[" + std::string(view_mode_name()) + "]";

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

    switch (view_mode) {
        case ViewMode::BACKBONE: project_backbone(); break;
        case ViewMode::GRID:     project_grid();     break;
        case ViewMode::SURFACE:  project_surface();  break;
    }

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
        case 'v': case 'V': {
            int m = (int)view_mode;
            m = (m + 1) % 3;
            view_mode = (ViewMode)m;
            break;
        }
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
