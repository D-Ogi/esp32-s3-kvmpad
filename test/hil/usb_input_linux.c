// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// usb_enum_linux.py passes open handles to the selected board and a timeout in seconds.
#include <limits.h>
#include "usb_input_probe.h"

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    int values[4];
    for (int i = 0; i < 4; ++i) {
        char *end;
        errno = 0;
        long value = strtol(argv[i + 1], &end, 10);
        if (errno || end == argv[i + 1] || *end || value < 0 || value > INT_MAX) return 2;
        values[i] = (int)value;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    observe_input(values[0], values[1], values[2], values[3], false);
    return 0;
}
