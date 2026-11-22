# Install KVMpad

Install Windows CLIENT on the operator PC. The computer that receives USB keyboard and mouse input uses its own USB drivers.

### Windows CLIENT

1. Download the Windows x64 MSI and `SHA256SUMS.txt` from the same [release](https://github.com/D-Ogi/esp32-s3-kvmpad/releases).
2. Run `Get-FileHash .\KVMpad-Client-x64.msi -Algorithm SHA256` in PowerShell. Compare the result with the MSI entry in `SHA256SUMS.txt`.
3. Open the MSI. Approve installation for all users.
4. Open **KVMpad Client** from Start. The installer includes the .NET desktop runtime and the Python tools.

The test release uses an unsigned MSI. Windows can show **Unknown publisher**. Use your organization's software approval procedure for this package.

To prepare video files for microSD, install FFmpeg and ffprobe on the operator PC. The reduced-memory video modes also use the separate card-video reader. [Build the card-video reader](../tools/live_view/DECODER-SOURCE.md#relink-the-windows-card-video-reader)

### First installation on a 7B

1. Check the model label on the board. This image fits the **Waveshare ESP32-S3-Touch-LCD-7B** with 16 MiB of flash.
2. Disconnect the native USB cable. Connect the **USB-UART** port to the operator PC with a data cable.
3. Open **Tools → Board firmware** in CLIENT. Select the COM port for the CH343 bridge.
4. Select **Install or recover from this PC**. Select **Choose backup and install**.
5. Choose a new file for the 16 MiB backup. Check the board and port in the dialog. Select **OK**. Keep the cable connected until verification finishes.
6. Check that the KVMpad touchscreen starts. Connect the native USB port to the computer you want to control.

CLIENT links the firmware locally. Allow about 425 MiB for the first download, including the two firmware packages and the Espressif tools. CLIENT checks the package hashes and the image signature before writing. You can also select **Save firmware** to create a reusable OTA `.bin` without a board.

The backup contains the previous flash contents, including any credentials. Store it with access restricted to the device owner. On a board with the same partition layout, wired installation preserves the settings partition. On a different layout, it erases flash after it verifies the backup. Wired installation sets the startup slot to the included firmware.

### Update an existing KVMpad

Use the signed update to keep settings and the previous bootable slot.

1. Put the signed `.bin` on an HTTPS server reachable from the board. Serve the file directly with a `Content-Length` header. The board requires an HTTP 200 response.
2. Connect KVMpad to Wi-Fi. Connect its USB-UART port to the operator PC.
3. Open **Tools → Board firmware**. Select the port. Select **Update a running KVMpad from an HTTPS server**.
4. Enter the direct image URL. Select **Start signed update**. Select **OK**.
5. Wait for CLIENT to report successful startup. Read the firmware version and slot.

The board checks the signing key against the running firmware. It restarts into the other slot and checks USB, input and the touchscreen. If the check fails, the bootloader can return to the previous slot. The PC does not need a Wi-Fi pairing to use the physical USB-UART port. [Firmware update procedure](OTA.md)

### Managed deployment

Use an administrator session or your Windows software distribution service. These commands install, repair or uninstall CLIENT for all users. Firmware installation is a separate operation in CLIENT.

```powershell
msiexec.exe /i KVMpad-Client-x64.msi /qn /norestart /L*v client-install.log
msiexec.exe /fa KVMpad-Client-x64.msi /qn /norestart /L*v client-repair.log
msiexec.exe /x KVMpad-Client-x64.msi /qn /norestart /L*v client-remove.log
```

Install a newer MSI with the same `/i` command. Windows Installer replaces the older CLIENT. Pairings reside in the operator's Windows credential store.

To stage firmware downloads, place `kvmpad-firmware-objects.zip` and `kvmpad-firmware-libraries.zip` from the release in one folder. Choose **Use local packages** in CLIENT. For a fully offline build PC, set `KVMPAD_RELINK_TOOLS` to the extracted `xtensa-esp-elf` directory from the matching Espressif tool download. Use **Save firmware**. You can distribute the resulting file to boards over your internal HTTPS server. [Linker version and source packages](../tools/live_view/DECODER-SOURCE.md)

### Licenses and source code

The KVMpad application and its firmware installation tool use PolyForm Noncommercial 1.0.0. The .NET desktop runtime, CPython, Tcl/Tk and the Python packages retain their own licenses. CLIENT runs Espressif's esptool and espsecure as separate programs. For their source code, download `kvmpad-programmer-source.zip` from the same release. For the firmware libraries, download `kvmpad-decoder-source.zip`. [KVMpad license](../LICENSE.md) · [Library build instructions](../tools/live_view/DECODER-SOURCE.md)
