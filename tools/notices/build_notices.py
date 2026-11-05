#!/usr/bin/env python3
"""Builds GeoQuake Lite's third-party notices from what the firmware actually links.

Reads the geoquake_lite linker map (.pio/build/geoquake_lite/firmware.map, so build the firmware first: `pio run -e
geoquake_lite`), maps every linked archive to a component below and fails when an archive is not listed: a new library
cannot ship without a notice. License texts live in tools/notices/licenses/. Fonts, LVGL's built-in modules, source
files that are not archives and the map data are not archives, so they are listed by hand below.

    python3 tools/notices/build_notices.py

Writes THIRD_PARTY_NOTICES.md and licenses/THIRD_PARTY_LICENSES.txt in the repository root.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIC = Path(__file__).resolve().parent / "licenses"
MAP = ROOT / ".pio" / "build" / "geoquake_lite" / "firmware.map"

ARDUINO_LIBS = ["FrameworkArduino", "FS", "HTTPClient", "LittleFS", "Preferences", "SPI", "WiFi",
                "WiFiClientSecure", "Wire"]
ESPRESSIF_LIBS = ["app_update", "bootloader_support", "driver", "efuse", "esp_common", "esp_diagnostics", "esp_eth",
                  "esp_event", "esp_hw_support", "esp_ipc", "esp_netif", "esp_phy", "esp_pm", "esp_ringbuf",
                  "esp_rom", "esp_system", "esp_timer", "esp_wifi", "espcoredump", "hal", "log", "nvs_flash",
                  "protocomm", "pthread", "soc", "spi_flash", "tcpip_adapter", "vfs", "wifi_provisioning", "cxx"]
ESP_RADIO = ["btbb", "btdm_app", "phy", "coexist", "core", "mesh", "net80211", "pp", "smartconfig"]

# id: name, version, SPDX license, source URL, license file(s), and what makes it part of the build:
# archives (lib<name>.a), symbols (a string in the link map) or include (a header used by this source tree).
COMPONENTS = [
    dict(id="arduino-esp32", name="Arduino core for the ESP32", version="2.0.17", license="LGPL-2.1-or-later",
         url="https://github.com/espressif/arduino-esp32/tree/2.0.17", files=["arduino-esp32.txt"],
         archives=ARDUINO_LIBS, section="software"),
    dict(id="sd", name="SD library (vendored copy in lib/SD)", version="2.0.17 with backport #12979",
         license="Apache-2.0", url="https://github.com/espressif/arduino-esp32/tree/2.0.17/libraries/SD",
         files=["esp-idf-apache.txt"], archives=["SD"], section="software",
         copyright="Copyright (c) 2015-2022 Espressif Systems (Shanghai) Co., Ltd.",
         note="The copy in lib/SD carries one upstream fix (backport #12979); its file headers are Apache-2.0."),
    dict(id="esp-idf", name="ESP-IDF (Espressif IoT Development Framework)", version="4.4.7", license="Apache-2.0",
         url="https://github.com/espressif/esp-idf/tree/v4.4.7", files=["esp-idf-apache.txt"],
         copyright="Copyright 2015-2024 Espressif Systems (Shanghai) Co., Ltd.", archives=ESPRESSIF_LIBS,
         section="software"),
    dict(id="esp-radio", name="Espressif Wi-Fi, Bluetooth and PHY libraries (binary)", version="ESP-IDF 4.4.7",
         license="Apache-2.0", url="https://github.com/espressif/esp32-wifi-lib", files=["esp32-wifi-lib.txt"],
         copyright="Copyright Espressif Systems (Shanghai) Co., Ltd. Parts derived from FreeBSD net80211, "
                   "Copyright (c) 2004-2008 Sam Leffler, Errno Consulting (BSD license).",
         archives=ESP_RADIO, section="software"),
    dict(id="bluedroid", name="Bluedroid Bluetooth stack (in ESP-IDF)", version="ESP-IDF 4.4.7", license="Apache-2.0",
         url="https://github.com/espressif/esp-idf/tree/v4.4.7/components/bt/host/bluedroid",
         files=["esp-idf-apache.txt"],
         copyright="Copyright (C) 2002-2012 Broadcom Corporation; Copyright Espressif Systems (Shanghai) Co., Ltd.",
         archives=["bt"], section="software"),
    dict(id="freertos", name="FreeRTOS kernel", version="ESP-IDF 4.4.7", license="MIT",
         url="https://github.com/espressif/esp-idf/tree/v4.4.7/components/freertos", files=["freertos.txt"],
         archives=["freertos"], section="software"),
    dict(id="lwip", name="lwIP TCP/IP stack", version="ESP-IDF 4.4.7", license="BSD-3-Clause",
         url="https://github.com/espressif/esp-lwip", files=["lwip.txt"], archives=["lwip"], section="software"),
    dict(id="mbedtls", name="Mbed TLS", version="ESP-IDF 4.4.7", license="Apache-2.0",
         url="https://github.com/espressif/mbedtls", files=["mbedtls.txt"],
         archives=["mbedcrypto", "mbedtls_2", "mbedx509"], section="software"),
    dict(id="newlib", name="Newlib C library", version="ESP-IDF 4.4.7 toolchain", license="BSD and similar",
         url="https://sourceware.org/newlib/", files=["newlib.txt"], archives=["newlib", "c", "m"],
         section="software"),
    dict(id="wpa_supplicant", name="wpa_supplicant", version="ESP-IDF 4.4.7", license="BSD-3-Clause",
         url="https://github.com/espressif/esp-idf/tree/v4.4.7/components/wpa_supplicant",
         files=["wpa_supplicant.txt"], archives=["wpa_supplicant"], section="software"),
    dict(id="fatfs", name="FatFs", version="R0.13c", license="FatFs (BSD-1-Clause style)",
         url="http://elm-chan.org/fsw/ff/", files=["fatfs.txt"], archives=["fatfs"], section="software"),
    dict(id="cjson", name="cJSON", version="ESP-IDF 4.4.7", license="MIT", url="https://github.com/DaveGamble/cJSON",
         files=["cjson.txt"], archives=["json"], section="software"),
    dict(id="protobuf-c", name="protobuf-c", version="ESP-IDF 4.4.7", license="BSD-2-Clause",
         url="https://github.com/protobuf-c/protobuf-c", files=["protobuf-c.txt"], archives=["protobuf-c"],
         section="software"),
    dict(id="tlsf", name="TLSF memory allocator (ESP-IDF heap)", version="ESP-IDF 4.4.7", license="BSD-3-Clause",
         url="https://github.com/espressif/esp-idf/blob/v4.4.7/components/heap/heap_tlsf.c", files=["tlsf.txt"],
         archives=["heap"], section="software"),
    dict(id="xtensa", name="Xtensa HAL", version="ESP-IDF 4.4.7", license="MIT",
         url="https://github.com/espressif/esp-idf/tree/v4.4.7/components/xtensa", files=["xtensa.txt"],
         archives=["xtensa", "xt_hal"], section="software"),
    dict(id="esp_littlefs", name="esp_littlefs and littlefs", version="esp_littlefs 41873c2",
         license="MIT and BSD-3-Clause", url="https://github.com/joltwallet/esp_littlefs",
         files=["esp_littlefs.txt", "littlefs.txt"], archives=["esp_littlefs"], section="software"),
    dict(id="gcc", name="GCC runtime libraries (libgcc, libstdc++)", version="GCC 8.4.0 (esp-2021r2-patch5)",
         license="GPL-3.0-or-later WITH GCC-exception-3.1",
         url="https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html", files=[], archives=["gcc", "stdc++"],
         section="software",
         note="Covered by the GCC Runtime Library Exception, which allows use in any program without further "
              "conditions."),
    dict(id="lvgl", name="LVGL graphics library", version="9.5.0", license="MIT", url="https://github.com/lvgl/lvgl",
         files=["lvgl.txt"], archives=["lvgl"], section="software"),
    dict(id="lvgl-tlsf", name="TLSF memory allocator (in LVGL)", version="LVGL 9.5.0", license="BSD-3-Clause",
         url="https://github.com/lvgl/lvgl/blob/v9.5.0/src/stdlib/lv_tlsf.c", files=["tlsf.txt"],
         always=True, section="software"),
    dict(id="lvgl-printf", name="printf (in LVGL)", version="LVGL 9.5.0", license="MIT",
         url="https://github.com/lvgl/lvgl/blob/v9.5.0/src/stdlib/lv_printf.c", files=["lvgl-printf.txt"],
         copyright="Copyright (c) 2014-2019 Marco Paland (info@paland.com), PALAN Design Hannover, Germany",
         always=True, section="software"),
    dict(id="arduino_gfx", name="GFX Library for Arduino", version="1.4.9", license="BSD-2-Clause",
         url="https://github.com/moononournation/Arduino_GFX", files=["arduino_gfx.txt"],
         archives=["GFX Library for Arduino"], section="software"),
    dict(id="arduinojson", name="ArduinoJson", version="7.4.2", license="MIT",
         url="https://github.com/bblanchon/ArduinoJson", files=["arduinojson.txt"], include="ArduinoJson.h",
         section="software"),
    dict(id="sensorlib", name="SensorLib", version="0.3.3", license="MIT",
         url="https://github.com/lewisxhe/SensorLib", files=["sensorlib.txt"],
         # Its drivers are header code compiled into this project's own files, so the archive alone misses them:
         # a class name in the link map counts too.
         symbols=["SensorQMI8658", "TouchDrvCST92xx"], section="software"),
    dict(id="xpowerslib", name="XPowersLib", version="0.2.9", license="MIT",
         url="https://github.com/lewisxhe/XPowersLib", files=["xpowerslib.txt"], archives=["XPowersLib"],
         section="software"),
    dict(id="improv", name="Improv Wi-Fi sdk-cpp", version="1.2.7 (commit 8e87b560)", license="Apache-2.0",
         url="https://github.com/improv-wifi/sdk-cpp", files=["esp-idf-apache.txt"], archives=["Improv"],
         section="software"),
    dict(id="es8311", name="ES8311 audio codec driver (src/es8311)", version="Espressif, 2015-2022",
         license="Apache-2.0", url="https://github.com/espressif/esp-adf", files=["esp-idf-apache.txt"],
         copyright="Copyright (c) 2015-2022 Espressif Systems (Shanghai) Co., Ltd.", always=True,
         note="Modified by Bulut Solutions LLC (2026): Arduino Wire I2C adaptation.", section="software"),
    dict(id="qoi", name="QOI format and decoder (src/qoi_decoder.h)", version="QOI spec",
         license="MIT", url="https://qoiformat.org/", files=["qoi.txt"], always=True, section="software"),
    dict(id="montserrat", name="Montserrat (converted subset, incl. LVGL's built-in Montserrat)", version="",
         license="OFL-1.1", url="https://github.com/JulietaUla/Montserrat", files=["font-montserrat.txt"],
         always=True, section="fonts"),
    dict(id="font-awesome", name="Font Awesome Free symbols (embedded in LVGL's Montserrat)", version="",
         license="OFL-1.1", url="https://fontawesome.com", files=["font-awesome.txt"], always=True,
         section="fonts"),
    dict(id="bruno-ace-sc", name="Bruno Ace SC (converted subset)", version="", license="OFL-1.1",
         url="https://github.com/googlefonts/Bruno-ace", files=["font-brunoacesc.txt"], always=True,
         section="fonts"),
    dict(id="dseg7", name="DSEG7 Classic (converted subset, font_seg7_48)", version="", license="OFL-1.1",
         url="https://github.com/keshikan/DSEG", files=["font-dseg.txt"], always=True, section="fonts",
         note='Derived from DSEG7 Classic (subset converted with lv_font_conv; renamed per the SIL OFL Reserved '
              'Font Name).'),
    dict(id="orbitron", name="Orbitron (converted subset, font_orb_28)", version="", license="OFL-1.1",
         url="https://github.com/theleagueof/orbitron", files=["font-orbitron.txt"], always=True, section="fonts",
         note='Derived from Orbitron (subset converted with lv_font_conv; renamed per the SIL OFL Reserved '
              'Font Name).'),
    dict(id="material-symbols", name="Material Symbols (converted subset, src/icons)", version="", license="Apache-2.0",
         url="https://fonts.google.com/icons", files=["material-symbols.txt"], always=True, section="fonts",
         note="Generated subset of Google Material Symbols (Apache-2.0), converted to an LVGL bitmap font "
              "(src/icons/icon_material_*.c)."),
    dict(id="natural-earth", name="Natural Earth map data", version="", license="Public domain",
         url="https://www.naturalearthdata.com/", files=["natural-earth.txt"], always=True, section="data",
         note="The map tiles and the core map pack are drawn from Natural Earth data (public domain, no credit "
              "required)."),
    dict(id="usgs", name="USGS earthquake data", version="", license="Public domain (credit requested)",
         url="https://earthquake.usgs.gov/earthquakes/feed/", files=["usgs-data.txt"], always=True,
         section="data", note="Live earthquake feed. USGS asks that it be credited as the source."),
]

SECTIONS = [("software", "Software"), ("fonts", "Fonts and icons"), ("data", "Data")]

NOTES = [
    "The font and icon files in this repository are modified (subset) versions of the originals, converted with "
    "lv_font_conv (https://github.com/lvgl/lv_font_conv). DSEG7 Classic and Orbitron carry a Reserved Font Name, so "
    "their converted subsets are shipped under the different names font_seg7_48 and font_orb_28.",
    "src/es8311/es8311.c is modified: Arduino Wire I2C adaptation (2026).",
    "src/icons/icon_material_*.c are generated subsets of Google Material Symbols.",
    "The Arduino core for the ESP32 is used under the GNU LGPL 2.1 or later. This repository publishes the complete "
    "source of GeoQuake Lite and pins the core version, so anyone can rebuild and relink the firmware with a "
    "modified core.",
    "The SD library vendored in lib/SD comes from the Arduino core and its file headers are Apache-2.0.",
]


def linked_archives():
    text = MAP.read_text(errors="replace")
    start = text.index("Archive member included")
    end = text.find("Discarded input sections", start)
    names = set()
    for hit in re.finditer(r"^(\S[^\n(]*?\.a)\(", text[start:end], re.M):
        base = hit.group(1).split("/")[-1]
        if base.startswith("lib"):
            names.add(base[3:-2])
    return names


def linked_text():
    """The link map without its discarded-sections list: a symbol named here is in the firmware."""
    text = MAP.read_text(errors="replace")
    start = text.find("Discarded input sections")
    end = text.find("Memory Configuration", start)
    return text if start < 0 or end < 0 else text[:start] + text[end:]


def source_uses(header):
    for p in list((ROOT / "src").rglob("*")) + list((ROOT / "include").rglob("*")):
        if p.suffix in (".c", ".cpp", ".h") and header in p.read_text(errors="replace"):
            return True
    return False


def main():
    if not MAP.exists():
        sys.exit(f"no link map at {MAP}: build the firmware first (pio run -e geoquake_lite)")
    archives = linked_archives()
    symbols = linked_text()

    by_archive = {a: c["id"] for c in COMPONENTS for a in c.get("archives", [])}
    unknown = sorted(a for a in archives if a not in by_archive)
    if unknown:
        sys.exit("no notice for linked archives: " + ", ".join(f"lib{a}.a" for a in unknown))

    used = []
    for c in COMPONENTS:
        hit = c.get("always", False)
        hit = hit or any(a in archives for a in c.get("archives", []))
        hit = hit or any(s in symbols for s in c.get("symbols", []))
        hit = hit or ("include" in c and source_uses(c["include"]))
        if hit:
            used.append(c)

    for c in used:
        for f in c["files"]:
            if not (LIC / f).exists():
                sys.exit(f"{c['name']}: missing license text tools/notices/licenses/{f}")

    md = ["# Third-Party Notices", "",
          "GeoQuake Lite is MIT licensed (see `LICENSE`). It is built with the open-source software, fonts and data "
          "listed below. Every component keeps its own license. The full license texts, with their copyright lines, "
          "are in `licenses/THIRD_PARTY_LICENSES.txt`.", ""]
    for key, title in SECTIONS:
        items = [c for c in used if c["section"] == key]
        if not items:
            continue
        md.append(f"## {title}")
        md.append("")
        for c in items:
            version = f" {c['version']}" if c["version"] else ""
            md.append(f"- [{c['name']}]({c['url']}){version} · {c['license']}")
        md.append("")
    md.append("## Notes")
    md.append("")
    md.extend(f"- {n}" for n in NOTES)
    md.append("")
    (ROOT / "THIRD_PARTY_NOTICES.md").write_text("\n".join(md))

    parts = []
    seen = set()
    for c in used:
        head = f"\n{'=' * 78}\n{c['name']}{' ' + c['version'] if c['version'] else ''}  ({c['license']})\n{c['url']}\n"
        if c.get("copyright"):
            head += c["copyright"] + "\n"
        if c.get("note"):
            head += c["note"] + "\n"
        body = ""
        for f in c["files"]:
            text = (LIC / f).read_text().strip()
            if f in seen:
                body += f"\n(License text: see {f} above.)\n"
            else:
                seen.add(f)
                body += f"{'-' * 78}\n{text}\n"
        parts.append(head + body)
    (ROOT / "licenses").mkdir(exist_ok=True)
    (ROOT / "licenses" / "THIRD_PARTY_LICENSES.txt").write_text("".join(parts))
    print(f"wrote THIRD_PARTY_NOTICES.md and licenses/THIRD_PARTY_LICENSES.txt: "
          f"{len(used)} components from {len(archives)} linked archives")


if __name__ == "__main__":
    main()
