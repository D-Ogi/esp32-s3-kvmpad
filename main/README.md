# `main/`

Firmware entry point (`app_main.c`) — only the assembly of components and the start of tasks.

TASK-000 creates it (together with `CMakeLists.txt`) after a working `idf.py build` on ESP-IDF v6.1.
Spec: 02 §3, §4, §11; 05 TASK-000.
