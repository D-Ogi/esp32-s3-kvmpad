# Extensions on the 7B

An extension adds screens to the KVMpad and stores its installed files on microSD. Pilot Panel combines a map, kneeboard, radio and autopilot in one package. USB Monitor Console puts a desktop viewport, touchpad and keyboard on a portrait screen. [Pilot Panel source](../extensions/pilot-panel/) · [USB Monitor Console source](../extensions/usb-monitor-console/)

## Install and open a package

1. In Extension Studio, choose **Install via card…** and select the root of a FAT32 or exFAT microSD card. Eject the card and insert it in the KVMpad.
2. On the LCD, open **Extensions → Install from card** and select the package. Review the requested permissions, then tap **Install and open**.
3. To open it again, choose **Extensions → Installed extensions**, select its name and tap **Open**.

Swipe up from the bottom edge of a multi-screen extension, or tap its **Screens** handle, to switch pages. Choose **Home** in the screen picker to leave the extension. Removing the card returns the device to its built-in interface. Change the theme for the board and its extensions under **Settings → Appearance**.

To update an extension, install a newer version with the same package ID. To remove one, choose **Installed extensions**, select the package and tap **Uninstall → Remove**.

## Build your own

Open **Tools → Extension Studio** in the Windows CLIENT. Start with the flight checklist to try buttons, images and local state. Use **Simulate** to check the layout in Day, Night and High contrast, then **Package…** to export a `.kvmpack`. The SDK also includes command-line validation, packaging and card staging. [Extension SDK guide](../tools/extension_sdk/README.md) · [Widget bindings](EXTENSION-BINDINGS.md) · [Rate slider example](../tools/kvmpack/samples/rate-slider/)

Static charts and checklists work offline. Extensions that use a Windows HOST reuse the device's existing CLIENT pairing. For Cursor Magnifier, choose the capture source on Windows and approve the display on the board. [Windows HOST guide](../hosts/windows/README.md)
