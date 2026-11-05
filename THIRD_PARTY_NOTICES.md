# Third-Party Notices

GeoQuake Lite is MIT licensed (see `LICENSE`). It is built with the open-source software, fonts and data listed below. Every component keeps its own license. The full license texts, with their copyright lines, are in `licenses/THIRD_PARTY_LICENSES.txt`.

## Software

- [Arduino core for the ESP32](https://github.com/espressif/arduino-esp32/tree/2.0.17) 2.0.17 · LGPL-2.1-or-later
- [SD library (vendored copy in lib/SD)](https://github.com/espressif/arduino-esp32/tree/2.0.17/libraries/SD) 2.0.17 with backport #12979 · Apache-2.0
- [ESP-IDF (Espressif IoT Development Framework)](https://github.com/espressif/esp-idf/tree/v4.4.7) 4.4.7 · Apache-2.0
- [Espressif Wi-Fi, Bluetooth and PHY libraries (binary)](https://github.com/espressif/esp32-wifi-lib) ESP-IDF 4.4.7 · Apache-2.0
- [Bluedroid Bluetooth stack (in ESP-IDF)](https://github.com/espressif/esp-idf/tree/v4.4.7/components/bt/host/bluedroid) ESP-IDF 4.4.7 · Apache-2.0
- [FreeRTOS kernel](https://github.com/espressif/esp-idf/tree/v4.4.7/components/freertos) ESP-IDF 4.4.7 · MIT
- [lwIP TCP/IP stack](https://github.com/espressif/esp-lwip) ESP-IDF 4.4.7 · BSD-3-Clause
- [Mbed TLS](https://github.com/espressif/mbedtls) ESP-IDF 4.4.7 · Apache-2.0
- [Newlib C library](https://sourceware.org/newlib/) ESP-IDF 4.4.7 toolchain · BSD and similar
- [wpa_supplicant](https://github.com/espressif/esp-idf/tree/v4.4.7/components/wpa_supplicant) ESP-IDF 4.4.7 · BSD-3-Clause
- [FatFs](http://elm-chan.org/fsw/ff/) R0.13c · FatFs (BSD-1-Clause style)
- [cJSON](https://github.com/DaveGamble/cJSON) ESP-IDF 4.4.7 · MIT
- [protobuf-c](https://github.com/protobuf-c/protobuf-c) ESP-IDF 4.4.7 · BSD-2-Clause
- [TLSF memory allocator (ESP-IDF heap)](https://github.com/espressif/esp-idf/blob/v4.4.7/components/heap/heap_tlsf.c) ESP-IDF 4.4.7 · BSD-3-Clause
- [Xtensa HAL](https://github.com/espressif/esp-idf/tree/v4.4.7/components/xtensa) ESP-IDF 4.4.7 · MIT
- [esp_littlefs and littlefs](https://github.com/joltwallet/esp_littlefs) esp_littlefs 41873c2 · MIT and BSD-3-Clause
- [GCC runtime libraries (libgcc, libstdc++)](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html) GCC 8.4.0 (esp-2021r2-patch5) · GPL-3.0-or-later WITH GCC-exception-3.1
- [LVGL graphics library](https://github.com/lvgl/lvgl) 9.5.0 · MIT
- [TLSF memory allocator (in LVGL)](https://github.com/lvgl/lvgl/blob/v9.5.0/src/stdlib/lv_tlsf.c) LVGL 9.5.0 · BSD-3-Clause
- [printf (in LVGL)](https://github.com/lvgl/lvgl/blob/v9.5.0/src/stdlib/lv_printf.c) LVGL 9.5.0 · MIT
- [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) 1.4.9 · BSD-2-Clause
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) 7.4.2 · MIT
- [SensorLib](https://github.com/lewisxhe/SensorLib) 0.3.3 · MIT
- [XPowersLib](https://github.com/lewisxhe/XPowersLib) 0.2.9 · MIT
- [Improv Wi-Fi sdk-cpp](https://github.com/improv-wifi/sdk-cpp) 1.2.7 (commit 8e87b560) · Apache-2.0
- [ES8311 audio codec driver (src/es8311)](https://github.com/espressif/esp-adf) Espressif, 2015-2022 · Apache-2.0
- [QOI format and decoder (src/qoi_decoder.h)](https://qoiformat.org/) QOI spec · MIT

## Fonts and icons

- [Montserrat (converted subset, incl. LVGL's built-in Montserrat)](https://github.com/JulietaUla/Montserrat) · OFL-1.1
- [Font Awesome Free symbols (embedded in LVGL's Montserrat)](https://fontawesome.com) · OFL-1.1
- [Bruno Ace SC (converted subset)](https://github.com/googlefonts/Bruno-ace) · OFL-1.1
- [DSEG7 Classic (converted subset, font_seg7_48)](https://github.com/keshikan/DSEG) · OFL-1.1
- [Orbitron (converted subset, font_orb_28)](https://github.com/theleagueof/orbitron) · OFL-1.1
- [Material Symbols (converted subset, src/icons)](https://fonts.google.com/icons) · Apache-2.0

## Data

- [Natural Earth map data](https://www.naturalearthdata.com/) · Public domain
- [USGS earthquake data](https://earthquake.usgs.gov/earthquakes/feed/) · Public domain (credit requested)

## Notes

- The font and icon files in this repository are modified (subset) versions of the originals, converted with lv_font_conv (https://github.com/lvgl/lv_font_conv). DSEG7 Classic and Orbitron carry a Reserved Font Name, so their converted subsets are shipped under the different names font_seg7_48 and font_orb_28.
- src/es8311/es8311.c is modified: Arduino Wire I2C adaptation (2026).
- src/icons/icon_material_*.c are generated subsets of Google Material Symbols.
- The Arduino core for the ESP32 is used under the GNU LGPL 2.1 or later. This repository publishes the complete source of GeoQuake Lite and pins the core version, so anyone can rebuild and relink the firmware with a modified core.
- The SD library vendored in lib/SD comes from the Arduino core and its file headers are Apache-2.0.
