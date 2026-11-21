# KVMpad Extension SDK

Create a local checklist, preview it at the panel's 1024 × 600 resolution and prepare a microSD installer. The ZIP includes Extension Studio, the command-line packager, the package schemas, four editable examples and their Python dependencies. Install Python with Tk for the operating system, architecture and Python version in `runtime.json`. The commands below use the matching interpreter as `python`. Run them in the extracted SDK directory.

## Make a checklist

1. Run `python -I -B start.py` to open Studio with **Flight checklist**. You can return to this example with **File → New from template → flight-checklist**.
2. Select **title** on the layout and change its text to **Departure checks** in the Widget inspector. Select **Apply widget changes**, then **File → Save source as…** and choose a new folder called `departure`.
3. Select **Simulate**. Press **Mark complete**, then use **Appearance** in the browser to check the completed view in **Day**, **Night** and **High contrast**. **System** follows your operating system. Press **Reset** to return to **Pending**. The preview runs locally.
4. Use **Package…** to save `departure.kvmpack`. Open the saved package in Studio with **File → Open package…** to inspect the result.

To create a separately installable profile, give the saved project's manifest a unique reverse-domain `id` and title before exporting it. The package identifier determines which extension an update replaces.

To add artwork to a checklist button, select the button in the layout and choose **Import PNG…**. Studio includes the PNG in the package. Select another button and choose the same file under **Button icon** to reuse it. Icons keep their color, transparency and proportions at up to 32 × 32 pixels. A button needs at least 64 × 40 pixels to show its icon alongside the label. Open **Simulate** to try the buttons before exporting.

When opening a signed package, select the publisher's public Ed25519 PEM file from a source you trust. After verification, **Package…**, **Install via card…** and **Install to paired device…** use the original signed archive. The board checks the signature against its own configured publishers during installation. To publish your edits as a separate project, choose **File → Save source as…**. Studio opens the resulting unsigned source for editing. A publisher can sign that source with the packager's `build --sign-key` option.

To extend the layout, choose a widget type below the canvas, select a **Parent** and click **Add widget**. Choose a Column, Row or Grid to group children. Drag a child or enter its coordinates relative to its parent in the Widget inspector. The Flight Yoke template shows the declarations for joystick controls. The Cursor Magnifier template includes the declarations for a native stream viewport.

Choose **File → Open USB Monitor Console example** for a portrait USB desktop with a touchpad and keyboard. The **Screens** handle opens a second page with a larger touchpad. The browser preview shows the disconnected layout. On firmware supporting the composite USB profile, start the desktop on KVMpad after connecting the computer. The source is in `extensions/usb-monitor-console`.

On the panel the keyboard uses the configured host layout. Tap **123** for numbers and punctuation, then **ABC** to return to letters. Tap **Shift**, **Ctrl**, **Alt** or **AltGr** to apply it to the next key. Hold an ordinary key to keep it pressed until you lift your finger.

**Validate** checks the manifest's `limits.max_memory_kib` against the 7B scene and the most demanding PNG in the package. Studio names the asset and gives the required minimum in KiB. You can open the project, change the limit on the **Manifest** tab, and validate again. The board reads one asset at a time. Running a view also needs memory for its widgets and any active media.

Open `docs/EXTENSION-BINDINGS.md` in the extracted directory for the parameters that connect state and telemetry to widgets, set numeric limits and smooth instrument readings.

When a connection fails validation, Studio selects the JSON tab and places the cursor at the offending value in your draft. The command line reports the same filename, line, column, JSON path and error code. For example, `$.connections[2].from [BINDING_SOURCE]` points to the source name in the third connection. Choose a name declared in the profile's sources or state and validate again.

## Use the command line

```text
python -I -B start.py validate departure
python -I -B start.py preview departure -o departure.html
python -I -B start.py build departure -o departure-v1.kvmpack
python -I -B start.py inspect departure-v1.kvmpack
```

Choose new output filenames for each build. To prepare the card in a reader, run `python -I -B start.py stage departure-v1.kvmpack --card MOUNT`, replacing `MOUNT` with the card root. Eject the card and insert it in the KVMpad. Choose **Extensions → Install from card**, select the installer and review the requested permissions. Use **Install and open** to launch it. **Home** returns to the firmware's built-in interface. Installed profiles appear under **Installed extensions**.

For an update, keep the profile identifier and increase the manifest version. Stage the new package and review it on the panel. To remove an installed profile, choose **Extensions → Installed extensions**, select it, then choose **Uninstall → Remove**.

On Windows, you can also send a package over an existing CLIENT pairing. Keep the microSD card in KVMpad and open **Extensions → Receive from paired client** on its screen. In Studio, choose **Install to paired device…**, select the board and choose **Send for device review**. Review the package and its permissions on KVMpad. After you approve it, keep the window open to see the device confirm installation. When using the standalone SDK, make `KVMpad.Client.exe` available on the Windows search path.

## Distribution

`SHA256SUMS` lists the digest of each payload file. `LICENSE.md` covers the KVMpad tools and examples. The `lib` directory includes the dependencies' own license notices alongside their package metadata. For a different platform, build its kit from the repository using the target Python interpreter with Tk, `packaging`, `jsonschema` 4.x, Pillow and cryptography installed:

```text
python tools/extension_sdk/build.py KVMpad-Extension-SDK.zip
```
