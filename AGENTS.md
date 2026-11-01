# AGENTS.md — mandatory working contract for implementation agents

> Read this first. If you cannot carry out any of the items, **stop the relevant fragment and report the limitation**, do not improvise.

## 1. Project

You are implementing KVM Essential for the **Waveshare ESP32-S3-Touch-LCD-7 (800×480, GT911 + CH422G + USB OTG)**, NOT for the 7B/7C model nor for a plain "ESP32-S3-DevKit". This is a HID controller, **not HDMI capture**.

**SOURCE ORDER:** `02-TECHNICAL-SPECIFICATION.md`/`03-CONTRACTS-AND-PROTOCOLS.md` → `01-FUNCTIONAL-AND-PROCESS-SPECIFICATION.md` → `04-TEST-PLAN-AND-ACCEPTANCE-CRITERIA.md` → `05-IMPLEMENTATION-PLAN.md` → `reference/kvm-essential-800x480.html` for UX only.

## 2. Six prohibitions without exceptions

1. Do not create functions/APIs on the basis of guesses: check the local headers of the **pinned** version of IDF, TinyUSB, LVGL, GT911.
2. Do not perform USB HID injection from a UI callback, a touch ISR or a network parser; **only** through typed intents → `InputArbiter` → `HidScheduler`.
3. Do not allow stuck keys: `RELEASE_ALL` on every loss of owner, USB, session, on a gesture error/timeout, on a mode switch/OTA.
4. Do not emit HID from remote without TLS + auth + a valid lease epoch + physical approval. No "temporary ws://" in a release build.
5. Do not credit yourself with test results if you did not run them. Without a physical board `PASS_HW` is FORBIDDEN.
6. Do not enable the secure boot/flash encryption eFuse and do not perform a factory reset from firmware tooling without the explicit consent of a human and a recovery plan.

## 3. Every task must be small

You have been given `TASK_ID`, `FILES_ALLOWED_TO_EDIT`, `FR/TECH-IDs` and `AT-IDs`. Do not change the remaining components. If you encounter a contract inconsistency — create an ADR proposal, not a refactor of the whole project. One PR = one feature or one coherent layer.

## 4. Checklist before editing

- [ ] I know the specific model and revision (if hardware is relevant).
- [ ] I have the version and `dependencies.lock`; I have checked the actual API.
- [ ] I understand who produces an event and who is allowed to send HID.
- [ ] I have listed all the `UP` and `RELEASE_ALL` paths.
- [ ] I have the tests assigned from `04` and I am able to run them or to mark them explicitly as NOT_RUN_HW.
- [ ] I will not put keyboard logs, pairing secrets or tokens into the repo.

## 5. Checklist before a PR

- [ ] Correct build/lint/unit tests (commands and result given, not imagined).
- [ ] Race/timeout/overflow/deenumeration/3+ fingers are handled.
- [ ] Zero allocations per sample/hid report on the hot path.
- [ ] Error tests (not only the happy path).
- [ ] I changed only the allowed files or I have approval.
- [ ] I have updated the traceability, the tests and the API documentation.
- [ ] The response contains the exact file names, `Status`, `Proof`, `Limitations`.

## 6. Minimum report format

```text
TASK: TASK-XXX
STATUS: IMPLEMENTED_SW / READY_FOR_HIL / VERIFIED_HW / BLOCKED
CHANGED: <file paths>
REQUIREMENTS: <FR/TECH IDs>
TESTS_ACTUALLY_EXECUTED: <exact command and output>
TESTS_NOT_RUN: <IDs and reasons>
KNOWN_RISKS: <list>
NEXT: <one specific task or ADR>
```

## 7. Conflict resolution priorities

1. Security and the absence of unauthorized actions on the HOST.
2. Correctness of USB HID (`DOWN→UP`, `RELEASE_ALL`, ownership/epoch).
3. Correctness of GT911 data, independence of USB from Wi-Fi.
4. Responsiveness and legibility of the 800×480 UX.
5. Animation performance/aesthetic effects.

If an improvement to item 5 violates items 1–4, it is rejected regardless of FPS.


## 8. UI: binding supplement v1.1.0

If the task concerns LVGL, navigation, components, GT911 routing or the UI, before editing **MUST READ** `07-KVMpad-UI-FRAMEWORK-ARCHITECTURE.md` and `AGENTS-UI.md`. Do not use `ui_lvgl` from v1.0 as a new monolith. Do not add a second GT911 read, do not create your own Redux/DOM, do not call LVGL outside the UI owner, do not edit `ui_generated`. The LVGL adapter is selected by TASK-060A with HIL proof.
