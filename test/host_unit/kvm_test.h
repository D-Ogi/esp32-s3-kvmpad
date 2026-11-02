// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Minimal host test harness: no dependencies, one binary per suite, non-zero exit on failure.
#pragma once

#include <stdio.h>
#include <stdlib.h>

static int kvm_test_failures;
static int kvm_test_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        kvm_test_checks++;                                                             \
        if (!(cond)) {                                                                 \
            kvm_test_failures++;                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        long long _a = (long long)(a);                                                              \
        long long _b = (long long)(b);                                                              \
        kvm_test_checks++;                                                                          \
        if (_a != _b) {                                                                             \
            kvm_test_failures++;                                                                    \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
                    #a, #b, _a, _b);                                                                \
        }                                                                                           \
    } while (0)

#define RUN(test)                    \
    do {                             \
        printf("  %s\n", #test);     \
        test();                      \
    } while (0)

#define TEST_MAIN_END(name)                                                                  \
    printf("%s: %d checks, %d failures\n", name, kvm_test_checks, kvm_test_failures);       \
    return kvm_test_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE
