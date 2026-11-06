#include "sd_card.hpp"
#include "generated_board.hpp"
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#if JET_SD_CARD
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_memory_utils.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#endif

namespace sdcard {

// std::tolower()/toupper()/isspace() take an int that must not be negative.
using uchar = unsigned char;

// The project name a package carries: magic, version, then a u32 length and the UTF-8 name.
// Works on a file just read and on the array embedded in the firmware.
std::string title_of(const uint8_t* data, size_t size) {
    if (!data || size < 12 || std::memcmp(data, "JETP", 4) != 0) return "";
    uint32_t length = 0;
    std::memcpy(&length, data + 8, 4);
    if (!length || length > 256 || size_t(12) + length > size) return "";
    return std::string(reinterpret_cast<const char*>(data + 12), length);
}

#if JET_SD_CARD

namespace {
constexpr const char* Tag = "sdcard";
constexpr size_t HeaderBytes = 512; // enough for the magic, the version and the project name

sdmmc_card_t* card = nullptr;
uint8_t* kept = nullptr; // the package the runtime is reading
std::vector<Game> found;
char error[160] = "";
bool own_bus = false;

void fail(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, sizeof(error), format, args);
    va_end(args);
    ESP_LOGE(Tag, "%s", error);
}

bool has_extension(const std::string& name, const char* extension) {
    const size_t length = std::strlen(extension);
    if (name.size() <= length) return false;
    const char* end = name.c_str() + name.size() - length;
    for (size_t i = 0; i < length; ++i)
        if (std::tolower(uchar(end[i])) != std::tolower(uchar(extension[i]))) return false;
    return true;
}

std::string title_from_file(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    std::replace(name.begin(), name.end(), '_', ' ');
    return name;
}

std::string title_of_file(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return "";
    uint8_t header[HeaderBytes];
    const size_t got = std::fread(header, 1, sizeof(header), file);
    std::fclose(file);
    return title_of(header, got);
}

// games.txt next to the packs: "file.jetpack = The Title", one per line, # starts a comment.
std::vector<std::pair<std::string, std::string>> read_titles(const std::string& folder) {
    std::vector<std::pair<std::string, std::string>> out;
    std::FILE* file = std::fopen((folder + "/games.txt").c_str(), "rb");
    if (!file) return out;
    auto trim = [](std::string& text) {
        while (!text.empty() && std::isspace(uchar(text.front()))) text.erase(text.begin());
        while (!text.empty() && std::isspace(uchar(text.back()))) text.pop_back();
    };
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        trim(text);
        if (text.empty() || text[0] == '#') continue;
        const size_t equals = text.find('=');
        if (equals == std::string::npos) continue;
        std::string name = text.substr(0, equals), title = text.substr(equals + 1);
        trim(name);
        trim(title);
        if (!name.empty() && !title.empty()) out.emplace_back(name, title);
    }
    std::fclose(file);
    return out;
}

void scan(const std::string& folder, const std::vector<std::pair<std::string, std::string>>& titles) {
    DIR* dir = opendir(folder.c_str());
    if (!dir) return;
    std::vector<std::string> names;
    while (const dirent* entry = readdir(dir))
        if (has_extension(entry->d_name, ".jetpack")) names.push_back(entry->d_name);
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        Game game;
        game.path = folder + "/" + name;
        for (const auto& titled : titles)
            if (titled.first == name) game.title = titled.second;
        if (game.title.empty()) game.title = title_of_file(game.path);
        if (game.title.empty()) game.title = title_from_file(name);
        found.push_back(std::move(game));
    }
}

bool mount() {
    if (!Board::sdSharedSpi) {
        // The card has its own bus: the display keeps SPI2_HOST and initializes it itself.
        spi_bus_config_t bus_config{};
        bus_config.mosi_io_num = Board::sdMosi;
        bus_config.miso_io_num = Board::sdMiso;
        bus_config.sclk_io_num = Board::sdSck;
        bus_config.quadwp_io_num = -1;
        bus_config.quadhd_io_num = -1;
        bus_config.max_transfer_sz = 4096;
        const esp_err_t bus_error = spi_bus_initialize(spi_host_device_t(Board::sdHost), &bus_config, SPI_DMA_CH_AUTO);
        if (bus_error != ESP_OK) {
            fail("The SPI bus for the SD card failed: %s", esp_err_to_name(bus_error));
            return false;
        }
        own_bus = true;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = spi_host_device_t(Board::sdHost);
    host.max_freq_khz = unsigned(Board::sdHz / 1000);
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = spi_host_device_t(Board::sdHost);
    slot.gpio_cs = gpio_num_t(Board::sdCs);
    esp_vfs_fat_sdmmc_mount_config_t config{};
    config.format_if_mount_failed = false;
    config.max_files = 4;
    config.allocation_unit_size = 16 * 1024;
    const esp_err_t error = esp_vfs_fat_sdspi_mount(Board::sdMount, &host, &slot, &config, &card);
    if (error != ESP_OK) {
        card = nullptr;
        fail("No SD card (%s). Check the wiring, the card's format (FAT32) and its power.", esp_err_to_name(error));
        if (own_bus) {
            spi_bus_free(spi_host_device_t(Board::sdHost));
            own_bus = false;
        }
        return false;
    }
    return true;
}
} // namespace

const char* last_error() { return error; }
bool mounted() { return card != nullptr; }
const std::vector<Game>& games() { return found; }

size_t free_bytes() {
    if (!card) return 0;
    struct statvfs info {};
    if (statvfs(Board::sdMount, &info) != 0) return 0;
    return size_t(info.f_bfree) * info.f_frsize;
}

bool init() {
    error[0] = '\0';
    found.clear();
    if (!mount()) return false;
    sdmmc_card_print_info(stdout, card);
    const std::string root = Board::sdMount;
    const std::string folder = root + "/games";
    struct stat info {};
    // games/ first, so a card can keep anything else in the root.
    if (stat(folder.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) scan(folder, read_titles(folder));
    scan(root, read_titles(root));
    ESP_LOGI(Tag, "%u game%s on the card, %u KB free", unsigned(found.size()), found.size() == 1 ? "" : "s",
             unsigned(free_bytes() / 1024));
    return true;
}

uint8_t* read(const std::string& path, size_t& size, bool keep) {
    size = 0;
    if (!card) {
        fail("The SD card is not mounted");
        return nullptr;
    }
    forget();
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        fail("Cannot open %s", path.c_str());
        return nullptr;
    }
    std::fseek(file, 0, SEEK_END);
    const long bytes = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (bytes < 16) {
        std::fclose(file);
        fail("%s is not a .jetpack file", path.c_str());
        return nullptr;
    }
    if (size_t(bytes) > MaxPackBytes) {
        std::fclose(file);
        fail("%s is %ld KB, more than the %u KB this board can hold", path.c_str(), bytes / 1024,
             unsigned(MaxPackBytes / 1024));
        return nullptr;
    }
    // PSRAM: the runtime reads the package's palette textures straight out of this buffer.
    uint8_t* buffer = (uint8_t*)heap_caps_malloc(size_t(bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) buffer = (uint8_t*)heap_caps_malloc(size_t(bytes), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buffer) {
        std::fclose(file);
        fail("Not enough RAM for a %ld KB package", bytes / 1024);
        return nullptr;
    }
    size_t got = 0;
    while (got < size_t(bytes)) {
        const size_t chunk = std::fread(buffer + got, 1, std::min(size_t(8192), size_t(bytes) - got), file);
        if (!chunk) break;
        got += chunk;
    }
    std::fclose(file);
    if (got != size_t(bytes)) {
        heap_caps_free(buffer);
        fail("Reading %s stopped after %u of %ld bytes", path.c_str(), unsigned(got), bytes);
        return nullptr;
    }
    ESP_LOGI(Tag, "%s: %u KB in %s", path.c_str(), unsigned(got / 1024),
             esp_ptr_external_ram(buffer) ? "PSRAM" : "internal RAM");
    size = got;
    kept = keep ? buffer : nullptr;
    return buffer;
}

void forget() {
    if (kept) {
        heap_caps_free(kept);
        kept = nullptr;
    }
}

#else // no SD card on this board

const char* last_error() { return ""; }
bool mounted() { return false; }
const std::vector<Game>& games() {
    static const std::vector<Game> none;
    return none;
}
size_t free_bytes() { return 0; }
bool init() { return false; }
uint8_t* read(const std::string&, size_t& size, bool) {
    size = 0;
    return nullptr;
}
void forget() {}

#endif
} // namespace sdcard
