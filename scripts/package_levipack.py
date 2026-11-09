import argparse
import json
import re
import sys
import zipfile
from pathlib import Path

VALUE_PATTERN = re.compile(r'^\s*inline\s+constexpr\s+std::string_view\s+(Name|Author|Description|Version)\s*=\s*"((?:\\.|[^"\\])*)";\s*$')
REQUIRED_VALUES = ("Name", "Author", "Description", "Version")
LIBRARY_NAME = "libBackgroundVideo.so"
VIDEO_ENTRY = "resources/background.mp4"


def parse_version(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = VALUE_PATTERN.match(line)
        if match:
            values[match.group(1)] = bytes(match.group(2), "utf-8").decode("unicode_escape")
    missing = [name for name in REQUIRED_VALUES if not values.get(name)]
    if missing:
        raise ValueError("Missing version metadata: " + ", ".join(missing))
    return values


def build_manifest(values: dict[str, str], with_video: bool) -> dict[str, object]:
    overwrite = ["icon.png"] + ([VIDEO_ENTRY] if with_video else [])
    return {
        "type": "preload-native",
        "name": values["Name"],
        "author": values["Author"],
        "description": values["Description"],
        "version": values["Version"],
        "entry": LIBRARY_NAME,
        "icon": "icon.png",
        "overwrite_files": overwrite,
        "overwrite_folders": [],
    }


def write_package(library: Path, icon: Path, video: Path | None, version_header: Path, output: Path) -> None:
    for label, path in (("Library", library), ("Icon", icon), ("Version header", version_header)):
        if not path.is_file():
            raise FileNotFoundError(f"{label} not found: {path}")
    with_video = video is not None and video.is_file()
    if not with_video:
        print("note: packaging without a video; set video_path in config.json on the device.", file=sys.stderr)

    manifest = build_manifest(parse_version(version_header), with_video)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr("manifest.json", manifest_bytes)
        archive.write(library, LIBRARY_NAME)
        archive.write(icon, "icon.png")
        if with_video:
            # mp4 is already compressed; store it as-is
            archive.write(video, VIDEO_ENTRY, compress_type=zipfile.ZIP_STORED)

    expected = {"manifest.json", LIBRARY_NAME, "icon.png"} | ({VIDEO_ENTRY} if with_video else set())
    with zipfile.ZipFile(output, "r") as archive:
        if set(archive.namelist()) != expected:
            raise RuntimeError(f"Unexpected package entries: {sorted(archive.namelist())}")
        if json.loads(archive.read("manifest.json")) != manifest:
            raise RuntimeError("Manifest verification failed")
        if archive.getinfo(LIBRARY_NAME).file_size != library.stat().st_size:
            raise RuntimeError("Library verification failed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--icon", required=True, type=Path)
    parser.add_argument("--video", type=Path, default=None)
    parser.add_argument("--version-header", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        write_package(args.library.resolve(), args.icon.resolve(),
                      args.video.resolve() if args.video else None,
                      args.version_header.resolve(), args.output.resolve())
    except Exception as error:
        print(error, file=sys.stderr)
        return 1
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
