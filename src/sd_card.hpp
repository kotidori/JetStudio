#pragma once
// The .jetpack games on an SD card in SPI mode, mounted as a FAT filesystem.
// The card shares the display's SPI bus when the board profile wires it that way (one bus, two chip
// selects): LovyanGFX then uses bus_shared and never touches the card while a field is being sent.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sdcard {
struct Game {
    std::string path;  // absolute, e.g. /sdcard/games/jetrun.jetpack
    std::string title; // from games.txt, else the project name in the package header, else the file name
};

// Mounts the card and collects its games. False when there is no card or no filesystem on it.
bool init();
const std::vector<Game>& games();
const char* last_error();
bool mounted();
size_t free_bytes();

// Reads a package into a buffer that stays valid until the next call (or forever, if `keep`):
// the runtime reads textures and other scenes straight out of it, so it must outlive the game.
uint8_t* read(const std::string& path, size_t& size, bool keep = true);
void forget(); // frees the buffer read with keep = false

// The project name stored in a package header (the launcher shows it), from memory or from a file.
std::string title_of(const uint8_t* data, size_t size);

constexpr size_t MaxPackBytes = 1536 * 1024; // 2 MB of PSRAM also holds the decoded scene
} // namespace sdcard
