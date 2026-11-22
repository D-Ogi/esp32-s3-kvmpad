# Firmware integration

| Entry point | Responsibility |
| --- | --- |
| [app_main.c](app_main.c) | Initializes the 7B hardware and starts the firmware services. |
| [app_config.c](app_config.c) | Connects persistent settings to the running application. |
| [ui_shell.c](ui_shell.c) | Connects the touchscreen UI to device services. |
| [pack_panel.c](pack_panel.c) | Opens the extension browser and runs installed packages. |
| [media_panel.c](media_panel.c) | Presents live media and video from microSD. |
| [ime_glue.c](ime_glue.c) | Loads keyboard language resources. |
| [kvm_console.c](kvm_console.c) | Registers development commands for the USB-UART console. |

[Build the 7B firmware and switch USB modes](../docs/HANDBOOK.md) · [Component layout](../docs/REPO-LAYOUT.md)
