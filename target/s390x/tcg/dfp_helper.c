/*
 * S/390 decimal-floating-point helpers
 *
 * Copyright (c) 2026 Yvan Janssens
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "tcg_s390x.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/helper-proto.h"

#define DECNUMDIGITS 34
#include "libdecnumber/decContext.h"
#include "libdecnumber/decNumber.h"
#include "libdecnumber/dpd/decimal64.h"
#include "libdecnumber/dpd/decimal128.h"

static void dfp_fetch_zoned(CPUS390XState *env, uint64_t addr,
                            uint8_t *zoned, uint32_t length, uintptr_t ra)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        zoned[i] = cpu_ldub_data_ra(env, addr + i, ra);
    }
}

static void dfp_store_zoned(CPUS390XState *env, uint64_t addr,
                            const uint8_t *zoned, uint32_t length,
                            uintptr_t ra)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        cpu_stb_data_ra(env, addr + i, zoned[i], ra);
    }
}

static void dfp_data_exception(CPUS390XState *env, uintptr_t ra)
{
    tcg_s390_data_exception(env, 0, ra);
}

static void dfp_number_from_zoned(CPUS390XState *env, decNumber *number,
                                  uint64_t addr, uint32_t length,
                                  uint32_t mask, decContext *context,
                                  uintptr_t ra)
{
    uint8_t zoned[34];
    char text[36];
    char *p = text;
    uint32_t i;

    dfp_fetch_zoned(env, addr, zoned, length, ra);
    if (mask & 8) {
        switch (zoned[length - 1] >> 4) {
        case 0xb:
        case 0xd:
            *p++ = '-';
            break;
        case 0xa:
        case 0xc:
        case 0xe:
        case 0xf:
            break;
        default:
            dfp_data_exception(env, ra);
        }
    }
    for (i = 0; i < length; i++) {
        uint8_t digit = zoned[i] & 0xf;

        if (digit > 9) {
            dfp_data_exception(env, ra);
        }
        *p++ = digit + '0';
    }
    *p = '\0';
    decNumberFromString(number, text, context);
}

static void dfp_put_decimal64(CPUS390XState *env, uint32_t reg,
                              const decNumber *number, decContext *context)
{
    decimal64 value;

    decimal64FromNumber(&value, number, context);
    memcpy(get_freg(env, reg), value.bytes, sizeof(value));
}

static void dfp_put_decimal128(CPUS390XState *env, uint32_t reg,
                               const decNumber *number, decContext *context)
{
    decimal128 value;

    decimal128FromNumber(&value, number, context);
    memcpy(get_freg(env, reg + 2), value.bytes, 8);
    memcpy(get_freg(env, reg), value.bytes + 8, 8);
}

void HELPER(cdzt)(CPUS390XState *env, uint64_t addr, uint32_t length,
                  uint32_t reg, uint32_t mask)
{
    decContext context;
    decNumber number;
    uintptr_t ra = GETPC();

    if (length > 16) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }
    decContextDefault(&context, DEC_INIT_DECIMAL64);
    dfp_number_from_zoned(env, &number, addr, length, mask, &context, ra);
    dfp_put_decimal64(env, reg, &number, &context);
}

void HELPER(cxzt)(CPUS390XState *env, uint64_t addr, uint32_t length,
                  uint32_t reg, uint32_t mask)
{
    decContext context;
    decNumber number;
    uintptr_t ra = GETPC();

    if (length > 34) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }
    decContextDefault(&context, DEC_INIT_DECIMAL128);
    dfp_number_from_zoned(env, &number, addr, length, mask, &context, ra);
    dfp_put_decimal128(env, reg, &number, &context);
}

static void dfp_get_decimal64(CPUS390XState *env, uint32_t reg,
                              decimal64 *value, decNumber *number)
{
    memcpy(value->bytes, get_freg(env, reg), sizeof(*value));
    decimal64ToNumber(value, number);
}

static void dfp_get_decimal128(CPUS390XState *env, uint32_t reg,
                               decimal128 *value, decNumber *number)
{
    memcpy(value->bytes, get_freg(env, reg + 2), 8);
    memcpy(value->bytes + 8, get_freg(env, reg), 8);
    decimal128ToNumber(value, number);
}

static uint32_t dfp_number_to_zoned(CPUS390XState *env, decNumber *number,
                                    decNumber *coefficient, uint64_t addr,
                                    uint32_t length, uint32_t mask,
                                    uintptr_t ra)
{
    uint8_t zoned[34];
    char text[DECNUMDIGITS + 16];
    bool negative = decNumberIsNegative(number);
    uint32_t cc;
    size_t digits;
    size_t skip = 0;
    size_t padding = 0;
    uint32_t i;

    if ((mask & 1) && decNumberIsZero(number)) {
        negative = false;
    }
    if (decNumberIsNaN(number) || decNumberIsInfinite(number)) {
        cc = 3;
        coefficient->exponent = 0;
        coefficient->bits &= ~DECNEG;
        decNumberToString(coefficient, text);
    } else {
        cc = decNumberIsZero(number) ? 0 : negative ? 1 : 2;
        number->exponent = 0;
        number->bits &= ~DECNEG;
        decNumberToString(number, text);
    }

    digits = strlen(text);
    if (digits <= length) {
        padding = length - digits;
    } else {
        skip = digits - length;
        cc = 3;
    }
    for (i = 0; i < length; i++) {
        uint8_t digit = padding ? 0 : text[skip++] - '0';

        if (padding) {
            padding--;
        }
        zoned[i] = digit | ((mask & 4) ? 0x30 : 0xf0);
    }
    if (mask & 8) {
        zoned[length - 1] &= 0x0f;
        zoned[length - 1] |= negative ? 0xd0 : (mask & 2) ? 0xf0 : 0xc0;
    }
    dfp_store_zoned(env, addr, zoned, length, ra);
    return cc;
}

uint32_t HELPER(czdt)(CPUS390XState *env, uint64_t addr, uint32_t length,
                      uint32_t reg, uint32_t mask)
{
    decimal64 value, coefficient_value;
    decNumber number, coefficient;
    uintptr_t ra = GETPC();

    if (length > 16) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }
    dfp_get_decimal64(env, reg, &value, &number);
    coefficient_value = value;
    if (decNumberIsNaN(&number) || decNumberIsInfinite(&number)) {
        uint64_t raw;

        memcpy(&raw, coefficient_value.bytes, sizeof(raw));
        raw &= UINT64_C(0x8003ffffffffffff);
        memcpy(coefficient_value.bytes, &raw, sizeof(raw));
        decimal64ToNumber(&coefficient_value, &coefficient);
    } else {
        decNumberZero(&coefficient);
    }
    return dfp_number_to_zoned(env, &number, &coefficient, addr, length,
                               mask, ra);
}

uint32_t HELPER(czxt)(CPUS390XState *env, uint64_t addr, uint32_t length,
                      uint32_t reg, uint32_t mask)
{
    decimal128 value, coefficient_value;
    decNumber number, coefficient;
    uintptr_t ra = GETPC();

    if (length > 34) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }
    dfp_get_decimal128(env, reg, &value, &number);
    coefficient_value = value;
    if (decNumberIsNaN(&number) || decNumberIsInfinite(&number)) {
        uint32_t high;

        memcpy(&high, coefficient_value.bytes + 12, sizeof(high));
        high &= 0x80003fff;
        memcpy(coefficient_value.bytes + 12, &high, sizeof(high));
        decimal128ToNumber(&coefficient_value, &coefficient);
    } else {
        decNumberZero(&coefficient);
    }
    return dfp_number_to_zoned(env, &number, &coefficient, addr, length,
                               mask, ra);
}

static void dfp_number_from_packed(CPUS390XState *env, decNumber *number,
                                   uint64_t addr, uint32_t length,
                                   uint32_t mask, decContext *context,
                                   uintptr_t ra)
{
    uint8_t packed[18];
    char text[36];
    char *p = text;
    bool is_signed = mask & 8;
    uint32_t i;

    dfp_fetch_zoned(env, addr, packed, length, ra);
    if (length == (context->digits == 16 ? 9 : 18) &&
        (is_signed ? packed[0] & 0xf0 : packed[0])) {
        dfp_data_exception(env, ra);
    }
    if (is_signed && !(mask & 1)) {
        switch (packed[length - 1] & 0xf) {
        case 0xb:
        case 0xd:
            *p++ = '-';
            break;
        case 0xa:
        case 0xc:
        case 0xe:
        case 0xf:
            break;
        default:
            dfp_data_exception(env, ra);
        }
    }
    for (i = 0; i < length; i++) {
        uint8_t high = packed[i] >> 4;
        uint8_t low = packed[i] & 0xf;

        if (high > 9) {
            dfp_data_exception(env, ra);
        }
        *p++ = high + '0';
        if (i == length - 1 && is_signed) {
            break;
        }
        if (low > 9) {
            dfp_data_exception(env, ra);
        }
        *p++ = low + '0';
    }
    *p = '\0';
    decNumberFromString(number, text, context);
}

void HELPER(cdpt)(CPUS390XState *env, uint64_t addr, uint32_t length,
                  uint32_t reg, uint32_t mask)
{
    decContext context;
    decNumber number;
    uintptr_t ra = GETPC();

    if (length > 9) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }
    decContextDefault(&context, DEC_INIT_DECIMAL64);
    dfp_number_from_packed(env, &number, addr, length, mask, &context, ra);
    dfp_put_decimal64(env, reg, &number, &context);
}

void HELPER(cxpt)(CPUS390XState *env, uint64_t addr, uint32_t length,
                  uint32_t reg, uint32_t mask)
{
    decContext context;
    decNumber number;
    uintptr_t ra = GETPC();

    if (length > 18) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }
    decContextDefault(&context, DEC_INIT_DECIMAL128);
    dfp_number_from_packed(env, &number, addr, length, mask, &context, ra);
    dfp_put_decimal128(env, reg, &number, &context);
}

static uint32_t dfp_number_to_packed(CPUS390XState *env, decNumber *number,
                                     decNumber *coefficient, uint64_t addr,
                                     uint32_t length, uint32_t mask,
                                     uintptr_t ra)
{
    uint8_t packed[18];
    char text[DECNUMDIGITS + 16];
    bool is_signed = mask & 8;
    bool negative = decNumberIsNegative(number);
    uint8_t sign = 0xc;
    uint32_t cc;
    size_t capacity = length * 2 - is_signed;
    size_t digits;
    size_t skip = 0;
    size_t padding = 0;
    size_t index = 0;
    uint32_t i;

    if (is_signed) {
        if (negative && !((mask & 1) && decNumberIsZero(number))) {
            sign = 0xd;
        } else if (mask & 2) {
            sign = 0xf;
        }
    }
    if (decNumberIsNaN(number) || decNumberIsInfinite(number)) {
        cc = 3;
        coefficient->exponent = 0;
        coefficient->bits &= ~DECNEG;
        decNumberToString(coefficient, text);
    } else {
        cc = decNumberIsZero(number) ? 0 : negative ? 1 : 2;
        number->exponent = 0;
        number->bits &= ~DECNEG;
        decNumberToString(number, text);
    }
    digits = strlen(text);
    if (digits <= capacity) {
        padding = capacity - digits;
    } else {
        skip = digits - capacity;
        cc = 3;
    }
    for (i = 0; i < length; i++) {
        uint8_t high = padding ? 0 : text[skip++] - '0';
        uint8_t low;

        if (padding) {
            padding--;
        }
        if (is_signed && index + 1 == capacity) {
            low = sign;
        } else {
            low = padding ? 0 : text[skip++] - '0';
            if (padding) {
                padding--;
            }
        }
        packed[i] = high << 4 | low;
        index += 2;
    }
    dfp_store_zoned(env, addr, packed, length, ra);
    return cc;
}

uint32_t HELPER(cpdt)(CPUS390XState *env, uint64_t addr, uint32_t length,
                      uint32_t reg, uint32_t mask)
{
    decimal64 value, coefficient_value;
    decNumber number, coefficient;
    uintptr_t ra = GETPC();

    if (length > 9) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }
    dfp_get_decimal64(env, reg, &value, &number);
    coefficient_value = value;
    if (decNumberIsNaN(&number) || decNumberIsInfinite(&number)) {
        uint64_t raw;

        memcpy(&raw, coefficient_value.bytes, sizeof(raw));
        raw &= UINT64_C(0x8003ffffffffffff);
        memcpy(coefficient_value.bytes, &raw, sizeof(raw));
        decimal64ToNumber(&coefficient_value, &coefficient);
    } else {
        decNumberZero(&coefficient);
    }
    return dfp_number_to_packed(env, &number, &coefficient, addr, length,
                                mask, ra);
}

uint32_t HELPER(cpxt)(CPUS390XState *env, uint64_t addr, uint32_t length,
                      uint32_t reg, uint32_t mask)
{
    decimal128 value, coefficient_value;
    decNumber number, coefficient;
    uintptr_t ra = GETPC();

    if (length > 18) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }
    dfp_get_decimal128(env, reg, &value, &number);
    coefficient_value = value;
    if (decNumberIsNaN(&number) || decNumberIsInfinite(&number)) {
        uint32_t high;

        memcpy(&high, coefficient_value.bytes + 12, sizeof(high));
        high &= 0x80003fff;
        memcpy(coefficient_value.bytes + 12, &high, sizeof(high));
        decimal128ToNumber(&coefficient_value, &coefficient);
    } else {
        decNumberZero(&coefficient);
    }
    return dfp_number_to_packed(env, &number, &coefficient, addr, length,
                                mask, ra);
}
