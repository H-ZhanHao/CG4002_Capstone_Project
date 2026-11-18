"""Archive Stage 4 without replacing earlier archives or rebuilding to flash.

Run from the repository root: archive_stage4.py ARCHIVE --node node1_stage4
immediately after its build, then --node node2_stage4 after that build, then
--finalize after updating the checkpoint. ARCHIVE must be outside .pio/build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[5]
FIRMWARE = ROOT / "communications/firmware/FireBeetle_to_Ultra96"


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest(folder):
    data = {p.relative_to(folder).as_posix(): checksum(p)
            for p in sorted(folder.rglob("*"))
            if p.is_file() and p != folder / "manifest.json"}
    with (folder / "manifest.json").open("x", encoding="utf-8") as stream:
        json.dump(data, stream, indent=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--node", choices=("node1_stage4", "node2_stage4"))
    group.add_argument("--finalize", action="store_true")
    args = parser.parse_args()
    destination = args.archive.resolve()
    if destination == FIRMWARE / ".pio/build" or FIRMWARE / ".pio/build" in destination.parents:
        parser.error("Archive must be outside .pio/build")
    destination.mkdir(parents=True, exist_ok=True)
    if args.node:
        source = FIRMWARE / ".pio/build" / args.node
        node = destination / args.node
        node.mkdir()  # Refuse to overwrite any existing node archive.
        for name in ("bootloader.bin", "partitions.bin", "firmware.bin"):
            shutil.copy2(source / name, node / name)
        shutil.copy2(FIRMWARE / f"sdkconfig.{args.node}", node)
        shutil.copy2(ROOT / ".build/firmware_stages/stage3_2026-11-01" /
                     "node1_stage3/flash.ps1", node)
        manifest(node)
    else:
        for name in ("node1_stage4", "node2_stage4"):
            if not (destination / name / "manifest.json").is_file():
                parser.error(f"Missing archived node: {name}")
        # Include communications sources, profiles, test fixtures and canonical
        # hardware used by components. Exclude generated builds and local logs.
        excluded = {".pio", ".tmp", ".git", "__pycache__", "Library", "Temp",
                    "obj", "bin", "Logs", "UserSettings", ".venv"}
        suffixes = {".py", ".ps1", ".c", ".cpp", ".h", ".hpp", ".ini", ".csv",
                    ".txt", ".md", ".json", ".cs", ".asmdef", ".meta", ".unity",
                    ".asset", ".cmake", ".yml", ".yaml"}
        with zipfile.ZipFile(destination / "source.zip", "x", zipfile.ZIP_DEFLATED) as archive:
            archive.write(ROOT / ".gitignore", ".gitignore")
            for base in (ROOT / "communications", ROOT / "hardware"):
                for path in sorted(base.rglob("*")):
                    relative = path.relative_to(ROOT)
                    if not path.is_file() or excluded.intersection(relative.parts):
                        continue
                    if (path.suffix in suffixes or path.name.startswith("sdkconfig") or
                            path.name in ("Kconfig", "Kconfig.projbuild", ".gitignore")):
                        archive.write(path, relative.as_posix())
        manifest(destination)
    print(f"Archived and checksummed: {destination}")


if __name__ == "__main__":
    main()
