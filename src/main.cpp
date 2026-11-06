#include "runtime.hpp"
#include "generated_board.hpp"
#include "PixelOps.hpp"
#include "generated_scene.hpp"
#include "generated_inputs.hpp"
#include "board_input.hpp"
#include "launcher.hpp"
#include "sd_card.hpp"
#include <sdkconfig.h>
//the queued DMA scanout drives the ESP32-S3's GDMA; the classic ESP32 sends row by row through LovyanGFX
#if CONFIG_IDF_TARGET_ESP32S3
#include "S3Scanout.hpp"
#endif
#include <esp_system.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Same frame pipeline as CubeCoders' JetExamples, with half-width interlaced fields and no z-buffer (firmware/config).
// Core 1, game task: scripts, transforms, frame setup and the upper raster band.
// Core 0, raster worker: the lower raster band.
// Core 0, display task: pushes the previous field row by row through the DMA queue.
// It sleeps while DMA runs, so the raster worker gets core 0 most of the time.
// Two field buffers: one is drawn while the other is sent.
// On one-core chips (ESP32-C3, C6) the game task draws everything itself, and the display task sends while it waits.

// Rarely used node data (extra fields, body state) goes to PSRAM: small blocks would otherwise take internal RAM
// from the buffers that drawing touches every frame.
void* jetstudio::coldAlloc(size_t bytes){
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if(!p) p = malloc(bytes);

    if(!p){
        ESP_LOGE("JetStudio", "Out of memory (%u bytes): the scene does not fit in this board's RAM", unsigned(bytes));
        abort();
    }

    return p;
}
void jetstudio::coldFree(void* p){ heap_caps_free(p); }

namespace{
    constexpr const char* Tag = "JetStudio";
    // The release profile (sdkconfig.release) has no console: the log is just drained and no frame stats are computed.
#if defined(CONFIG_ESP_CONSOLE_NONE)
    constexpr bool SerialLog = false;
#else
    constexpr bool SerialLog = true;
#endif
    static_assert(FIELD_BUFFERS && HALF_WIDTH_BUFFERS && !Z_BUFFERING, "the firmware expects firmware/config/JetConfig.hpp");

    // accumulated for the statistics line
    int64_t rasterTime = 0;
    // hold the menu button this long to leave the game
    constexpr int64_t MenuHoldMicroseconds = 2000000;

    // Jet split rendering: the game task rasterizes the rows above `split`, the worker the rows below.
    // The split moves work away from the slower band and stays even, like JetExamples' S3RasterWorker.
    namespace raster{
        TaskHandle_t worker = nullptr;
        SemaphoreHandle_t done = nullptr;
        Renderer::Scene* job = nullptr;
        std::vector<uint8_t> upper, lower;
        int height = 0, split = 0;
        int64_t lowerTime = 0;

        void workerLoop(void*){
            for(;;){
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                const int64_t start = esp_timer_get_time();
                job->rasterizeBand(split, height, lower.data());
                lowerTime = esp_timer_get_time() - start;
                xSemaphoreGive(done);
            }
        }

        void execute(Renderer::Scene& scene){
            const int64_t start = esp_timer_get_time();

            if(!worker || scene.lastFrameDrawnTriangles <= 0){
                scene.rasterizeBand(0, height);
                rasterTime += esp_timer_get_time() - start;
                return;
            }

            //one flag array per band, so a triangle drawn by both bands is counted once (Jet's parallel contract).
            upper.assign(size_t(scene.lastFrameDrawnTriangles), 0);
            lower.assign(size_t(scene.lastFrameDrawnTriangles), 0);
            job = &scene;
            xTaskNotifyGive(worker);
            scene.rasterizeBand(0, split, upper.data());
            const int64_t upperTime = esp_timer_get_time() - start;
            xSemaphoreTake(done, portMAX_DELAY);

            if(lowerTime > upperTime + 250) split = std::min(height - 32, split + 4);
            else if(upperTime > lowerTime + 250) split = std::max(32, split - 4);
            int count = 0;

            for(size_t i = 0; i < upper.size(); ++i) count += (upper[i] | lower[i]) != 0;

            scene.lastFrameRasterizedTriangles = count;
            rasterTime += esp_timer_get_time() - start;
        }

        bool start(int screen_height){
            height = screen_height;
            split = (height / 2) & ~1;
            done = xSemaphoreCreateBinary();
            return done && xTaskCreatePinnedToCore(workerLoop, "jet_raster", 8192, nullptr, 1, &worker, 0) == pdPASS;
        }
    }

    // Display task, one field per notification.
    // It sets up the scanout itself because the DMA queue interrupt has to live on the producer's core.
    namespace present{
        TaskHandle_t task = nullptr;
        // given when the previous field has been sent
        SemaphoreHandle_t idle = nullptr;
        LGFX* display = nullptr;
        uint16_t* rows[2] = {};
#if CONFIG_IDF_TARGET_ESP32S3
        S3Scanout scanout;
#endif
        bool queued = false;
        const uint16_t* field = nullptr;
        int parity = 0, width = 0, height = 0, fieldWidth = 0;
        // 2D nodes of the field being sent
        std::vector<jetstudio::OverlayItem> overlay;
        int overlayTop = 0, overlayBottom = 0;
        // microseconds, added by this task on core 0 and read by the game on core 1
        std::atomic<uint32_t> sendTime{0};

        void setOverlay(const std::vector<jetstudio::OverlayItem>& items){
            overlay = items;
            overlayTop = height;
            overlayBottom = 0;

            for(const auto& it : overlay){
                overlayTop = std::min(overlayTop, std::max(0, it.y));
                overlayBottom = std::max(overlayBottom, std::min(height, it.y + it.h));
            }
        }

        // Field row (half width, native byte order) -> panel row (doubled, swapped) + HUD.
        void prepareRow(uint16_t* out, int y){
            Renderer::expandSwapRGB565(out, field + size_t(y >> 1) * fieldWidth, size_t(fieldWidth));

            if(y >= overlayTop && y < overlayBottom) jetstudio::composeOverlayRow(overlay, out, y, width, true);
        }

        void sendField(){
#if CONFIG_IDF_TARGET_ESP32S3
            if(queued){
                scanout.begin(parity);

                for(int y = parity; y < height; y += 2){
                    while(!scanout.freeSlots()){ scanout.kick(); scanout.waitForSpace(); }
                    prepareRow(scanout.nextRow(), y);
                    scanout.publish();

                    if(!scanout.freeSlots()) scanout.kick();
                }

                scanout.kick();
                scanout.finish();
                return;
            }
#endif

            //fallback: ping-pong rows, LGFX waits for each DMA transfer.
            int current = 0;
            display->startWrite();

            for(int y = parity; y < height; y += 2){
                prepareRow(rows[current], y);
                display->setAddrWindow(0, y, width, 1);
                display->writePixelsDMA(rows[current], width, false);
                current ^= 1;
            }

            display->endWrite();
        }

        void loop(void*){
#if CONFIG_IDF_TARGET_ESP32S3
            queued = scanout.init(display, rows[0], rows[1], unsigned(width), unsigned(height));
#endif
            xSemaphoreGive(idle);

            for(;;){
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                const int64_t start = esp_timer_get_time();
                sendField();
                sendTime += uint32_t(esp_timer_get_time() - start);
                xSemaphoreGive(idle);
            }
        }

        bool start(LGFX* target, int screen_width, int screen_height){
            display = target;
            width = screen_width;
            height = screen_height;
            fieldWidth = width / 2;

            for(auto& row : rows){
                row = (uint16_t*)heap_caps_aligned_alloc(16, size_t(width) * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

                if(!row) return false;
            }

            //prime LGFX's screen-width DMA descriptor before the queued interrupt uses it.
            std::memset(rows[0], 0, size_t(width) * sizeof(uint16_t));
            display->startWrite();
            display->setAddrWindow(0, 0, width, 1);
            display->writePixelsDMA(rows[0], width, false);
            display->waitDMA();
            display->endWrite();
            idle = xSemaphoreCreateBinary();

            if(!idle || xTaskCreatePinnedToCore(loop, "jet_present", 4096, nullptr, 2, &task, 0) != pdPASS) return false;

            //scanout initialized
            xSemaphoreTake(idle, portMAX_DELAY);
            xSemaphoreGive(idle);
            return true;
        }
    }

    // What to run: the game embedded in the firmware, or one of the .jetpack files on the SD card.
    // The package the runtime gets must stay alive: with `inPlace` its palette textures are read
    // straight out of it (like the STM32 firmware reads them out of flash), and every other scene of
    // the project is decoded from it again on a change_scene_to_file().
    struct Chosen{
        const uint8_t* data = nullptr;
        size_t size = 0;
        bool inPlace = false;
        // the player chose it: holding the menu button comes back here
        bool menu = false;
        std::string title;
    };

    Chosen chooseGame(LGFX& display){
        for(;;){
            std::vector<launcher::Item> items;
            int width = 0, height = 0;

            if(jetstudio::packResolution(scene_data, sizeof(scene_data), width, height)){
                std::string title = sdcard::title_of(scene_data, sizeof(scene_data));
                items.push_back({title.empty() ? std::string("Built-in game") : title, ""});
            }

#if JET_SD_CARD
            if(sdcard::init())
                for(const auto& game : sdcard::games()) items.push_back({game.title, game.path});
            else ESP_LOGW(Tag, "%s", sdcard::last_error());
#endif

            if(items.empty()){
                ESP_LOGE(Tag, "Nothing to run: no SD card game and no package in the firmware");
                launcher::message(display, "NOTHING TO PLAY",
                                  {"No .jetpack was found.", "", "Put the games on the SD card, in a folder",
                                   "called games, and reset the board.", "", "Or build the firmware with a",
                                   "package embedded (Jet Studio's Build", "and Flash do it for you)."},
                                  true);
            }

            Chosen chosen;
            //only the firmware's own game: run it at once, as before the SD card support.
            if(items.size() == 1 && items[0].path.empty()){
                chosen.data = scene_data;
                chosen.size = sizeof(scene_data);
                chosen.title = items[0].title;
                return chosen;
            }

            const int index = launcher::run(display, items);
            chosen.title = items[size_t(index)].title;
            chosen.menu = true;

            if(items[size_t(index)].path.empty()){
                chosen.data = scene_data;
                chosen.size = sizeof(scene_data);
                return chosen;
            }

#if JET_SD_CARD
            size_t size = 0;
            const uint8_t* data = sdcard::read(items[size_t(index)].path, size);

            if(data){
                chosen.data = data;
                chosen.size = size;
                chosen.inPlace = true;
                return chosen;
            }

            launcher::message(display, "CANNOT LOAD", {items[size_t(index)].title, "", sdcard::last_error()}, false);
#endif
        }
    }

    void printLog(jetstudio::Runtime& runtime){
        for(const auto& line : runtime.take_log()){
            if(!SerialLog) continue;

            if(line.error && !line.path.empty()) ESP_LOGE(Tag, "%s:%d: %s", line.path.c_str(), line.line, line.text.c_str());
            else if(line.error) ESP_LOGE(Tag, "%s", line.text.c_str());
            else ESP_LOGI(Tag, "%s", line.text.c_str());
        }
    }

    void stop(const char* reason){
        ESP_LOGE(Tag, "%s", reason);
        vTaskDelete(nullptr);
    }

    // Own task because the script VM and frame setup need more stack than the default main task has.
    void game(void*){
        static LGFX display;
        //with the SD card on the display's bus the firmware owns the bus and LovyanGFX only attaches to it.
        if(Board::init_shared_spi() != 0) ESP_LOGE(Tag, "The SPI bus shared with the SD card could not be initialized");

        if(!display.init()) stop("Display initialization failed");

        display.setRotation(Board::rotation);
        display.fillScreen(0);
        input::init();
        const Chosen chosen = chooseGame(display);
        display.fillScreen(0);
        //scene data (meshes, Jet objects, textures) goes to PSRAM like in the demos.
        //internal RAM stays free for the fields, the DMA rows and Jet's transform scratch.
#if CONFIG_SPIRAM
        heap_caps_malloc_extmem_enable(1024);
#endif
        std::string error;
        int width = 0, height = 0;

        if(!jetstudio::packResolution(chosen.data, chosen.size, width, height)){
            ESP_LOGE(Tag, "Invalid scene: export the project again");
            launcher::message(display, "INVALID GAME", {chosen.title, "", "The package is damaged or was made",
                                                        "by another version of Jet Studio.", "", "Export it again."}, true);
            vTaskDelete(nullptr);
        }

        if(width != Board::screenWidth || height != Board::screenHeight){
            ESP_LOGE(Tag, "%s is %dx%d, the display (%s) is %dx%d: export it with this project's hardware settings",
                chosen.title.c_str(), width, height, Board::name, Board::screenWidth, Board::screenHeight);
            char sizeText[96];
            std::snprintf(sizeText, sizeof(sizeText), "It is %dx%d, this screen is %dx%d.", width, height,
                          Board::screenWidth, Board::screenHeight);
            launcher::message(display, "WRONG SIZE", {chosen.title, "", sizeText, "", "Choose this board and display",
                                                      "in Project Settings and", "export the game again."}, true);
            vTaskDelete(nullptr);
        }

        ESP_LOGI(Tag, "Running %s (%u KB%s)", chosen.title.c_str(), unsigned(chosen.size / 1024),
                 chosen.inPlace ? ", from the SD card" : ", from flash");

        //allocate the fields before the scene so they get internal DMA RAM when possible.
        //PSRAM works too, the scanout copies the rows with the CPU anyway, it is just slower.
        const size_t fieldBytes = size_t(width / jetstudio::BufferColumnStep) * (height / jetstudio::BufferRowStep) * sizeof(uint16_t);
        uint16_t* fields[2] = {};
        bool internal = true;
        //one core can not draw while it sends, so a second buffer would only take RAM
        const int buffers = configNUMBER_OF_CORES > 1 ? 2 : 1;

        for(int i = 0; i < buffers; ++i){
            auto& f = fields[i];
            f = (uint16_t*)heap_caps_aligned_alloc(16, fieldBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

            if(!f){
                internal = false;
                f = (uint16_t*)heap_caps_aligned_alloc(16, fieldBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }

            if(!f) stop("Not enough RAM for the field buffers");

            std::memset(f, 0, fieldBytes);
        }

        if(buffers == 1) fields[1] = fields[0];

        if(!present::start(&display, width, height)) stop("Display task not started");

        //the package stays in flash (or on the card): only the scene being played is decoded into RAM.
        //without PSRAM (ESP32-C3, C6) palette textures are read from the package too, as on the STM32 boards.
        static jetstudio::Runtime runtime;
        const bool noPsram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0;

        if(!runtime.load_pack(chosen.data, chosen.size, error, chosen.inPlace || noPsram)){
            ESP_LOGE(Tag, "Invalid scene: %s", error.c_str());
            launcher::message(display, "INVALID GAME", {chosen.title, "", error}, true);
            vTaskDelete(nullptr);
        }

        std::vector<jetstudio::ScriptDiagnostic> diagnostics;

        if(!runtime.start(diagnostics)){
            for(const auto& d : diagnostics)
                ESP_LOGE(Tag, "%s:%d:%d %s", d.path.c_str(), d.line, d.column, d.message.c_str());
            vTaskDelete(nullptr);
        }

#if CONFIG_SPIRAM
        heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
        printLog(runtime);

        if(configNUMBER_OF_CORES > 1){
            if(raster::start(runtime.height())) runtime.setRasterExecutor(raster::execute);
            else ESP_LOGW(Tag, "Raster worker not started: single-core rendering");
        }

        ESP_LOGI(Tag, "%s: %dx%d, fields of %dx%d in %s, DMA queue %s", Board::name, runtime.width(), runtime.height(),
            runtime.bufferWidth(), runtime.bufferRows(), internal ? "internal RAM" : "PSRAM (slower)", present::queued ? "on" : "off (polling)");
        ESP_LOGI(Tag, "Scene loaded: %u nodes (%u bytes), %u meshes, %u scripts; internal free %u, PSRAM free %u",
            unsigned(runtime.node_count()), unsigned(runtime.nodeBytes()), unsigned(runtime.meshCount()), unsigned(runtime.description().scripts.size()),
            unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

        int64_t previous = esp_timer_get_time(), total = 0, scriptsTime = 0, rendering = 0, waiting = 0, yielded = previous;
        unsigned count = 0;
        //holding the menu button comes back to the launcher (a reset, so the card is mounted cleanly).
        const int menuBit = chosen.menu ? input::action_bit(Board::menuAction) : -1;
        int64_t menuHeldSince = 0;

        //the task watchdog wants the idle task to run on this core.
        //at FREERTOS_HZ=100 a 1 ms delay is 0 ticks and never blocks, so yield a full tick every half second
        //(JetExamples' FramePacer does the same). On one core it happens right after the display task finished
        //a field, when nothing else is ready to run; with two, after handing it the next one.
        auto letIdleRun = [&](){
            if(esp_timer_get_time() - yielded < 500000) return;

            vTaskDelay(1);
            yielded = esp_timer_get_time();
        };

        for(;;){
            const int64_t start = esp_timer_get_time();
            const int parity = runtime.nextField();
            runtime.set_framebuffer(fields[parity]);
            const uint32_t buttons = input::read();
            runtime.set_input(buttons);

            if(menuBit >= 0){
                if(buttons & (1u << menuBit)){
                    if(!menuHeldSince) menuHeldSince = start;
                    else if(start - menuHeldSince > MenuHoldMicroseconds){
                        ESP_LOGW(Tag, "%s held for %d s: back to the launcher", Board::menuAction, int(MenuHoldMicroseconds / 1000000));
                        esp_restart();
                    }
                } else menuHeldSince = 0;
            }

            runtime.update(float(start - previous) / 1000000.0f);
            const int64_t updated = esp_timer_get_time();
            previous = start;

            //one buffer: the previous field must be out before this one is drawn over it
            if(buffers == 1){
                xSemaphoreTake(present::idle, portMAX_DELAY);
                letIdleRun();
            }

            const int64_t drawing = esp_timer_get_time();
            runtime.render();
            const int64_t rendered = esp_timer_get_time();

            //two buffers: wait until the previous field is out, then send this one while the next is drawn.
            if(buffers == 2) xSemaphoreTake(present::idle, portMAX_DELAY);

            present::field = fields[parity];
            present::parity = parity;
            present::setOverlay(runtime.overlay());
            xTaskNotifyGive(present::task);

            if(buffers == 2) letIdleRun();

            printLog(runtime);
            scriptsTime += updated - start;
            rendering += rendered - drawing;
            waiting += (drawing - updated) + (esp_timer_get_time() - rendered);
            total += esp_timer_get_time() - start;

            if(SerialLog && ++count == 60){
                const uint32_t sendTime = present::sendTime.exchange(0);
                //the editor's Serial monitor parses the first part, keep the format.
                ESP_LOGI(Tag, "Frame %.2f ms (scripts %.2f ms); %d raster tris; free heap %u, PSRAM %u; render %.2f ms (raster %.2f, split %d), field send %.2f ms (waited %.2f); internal %u",
                    total / 60000.0, scriptsTime / 60000.0, runtime.triangles(), unsigned(esp_get_free_heap_size()),
                    unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), rendering / 60000.0, rasterTime / 60000.0, raster::split,
                    sendTime / 60000.0, waiting / 60000.0, unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
                count = 0; total = 0; scriptsTime = 0; rendering = 0; waiting = 0; rasterTime = 0;
            }
        }
    }
}

extern "C" void app_main(){
    xTaskCreatePinnedToCore(game, "jet_game", 32 * 1024, nullptr, 5, nullptr, configNUMBER_OF_CORES > 1 ? 1 : 0);
}
