# Board and display profiles (hardware/) and the firmware files generated from a project's hardware settings.
# Pins are GPIO numbers on ESP32 and STM32 PinName values on STM32 (port * 16 + pin, so PA5 = 5, PB5 = 21).
# A pin of EXPANDER_PIN_BASE + n is not a GPIO: it is bit n of the board's I2C port expander (PCF8574),
# which is how a handheld wires a D-pad when it runs out of pins.
# editor/scripts/hardware.gd reads the same profiles and applies the same rules, keep them in sync.
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARDWARE = ROOT / "hardware"
FIRMWARE = ROOT / "firmware"
FIRMWARE_STM32 = ROOT / "firmware_stm32"
DEFAULT_BOARD, DEFAULT_DISPLAY = "nano_esp32", "ili9341_240x320"
ORIENTATIONS = ["landscape", "portrait", "landscape_flipped", "portrait_flipped"]
SPI_MODES = [0, 1, 2, 3]  # CPOL/CPHA pairs the SPI master driver accepts
DISPLAY_PINS = ["sck", "mosi", "cs", "dc", "reset", "backlight"]
SD_PINS = ["sck", "mosi", "miso", "cs"]
# Pins a display can do without: reset (the panel is then never hard-reset), backlight (always on)
# and cs (modules that don't bring CS out: the panel is permanently selected).
OPTIONAL_PINS = {"reset", "backlight", "cs"}
CONTROLLERS = {"ILI9341": "Panel_ILI9341", "ST7789": "Panel_ST7789", "ST7796": "Panel_ST7796",
               "GC9A01": "Panel_GC9A01", "ST7735S": "Panel_ST7735S"}
CUSTOM_DISPLAY = {"name": "Custom", "controller": "ILI9341", "panel": [240, 320], "memory": [240, 320],
                  "offset": [0, 0], "invert": False, "rgb_order": False, "spi_hz": 40000000, "spi_mode": 0}
# I2C port expanders the firmware knows: 8 quasi-bidirectional bits, read as one byte.
EXPANDERS = {"PCF8574": 8, "PCF8574A": 8}
EXPANDER_PIN_BASE = 100
EXPANDER_ADDRESS = {"PCF8574": 0x20, "PCF8574A": 0x38}
SD_DEFAULT_HZ = 20000000
SD_MOUNT = "/sdcard"


def platform(board):
    return board.get("platform", "esp32")


def firmwareDir(board):
    return FIRMWARE_STM32 if platform(board) == "stm32" else FIRMWARE


def isExpanderPin(pin):
    return pin >= EXPANDER_PIN_BASE


def expanderName(board):
    expander = board.get("expander", {})
    return expander.get("type", "PCF8574") if expander else ""


def pinLabel(board, pin):
    if isExpanderPin(pin):
        return f"{expanderName(board)} · P{pin - EXPANDER_PIN_BASE}"

    for label, number in board["pins"]:
        if number == pin:
            return label

    if platform(board) == "esp32":
        return f"GPIO{pin}"

    return f"P{'ABCDEFGHIJK'[pin >> 4]}{pin & 15}" if 0 <= pin < 176 else f"pin {pin}"


def catalog(kind):
    return {path.stem: json.loads(path.read_text(encoding="utf-8")) for path in sorted((HARDWARE / kind).glob("*.json"))}


# Overlay a project's pin edits on a profile's pins (int() is lenient on purpose: the editor
# sends strings out of its line edits, and anything unparseable falls back to the profile).
def _pinsOf(section, names, profile):
    pins = {name: int(profile.get(name, -1)) for name in names}

    for name in names:
        value = section.get(name)

        if value is None:
            continue

        try:
            pins[name] = int(value)
        except (TypeError, ValueError):
            pass

    return pins


# The project's hardware settings, with missing values taken from the profiles.
def settings(config):
    boards, displays = catalog("boards"), catalog("displays")
    hw = dict(config.get("hardware", {}))

    if hw.get("board") not in boards:
        hw["board"] = DEFAULT_BOARD

    if hw.get("display") != "custom" and hw.get("display") not in displays:
        hw["display"] = DEFAULT_DISPLAY

    display = dict(CUSTOM_DISPLAY, **hw.get("custom_display", {})) if hw["display"] == "custom" else displays[hw["display"]]

    if hw.get("orientation") not in ORIENTATIONS:
        hw["orientation"] = "landscape"

    hw["spi_hz"] = int(hw.get("spi_hz") or display["spi_hz"])
    # The SPI mode comes from the display profile unless the project overrides it: plenty of
    # ST7789/ST7735 modules only talk mode 3, and ESP-IDF's sdspi driver asks for mode 0 on
    # the same bus — devices on a shared bus get their own mode applied per transaction.
    try:
        mode = int(hw.get("spi_mode"))
    except (TypeError, ValueError):
        mode = int(display.get("spi_mode", 0))
    hw["spi_mode"] = mode if mode in SPI_MODES else int(display.get("spi_mode", 0))
    pins = dict(boards[hw["board"]]["display_pins"])
    pins.update({k: int(v) for k, v in hw.get("pins", {}).items() if k in DISPLAY_PINS})
    hw["pins"] = pins
    board = boards[hw["board"]]
    # SD card: the game package can live on a card instead of (or next to) the firmware's flash.
    sdProfile = board.get("sd", {})
    sd = dict(hw.get("sd", {}))
    hw["sd"] = {"enabled": bool(sd.get("enabled", bool(sdProfile))),
                "mount": str(sd.get("mount") or sdProfile.get("mount") or SD_MOUNT),
                "hz": int(sd.get("hz") or sdProfile.get("hz") or SD_DEFAULT_HZ)}
    hw["sd"].update(_pinsOf(sd, SD_PINS, sdProfile))
    # I2C port expander carrying the buttons (pin numbers >= EXPANDER_PIN_BASE).
    expanderProfile = board.get("expander", {})
    expander = dict(hw.get("expander", {}))
    kind = str(expander.get("type") or expanderProfile.get("type") or "PCF8574")
    hw["expander"] = {"enabled": bool(expander.get("enabled", bool(expanderProfile))),
                      "type": kind if kind in EXPANDERS else "PCF8574",
                      "sda": int(expander.get("sda", expanderProfile.get("sda", -1))),
                      "scl": int(expander.get("scl", expanderProfile.get("scl", -1))),
                      "address": int(expander.get("address", expanderProfile.get("address", EXPANDER_ADDRESS.get(kind, 0x20)))),
                      "active_high": bool(expander.get("active_high", expanderProfile.get("active_high", False)))}
    # Action the launcher binds to "back/exit to menu".
    hw["menu_action"] = str(hw.get("menu_action") or board.get("menu_action") or "ui_cancel")
    return hw, board, display


def resolution(display, orientation):
    w, h = display["panel"]
    landscape = orientation.startswith("landscape")
    return (max(w, h), min(w, h)) if landscape else (min(w, h), max(w, h))


# LovyanGFX setRotation() value for an orientation.
def rotation(display, orientation):
    w, h = display["panel"]
    base = {"portrait": 0, "landscape": 1, "portrait_flipped": 2, "landscape_flipped": 3}[orientation]
    #landscape-native panel
    return (base + 1) % 4 if w > h else base


# The SD card sits on the display's SPI bus (one bus, two chip selects) or on its own.
def sdSharesBus(hw):
    sd = hw["sd"]
    return bool(sd["enabled"]) and sd["sck"] == hw["pins"]["sck"] and sd["mosi"] == hw["pins"]["mosi"]


def validate(hw, board, display, buttonPins=()):
    stm32 = platform(board) == "stm32"
    usable = {gpio for _, gpio in board["pins"]}
    used = {}

    def take(pin, name, allowExpander=False):
        if pin < 0:
            raise RuntimeError(f"{name} pin is not set")

        if allowExpander and isExpanderPin(pin):
            bits = EXPANDERS.get(hw["expander"]["type"], 8)

            if not hw["expander"]["enabled"]:
                raise RuntimeError(f"{pinLabel(board, pin)} ({name}) needs an I2C expander: enable one in Project Settings")

            if pin - EXPANDER_PIN_BASE >= bits:
                raise RuntimeError(f"{pinLabel(board, pin)} ({name}) is out of range")

            if pin in used:
                raise RuntimeError(f"{pinLabel(board, pin)} is used by both {used[pin]} and {name}")

            used[pin] = name
            return

        if pin not in usable:
            raise RuntimeError(f"{pinLabel(board, pin)} ({name}) is not usable on {board['name']}")

        if pin in used:
            raise RuntimeError(f"{pinLabel(board, pin)} is used by both {used[pin]} and {name}")

        used[pin] = name

    for name in DISPLAY_PINS:
        pin = hw["pins"].get(name, -1)

        if pin < 0:
            if name not in OPTIONAL_PINS:
                raise RuntimeError(f"Display pin {name} is not set")

            continue

        take(pin, name)

    sd = hw["sd"]

    if sd["enabled"]:
        if sdSharesBus(hw):
            # One bus, two chip selects: SCK and MOSI are the display's, the card adds MISO (which the
            # display never drives) and its own CS.
            if sd["miso"] < 0:
                raise RuntimeError("SD miso is not set (the card has to answer on it)")

            take(sd["miso"], "SD miso")
            take(sd["cs"], "SD cs")
        else:
            for name in SD_PINS:
                take(sd[name], f"SD {name}")

        if not 400000 <= sd["hz"] <= 40000000:
            raise RuntimeError("The SD card's SPI clock must be between 0.4 and 40 MHz")

    expander = hw["expander"]

    if expander["enabled"]:
        if expander["type"] not in EXPANDERS:
            raise RuntimeError(f"Unknown I2C expander {expander['type']}")

        for name in ("sda", "scl"):
            take(expander[name], f"I2C {name}")

        if not 0x08 <= expander["address"] <= 0x77:
            raise RuntimeError("The expander's I2C address must be between 0x08 and 0x77")

    for action, pin in buttonPins:
        if pin < 0:
            continue

        if isExpanderPin(pin):
            if stm32:
                raise RuntimeError(f"{action}: the STM32 firmware reads buttons on GPIO pins only")

            if pin in used:
                raise RuntimeError(f"{pinLabel(board, pin)} of {action} is also {used[pin]}")

            used[pin] = action
            continue

        if pin in used:
            raise RuntimeError(f"{pinLabel(board, pin)} of {action} is also the {used[pin]} pin")

        if pin not in usable:
            raise RuntimeError(f"{pinLabel(board, pin)} of {action} is not usable on {board['name']}")

    if display.get("controller") not in CONTROLLERS:
        raise RuntimeError(f"Unsupported display controller {display.get('controller')}")

    width, height = resolution(display, hw["orientation"])

    if width % 2 or height % 2 or min(width, height) < 64:
        raise RuntimeError(f"Display resolution {width}x{height} is not supported (even sides, at least 64)")

    if not 1000000 <= hw["spi_hz"] <= 80000000:
        raise RuntimeError("SPI clock must be between 1 and 80 MHz")

    if hw.get("spi_mode") not in SPI_MODES:
        raise RuntimeError(f"SPI mode {hw.get('spi_mode')} is not one of {SPI_MODES}")


# Things that still build and usually work, but that the user should know about.
def warnings(hw):
    out = []

    if hw["pins"].get("cs", -1) < 0:
        if hw["spi_mode"] != 3:
            out.append("A display without CS usually wants SPI mode 3: with the clock idling high "
                       "the panel does not take the start of a transfer for a data bit.")

        if hw["sd"]["enabled"] and sdSharesBus(hw):
            out.append("The display has no CS and shares the bus with the SD card, so the panel stays "
                       "selected while the card is read and its traffic can leave stray pixels on the "
                       "screen. Wire the panel's CS (many modules have the pad on the back), or give "
                       "the card its own SCK/MOSI.")

    return out


def writeIfChanged(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)

    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


# STM32 SCK/MOSI that the board's SPI peripheral can not drive: the firmware sends bit by bit instead.
def softwareSpi(hw, board):
    spi = board.get("spi")
    return bool(spi) and (hw["pins"]["sck"] not in spi["sck"] or hw["pins"]["mosi"] not in spi["mosi"])


def boardHeader(hw, board, display):
    p = hw["pins"]
    width, height = resolution(display, hw["orientation"])
    panelW, panelH = display["panel"]
    memoryW, memoryH = display.get("memory", display["panel"])
    offset_x, offset_y = display.get("offset", [0, 0])
    stm32 = platform(board) == "stm32"
    source = f"hardware/boards/{hw['board']}.json, " + (
        "custom display" if hw["display"] == "custom" else f"hardware/displays/{hw['display']}.json")
    sd, expander = hw["sd"], hw["expander"]
    sdEnabled = bool(sd["enabled"]) and not stm32
    expanderEnabled = bool(expander["enabled"]) and expander["sda"] >= 0 and expander["scl"] >= 0
    shared = sdEnabled and sdSharesBus(hw)
    spiHost = board.get("spi_host", "SPI2_HOST")
    busClass = "lgfx::Bus_SPI"

    if stm32 and softwareSpi(hw, board):
        includes = '#define JET_SOFT_SPI 1\n#include "soft_spi.hpp"\n'
        pinsNote = "    // Pins are STM32 PinName values (port * 16 + pin). SCK/MOSI are not on the SPI peripheral: software SPI.\n"
        boardExtra = """    constexpr const char* port = "software SPI";
    inline void begin_spi(){}
    inline int init_shared_spi() { return 0; }
"""
        bus, busClass = "", "SoftSpiBus"
        #main.cpp switches the backlight on
        lightMember, light = "", ""
    elif stm32:
        #LovyanGFX's STM32 bus writes the SPI registers itself but takes transactions from Arduino's SPI object.
        #begin_spi() starts that object on the same pins and peripheral.
        includes = "#define JET_SOFT_SPI 0\n#include <SPI.h>\n"
        pinsNote = "    // Pins are STM32 PinName values (port * 16 + pin).\n"
        boardExtra = f"""    constexpr const char* port = "{board['spi']['port']}";
    inline SPI_TypeDef* spi(){{ return {board['spi']['port']}; }}

    inline void begin_spi(){{
        SPI.setSCLK(PinName(clock));
        SPI.setMOSI(PinName(mosi));
        SPI.begin();
    }}

    inline int init_shared_spi(){{ return 0; }}
"""
        bus = f"""            b.spi_port = {board['spi']['port']};
            b.spi_mode = Board::spiMode;
            b.freq_write = Board::writeHz;
            b.freq_read = 8000000;"""
        #main.cpp switches the backlight on
        lightMember, light = "", ""
    else:
        includes, pinsNote = "", ""
        if shared:
            # The bus belongs to the firmware (the SD card is on it too): LovyanGFX only attaches its device.
            includes = "#include <driver/spi_master.h>\n"
            boardExtra = f"""    // The display and the SD card share this bus: the firmware initializes it, LovyanGFX attaches to it.
    inline int init_shared_spi(){{
        spi_bus_config_t config = {{}};
        config.mosi_io_num = Board::mosi;
        config.miso_io_num = Board::miso;
        config.sclk_io_num = Board::clock;
        config.quadwp_io_num = -1;
        config.quadhd_io_num = -1;
        config.max_transfer_sz = 4096;
        return int(spi_bus_initialize({spiHost}, &config, SPI_DMA_CH_AUTO));
    }}
"""
        else:
            boardExtra = "    inline int init_shared_spi() { return 0; }\n"
        #SPI2 on the ESP32-S3, where the queued DMA scanout drives it; a board profile may name another host
        note = "            //required by the queued DMA scanout\n" if board.get("chip", "esp32s3") == "esp32s3" else ""
        bus = f"""{note}            b.spi_host = {spiHost};
            b.spi_mode = Board::spiMode;
            b.freq_write = Board::writeHz;
            b.freq_read = 16000000;
            b.spi_3wire = false;
            b.use_lock = true;
            b.dma_channel = SPI_DMA_CH_AUTO;"""
        lightMember = "        lgfx::Light_PWM light;\n"
        light = """
            if(Board::backlight >= 0){
                auto l = light.config();
                l.pin_bl = Board::backlight;
                l.invert = false;
                l.freq = 44100;
                l.pwm_channel = 7;
                light.config(l);
                panel.setLight(&light);
            }
"""

    busLines = bus + "\n" if bus else ""
    # SD card and I2C expander, as constants the firmware reads (0 disables the code that uses them).
    macros = f"""#define JET_SD_CARD {int(sdEnabled)}
#define JET_I2C_EXPANDER {int(expanderEnabled)}
"""
    peripherals = ""
    if sdEnabled:
        host = spiHost if shared else "SPI3_HOST"
        peripherals += f"""    constexpr bool sdSharedSpi = {'true' if shared else 'false'};
    constexpr int sdHost = {host};
    constexpr int sdSck = {sd['sck']}, sdMosi = {sd['mosi']}, sdMiso = {sd['miso']}, sdCs = {sd['cs']};
    constexpr int sdHz = {sd['hz']};
    constexpr const char* sdMount = {json.dumps(sd['mount'])};
"""
    else:
        peripherals += """    constexpr bool sdSharedSpi = false;
    constexpr int sdHost = 0, sdSck = -1, sdMosi = -1, sdMiso = -1, sdCs = -1, sdHz = 0;
    constexpr const char* sdMount = "";
"""
    if expanderEnabled:
        peripherals += f"""    constexpr int expanderSda = {expander['sda']}, expanderScl = {expander['scl']};
    constexpr int expanderAddress = {expander['address']};
    constexpr int expanderBits = {EXPANDERS[expander['type']]};
    constexpr bool expanderActiveHigh = {'true' if expander['active_high'] else 'false'};
"""
    else:
        peripherals += "    constexpr int expanderSda = -1, expanderScl = -1, expanderAddress = 0, expanderBits = 0;\n    constexpr bool expanderActiveHigh = false;\n"
    peripherals += f"    constexpr const char* menuAction = {json.dumps(hw['menu_action'])};\n"
    miso = sd["miso"] if shared else -1
    return f"""#pragma once
// Generated by tools/hardware.py from {source} and the project's hardware settings.
// Do not edit: change them in Project Settings.
// Panel/bus setup adapted from board_3d / JetExamples (MIT, see hardware/LICENSE.JetExamples).
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
{includes}
{macros}
namespace Board{{
    constexpr const char* name = {json.dumps(board['name'] + ' + ' + display['name'], ensure_ascii=False)};
    constexpr int screenWidth = {width}, screenHeight = {height}, rotation = {rotation(display, hw['orientation'])};
{pinsNote}    constexpr int clock = {p['sck']}, mosi = {p['mosi']}, miso = {miso}, dc = {p['dc']}, cs = {p['cs']}, reset = {p.get('reset', -1)}, backlight = {p.get('backlight', -1)};
    constexpr int spiMode = {hw['spi_mode']};
    constexpr int writeHz = {hw['spi_hz']};
{peripherals}{boardExtra}}}

class LGFX : public lgfx::LGFX_Device{{
    public:
        {busClass}& panel_bus(){{ return bus; }}

        LGFX(){{
            auto b = bus.config();
{busLines}            b.pin_sclk = Board::clock;
            b.pin_mosi = Board::mosi;
            b.pin_miso = Board::miso;
            b.pin_dc = Board::dc;
            bus.config(b);
            panel.setBus(&bus);

            auto c = panel.config();
            c.pin_cs = Board::cs;
            c.pin_rst = Board::reset;
            c.pin_busy = -1;
            c.panel_width = {panelW};
            c.panel_height = {panelH};
            c.memory_width = {memoryW};
            c.memory_height = {memoryH};
            c.offset_x = {offset_x};
            c.offset_y = {offset_y};
            c.offset_rotation = 0;
            c.dummy_read_pixel = 8;
            c.dummy_read_bits = 1;
            c.readable = false;
            c.invert = {'true' if display.get('invert') else 'false'};
            c.rgb_order = {'true' if display.get('rgb_order') else 'false'};
            c.dlen_16bit = false;
            c.bus_shared = Board::sdSharedSpi;
            panel.config(c);
{light}
            setPanel(&panel);
        }}

    private:
        lgfx::{CONTROLLERS[display['controller']]} panel;
{lightMember}        {busClass} bus;
}};
"""


def boardsIni(target):
    lines = ["; Generated by tools/hardware.py from hardware/boards/*.json. Do not edit.",
             "; One environment per board, debug and release.", ""]

    for key, board in catalog("boards").items():
        if firmwareDir(board) != target:
            continue

        if target == FIRMWARE:
            #PlatformIO writes its board's flash size into the image header, whatever sdkconfig says
            sizes = [line.split("=")[0].removeprefix("CONFIG_ESPTOOLPY_FLASHSIZE_") for line in board.get("sdkconfig", [])
                     if line.startswith("CONFIG_ESPTOOLPY_FLASHSIZE_")]
            flash = [f"board_upload.flash_size = {sizes[0]}"] if sizes else []
            #extra platformio.ini lines the board profile needs (partitions, build flags, ...)
            extra = list(board.get("platformio", []))
            lines += [f"[env:{key}]", "extends = jet", f"board = {board['platformio_board']}", *flash, *extra,
                      f"board_build.cmake_extra_args = -DJET_BOARD={key}", "",
                      f"[env:{key}_release]", f"extends = env:{key}",
                      f"board_build.cmake_extra_args = -DJET_BOARD={key} -DJET_RELEASE=ON", ""]
        else:
            #USB CDC for the log on boards without an ST-LINK virtual COM port.
            flags = "${jet.build_flags}" + (" -DPIO_FRAMEWORK_ARDUINO_ENABLE_CDC -DUSBCON" if board.get("usb_serial") else "")
            extra = list(board.get("platformio", []))
            lines += [f"[env:{key}]", "extends = jet", f"board = {board['platformio_board']}", *extra,
                      f"upload_protocol = {board.get('upload', 'stlink')}", f"build_flags = {flags}", "",
                      f"[env:{key}_release]", f"extends = env:{key}", f"build_flags = {flags} -DJET_RELEASE", ""]

    return "\n".join(lines)


# Writes the firmware files; returns (environment, (width, height), board label, firmware dir).
def generate(config, buttonPins=()):
    hw, board, display = settings(config)
    validate(hw, board, display, buttonPins)
    target = firmwareDir(board)
    writeIfChanged(target / "src/generated_board.hpp", boardHeader(hw, board, display))

    for folder in (FIRMWARE, FIRMWARE_STM32):
        writeIfChanged(folder / "boards.ini", boardsIni(folder))

    for key, data in catalog("boards").items():
        if platform(data) == "esp32":
            writeIfChanged(FIRMWARE / "boards" / f"{key}.defaults",
                             f"# Generated from hardware/boards/{key}.json (flash and PSRAM).\n" + "\n".join(data["sdkconfig"]) + "\n")

    return hw["board"], resolution(display, hw["orientation"]), f"{board['name']} + {display['name']}", target
