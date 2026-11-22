# Working in the KVMpad repository

## Where to start

Find the component and its tests in the [repository map](docs/REPO-LAYOUT.md). For hardware changes, check the target board's connectors and measured memory sizes before flashing. [Board record](hardware/boards.md)

The [simulator guide](simulator/README.md) covers running the firmware UI on a computer. The [Windows CLIENT guide](clients/windows/README.md) covers building and pairing the desktop application. For a new extension, start with the [extension SDK](tools/extension_sdk/README.md).

## Language

Use American English for code, comments, documentation, test reports, commit messages and pull requests.

## Product constraints

| Area | Decision |
|---|---|
| UI | Write the UI in C with LVGL 9.6, its native Observer and KVMpad UI Core. |
| Wi-Fi | Keep modem sleep off and use the cached BSSID and channel for fast reconnection. |
| USB HID | Expose a boot keyboard and mouse. USB profiles can also expose Consumer Control. The mouse reports `AC Pan` in report protocol. The ESP32-S3 has four usable IN endpoints, so the OTG port has no CDC interface. |
| Text input | The Windows CLIENT implements “Type text” using `keyboard.key`. |
| Keep-awake | Keep-awake is off by default and is the only exception to TECH-064. |
| Pairing | The native Windows CLIENT pins the certificate. The CLIENT discovers the board over mDNS and pairs with PAKE using a six-digit code entered by the user. The pairing window lasts at most 180 seconds. |
| Control approval | The local Settings view controls approval for pairing, control and Alt+F4. The CLIENT cannot change these settings. |
| Keyboard layouts | Generate host layout tables from xkeyboard-config. They cover legends, ANSI/ISO/JIS/ABNT2/KR geometry and IME keys. The on-device IME for Japanese, Simplified Chinese and Korean delivers Unicode through per-OS input modes. |
| Input languages | Tap the language key to cycle through active languages. Hold it to open a picker. Enabled languages are stored in Settings → Keyboard and default to English only. |
| Board | The Waveshare ESP32-S3-Touch-LCD-7B uses a 1024×600 screen, a CH32V003 I/O expander at 0x24, inverted backlight PWM and EXIO5 LOW for USB. Scale UI geometry from the 800×480 reference. |

## Making a change

- Keep each pull request focused on one change. Document the affected requirements, test commands, observed results and any cases that still need a board in the [pull request template](.github/pull_request_template.md).
- Check firmware and dependency changes with `idf.py build` using ESP-IDF v6.1. For UI changes, include a reproducible simulator screenshot and test observer cleanup when the view closes.
- Install tools and virtual environments outside the repository. Keep raw logs, screenshots and packet captures outside the checkout. Exclude typed keystrokes, pairing material and access tokens from test evidence.
- Use software signing for OTA images with each builder's own offline key. Leave device eFuses, secure boot and flash encryption untouched. [OTA guide](docs/OTA.md)

## License headers

Every authored source file, including generated sources, starts with these three lines in the language's comment syntax, after the shebang when present.

```c
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
```

Markdown, JSON, lock files, Visual Studio solution files, binary assets, dotfiles and the frozen specification package are exempt. Keep third-party headers intact and list each component's path, upstream, version and license in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The firmware and CLIENT also display the notice in About.

Run `python -I tools/check_license_headers.py` before opening a pull request. The checker reports missing headers and unlisted third-party sources.

## Contribution license

The project is released under PolyForm Noncommercial 1.0.0 and the author (D-Ogi) alone retains the right to license it
commercially. By submitting a contribution (pull request, patch) you grant the author a non-exclusive, perpetual license
to use, modify and relicense it — including commercially — and you confirm that you are entitled to do so.
Contributions without this consent cannot be accepted.
