# 7B firmware handbook

The 7B has a 1024 × 600 RGB565 panel, 16 MiB of flash and 8 MiB of PSRAM. Connect the target PC to the native USB port. Connect a development PC to the CH343 USB-UART port for flashing, logs and framebuffer captures. [Board identification](../hardware/boards.md)

## USB startup and display modes

Use the same 7B firmware image with every target computer. At startup, the native USB port presents a boot keyboard and mouse. To use a display mode, select it on the board after startup. The USB-UART connector provides a separate development console.
Open **Extensions → USB monitor** on the LCD and select a mode.

- **Choose in Windows** lets Windows Display settings select the resolution. **640 x 480** and **1024 x 600 / Native** select a fixed size. The Windows display path uses the partner USB display driver.
- **Monochrome / Linux GUD** starts a wired Linux display session with a boot keyboard and mouse. The development board uses the shared private-test ID `1209:0001`, which requires binding the Linux `gud` driver to that ID on the test host. A distributed device needs its own assigned ID. [Linux driver compatibility](COMPATIBILITY.md) · [Test PID terms](https://pid.codes/1209/0001/)
- **BIOS screenshots / USB drive** presents a capture volume on microSD to the target. Use the target firmware's screenshot command and select **View latest** on the board to display the saved BMP. This shows one still image per capture. [Capture instructions](../components/usb_monitor/README.md#bios-setup-screenshots)

Tap **Home** to end the session and restore the startup interfaces. Reopen **Extensions → USB monitor** to choose another mode. For a Windows desktop with on-screen keyboard and touchpad, use **Start USB monitor** in the USB Console extension. [Tested operating systems and modes](COMPATIBILITY.md)

For a responsive Windows PC with working keyboard input, open **Shortcuts**
on the KVMpad and choose **Secure attention** (Ctrl+Alt+Delete). Windows
provides the **Power** menu on that screen. Choose **Restart** there when
ready.

## Build the 7B firmware

The 7B firmware includes the USB boot keyboard and mouse, USB monitor modes, local H.264 playback and paired SSH input. The project uses ESP-IDF v6.1, `sdkconfig.defaults` and `dependencies.lock`. Prepare your OTA signing key before building. [Signing instructions](OTA.md)

The decoder build needs the project's FFmpeg 6.1.1 source archive with its reconstruction overlays, the Xtensa ESP32-S3 compiler, a shell and Make. The script prepares `picolibc.specs` from the installed Xtensa compiler. From the repository root in the activated ESP-IDF environment, replace the example archive path with the file on your machine. [Decoder build script](../tools/live_view/build_ffmpeg.py)

```sh
python tools/live_view/build_ffmpeg.py path/to/decoder-source.zip --target xtensa --out ../ffmpeg-build
idf.py build
```

By default the firmware build reads the decoder from `../ffmpeg-build/xtensa`. If you already have it elsewhere, set the `KVM_MEDIA_FFMPEG` environment variable to that directory before running `idf.py build`. To check the linked features and the two startup HID interfaces, run the image check from the same environment.

```sh
python test/host_unit/check_hpe_descriptors.py build/kvmpad.elf --interfaces 2 --firmware-image build/kvmpad.bin
```

## Authorize SSH input

Enable remote access on the board and connect the paired Windows CLIENT. Open **Host input → SSH command…** and select your OpenSSH public key. CLIENT checks the board's SSH host key over the paired connection and generates a command for PowerShell. Run it, then tap **Allow** on the LCD to authorize keyboard and mouse input. Disconnecting CLIENT ends the binding. [Windows CLIENT instructions](../clients/windows/README.md#direct-ssh-input)

## Capture the LCD over UART

On a development build with console access, send `ui snap 1 compact` to the UART at 115200 baud and save the output. Run `python test/hil/ui_snapshot.py capture.log screen.png` to decode the framebuffer. [Hardware test tools](../test/hil/)
