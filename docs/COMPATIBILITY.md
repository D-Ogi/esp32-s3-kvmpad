# USB compatibility

Connect the **native USB** port on KVMpad to the target computer. Use the **USB-UART** port for the development console. The same firmware supports all the functions below.

| Function | Support and requirements |
| --- | --- |
| USB keyboard and mouse | Windows and Linux use their built-in USB drivers. KVMpad starts in this mode. |
| Windows extended desktop (640 × 480 or 1024 × 600) | We do not recommend the xfz1986 driver (1.2.25.2). We cannot match the binary to public source code. The driver uses an untrusted test certificate. |
| Linux USB monitor | The GUD driver supports 640 × 480 and 800 × 600 in monochrome. Register the KVMpad USB ID with the driver. The keyboard and mouse remain available in this mode. |
| BIOS and UEFI | KVMpad provides a USB boot keyboard and mouse for firmware that supports these devices. The USB monitor requires an operating system with a compatible display driver. |

Select a display mode under **Extensions → USB monitor**. Tap **Home** to return to keyboard and mouse mode. [USB modes](HANDBOOK.md#usb-startup-and-display-modes) · [Linux GUD setup](../test/usb_monitor/gud_guest/)

For remote keyboard and mouse input, connect the paired Windows CLIENT to KVMpad. Keep CLIENT connected during an SSH session. [SSH setup](../clients/windows/README.md#direct-ssh-input)
