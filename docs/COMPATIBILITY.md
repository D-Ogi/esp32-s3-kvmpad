# USB and firmware compatibility

Use the native USB connector for the host computer. The separate UART connector provides a development console. The default 7B firmware starts as a standard USB boot keyboard and mouse. Open **Extensions → USB monitor** on the LCD to select a display mode. **Home** restores keyboard and mouse mode.

| Function | Verified behavior |
| --- | --- |
| Keyboard and mouse | Both interfaces enumerated on Windows and Debian 13. Linux exposed relative X, Y and wheel input and sent Caps Lock LED changes to the board. The device, configuration and both report descriptors read by Linux matched the qualified firmware. |
| Windows extended desktop | The LCD displayed full-color test patterns at 640 × 480 and 1024 × 600 with the external USB display driver. Returning to VGA through Windows restarted the driver during testing and the desktop reverted to native resolution. The installed package failed signature verification. An in-box Windows display path has not been demonstrated. |
| Linux GUD monitor | The 7B displayed a 640 × 480 monochrome desktop and accepted partial updates from the Linux GUD driver. The tested kernel required a runtime match for the prototype USB ID. Leaving display mode restored the boot keyboard and mouse. |
| UEFI | OVMF enumerated the board as an input device. Live UEFI Setup video from a physical PC over this cable has not been demonstrated. |
| Combined media and SSH build | The signed 7B image includes local H.264 playback, USB monitor support and the optional paired SSH service with LCD approval. The default source build and the combined build share the same two startup HID interfaces. |

For build options and operating instructions, see [the handbook](HANDBOOK.md#usb-startup-and-display-profiles) and [SSH integration](HANDBOOK.md#combined-media-and-ssh-build). The [Windows enumeration test](../test/hil/usb_enum_windows.ps1), [Linux enumeration test](../test/hil/usb_enum_linux.py) and [GUD display fixture](../test/usb_monitor/gud_guest/) are available for independent verification.
