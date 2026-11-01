# `reports/`

Test reports in the format from `docs/spec/04-TEST-PLAN-AND-ACCEPTANCE-CRITERIA.md` §12.

File name: `<YYYY-MM-DD>_<test_id>_<build_sha8>.yaml`.

```yaml
test_id: AT-HID-05
result: PASS | FAIL | NOT_RUN_HW | BLOCKED
build_sha: "<git commit>"
firmware_sha256: "<sha256>"
hardware: "Waveshare ESP32-S3-Touch-LCD-7 rev <...>"
flash_psram: "<actual>"
usb_host_os: "<OS/build>"
idf: "<tag>"
components_lock_sha256: "<sha256>"
when_utc: "<ISO8601>"
steps: "<commands/touch inputs>"
observed: "<actual report/metrics>"
evidence: ["logs/xxx.txt", "usb/xxx.pcapng"]
issues: []
```

**Sanitization:** logs, dumps and pcap must not contain typed passwords, tokens or pairing secrets.
