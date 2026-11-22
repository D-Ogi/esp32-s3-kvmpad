# Diagnostics

The firmware exposes diagnostic counters through the UART console. The implementation lives in [main/kvm_diag.c](../../main/kvm_diag.c). This directory documents the component boundary.

Collect aggregate metrics such as touch samples, input release counts and latency. Pairing uses attempt, failure and lockout counters. Keep raw keystrokes, message payloads, pairing codes, PAKE keys and access tokens out of logs.
