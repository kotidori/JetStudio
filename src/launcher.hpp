#pragma once
// The game menu drawn on the panel before the runtime starts: the list of the .jetpack games found
// on the SD card (plus the one embedded in the firmware, if there is one), chosen with the board's
// buttons. It uses the runtime's own 7x15 bitmap font, so the menu looks like the games' Labels and
// needs no LovyanGFX font data in flash.
#include <string>
#include <vector>

class LGFX;

namespace launcher {
struct Item {
    std::string title;
    std::string path; // empty: the game embedded in the firmware
};

// Shows the menu and returns the index the player chose. Blocks.
int run(LGFX& display, const std::vector<Item>& items);

// A full-screen message (no card, no games, a package that can't be read) with a hint line.
// Blocks while `forever`, otherwise returns after the player presses a button.
void message(LGFX& display, const std::string& title, const std::vector<std::string>& lines, bool forever);
} // namespace launcher
