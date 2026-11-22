# 7B firmware handbook

The 7B has a 1024 × 600 RGB565 panel, 16 MiB of flash and 8 MiB of PSRAM. Connect the target PC to the native USB port. Connect a development PC to the CH343 USB-UART port for flashing, logs and framebuffer captures. [Board identification](../hardware/boards.md)

## USB startup and display profiles

A fresh 7B build uses `sdkconfig.defaults` and `sdkconfig.boot-hid` to expose a boot keyboard and mouse on the native USB port. The USB-UART connector provides a separate development console.
Open **Extensions → USB monitor** on the LCD and select a mode:

- **Choose in Windows** lets Windows Display settings select the resolution. **640 x 480** and **1024 x 600 / Native** select a fixed size. The Windows display path uses the partner USB display driver.
- **Monochrome / Linux GUD** starts a wired Linux display session with a boot keyboard and mouse. This choice is enabled by default in monitor builds. The development board uses the shared private-test ID `1209:0001`, which requires binding the Linux `gud` driver to that ID on the test host. A distributed device needs its own assigned ID. [Linux driver compatibility](COMPATIBILITY.md) · [Test PID terms](https://pid.codes/1209/0001/)

Tap **Home** to end the session and restore the startup interfaces. Reopen **Extensions → USB monitor** to choose another mode. For a Windows desktop with on-screen keyboard and touchpad, use **Start USB monitor** in the USB Console extension.
Native Precision Touchpad and dedicated media keys require an enhanced HID
configuration. To select Consumer Control, set
`-DSDKCONFIG_DEFAULTS=sdkconfig.defaults`. To include Precision Touchpad,
set `"-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hpe"`.
Use separate build directories and separate `-DSDKCONFIG=...` paths when
comparing profiles. ESP-IDF preserves saved configuration values on rebuild.
See [USB compatibility](COMPATIBILITY.md) for hardware verification.

For a responsive Windows PC with working keyboard input, open **Shortcuts**
on the KVMpad and choose **Secure attention** (Ctrl+Alt+Delete). Windows
provides the **Power** menu on that screen. Choose **Restart** there when
ready.

## Combined media and SSH build

The combined profile includes USB monitor support, local video playback and SSH input. It requires ESP-IDF v6.1, the Xtensa FFmpeg decoder and an OTA signing key. Use a fresh build directory and point `KVM_MEDIA_FFMPEG` at the decoder installation. [Decoder preparation](../tools/live_view/build_ffmpeg.py) · [Firmware signing](OTA.md)

```powershell
$decoder = (Resolve-Path ../ffmpeg-build/xtensa).Path
idf.py --no-ccache -B ../build-7b-media-ssh -DIDF_TARGET=esp32s3 -DKVM_MEDIA_SSH_PROFILE=ON "-DKVM_MEDIA_FFMPEG=$decoder" build
python test/host_unit/check_hpe_descriptors.py ../build-7b-media-ssh/kvmpad.elf --interfaces 2 --media-ssh-image ../build-7b-media-ssh/kvmpad.bin
```

The profile saves its `sdkconfig` in the build directory and uses `dependencies.media-ssh.lock`. ESP-IDF shares managed components within a checkout. Use separate checkouts for concurrent builds of different profiles.

To authorize SSH, connect the paired Windows CLIENT, open **Host input → SSH command…** and bind your OpenSSH public key. The CLIENT verifies the board's SSH host key through the paired connection. Approve the control request on the LCD. Closing the sponsoring connection ends the authorization. [Windows CLIENT instructions](../clients/windows/README.md)

## Capture the LCD over UART

On a development build with console access, send `ui snap 1 compact` to the UART at 115200 baud and save the output. Run `python test/hil/ui_snapshot.py capture.log screen.png` to decode the framebuffer. [Hardware test tools](../test/hil/)
