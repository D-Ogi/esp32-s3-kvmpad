# Waveshare ESP32-S3-Touch-LCD-7B

| Component | 7B configuration |
| --- | --- |
| Display | 1024 × 600 parallel RGB panel, RGB565 framebuffer |
| Touch | GT911 capacitive controller, up to five contacts |
| MCU | ESP32-S3, measured on revision v0.2 |
| Memory | 16 MiB quad flash and 8 MiB octal PSRAM |
| I/O expander | CH32V003 at I²C address `0x24` |
| Native USB | Keyboard, mouse and selectable USB display profiles |
| USB-UART | CH343 development console, 115200 baud |

Before flashing a board, verify the model printed on the PCB and its flash size. From an activated ESP-IDF environment, run `esptool --chip esp32s3 --port PORT flash-id` with the port assigned to the USB-UART connector. The 7B firmware uses a 16 MiB partition table. [Firmware handbook](../docs/HANDBOOK.md) · [USB compatibility](../docs/COMPATIBILITY.md)
