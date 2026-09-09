// keqingsay -- a cowsay-like CLI featuring Keqing ASCII-art animations.

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "frames_data.hpp"

namespace {

constexpr int ANIM_W = 64;
constexpr int ANIM_H = 29;
constexpr int MIN_TERM_W = 72;
constexpr int MIN_TERM_H = 30;

struct FrameSet {
    const std::string_view *frames;
    int count;
    int interval_ms;
};

const FrameSet &pick_set(int variant) {
    static const FrameSet s1{SET1, static_cast<int>(std::size(SET1)), 60};
    static const FrameSet s2{SET2, static_cast<int>(std::size(SET2)), 100};
    static const FrameSet s3{SET3, static_cast<int>(std::size(SET3)), 40};
    if (variant == 2)
        return s2;
    if (variant == 3)
        return s3;
    return s1;
}

std::vector<std::string_view> split_lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (true) {
        size_t nl = s.find('\n', start);
        if (nl == std::string_view::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

int sat_sub(int a, int b) { return a > b ? a - b : 0; }

std::string move_to(int x, int y) {
    return "\033[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
}

// speech bubble

std::vector<std::string> wrap_text(const std::string &text, size_t max_width) {
    std::vector<std::string> lines;
    std::string cur;
    std::istringstream iss(text);
    std::string word;
    while (iss >> word) {
        if (cur.empty()) {
            cur = word;
        } else if (cur.size() + word.size() < max_width) {
            cur += ' ';
            cur += word;
        } else {
            lines.push_back(cur);
            cur = word;
        }
    }
    if (!cur.empty())
        lines.push_back(cur);
    if (lines.empty())
        lines.push_back("");
    return lines;
}

std::vector<std::string> speech_bubble(const std::string &text) {
    auto lines = wrap_text(text, 30);
    size_t width = 1;
    for (const auto &l : lines)
        width = std::max(width, l.size());

    std::string bar;
    for (size_t i = 0; i < width + 2; i++)
        bar += "─"; // ─

    std::vector<std::string> b;
    b.push_back("┌" + bar + "┐"); // ┌ ┐
    for (const auto &l : lines) {
        std::string padded = l;
        padded.append(width - l.size(), ' ');
        b.push_back("│ " + padded + " │"); // │ │
    }
    b.push_back("└" + bar + "┘"); // └ ┘
    // tail points left toward the character
    b.push_back("   /");
    b.push_back("  /");
    b.push_back(" /");
    return b;
}

// terminal control

termios g_orig_termios;
bool g_raw_active = false;

bool term_size(int &w, int &h) {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0)
        return false;
    w = ws.ws_col;
    h = ws.ws_row;
    return true;
}

bool terminal_big_enough() {
    int w = 0, h = 0;
    if (!term_size(w, h))
        return false;
    return w >= MIN_TERM_W && h >= MIN_TERM_H;
}

bool setup_terminal() {
    std::cout << "\033[?1049h\033[?25l"
              << std::flush; // alt screen, hide cursor
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0)
        return false;
    termios raw = g_orig_termios;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return false;
    g_raw_active = true;
    return true;
}

bool cleanup_terminal() {
    if (g_raw_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_raw_active = false;
    }
    std::cout << "\033[?25h\033[?1049l" << std::flush; // show cursor, leave alt
    return true;
}

// Poll stdin for up to timeout_ms; return true if q / Esc / Ctrl-C was pressed.
bool exit_key_pressed(int timeout_ms) {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0 || !(pfd.revents & POLLIN))
        return false;
    char buf[64];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == 'q' || buf[i] == 0x1b || buf[i] == 0x03)
            return true;
    }
    return false;
}

// rendering

void render_say(const std::string &text) {
    auto bubble = speech_bubble(text);
    auto frame_lines = split_lines(STATIC_FRAME);
    size_t max_h = std::max(frame_lines.size(), bubble.size());
    for (size_t i = 0; i < max_h; i++) {
        std::string fl =
            i < frame_lines.size() ? std::string(frame_lines[i]) : "";
        std::string bl = i < bubble.size() ? bubble[i] : "";
        std::cout << fl << " " << bl << "\n";
    }
}

// Play one full animation cycle. Returns true if the user asked to exit.
bool animate_once(const FrameSet &set, const std::string *text) {
    std::vector<std::string> bubble;
    if (text)
        bubble = speech_bubble(*text);

    int tw = 0, th = 0;
    if (!term_size(tw, th))
        return true;

    size_t bubble_w = 0;
    for (const auto &l : bubble)
        bubble_w = std::max(bubble_w, l.size());

    for (int fi = 0; fi < set.count; fi++) {
        if (exit_key_pressed(0))
            return true;

        std::string out = "\033[2J";
        int total_w = text ? ANIM_W + static_cast<int>(bubble_w) + 2 : ANIM_W;
        int start_x = sat_sub(tw, total_w) / 2;
        int start_y = sat_sub(th, ANIM_H) / 2;

        auto lines = split_lines(set.frames[fi]);
        for (size_t i = 0; i < lines.size(); i++) {
            out += move_to(start_x, start_y + static_cast<int>(i));
            out += std::string(lines[i]);
        }
        if (text) {
            int bx = start_x + ANIM_W + 2;
            int by =
                start_y + sat_sub(ANIM_H, static_cast<int>(bubble.size())) / 2;
            for (size_t i = 0; i < bubble.size(); i++) {
                out += move_to(bx, by + static_cast<int>(i));
                out += bubble[i];
            }
        }
        std::cout << out << std::flush;

        if (exit_key_pressed(set.interval_ms))
            return true;
    }
    return false;
}

// args

struct Args {
    enum Mode { SAY, ANIMATE, FREESTYLE } mode = SAY;
    bool help = false;
    bool version = false;
    bool has_text = false;
    std::string text;
    int variant = 1;
};

void print_help() {
    std::cout
        << "Keqingsay is a CLI program like cowsay, but instead of a talking "
           "cow,\n"
           "it's Keqing from Genshin Impact.\n\n"
           "Usage: keqingsay [TEXT]\n"
           "       keqingsay animate [TEXT] [-v|--variant <1|2|3>]\n"
           "       keqingsay freestyle [TEXT]\n\n"
           "Options:\n"
           "  -v, --variant <N>  Animation variant (1, 2, or 3) [default: 1]\n"
           "  -h, --help         Print help\n"
           "  -V, --version      Print version\n";
}

Args parse_args(int argc, char **argv) {
    Args a;
    std::vector<std::string> v(argv + 1, argv + argc);

    size_t i = 0;
    bool sub = !v.empty() && (v[0] == "animate" || v[0] == "freestyle");
    if (sub) {
        a.mode = v[0] == "animate" ? Args::ANIMATE : Args::FREESTYLE;
        i = 1;
    }
    for (; i < v.size(); i++) {
        const std::string &s = v[i];
        if (s == "-h" || s == "--help") {
            a.help = true;
        } else if (!sub && (s == "-V" || s == "--version")) {
            a.version = true;
        } else if (sub && (s == "-v" || s == "--variant") && i + 1 < v.size()) {
            a.variant = std::atoi(v[++i].c_str());
        } else if (sub && s.rfind("--variant=", 0) == 0) {
            a.variant = std::atoi(s.substr(10).c_str());
        } else if (sub && s.rfind("-v", 0) == 0 && s.size() > 2) {
            a.variant = std::atoi(s.substr(2).c_str());
        } else if (!a.has_text) {
            a.text = s;
            a.has_text = true;
        }
    }
    return a;
}

} // namespace

int main(int argc, char **argv) {
    Args args = parse_args(argc, argv);
    if (args.help) {
        print_help();
        return 0;
    }
    if (args.version) {
        std::cout << "keqingsay 0.1.0\n";
        return 0;
    }

    if (args.mode == Args::SAY) {
        render_say(args.text);
        return 0;
    }

    if (!terminal_big_enough()) {
        std::cout << "your terminal is too small for keqing\n";
        return 0;
    }
    if (!setup_terminal()) {
        std::cerr << "Error setting up terminal\n";
        return 1;
    }

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> variant_dist(1, 3);
    const std::string *text = args.has_text ? &args.text : nullptr;

    bool should_exit = false;
    while (!should_exit) {
        int variant =
            args.mode == Args::FREESTYLE ? variant_dist(rng) : args.variant;
        should_exit = animate_once(pick_set(variant), text);
    }

    if (!cleanup_terminal()) {
        std::cerr << "Error cleaning up terminal\n";
        return 1;
    }
    return 0;
}
