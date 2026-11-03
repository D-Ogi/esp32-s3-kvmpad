# `test/hil/` — hardware-in-the-loop helpers (L3–L4)

Run from the workspace with the tools in `../.tools/` (never installed into the repository). Evidence goes to
`../evidence/hardware/` with its SHA256; reports cite the file name and the hash (`test/reports/`).

| Script | Use |
|---|---|
| `kvm_serial.py` | UART console of the board (pyserial, `COM3` by default). `boot` resets via RTS and checks the boot (EXIO5 verified LOW, GT911 1024×600, USB started and mounted); `cmd "<line>" …` runs console commands without a reset; `bootloop N` repeats `boot` N times. `--no-host` when no USB HOST is attached. |
| `usb_enum_windows.ps1` | AT-HID-01 on a Windows HOST: the composite device and its three HID interfaces with the expected class/subclass/protocol, every node started; `-CapsLed` toggles Caps Lock twice (state restored) and checks the LED output reports on the device. |
| `wifi_latency.py` | ADR-017 benches over the console, no reset: `reconnect --count N` (AT-EXT-02, alternating cached and full-scan reconnects timed on the board) and `ping --host H --idle 10 60` (AT-EXT-01, first-packet latency after silence with `WIFI_PS_NONE` vs `WIFI_PS_MIN_MODEM`; `ps none` is restored at the end). |
| `remote_probe.py` | TASK-100 transport from the LAN, no pairing and no token: `mdns` (`_kvmpad._tcp` PTR/SRV/TXT/A; `--iface <LAN address>` on a PC with virtual adapters, `--multicast` when the firewall drops the unicast answer), `tls` (version, certificate, SHA-256 pin, `--expect-sha256` = the `remote status` value), `admit` (HTTP Upgrade refusals before any WebSocket: 403 / 400 / 404, never 101), `plain` (no plain-text listener). `--save <dir>` writes the output and prints its SHA-256. |

Keep reset loops short (15 cycles): every reset re-enumerates the USB device on the HOST.
Without the physical board every hardware result is `NOT_RUN_HW`.
