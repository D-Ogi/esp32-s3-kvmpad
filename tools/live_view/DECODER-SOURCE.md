# Rebuild the decoder and relink KVMpad

The firmware in this release uses the application source at commit `e844f07ab92dc9af040a038f1f16a82e7790e958`. The Windows card-video reader also links the three FFmpeg libraries. Download the source for the same KVMpad release and its `kvmpad-decoder-source.zip` asset. The source ZIP includes build scripts, the original FFmpeg 6.1.1 release, the reconstruction code, the coefficient tables and the source for the libssh ESP-IDF component. `SHA256SUMS` lists each file. [Application source](https://github.com/D-Ogi/esp32-s3-kvmpad/tree/e844f07ab92dc9af040a038f1f16a82e7790e958)

## Build or change the libraries

Use Python 3.11 or later, Git, a POSIX shell and GNU Make. For the firmware, activate ESP-IDF v6.1 with its ESP32-S3 Xtensa compiler. Extract the ZIP. From the directory containing the ZIP, run its build script.

```sh
python decoder-source/build/build_ffmpeg.py kvmpad-decoder-source.zip --target xtensa --out ffmpeg-build
```

This example assumes that you extracted the ZIP to `decoder-source`. The script configures FFmpeg with the H.264 decoder and parser and the MOV and H.264 demuxers. It builds `libavformat.a`, `libavcodec.a` and `libavutil.a`. The exact configure options are in `build/build_ffmpeg.py`.

To change the decoder, first prepare its source. Edit the files in `ffmpeg-build/ffmpeg`, then build the edited tree.

```sh
python decoder-source/build/build_ffmpeg.py kvmpad-decoder-source.zip --target xtensa --out ffmpeg-build --prepare-only
python decoder-source/build/build_ffmpeg.py --source ffmpeg-build/ffmpeg --target xtensa --out ffmpeg-build
```

## Relink the firmware

From the application source, set `KVM_MEDIA_FFMPEG` to the absolute path of `ffmpeg-build/xtensa`. Keep `ffmpeg-build/ffmpeg` beside that directory. Use the release's `sdkconfig.defaults` and `dependencies.lock`. Create your own OTA signing key as described in the firmware guide. Run `idf.py build` in a fresh build directory. This links the application with your decoder and produces the ELF and firmware image. An installed device accepts OTA images signed with its configured key. [Firmware build guide](https://github.com/D-Ogi/esp32-s3-kvmpad/blob/e844f07ab92dc9af040a038f1f16a82e7790e958/docs/HANDBOOK.md#build-the-7b-firmware) · [Signing instructions](https://github.com/D-Ogi/esp32-s3-kvmpad/blob/e844f07ab92dc9af040a038f1f16a82e7790e958/docs/OTA.md)

The firmware also links the `david-cermak/libssh` ESP-IDF component at version 0.12.2. The ZIP's `libssh` directory contains its ESP build files, port, library headers, sources and upstream patches. The project's `tools/cmake/kvmpad_ssh_buffers.cmake` adapts the packet-buffer allocation for PSRAM. A copy is in the ZIP's `build` directory.

## Link locally on Windows without compiling

Download `kvmpad-firmware-objects.zip` and `kvmpad-firmware-libraries.zip` from the same release. The first contains relocatable application objects, linker scripts and the public signature sector. The second contains the four independent library archives. Use the release's `tools/live_view/relink_firmware.py` with Python 3.11 or later and esptool 5.6.0. The Windows CLIENT includes this Python environment.

```powershell
python tools/live_view/relink_firmware.py --objects kvmpad-firmware-objects.zip --libraries kvmpad-firmware-libraries.zip --output kvmpad.bin
```

On first use the tool downloads Espressif's Windows toolchain archive and verifies its SHA-256. It extracts the linker, its processor configuration and four runtime libraries to your local application data directory. For offline use, pass `--tools path/to/xtensa-esp-elf` with the verified files in their original subdirectories. The tool links a new ELF, checks its hash and runs esptool to generate the binary image. It attaches the release signature after it verifies that the new image matches the signed release. [Espressif toolchain](https://github.com/espressif/crosstool-NG/releases/tag/esp-15.2.0_20251204)

To try a modified library, place one or more replacement archives in a directory. Keep the original names `libavformat.a`, `libavcodec.a`, `libavutil.a` or `libdavid-cermak__libssh.a`. Add `--library-dir path/to/replacements --unsigned` to the command and select a new output filename. Use your own key if you want to sign that image.

## Relink the Windows card-video reader

Build the same source with `--target host` using an x64 MinGW Clang toolchain. From the application source, run the following command with the path to the `host` directory containing the three built libraries. Use the resulting reader as `media/card-pixels.exe` beside CLIENT.

```sh
python clients/windows/native/build-card-video-reader.py path/to/ffmpeg-build/host --output card-pixels.exe
```

## Licenses

The linked FFmpeg configuration uses LGPL-2.1-or-later. The KVMpad reconstruction changes, decoder patches and coefficient tables in this source distribution are available under LGPL-2.1-or-later. In the November 22, 2026 release, the changes apply to `h264_picture.c`, `h264_slice.c`, `h264dec.c`, `h264dec.h`, `h264_ps.c` and `h264_ps.h`. The added headers are `kvm_rr.h`, `kvm_rr_hint.h` and `model_arrays.h`. Upstream files retain their individual notices. The libssh port declares LGPL-2.1 and includes `libssh-mirror/COPYING` and `libssh-mirror/BSD`. The project build scripts carry their own license in `licenses/KVMpad-tools.md`. [FFmpeg licensing](https://ffmpeg.org/legal.html) · [libssh ESP-IDF component](https://components.espressif.com/components/david-cermak/libssh/versions/0.12.2)
