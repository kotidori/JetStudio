#!/usr/bin/env python3
"""Puts exported .jetpack games on the SD card of a handheld, in the layout the launcher reads.

    ./jet-studio sd --pack build/game.jetpack            # one game
    python3 tools/sdcard.py push a.jetpack b.jetpack     # several, into the only card found
    python3 tools/sdcard.py list                         # what looks like a card right now
    python3 tools/sdcard.py push --dest /media/me/CARD --titles a.jetpack

The launcher reads <card>/games/*.jetpack (and the *.jetpack files in the card's root), and takes
the titles it shows from <card>/games/games.txt, else from the project name inside each package.
--titles writes games.txt from the packages, so the menu shows the project names even for file
names like build_17.jetpack. Nothing else on the card is touched.
"""
import argparse
import os
import shutil
import struct
import sys
from pathlib import Path

FOLDER = "games"
TITLES = "games.txt"


def project_name(pack: Path) -> str:
    """The project name a package carries: magic, version, then a u32 length and the UTF-8 name."""
    with open(pack, "rb") as file:
        header = file.read(512)
    if len(header) < 12 or header[:4] != b"JETP":
        raise RuntimeError(f"{pack.name} is not a .jetpack file: export the project again")
    (length,) = struct.unpack_from("<I", header, 8)
    if not length or length > 256 or 12 + length > len(header):
        return ""
    return header[12:12 + length].decode("utf-8", "replace")


def windows_cards():
    """Drive letters that look like a card: removable media, or a fixed drive we wrote games to.

    Card readers disagree about GetDriveType: a USB reader usually reports DRIVE_REMOVABLE (2), some
    report DRIVE_FIXED (3), so a fixed drive only counts when it already looks like a game card.
    """
    import ctypes
    import string
    system = os.environ.get("SystemDrive", "C:").rstrip(":").upper()
    kernel = ctypes.windll.kernel32
    found = []
    for letter in string.ascii_uppercase:
        if letter == system:
            continue
        root = f"{letter}:\\"
        if not os.path.exists(root):
            continue
        kind = kernel.GetDriveTypeW(root)
        if kind == 2 or (kind == 3 and ((Path(root) / FOLDER).is_dir()
                                       or any(Path(root).glob("*.jetpack")))):
            found.append(Path(root))
    return found


def cards():
    """Mounted FAT filesystems, which is what an SD card for the launcher is formatted as."""
    if sys.platform == "win32":
        return windows_cards()
    roots = ("/media/", "/mnt/", "/run/media/", "/Volumes/")
    found = []
    try:
        mounts = Path("/proc/mounts").read_text().splitlines()
    except OSError:
        return found  # not Linux: --dest says where the card is
    for line in mounts:
        parts = line.split()
        if len(parts) < 3 or parts[2] not in ("vfat", "exfat"):
            continue
        # /proc/mounts escapes spaces and unicode as \040 and \xxx.
        path = str(Path(parts[1].encode().decode("unicode_escape")))
        if path.startswith(roots) and Path(path).is_dir():
            found.append(Path(path))
    return sorted(set(found))


def destination(argument):
    if argument:
        path = Path(argument).expanduser()
        if not path.is_dir():
            raise RuntimeError(f"{path} is not a mounted folder")
        return path
    found = cards()
    if len(found) == 1:
        return found[0]
    if not found:
        raise RuntimeError("No SD card found. Mount it (WSL2: usbipd attach, then "
                           "sudo mount /dev/sdb1 /mnt/sd) and pass --dest, or copy the games yourself.")
    raise RuntimeError("Several cards are mounted: " + ", ".join(map(str, found)) + ". Pass --dest.")


def sync() -> None:
    """Flush the pending writes of the card. Windows has no sync(); there the copied
    files are already closed by copyfile and the volume flushes on its own."""
    if hasattr(os, "sync"):
        os.sync()


def free_bytes(path: Path) -> int:
    usage = shutil.disk_usage(path)
    return usage.free


def push(packs, dest, titles):
    folder = dest / FOLDER
    folder.mkdir(exist_ok=True)
    print(f"Card: {dest} ({free_bytes(dest) // (1 << 20)} MB free)")
    written = []
    for pack in map(Path, packs):
        if not pack.is_file():
            raise RuntimeError(f"{pack} does not exist: export the project first")
        name = project_name(pack)
        size = pack.stat().st_size
        if size > free_bytes(dest):
            raise RuntimeError(f"{pack.name} is {size // (1 << 20)} MB, the card has {free_bytes(dest) // (1 << 20)} MB free")
        target = folder / pack.name
        shutil.copyfile(pack, target)
        sync()
        print(f"  {target} · {size // 1024} KB" + (f" · {name}" if name else ""))
        written.append((pack.name, name or pack.stem.replace("_", " ").replace("-", " ").title()))
    if titles:
        # Merged with what is already there: pushing one more game must not lose the other titles.
        known = {}
        path = folder / TITLES
        if path.is_file():
            for line in path.read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                name, title = line.split("=", 1)
                if name.strip() and title.strip():
                    known[name.strip()] = title.strip()
        for name, title in written:
            if title:
                known[name] = title
        lines = ["# Written by tools/sdcard.py: what the launcher shows for each game."]
        # Only the games that are on the card: a title for a deleted file only confuses.
        lines += [f"{name} = {title}" for name, title in known.items() if (folder / name).is_file()]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        sync()
        print(f"  {path} · {len(lines) - 1} title(s)")
    print(f"{len(written)} game(s) in {folder}. The launcher lists them at the next start;")
    print("holding the menu button for 2 s comes back to it from a game.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("list", help="show the mounted cards")
    pushing = actions.add_parser("push", help="copy .jetpack files into games/ on the card")
    pushing.add_argument("packs", nargs="*", type=Path, help=".jetpack files to copy into games/")
    pushing.add_argument("--pack", action="append", type=Path, default=[],
                         help="same as a positional pack, for ./jet-studio sd --pack file.jetpack")
    pushing.add_argument("--dest", help="where the card is mounted (default: the only card found)")
    pushing.add_argument("--titles", action="store_true", help="write games/games.txt from the project names")
    args = parser.parse_args()
    if args.action == "list":
        found = cards()
        print("\n".join(map(str, found)) if found else "No SD card is mounted.")
        return 0
    packs = list(args.packs) + list(args.pack)
    if not packs:
        raise RuntimeError("Nothing to copy: pass the .jetpack files (or --pack file.jetpack)")
    push(packs, destination(args.dest), args.titles)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
