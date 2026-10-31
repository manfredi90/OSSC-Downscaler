//
// Copyright (C) 2018  Markus Hiienkari <mhiienka@niksula.hut.fi>
//
// This file is part of Open Source Scan Converter project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <stdio.h>
#include <stdarg.h>
#include "sys/alt_stdio.h"
#include "utils.h"
#include "system.h"
#include "sysconfig.h"
#include "io.h"

inline __attribute__((flatten, __section__(".text_bram"))) unsigned char bitswap8(unsigned char v)
{
    return ((v * 0x0802LU & 0x22110LU) |
            (v * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16;
}

alt_u32 bswap32(alt_u32 w)
{
    return (((w << 24) & 0xff000000) |
            ((w <<  8) & 0x00ff0000) |
            ((w >>  8) & 0x0000ff00) |
            ((w >> 24) & 0x000000ff));
}

// CRC-32 (zlib compatible: reflected 0xEDB88320, init/final 0xFFFFFFFF),
// same results as the hw_crc32 component it replaces (removed to free logic).
unsigned long crc32(unsigned char *input_data, unsigned long input_data_length, int do_initialize)
{
    static uint32_t state;
    unsigned long i;
    int b;

    if (do_initialize)
        state = 0xFFFFFFFFUL;
    for (i=0; i<input_data_length; i++) {
        state ^= input_data[i];
        for (b=0; b<8; b++)
            state = (state >> 1) ^ (0xEDB88320UL & (0UL - (state & 1)));
    }
    return state ^ 0xFFFFFFFFUL;
}

/* printf for direct driver interface */
int dd_printf(const char *__restrict fmt, ...) {
    int ret;
    va_list ap;
    char buf[PRINTF_BUFSIZE];

    va_start(ap, fmt);
    ret = vsnprintf(buf, PRINTF_BUFSIZE, fmt, ap);
    va_end(ap);
    if (ret > 0)
        alt_putstr(buf);

    return 0;
}
