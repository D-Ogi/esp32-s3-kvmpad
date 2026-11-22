# USB and firmware compatibility

Use the native USB connector for the host computer. The separate UART connector provides a development console. All host computers use the same 7B firmware. At startup, it presents a USB boot keyboard and mouse. Open **Extensions → USB monitor** on the LCD to select a display mode. **Home** restores keyboard and mouse mode.

| Function | Verified behavior |
| --- | --- |
| Keyboard and mouse | Both interfaces enumerated on Windows and Debian 13. Linux exposed relative X, Y and wheel input and sent Caps Lock LED changes to the board. The device, configuration and both report descriptors read by Linux matched the qualified firmware. |
| Windows extended desktop | The LCD displayed full-color test patterns at 640 × 480 and 1024 × 600 with the external USB display driver. Returning to VGA through Windows restarted the driver during testing and the desktop reverted to native resolution. The installed package failed signature verification. An in-box Windows display path has not been demonstrated. |
| Linux GUD monitor | The Linux GUD driver switched the 7B from 640 × 480 to 800 × 600 and back. Linux also detected the keyboard and mouse while the display was active. Full images and partial updates matched the board's LCD framebuffer at both resolutions. Leaving display mode restored the USB keyboard and mouse without a board restart. The tested kernel required a runtime match for the prototype USB ID. |
| UEFI | OVMF enumerated the board as an input device. Live UEFI Setup video from a physical PC over this cable has not been demonstrated. |
| Paired SSH input | Windows OpenSSH authenticated on all six attempts after startup and all 12 attempts after a Linux GUD display session. The latter used two operator keys after the board returned to keyboard and mouse mode. Disconnecting CLIENT closed the SSH listener. In a separate input test, the LCD requested approval and delivered zero commands when the prompt expired. |

For setup and operation, see the [firmware build instructions](HANDBOOK.md#build-the-7b-firmware), [USB modes](HANDBOOK.md#usb-startup-and-display-modes) and [SSH authorization](HANDBOOK.md#authorize-ssh-input). The [Windows enumeration test](../test/hil/usb_enum_windows.ps1), [Linux enumeration test](../test/hil/usb_enum_linux.py) and [GUD display fixture](../test/usb_monitor/gud_guest/) are available for independent verification.
