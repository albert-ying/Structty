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
    raw_mode_active = true;
}

void UnicodeScreen::exit_raw_mode() {
    if (!raw_mode_active) return;
    // Show cursor, exit alternate screen, reset colors
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
    // Reserve bottom 4 rows for info overlay
    int render_rows = std::max(4, term_rows - 4);
    buf_width = term_cols;
    buf_height = render_rows * 2;  // half-block doubles vertical resolution
}

// --- Pywal colors ---

void UnicodeScreen::load_pywal_colors() {
    // Default fallback: warm rainbow
    pywal_colors = {
        {220, 50, 47},   // red
        {203, 75, 22},   // orange
        {181, 137, 0},   // yellow
        {133, 153, 0},   // green
        {42, 161, 152},  // cyan
        {38, 139, 210},  // blue
        {108, 113, 196}, // violet
        {211, 54, 130},  // magenta
    };
    bg_color = {0, 0, 0};
    fg_color = {197, 195, 196};

    // Try to read pywal colors
    const char* home = getenv("HOME");
    if (!home) return;

    std::string colors_path = std::string(home) + "/.cache/wal/colors";
    std::ifstream file(colors_path);
    if (!file.is_open()) return;

    std::vector<RGB> wal_colors;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] != '#') continue;
        if (line.size() < 7) continue;
        unsigned int r, g, b;
        if (sscanf(line.c_str(), "#%02x%02x%02x", &r, &g, &b) == 3) {
            wal_colors.push_back({(uint8_t)r, (uint8_t)g, (uint8_t)b});
        }
    }

    if (wal_colors.size() >= 8) {
        bg_color = wal_colors[0];
        fg_color = wal_colors[7];
        // Use colors 1-6 for the gradient (skip bg and fg)
        pywal_colors.clear();
        for (int i = 1; i <= 6; i++) {
            pywal_colors.push_back(wal_colors[i]);
        }
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
    for (Protein* p : data) delete p;
    data.clear();
    if (vectorpointer) {
        for (size_t i = 0; i < data.size(); i++) delete[] vectorpointer[i];
        delete[] vectorpointer;
        vectorpointer = nullptr;
    }
}

// --- Data setup (mirrors Screen interface) ---

void UnicodeScreen::set_protein(const std::string& in_file, int ii, const bool& show_structure) {
    Protein* protein = new Protein(in_file, chainVec.at(ii), show_structure);
    data.push_back(protein);
    pan_x.push_back(0.0f);
    pan_y.push_back(0.0f);
}

void UnicodeScreen::set_tmatrix() {
    size_t filenum = data.size();
    vectorpointer = new float*[filenum];
    for (size_t i = 0; i < filenum; i++) {
        vectorpointer[i] = new float[3]{0, 0, 0};
    }
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
    framebuffer.resize(buf_width * buf_height, {0, 0, 0, 0.0f, false});
}

// --- Pixel operations ---

void UnicodeScreen::clear_framebuffer() {
    std::fill(framebuffer.begin(), framebuffer.end(), Pixel{bg_color.r, bg_color.g, bg_color.b, 0.0f, false});
}

RGB UnicodeScreen::depth_shade(RGB color, float brightness) {
    brightness = std::clamp(brightness, 0.2f, 1.0f);
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

        // Draw main pixel + neighbors for thickness
        float t = (steps > 0) ? (float)i / steps : 0.0f;
        float br = brightness * (1.0f - t * 0.0f); // uniform brightness along line
        plot_pixel(ix, iy, z, color, br);

        // Thicken: draw adjacent pixels with slightly less brightness
        if (abs(dx) >= abs(dy)) {
            plot_pixel(ix, iy - 1, z, color, br * 0.5f);
            plot_pixel(ix, iy + 1, z, color, br * 0.5f);
        } else {
            plot_pixel(ix - 1, iy, z, color, br * 0.5f);
            plot_pixel(ix + 1, iy, z, color, br * 0.5f);
        }

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

// --- Auto-rotation ---

void UnicodeScreen::auto_rotate_step() {
    if (!auto_rotate) return;

    float angle = rotation_speed;
    float cosA = cosf(angle);
    float sinA = sinf(angle);

    for (auto* protein : data) {
        // Compute center of bounding box
        protein->set_bounding_box();
        BoundingBox& bb = protein->get_bounding_box();
        float cx = (bb.min_x + bb.max_x) * 0.5f;
        float cy = (bb.min_y + bb.max_y) * 0.5f;
        float cz = (bb.min_z + bb.max_z) * 0.5f;

        // Y-axis rotation around center
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

    // Count total atoms across all proteins for global rainbow
    int global_total = 0;
    for (auto* p : data) {
        for (const auto& [cid, atoms] : p->get_atoms())
            global_total += p->get_chain_length(cid);
    }

    int global_idx = 0;

    for (size_t ii = 0; ii < data.size(); ii++) {
        Protein* target = data[ii];

        for (const auto& [chainID, chain_atoms] : target->get_atoms()) {
            int num_atoms = target->get_chain_length(chainID);
            if (num_atoms == 0) continue;

            int prevSX = -1, prevSY = -1;
            float prevZ = 0;
            RGB prevColor = {0, 0, 0};
            float prevBr = 0;

            for (int i = 0; i < num_atoms; i++) {
                float* pos = chain_atoms[i].get_position();
                float x = pos[0];
                float y = pos[1];
                float z = pos[2] + focal_offset;

                float projX = (x / z) * fovRads + pan_x[ii];
                float projY = (y / z) * fovRads + pan_y[ii];
                int sx = (int)((projX + 1.0f) * 0.5f * buf_width);
                int sy = (int)((1.0f - projY) * 0.5f * buf_height);

                // Depth-based brightness
                float min_z = target->get_scaled_min_z();
                float max_z = target->get_scaled_max_z();
                float zn = (max_z > min_z) ? ((z - focal_offset - min_z) / (max_z - min_z)) : 0.5f;
                zn = std::clamp(zn, 0.0f, 1.0f);
                float brightness = 1.0f - zn * 0.7f;  // near=1.0, far=0.3

                RGB color = get_color_for_point(global_idx, global_total);

                if (prevSX >= 0) {
                    draw_line(prevSX, prevSY, prevZ, sx, sy, z, color, brightness);
                }

                prevSX = sx; prevSY = sy; prevZ = z;
                prevColor = color; prevBr = brightness;
                global_idx++;
            }
        }
    }
}

// --- Half-block rendering ---

void UnicodeScreen::render_halfblocks() {
    std::string out;
    out.reserve(buf_width * term_rows * 40);  // rough estimate

    out += "\033[H";  // cursor home

    int render_rows = buf_height / 2;

    for (int row = 0; row < render_rows; row++) {
        for (int col = 0; col < buf_width; col++) {
            int top_idx = (row * 2) * buf_width + col;
            int bot_idx = (row * 2 + 1) * buf_width + col;

            Pixel& top = framebuffer[top_idx];
            Pixel& bot = framebuffer[bot_idx];

            if (!top.active && !bot.active) {
                // Both background
                out += "\033[48;2;";
                out += std::to_string(bg_color.r) + ";" +
                       std::to_string(bg_color.g) + ";" +
                       std::to_string(bg_color.b) + "m ";
            } else if (top.active && !bot.active) {
                // Top pixel only: use ▀ with fg=top, bg=bg
                out += "\033[38;2;";
                out += std::to_string(top.r) + ";" +
                       std::to_string(top.g) + ";" +
                       std::to_string(top.b) + "m";
                out += "\033[48;2;";
                out += std::to_string(bg_color.r) + ";" +
                       std::to_string(bg_color.g) + ";" +
                       std::to_string(bg_color.b) + "m";
                out += "\xe2\x96\x80";  // ▀ (U+2580)
            } else if (!top.active && bot.active) {
                // Bottom pixel only: use ▄ with fg=bot, bg=bg
                out += "\033[38;2;";
                out += std::to_string(bot.r) + ";" +
                       std::to_string(bot.g) + ";" +
                       std::to_string(bot.b) + "m";
                out += "\033[48;2;";
                out += std::to_string(bg_color.r) + ";" +
                       std::to_string(bg_color.g) + ";" +
                       std::to_string(bg_color.b) + "m";
                out += "\xe2\x96\x84";  // ▄ (U+2584)
            } else {
                // Both active: use ▀ with fg=top, bg=bot
                out += "\033[38;2;";
                out += std::to_string(top.r) + ";" +
                       std::to_string(top.g) + ";" +
                       std::to_string(top.b) + "m";
                out += "\033[48;2;";
                out += std::to_string(bot.r) + ";" +
                       std::to_string(bot.g) + ";" +
                       std::to_string(bot.b) + "m";
                out += "\xe2\x96\x80";  // ▀ (U+2580)
            }
        }
        if (row < render_rows - 1) out += "\033[0m\n";
    }

    out += "\033[0m";
    write(STDOUT_FILENO, out.c_str(), out.size());
}

// --- Info overlay ---

void UnicodeScreen::draw_info_overlay() {
    std::string out;

    // Move to info area (below the rendered image)
    out += "\033[0m\n";

    // Separator
    std::string sep(std::min(term_cols, 60), '\xe2');
    // Use thin line: ─ (U+2500)
    out += "\033[38;2;" + std::to_string(fg_color.r) + ";" +
           std::to_string(fg_color.g) + ";" +
           std::to_string(fg_color.b) + "m";

    for (int i = 0; i < std::min(term_cols, 60); i++)
        out += "\xe2\x94\x80";  // ─
    out += "\n";

    // Protein info
    for (size_t i = 0; i < data.size(); i++) {
        auto* p = data[i];
        std::string name = p->get_file_name();
        // Strip path
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);

        // Color the name with first pywal gradient color
        RGB nc = interpolate_color((float)i / std::max(1, (int)data.size() - 1));
        out += "\033[38;2;" + std::to_string(nc.r) + ";" +
               std::to_string(nc.g) + ";" +
               std::to_string(nc.b) + "m";
        out += " " + name;

        // Chain/residue info in fg color
        out += "\033[38;2;" + std::to_string(fg_color.r) + ";" +
               std::to_string(fg_color.g) + ";" +
               std::to_string(fg_color.b) + "m";

        auto chain_lengths = p->get_chain_length();
        auto residue_counts = p->get_residue_count();
        out += "  ";
        int count = 0;
        for (const auto& [cid, len] : chain_lengths) {
            int res = 0;
            auto it = residue_counts.find(cid);
            if (it != residue_counts.end()) res = it->second;
            out += cid + ":" + std::to_string(res) + " ";
            count++;
            if (count > 8) { out += "..."; break; }
        }
        out += "\n";
    }

    // Controls
    out += "\033[38;2;" + std::to_string(fg_color.r / 2) + ";" +
           std::to_string(fg_color.g / 2) + ";" +
           std::to_string(fg_color.b / 2) + "m";
    out += " WASD:pan  XYZ:rotate  R/F:zoom  Space:auto  Q:quit\033[0m";

    write(STDOUT_FILENO, out.c_str(), out.size());
}

// --- Main draw ---

void UnicodeScreen::draw_screen() {
    // Re-query terminal size each frame for resize support
    int old_w = buf_width, old_h = buf_height;
    query_terminal_size();
    if (buf_width != old_w || buf_height != old_h) {
        framebuffer.resize(buf_width * buf_height);
    }

    auto_rotate_step();
    clear_framebuffer();
    project();
    render_halfblocks();
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
