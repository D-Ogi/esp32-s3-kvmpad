# `main/`

Firmware entry point (`app_main.c`) — only the assembly of components and the start of tasks.

TASK-000 creates it (together with `CMakeLists.txt`) after a working `idf.py build` on ESP-IDF v6.1.
Spec: 02 §3, §4, §11; 05 TASK-000.

Extensions: ADR-025 TECH-084 — a retained `.rodata` string with the SPDX id and the Required Notice
(`__attribute__((used))`), so that `strings app.bin` finds it in every OTA image (AT-EXT-29).
