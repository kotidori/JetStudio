#include "launcher.hpp"
#include "board_input.hpp"
#include "generated_board.hpp"
#include "font_data.hpp"
#include <LovyanGFX.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace launcher {

// std::tolower()/toupper()/isspace() take an int that must not be negative.
using uchar = unsigned char;
namespace {
constexpr const char* Tag = "launcher";

struct Text {
    int x, y, scale;
    uint16_t color;
    std::string text;
};
struct Rect {
    int x, y, w, h;
    uint16_t color;
};

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
const uint16_t Background = rgb(0x10, 0x14, 0x1c);
const uint16_t Panel = rgb(0x18, 0x1e, 0x2a);
const uint16_t Accent = rgb(0x4f, 0x8c, 0xff);
const uint16_t Bright = rgb(0xe2, 0xe7, 0xf0);
const uint16_t Muted = rgb(0x8a, 0x93, 0xa6);
const uint16_t Dark = rgb(0x0b, 0x0e, 0x14);
const uint16_t Warning = rgb(0xff, 0xb4, 0x54);

constexpr int LineHeight = 15;
constexpr int RowHeight = LineHeight + 5;
constexpr int Margin = 8;

// The runtime's font: code points 32..126, then 160..255.
const uint8_t* glyph(char c) {
    const unsigned char code = uchar(c);
    if (code >= 32 && code <= 126) return jetstudio::font::Glyphs[code - 32];
    if (code >= 160) return jetstudio::font::Glyphs[95 + (code - 160)];
    return jetstudio::font::Glyphs[0];
}

// The runtime's font is Latin-1 (code points 32..126 and 160..255): a title in another alphabet -
// games.txt and the packages are UTF-8 - is transliterated, so a Russian game name stays readable
// in the menu instead of turning into two garbage bytes per letter. Everything the launcher draws
// and measures goes through latin(), so centering, truncation and wrapping stay consistent.
const char* cyrillic(uint32_t code) {
    switch (code) {
        case 0x410: return "A";   case 0x430: return "a";   case 0x411: return "B";   case 0x431: return "b";
        case 0x412: return "V";   case 0x432: return "v";   case 0x413: return "G";   case 0x433: return "g";
        case 0x490: return "G";   case 0x491: return "g";   case 0x414: return "D";   case 0x434: return "d";
        case 0x415: return "E";   case 0x435: return "e";   case 0x401: return "Yo";  case 0x451: return "yo";
        case 0x416: return "Zh";  case 0x436: return "zh";  case 0x417: return "Z";   case 0x437: return "z";
        case 0x418: return "I";   case 0x438: return "i";   case 0x419: return "Y";   case 0x439: return "y";
        case 0x406: return "I";   case 0x456: return "i";   case 0x407: return "Yi";  case 0x457: return "yi";
        case 0x41A: return "K";   case 0x43A: return "k";   case 0x41B: return "L";   case 0x43B: return "l";
        case 0x41C: return "M";   case 0x43C: return "m";   case 0x41D: return "N";   case 0x43D: return "n";
        case 0x41E: return "O";   case 0x43E: return "o";   case 0x41F: return "P";   case 0x43F: return "p";
        case 0x420: return "R";   case 0x440: return "r";   case 0x421: return "S";   case 0x441: return "s";
        case 0x422: return "T";   case 0x442: return "t";   case 0x423: return "U";   case 0x443: return "u";
        case 0x40E: return "U";   case 0x45E: return "u";   case 0x424: return "F";   case 0x444: return "f";
        case 0x425: return "Kh";  case 0x445: return "kh";  case 0x426: return "Ts";  case 0x446: return "ts";
        case 0x427: return "Ch";  case 0x447: return "ch";  case 0x428: return "Sh";  case 0x448: return "sh";
        case 0x429: return "Shch"; case 0x449: return "shch";
        case 0x42A: return "";    case 0x44A: return "";    case 0x42B: return "Y";   case 0x44B: return "y";
        case 0x42C: return "";    case 0x44C: return "";    case 0x42D: return "E";   case 0x44D: return "e";
        case 0x42E: return "Yu";  case 0x44E: return "yu";  case 0x42F: return "Ya";  case 0x44F: return "ya";
        case 0x404: return "Ye";  case 0x454: return "ye";  case 0x2013: return "-"; case 0x2014: return "-";
        case 0x2018: case 0x2019: return "'";  case 0x201C: case 0x201D: return "\"";
        default: return nullptr;
    }
}

std::string latin(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = uchar(text[i]);
        const size_t extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
        uint32_t code = lead;
        bool valid = true;
        if (extra) {
            code = lead & uint8_t(0x7F >> (extra + 1));
            if (i + extra >= text.size()) valid = false;
            for (size_t k = 1; valid && k <= extra; ++k) {
                const unsigned char tail = uchar(text[i + k]);
                if ((tail & 0xC0) != 0x80) { valid = false; break; }
                code = (code << 6) | (tail & 0x3F);
            }
        }
        if (!valid) {
            // Not UTF-8. A printable Latin-1 byte goes through (the font has 160..255), which also
            // makes latin() idempotent - it is applied again to what truncate() and wrap() return.
            out += lead >= 0xA0 ? char(lead) : '?';
            ++i;
            continue;
        }
        i += extra + 1;
        if (code < 128) { out += char(code); continue; }
        const char* mapped = cyrillic(code);
        if (mapped) out += mapped;
        else if (code <= 255) out += char(code); // Latin-1: the font has it
        else out += '?';
    }
    return out;
}

int text_width(const std::string& text, int scale) { return int(latin(text).size()) * jetstudio::font::Width * scale; }

// The whole screen is composed row by row into one line buffer: a menu of a few dozen rows costs a
// few milliseconds, and drawing pixel by pixel through LovyanGFX would cost a hundred times more.
class Screen {
public:
    void clear() { texts.clear(); rects.clear(); }
    void rect(int x, int y, int w, int h, uint16_t color) { rects.push_back({x, y, w, h, color}); }
    void text(int x, int y, const std::string& string, uint16_t color, int scale = 1) { texts.push_back({x, y, scale, color, latin(string)}); }
    void centered(int y, const std::string& string, uint16_t color, int scale = 1) {
        text(std::max(0, (width - text_width(string, scale)) / 2), y, string, color, scale);
    }
    void draw(LGFX& display) {
        if (!row || row_width != width) {
            row.reset(new uint16_t[size_t(width)]);
            row_width = width;
        }
        display.startWrite();
        display.setSwapBytes(true); // the buffer is host-order RGB565, the panel takes it big endian
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) row[x] = Background;
            for (const auto& r : rects)
                if (y >= r.y && y < r.y + r.h)
                    for (int x = std::max(0, r.x); x < std::min(width, r.x + r.w); ++x) row[x] = r.color;
            for (const auto& t : texts) {
                const int top = t.y, bottom = t.y + LineHeight * t.scale;
                if (y < top || y >= bottom) continue;
                const int cell_row = (y - top) / t.scale;
                int x = t.x;
                for (char c : t.text) {
                    const uint8_t bits = glyph(c)[cell_row];
                    for (int i = 0; i < jetstudio::font::Width; ++i) {
                        if (!(bits & (0x40 >> i))) continue;
                        // One screen row: the glyph's cell_row, widened by the scale.
                        for (int sx = 0; sx < t.scale; ++sx) {
                            const int px = x + i * t.scale + sx;
                            if (px >= 0 && px < width) row[px] = t.color;
                        }
                    }
                    x += jetstudio::font::Width * t.scale;
                }
            }
            display.pushImage(0, y, width, 1, row.get());
        }
        display.endWrite();
    }
    int width = 0, height = 0;

private:
    std::vector<Text> texts;
    std::vector<Rect> rects;
    std::unique_ptr<uint16_t[]> row;
    int row_width = 0;
};

std::string upper(std::string text) {
    for (char& c : text) c = char(std::toupper(uchar(c)));
    return text;
}

std::string truncate(const std::string& text, int pixels) {
    const std::string shown = latin(text);
    if (text_width(shown, 1) <= pixels) return shown;
    const size_t characters = size_t(pixels / jetstudio::font::Width);
    return characters > 1 ? shown.substr(0, characters - 1) + "~" : shown;
}

std::vector<std::string> wrap(const std::string& text, int pixels) {
    std::vector<std::string> lines;
    const std::string shown = latin(text);
    const size_t per_line = std::max(size_t(1), size_t(pixels / jetstudio::font::Width));
    size_t start = 0;
    while (start < shown.size()) {
        size_t end = std::min(shown.size(), start + per_line);
        if (end < shown.size()) {
            const size_t space = shown.rfind(' ', end);
            if (space != std::string::npos && space > start) end = space;
        }
        lines.push_back(shown.substr(start, end - start));
        start = end;
        while (start < text.size() && text[start] == ' ') ++start;
    }
    return lines;
}

// Edge-detected buttons with auto-repeat, so holding a direction scrolls.
struct Buttons {
    uint32_t state = 0, previous = 0;
    int up = -1, down = -1, accept = -1;
    int64_t held_since = 0, last_repeat = 0;
    int direction = 0;

    void begin() {
        up = input::action_bit("ui_up");
        down = input::action_bit("ui_down");
        accept = input::action_bit("ui_accept");
        if (up < 0) up = input::action_bit("ui_left");
        if (down < 0) down = input::action_bit("ui_right");
    }
    bool held(int action) const { return action >= 0 && (state & (1u << action)) != 0; }
    bool pressed(int action) const { return held(action) && !(previous & (1u << action)); }

    void poll() {
        previous = state;
        state = input::read();
    }
    // -1, 0 or +1 for this frame: an edge, then a repeat every 120 ms after 400 ms.
    int move() {
        const int now = held(up) ? -1 : held(down) ? 1 : 0;
        const int64_t time = esp_timer_get_time();
        if (now == 0) {
            direction = 0;
            held_since = last_repeat = 0;
            return 0;
        }
        if (now != direction) {
            direction = now;
            held_since = last_repeat = time;
            return now;
        }
        if (time - held_since > 400000 && time - last_repeat > 120000) {
            last_repeat = time;
            return now;
        }
        return 0;
    }
};
} // namespace

int run(LGFX& display, const std::vector<Item>& items) {
    if (items.empty()) {
        message(display, "NOTHING TO PLAY", {"Put .jetpack files on the SD card", "in a folder called games.", "",
                                            "Export them from Jet Studio with", "this board and display selected."}, true);
        return 0;
    }
    Screen screen;
    screen.width = display.width();
    screen.height = display.height();
    Buttons buttons;
    buttons.begin();
    buttons.poll(); // a button held while resetting must not select anything
    buttons.previous = buttons.state;

    int selected = 0, scroll = 0;
    const int header = Margin + LineHeight * 2 + 6;
    const int footer = Margin + LineHeight * 2;
    const int visible = std::max(1, (screen.height - header - footer) / RowHeight);
    const int list_width = screen.width - Margin * 2;
    bool redraw = true;
    int64_t started = esp_timer_get_time();

    for (;;) {
        buttons.poll();
        const int move = buttons.move();
        if (move) {
            selected = (selected + move + int(items.size())) % int(items.size());
            if (selected < scroll) scroll = selected;
            if (selected >= scroll + visible) scroll = selected - visible + 1;
            redraw = true;
        }
        if (buttons.pressed(buttons.accept) && esp_timer_get_time() - started > 200000) {
            ESP_LOGI(Tag, "Chosen: %s", items[size_t(selected)].title.c_str());
            return selected;
        }
        if (!redraw) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        redraw = false;
        screen.clear();
        screen.rect(0, 0, screen.width, header - 4, Panel);
        screen.centered(Margin, "JET STUDIO", Bright, 2);
        screen.centered(Margin + LineHeight * 2 + 1, upper(std::to_string(items.size()) + (items.size() == 1 ? " GAME" : " GAMES")), Muted);
        for (int i = 0; i < visible; ++i) {
            const int index = scroll + i;
            if (index >= int(items.size())) break;
            const int y = header + i * RowHeight;
            const bool current = index == selected;
            if (current) screen.rect(Margin - 4, y - 3, list_width + 8, RowHeight, Accent);
            screen.text(Margin + 2, y, truncate(items[size_t(index)].title, list_width - 14), current ? Dark : Bright);
            if (items[size_t(index)].path.empty())
                screen.text(screen.width - Margin - text_width("flash", 1), y, "flash", current ? Dark : Muted);
        }
        if (scroll > 0) screen.centered(header - LineHeight, "^", Muted);
        if (scroll + visible < int(items.size())) screen.centered(header + visible * RowHeight, "v", Muted);
        screen.rect(0, screen.height - footer + 2, screen.width, footer, Panel);
        screen.centered(screen.height - footer + Margin, "UP / DOWN move   OK plays", Muted);
        if (input::uses_expander() && !input::expander_online())
            screen.centered(screen.height - Margin - 2, "no buttons on the I2C expander", Warning);
        screen.draw(display);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void message(LGFX& display, const std::string& title, const std::vector<std::string>& lines, bool forever) {
    Screen screen;
    screen.width = display.width();
    screen.height = display.height();
    Buttons buttons;
    buttons.begin();
    screen.rect(0, 0, screen.width, Margin + LineHeight * 2 + 4, Panel);
    screen.centered(Margin, upper(title), Warning, 2);
    int y = Margin + LineHeight * 2 + 12;
    for (const auto& line : lines) {
        for (const auto& part : wrap(line, screen.width - Margin * 2)) {
            screen.text(Margin, y, part, Bright);
            y += LineHeight + 3;
        }
    }
    screen.centered(screen.height - Margin - LineHeight, forever ? "" : "press any button", Muted);
    screen.draw(display);
    if (forever)
        for (;;) vTaskDelay(pdMS_TO_TICKS(100));
    // Any button press closes it; the first 300 ms are ignored, so a hand still on the pad at reset
    // doesn't skip the message.
    const int64_t started = esp_timer_get_time();
    for (;;) {
        buttons.poll();
        if (esp_timer_get_time() - started > 300000 && buttons.pressed(buttons.accept >= 0 ? buttons.accept : 0)) return;
        if (esp_timer_get_time() - started > 300000 && buttons.state && !buttons.previous) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
} // namespace launcher
