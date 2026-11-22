## Change

Describe the behavior this changes and how to exercise it. Include relevant requirement or test IDs.

## Verification

List the exact commands you ran and their results. State which checks still need a physical board. For UI changes, attach a simulator screenshot and describe how you tested observer cleanup.

## Checklist

- [ ] Exercised affected error paths, including applicable timeouts, queue overflow, USB disconnects or extra touch contacts.
- [ ] Checked release paths for changed input handling, including `UP` and `RELEASE_ALL`.
- [ ] Kept the hot path free of per-sample and per-report allocations.
- [ ] Updated affected API documentation and tests.
- [ ] Checked the diff and attachments for typed keystrokes, pairing material, access tokens and keys.
- [ ] Ran `python -I tools/check_license_headers.py` and listed any new third-party components in `THIRD_PARTY_NOTICES.md`.
