#!/usr/bin/env python3
"""Release consistency checks for GeoQuake Lite.

Checks the partition table, the version in src/version.h and docs/manifest.json, the manifest parts, the map pack
and (when they exist) the build outputs. Prints one PASS, FAIL or SKIP line per check and a final RESULT line.
Exit code 0 means no check failed. Standard library only.
"""
import argparse
import csv
import glob
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLASH_END = 0x1000000  # 16 MiB
LEVELS_BY_COUNT = {340: 4, 1364: 5, 5460: 6}
failures = []


def report(status, name, detail):
    print(f"{status:4}  {name}: {detail}")
    if status == "FAIL":
        failures.append(name)


def check(name, ok, detail):
    report("PASS" if ok else "FAIL", name, detail)
    return ok


def read_partitions(path):
    rows = {}
    with open(path, newline="") as f:
        for row in csv.reader(f):
            cells = [c.strip() for c in row]
            if not cells or not cells[0] or cells[0].startswith("#"):
                continue
            rows[cells[0]] = (int(cells[3], 0), int(cells[4], 0))
    return rows


def check_partitions(parts):
    missing = [n for n in ("nvs", "factory", "spiffs") if n not in parts]
    if not check("partitions: rows", not missing, "missing " + ", ".join(missing) if missing else "nvs, factory, spiffs"):
        return False
    spans = sorted((off, off + size, name) for name, (off, size) in parts.items())
    overlaps = [f"{a[2]}/{b[2]}" for a, b in zip(spans, spans[1:]) if b[0] < a[1]]
    check("partitions: no overlap", not overlaps, ", ".join(overlaps) or "none")
    end = max(e for _, e, _ in spans)
    check("partitions: fit in 16 MiB", end <= FLASH_END, f"last byte ends at {end:#x}")
    check("partitions: factory offset", parts["factory"][0] == 0x10000, f"{parts['factory'][0]:#x}")
    return True


def check_version(manifest, tag):
    text = open(os.path.join(ROOT, "src", "version.h")).read()
    m = re.search(r'#define\s+FIRMWARE_VERSION_STRING\s+"([^"]+)"', text)
    header = m.group(1) if m else None
    nums = [re.search(rf"#define\s+FIRMWARE_VERSION_{k}\s+(\d+)", text) for k in ("MAJOR", "MINOR", "PATCH")]
    parts = ".".join(n.group(1) for n in nums) if all(nums) else None
    check("version: header fields", header is not None and header == parts, f"string {header}, numbers {parts}")
    check("version: manifest", header == manifest.get("version"), f"version.h {header}, manifest {manifest.get('version')}")
    if tag is not None:
        check("version: tag", tag.lstrip("v") == header and tag.startswith("v"), f"tag {tag}, version.h {header}")


def check_manifest_parts(manifest, parts):
    builds = manifest.get("builds", [])
    found = {os.path.basename(p["path"]): p["offset"] for b in builds for p in b.get("parts", [])}
    want = {"bootloader.bin": 0, "partitions.bin": 0x8000,
            "firmware.bin": parts["factory"][0], "littlefs.bin": parts["spiffs"][0]}
    count = sum(len(b.get("parts", [])) for b in builds)
    check("manifest: four parts", count == 4 and set(found) == set(want), f"{count} parts: {', '.join(sorted(found))}")
    for name, off in want.items():
        check(f"manifest: {name} offset", found.get(name) == off, f"{found.get(name)} (want {off:#x})")
    # Each release has its own image folder, so a browser can never mix cached images of two releases.
    version = manifest.get("version")
    paths = [p["path"] for b in builds for p in b.get("parts", [])]
    odd = [p for p in paths if p != f"firmware/{version}/{os.path.basename(p)}"]
    check("manifest: image folder", not odd, ", ".join(odd) if odd else f"firmware/{version}/")


def check_pack(path, spiffs_size, levels):
    if not check("pack: present", os.path.isfile(path), path):
        return
    data = open(path, "rb").read()
    if not check("pack: header", len(data) >= 16 and data[:4] == b"QPAK", f"{len(data)} bytes, magic {data[:4]!r}"):
        return
    version, count = struct.unpack_from("<HH", data, 4)
    check("pack: version 2", version == 2, f"version {version}")
    found = LEVELS_BY_COUNT.get(count)
    if not check("pack: tile count", found is not None, f"{count} tiles" + (f" = z1..z{found}" if found else "")):
        return
    if levels is not None:
        check("pack: levels", found == levels, f"z1..z{found}, wanted z1..z{levels}")
    toc_end = 16 + count * 12
    bad = 0
    if toc_end > len(data):
        bad = count
    else:
        for i in range(count):
            off, length = struct.unpack_from("<II", data, 16 + i * 12 + 4)
            if off < toc_end or length == 0 or off + length > len(data):
                bad += 1
    check("pack: entries inside the file", bad == 0, f"{bad} of {count} entries out of bounds")
    need = len(data) * 1.02
    check("pack: fits the partition", need < spiffs_size, f"{len(data)} bytes + 2% = {need:.0f} < {spiffs_size}")


def unpack_image(image, size, out):
    """Unpacks image with the first mklittlefs that runs here. Returns the exit code, or None without a tool."""
    for tool in sorted(glob.glob(os.path.expanduser("~/.platformio/packages/tool-mklittlefs*/mklittlefs"))):
        try:
            cmd = [tool, "-u", out, "-b", "4096", "-p", "256", "-s", str(size), image]
            return subprocess.run(cmd, capture_output=True).returncode
        except OSError:  # wrong CPU type or not executable: try the next folder
            continue
    return None


def read_partition_table(path):
    """{name: (offset, size)} from a generated partitions.bin (32-byte entries, magic AA 50)."""
    data = open(path, "rb").read()
    rows = {}
    for i in range(0, len(data) - 31, 32):
        entry = data[i:i + 32]
        if entry[:2] != b"\xaa\x50":  # the checksum entry or padding ends the table
            break
        offset, size = struct.unpack_from("<II", entry, 4)
        rows[entry[12:28].split(b"\0")[0].decode()] = (offset, size)
    return rows


def check_generated(build_dir, parts, missing):
    """The partition table and image headers the build wrote match the csv and the 16 MB chip."""
    table = os.path.join(build_dir, "partitions.bin")
    if os.path.isfile(table):
        built = read_partition_table(table)
        check("build: partition table", built == parts,
              ", ".join(f"{n} {o:#x}+{s:#x}" for n, (o, s) in sorted(built.items(), key=lambda r: r[1])))
    else:
        missing("build: partition table", f"{table} not found")
    for name in ("bootloader.bin", "firmware.bin"):
        path = os.path.join(build_dir, name)
        if not os.path.isfile(path):
            missing(f"build: {name} header", f"{path} not found")
            continue
        head = open(path, "rb").read(4)
        # ESP image header: magic E9, and the flash size in the high half of byte 3 (4 = 16 MB)
        ok = len(head) == 4 and head[0] == 0xE9 and head[3] >> 4 == 4
        check(f"build: {name} header", ok, f"bytes {head.hex()} (want e9 .. .. 4x = 16 MB flash)")


def check_site(site_dir, build_dir, manifest):
    """Every image the manifest names is in the site folder and is the file the build wrote."""
    for build in manifest.get("builds", []):
        for part in build.get("parts", []):
            served = os.path.join(site_dir, part["path"])
            built = os.path.join(build_dir, os.path.basename(part["path"]))
            if not (os.path.isfile(served) and os.path.isfile(built)):
                report("FAIL", f"site: {part['path']}", "missing in the site folder or the build folder")
                continue
            same = hashlib.sha256(open(served, "rb").read()).digest() == hashlib.sha256(open(built, "rb").read()).digest()
            check(f"site: {part['path']}", same, "same file as the build output" if same else "differs from the build output")


def check_build(build_dir, parts, pack_path, require):
    def missing(name, detail):
        report("FAIL" if require else "SKIP", name, detail)

    check_generated(build_dir, parts, missing)

    fw = os.path.join(build_dir, "firmware.bin")
    if os.path.isfile(fw):
        size = os.path.getsize(fw)
        check("build: firmware size", size <= parts["factory"][1], f"{size} <= {parts['factory'][1]}")
    else:
        missing("build: firmware size", f"{fw} not found")
    image = os.path.join(build_dir, "littlefs.bin")
    spiffs_size = parts["spiffs"][1]
    if not os.path.isfile(image):
        missing("build: littlefs size", f"{image} not found")
        missing("build: pack in image", f"{image} not found")
        return
    size = os.path.getsize(image)
    if not check("build: littlefs size", size == spiffs_size, f"{size} (partition {spiffs_size})"):
        report("FAIL", "build: pack in image", "image size is wrong, not unpacked")
        return
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "fs")
        code = unpack_image(image, spiffs_size, out)
        if code is None:
            missing("build: pack in image", "no mklittlefs that runs here")
            return
        inner = os.path.join(out, "GeoQuakeLite", "tiles", "core.pak")
        if code != 0 or not os.path.isfile(inner):
            report("FAIL", "build: pack in image", f"unpack failed (exit {code}) or core.pak missing")
            return
        a = hashlib.sha256(open(inner, "rb").read()).hexdigest()
        b = hashlib.sha256(open(pack_path, "rb").read()).hexdigest() if os.path.isfile(pack_path) else None
        check("build: pack in image", a == b, f"image {a[:16]}, data {b[:16] if b else 'missing'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tag", help="release tag such as v1.0.2")
    ap.add_argument("--levels", type=int, choices=(4, 5, 6), help="required number of zoom levels in the pack")
    ap.add_argument("--pack", default=os.path.join(ROOT, "data", "GeoQuakeLite", "tiles", "core.pak"))
    ap.add_argument("--build-dir", default=os.path.join(ROOT, ".pio", "build", "geoquake_lite"))
    ap.add_argument("--require-build", action="store_true", help="missing build files or mklittlefs FAIL")
    ap.add_argument("--site", help="installer site folder (docs): the images the manifest names must be in it")
    args = ap.parse_args()

    parts = read_partitions(os.path.join(ROOT, "partitions", "geoquake_lite_16MB.csv"))
    manifest = json.load(open(os.path.join(ROOT, "docs", "manifest.json")))
    if check_partitions(parts):
        check_version(manifest, args.tag)
        check_manifest_parts(manifest, parts)
        check_pack(args.pack, parts["spiffs"][1], args.levels)
        check_build(args.build_dir, parts, args.pack, args.require_build)
        if args.site:
            check_site(args.site, args.build_dir, manifest)
    else:
        check_version(manifest, args.tag)
    print(f"RESULT: {'FAIL (' + ', '.join(failures) + ')' if failures else 'PASS'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
