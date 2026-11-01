<!-- Format required by AGENTS.md §6. One PR = one TASK. -->

```text
TASK: TASK-XXX
STATUS: IMPLEMENTED_SW / READY_FOR_HIL / VERIFIED_HW / BLOCKED
CHANGED: <file paths>
REQUIREMENTS: <FR/TECH/SEC IDs>
TESTS_ACTUALLY_EXECUTED: <exact command and output>
TESTS_NOT_RUN: <IDs and reasons, e.g. NOT_RUN_HW>
KNOWN_RISKS: <list>
NEXT: <one specific task or ADR>
```

## Checklist (AGENTS.md §5)

- [ ] Build / lint / unit tests run — commands and results given above
- [ ] Race / timeout / overflow / deenumeration / 3+ fingers handled
- [ ] Zero allocations per sample / per HID report on the hot path
- [ ] Error-path tests, not only the happy path
- [ ] Only the files allowed for the task changed (or approval is in place)
- [ ] All `UP` and `RELEASE_ALL` paths listed and tested
- [ ] Traceability, tests and API documentation updated
- [ ] No keyboard logs, pairing secrets, tokens or keys in the diff
- [ ] Without a physical board: no `PASS_HW`

<!-- UI (AGENTS-UI.md): attach an SDL screenshot, an observer lifecycle test and a list of AT-UIFW PASS/FAIL/NOT_RUN_HW. -->
