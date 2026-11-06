#!/usr/bin/env python3
# Builds and flashes started by the editor, run as a separate process so the UI stays responsive.
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import traceback

import hardware

ROOT = Path(__file__).resolve().parents[1]

# A firmware with no embedded game: everything comes from the SD card (launcher).
EMPTY_SCENE = """#pragma once
// Generated from the editor scene. Do not edit.
#include <cstdint>
// No game is embedded in this firmware: it runs the .jetpack files on the SD card.
static constexpr uint8_t scene_data[] = {0};
"""


def generateScene(pack, firmware, sdCard=False):
    data = pack.read_bytes()
    #just a sanity check, the device validates the whole package anyway.
    if len(data) < 12 or data[:4] != b"JETP":
        raise RuntimeError("Not a .jetpack file: export the project again")

    version = struct.unpack_from("<I", data, 4)[0]

    if version < 18:
        #the editor exports with the bridge it loaded at startup, so an old version means a stale editor.
        raise RuntimeError(f"The package is version {version}, the firmware reads version 18: Jet Studio was updated "
                           "while it was running. Restart Jet Studio and build again")

    if version != 18:
        raise RuntimeError(f"Unknown .jetpack version {version} (expected 18): update Jet Studio")

    if len(data) > 1024 * 1024:
        if sdCard:
            # Too big to sit next to the firmware in flash, and it doesn't have to: the card holds it.
            print(f"Scene is {len(data) // 1024} KB: not embedded, it runs from the SD card", flush=True)
            hardware.writeIfChanged(firmware / "src/generated_scene.hpp", EMPTY_SCENE)
            return

        raise RuntimeError("Scene package larger than 1 MiB")

    lines = ["#pragma once", "// Generated from the editor scene. Do not edit.", "#include <cstdint>",
             "static constexpr uint8_t scene_data[] = {"]

    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset+16]) + ",")

    lines.append("};\n")
    hardware.writeIfChanged(firmware / "src/generated_scene.hpp", "\n".join(lines))
    print(f"Scene embedded: {len(data)} bytes", flush=True)


# Godot's built-in actions come first in the Input Map, same as in editor/scripts/input_map.gd.
BUILTIN_ACTIONS = ["ui_left", "ui_right", "ui_up", "ui_down", "ui_accept", "ui_cancel"]


# [(action, pin)] in bit order, built the same way as in the editor.
def input_map(config):
    stored = [a for a in config.get("input_map", []) if isinstance(a, dict)]
    #old project format
    legacy = config.get("input_pins", {})
    byName = {str(a.get("name", "")): a for a in stored}
    out = [(name, int(byName.get(name, {}).get("pin", legacy.get(name, -1)))) for name in BUILTIN_ACTIONS]

    for action in stored:
        name = str(action.get("name", ""))

        if name not in BUILTIN_ACTIONS and name and len(out) < 32:
            out.append((name, int(action.get("pin", -1))))

    return out


# Writes generated_inputs.hpp; returns the [(action, pin)] that are wired, for the hardware check.
def generateInputs(config, firmware, board):
    actions = input_map(config)
    pins = [pin for _, pin in actions]
    used = [p for p in pins if p >= 0]

    if len(used) != len(set(used)):
        raise RuntimeError("Two actions use the same pin")

    active_high = sum(1 << i for i, pin in enumerate(pins) if pin >= 0 and pin in board.get("active_high", []))
    content = ("#pragma once\n#include <cstdint>\n// Generated from the project's Input Map (project.jetproject). Do not edit.\n"
               "// One pin per action, in package order (bit i = action i). -1 means no button.\n"
               "// ESP32: GPIO number. STM32: PinName (port * 16 + pin).\n"
               "// " + ", ".join(name for name, _ in actions) + "\n"
               "// Buttons connect the pin to GND (internal pull-up). Bit i of input_active_high: the board's own button,\n"
               "// pressed = high (it has its own pull-down).\n"
               f"static constexpr int input_pins[{len(pins)}] = {{{', '.join(map(str, pins))}}};\n"
               f"static constexpr uint32_t input_active_high = 0x{active_high:x};\n"
               "// A pin of 100 + n is bit n of the board's I2C expander (see tools/hardware.py).\n"
               f"static constexpr const char* const input_names[{len(pins)}] = "
               f"{{{', '.join(json.dumps(name) for name, _ in actions)}}};\n")
    hardware.writeIfChanged(firmware / "src/generated_inputs.hpp", content)
    print("Buttons: " + (", ".join(f"{a}={hardware.pinLabel(board, p)}" for a, p in actions if p >= 0) or "none"), flush=True)
    return [(a, p) for a, p in actions if p >= 0]


# Reads (width, height) and the action names from a .jetpack header.
def packHeader(pack):
    data = pack.read_bytes()
    nameLength = struct.unpack_from("<I", data, 8)[0]
    offset = 12 + nameLength
    width, height = struct.unpack_from("<HH", data, offset)
    offset += 4
    count = data[offset]
    offset += 1
    names = []

    for _ in range(count):
        length = struct.unpack_from("<I", data, offset)[0]
        names.append(data[offset + 4:offset + 4 + length].decode("utf-8"))
        offset += 4 + length
        keys = data[offset]
        offset += 1

        for _ in range(keys):
            offset += 4 + struct.unpack_from("<I", data, offset)[0]

    return (width, height), names


# ESP-IDF only reads sdkconfig.defaults when creating sdkconfig.<env>, so delete it when a default changed.
def refreshSdkconfig(environment):
    firmware = ROOT / "firmware"
    generated = firmware / f"sdkconfig.{environment}"
    board = environment.removesuffix("_release")
    #sdkconfig.defaults.<chip> too, ESP-IDF reads it for that chip
    defaults = sorted(firmware.glob("sdkconfig.defaults*")) + [firmware / "boards" / f"{board}.defaults"]

    if environment.endswith("_release"):
        defaults.append(firmware / "sdkconfig.release")

    if generated.exists() and any(d.stat().st_mtime > generated.stat().st_mtime for d in defaults):
        print(f"Profile settings changed: regenerating {generated.name}", flush=True)
        generated.unlink()


def pioExecutable():
    executable = shutil.which("pio")

    if executable:
        return executable

    for local in [Path.home() / ".platformio/penv/bin/pio", Path.home() / ".platformio/penv/Scripts/pio.exe"]:
        if local.is_file():
            return str(local)

    raise RuntimeError("PlatformIO Core is missing. Install it before building the firmware.")


# Environment for pio/esptool. GIT_CEILING_DIRECTORIES stops git's upward search for a
# repository: a stray .git above the project (an empty one in the user's home, say) makes
# ESP-IDF's revision probe blow up — grabRef.cmake dies reading a head-ref that a HEAD with
# no commits never produced. The project's own repository (at ROOT) stays visible.
def pioEnvironment():
    environment = os.environ.copy()
    ceiling = [str(ROOT.parent), str(Path.home())]
    existing = environment.get("GIT_CEILING_DIRECTORIES", "")

    if existing:
        ceiling.append(existing)

    environment["GIT_CEILING_DIRECTORIES"] = os.pathsep.join(dict.fromkeys(ceiling))
    return environment


def execute(args):
    if args.target == "desktop":
        if args.action == "flash":
            raise RuntimeError("Select a board target for flashing")

        if not (ROOT / "CMakeLists.txt").exists():
            print("Release package: the PC players are already built.", flush=True)
            return 0

        commands = [["cmake", "-S", str(ROOT), "-B", str(ROOT / "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                    ["cmake", "--build", str(ROOT / "build"), "--target", "jet_player", "jet_player_device", "-j", str(os.cpu_count() or 4)]]
        cwd = ROOT
    else:
        if not (ROOT / "firmware/components/LovyanGFX/CMakeLists.txt").exists():
            raise RuntimeError("LovyanGFX missing. Run ./jet-studio setup")

        config = json.loads((args.project / "project.jetproject").read_text(encoding="utf-8")) if args.project else {}
        hw, profile, _ = hardware.settings(config)
        firmware = hardware.firmwareDir(profile)

        if args.launcher:
            # A firmware with nothing in it but the menu of the card's games.
            if not hw["sd"]["enabled"]:
                raise RuntimeError("The launcher firmware runs the card's games: enable the SD card "
                                   "in Project Settings > Hardware")

            hardware.writeIfChanged(firmware / "src/generated_scene.hpp", EMPTY_SCENE)
            print("Launcher firmware: no game embedded, it boots into the SD card's menu", flush=True)
        else:
            generateScene(args.pack, firmware, sdCard=bool(hw["sd"]["enabled"]))

        buttons = generateInputs(config, firmware, profile)
        board, size, name, firmware = hardware.generate(config, buttons)

        if not args.launcher:
            resolution, actions = packHeader(args.pack)

            if tuple(resolution) != tuple(size):
                raise RuntimeError(f"The scene is {resolution[0]}x{resolution[1]} but the display is "
                                   f"{size[0]}x{size[1]}: export the project again with its hardware settings")

            if actions != [action for action, _ in input_map(config)]:
                raise RuntimeError("The package's Input Map differs from the project's: export the project again")

        environment = board + ("_release" if args.profile == "release" else "")
        esp32 = firmware == hardware.FIRMWARE

        if esp32:
            refreshSdkconfig(environment)

        print(f"Hardware: {name}, {size[0]}x{size[1]}", flush=True)
        print(f"Profile: {args.profile} ({environment})", flush=True)

        for warning in hardware.warnings(hw):
            print(f"Note: {warning}", flush=True)

        command = [pioExecutable(), "run", "-e", environment]

        if args.action == "flash": command += ["-t", "upload"]
        #STM32: ST-LINK, no port
        if args.port and esp32: command += ["--upload-port", args.port]

        commands, cwd = [command], firmware

    for command in commands:
        print("+", " ".join(command), flush=True)
        # Explicit handles: under the Windows editor the inherited ones are invalid (see --job),
        # and pio/esptool would lose their output or die writing it.
        result = subprocess.run(command, cwd=cwd, stdout=sys.stdout, stderr=sys.stderr,
                                env=pioEnvironment())

        if result.returncode:
            return result.returncode

    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["build", "flash"])
    #"nano" is the old name of "device".
    parser.add_argument("--target", choices=["desktop", "device", "nano"], default="desktop")
    parser.add_argument("--pack", type=Path, help="The .jetpack the editor exported: embedded in the firmware "
                        "(or run from the card when it is too big). Not needed with --launcher")
    parser.add_argument("--launcher", action="store_true",
                        help="Device firmware with no game in it: it boots into the SD card's menu")
    parser.add_argument("--port")
    parser.add_argument("--profile", choices=["debug", "release"], default="debug",
                        help="Firmware: debug keeps the serial log, release removes console and logs")
    parser.add_argument("--project", type=Path, help="Project folder: hardware and button pins from project.jetproject")
    parser.add_argument("--job", type=Path)
    args = parser.parse_args()

    if args.launcher and args.target == "desktop":
        parser.error("--launcher is for the device firmware: the PC player always runs a package")

    if not args.launcher and not args.pack:
        parser.error("Specify --pack path/to/game.jetpack, or --launcher for a firmware with no game in it")

    result = 1
    stream = None

    if args.job:
        args.job.parent.mkdir(parents=True, exist_ok=True)
        stream = open(str(args.job) + ".log", "w", buffering=1, encoding="utf-8", errors="replace")

        if sys.platform == "win32":
            # The editor is a GUI process: it starts this job without a console, so the std
            # handles it was born with are invalid — and sys.stdout/sys.stderr cached them at
            # startup, which os.dup2() cannot repair. Replace the streams themselves; the dup2
            # is a best effort for anything that writes to fd 1/2 behind Python's back.
            sys.stdout = stream
            sys.stderr = stream

            try:
                os.dup2(stream.fileno(), 1)
                os.dup2(stream.fileno(), 2)
            except OSError:
                pass
        else:
            os.dup2(stream.fileno(), 1)
            os.dup2(stream.fileno(), 2)

        #the editor reads the log as UTF-8, whatever the system code page
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
        sys.stderr.reconfigure(encoding="utf-8")

    try:
        result = execute(args)
    except RuntimeError as error:
        print(f"ERROR: {error}", flush=True)
    except Exception as error:
        #a bug, not a user mistake: the traceback goes to the log too
        print(f"ERROR: {error}", flush=True)
        traceback.print_exc()
    finally:
        sys.stdout.flush()
        sys.stderr.flush()

        if args.job:
            path = Path(str(args.job) + ".json")
            temporary = path.with_suffix(".tmp")
            temporary.write_text(json.dumps({"returncode": result}), encoding="utf-8")
            temporary.replace(path)

    return result


if __name__ == "__main__":
    sys.exit(main())
