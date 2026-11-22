# USB and firmware compatibility

Use the native USB connector for the host computer. The separate UART connector provides a development console. All host computers use the same 7B firmware. At startup, it presents a USB boot keyboard and mouse. Open **Extensions → USB monitor** on the LCD to select a display mode. **Home** restores keyboard and mouse mode.

| Function | Verified behavior |
| --- | --- |
| Keyboard and mouse | Both interfaces enumerated on Windows and Debian 13. Linux exposed relative X, Y and wheel input and sent Caps Lock LED changes to the board. The device, configuration and both report descriptors read by Linux matched the qualified firmware. |
| Windows extended desktop | The LCD displayed full-color test patterns at 640 × 480 and 1024 × 600 with the external USB display driver. Returning to VGA through Windows restarted the driver during testing and the desktop reverted to native resolution. The installed package failed signature verification. An in-box Windows display path has not been demonstrated. |
| Linux GUD monitor | The 7B displayed a 640 × 480 monochrome desktop and accepted partial updates from the Linux GUD driver. The tested kernel required a runtime match for the prototype USB ID. Leaving display mode restored the boot keyboard and mouse. |
| UEFI | OVMF enumerated the board as an input device. Live UEFI Setup video from a physical PC over this cable has not been demonstrated. |
| Paired SSH input | The 7B firmware includes SSH input alongside H.264 playback and USB monitor support. Windows OpenSSH authenticated in all 12 login tests using two operator keys. A separate input request reached the LCD approval prompt. Letting that prompt expire delivered zero commands. Disconnecting CLIENT closed the SSH listener. |

For setup and operation, see the [firmware build instructions](HANDBOOK.md#build-the-7b-firmware), [USB modes](HANDBOOK.md#usb-startup-and-display-modes) and [SSH authorization](HANDBOOK.md#authorize-ssh-input). The [Windows enumeration test](../test/hil/usb_enum_windows.ps1), [Linux enumeration test](../test/hil/usb_enum_linux.py) and [GUD display fixture](../test/usb_monitor/gud_guest/) are available for independent verification.
