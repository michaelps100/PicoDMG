/*
 * File: mos6507-microcode.c
 * Author: dgrubb
 * Date: 06/27/2017
 *
 * Provides implementation of CPU logic without reference to
 * specific addressing modes.
 */

#include "mos6507-microcode.h"
#include "mos6507.h"

/* Add memory to Accumulator.
 * A + M + C -> A, C
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - + |
 */
void mos6507_ADC(uint8_t data)
{
    uint8_t accumulator = 0;
    mos6507_get_register(MOS6507_REG_A, &accumulator);
    uint8_t carry = mos6507_get_status_flag(MOS6507_STATUS_FLAG_CARRY) ? 1 : 0;

    if (mos6507_get_status_flag(MOS6507_STATUS_FLAG_DECIMAL)) {
        uint16_t al = (accumulator & 0x0F) + (data & 0x0F) + carry;
        if (al > 9) al += 6;
        uint16_t ah = (accumulator >> 4) + (data >> 4) + (al > 0x0F ? 1 : 0);
        uint16_t binary_sum = (uint16_t)accumulator + (uint16_t)data + carry;
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(binary_sum & 0xFF));
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (ah << 4) & 0x80);
        mos6507_set_status_flag(
            MOS6507_STATUS_FLAG_OVERFLOW,
            !((accumulator ^ data) & 0x80) && ((accumulator ^ binary_sum) & 0x80)
        );
        if (ah > 9) ah += 6;
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (ah > 0x0F));
        uint8_t result = (uint8_t)(((ah & 0x0F) << 4) | (al & 0x0F));
        mos6507_set_register(MOS6507_REG_A, result);
    } else {
        uint16_t tmp = (uint16_t)data + (uint16_t)accumulator + carry;
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (tmp > 0xFF));
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));
        mos6507_set_status_flag(
            MOS6507_STATUS_FLAG_OVERFLOW,
            !((accumulator ^ data) & 0x80) && ((accumulator ^ tmp) & 0x80)
        );
        mos6507_set_register(MOS6507_REG_A, (uint8_t)(tmp & 0xFF));
    }
}

/* Logical AND memory with Accumulator.
 * A & M -> A
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + - - - - |
 */
void mos6507_AND(uint8_t data)
{
    uint8_t accumulator, tmp = 0;
    mos6507_get_register(MOS6507_REG_A, &accumulator);

    tmp = accumulator & data;
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));

    mos6507_set_register(MOS6507_REG_A, tmp);
}

/* Shift left by one bit.
 * C <- [76543210] <- 0
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - - |
 */
void mos6507_ASL(uint8_t *data)
{
    uint16_t tmp = *data;

    tmp <<= 1;
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (tmp & 0x100));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));

    *data = (tmp & 0xFF);
}

/* Shift Accumulator left by one bit.
 * C <- [76543210] <- 0
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - - |
 */
void mos6507_ASL_Accumulator(void)
{
    uint8_t accumulator;
    uint16_t tmp;
    mos6507_get_register(MOS6507_REG_A, &accumulator);
    tmp = accumulator;

    tmp <<= 1;
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (tmp & 0x100));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));

    accumulator = (tmp & 0xFF);
    mos6507_set_register(MOS6507_REG_A, accumulator);
}

/* Test bits in memory with Accumulator. Bits 7 and 6 of operand are transfered
 * to bit 7 and 6 of status register (N, V). Zeroflag is set to the result of
 * operand & Accumulator.
 * A & M, M[7] -> N, M[6] -> V
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | 7 + - - - 6 |
 */
void mos6507_BIT(uint8_t data)
{
    uint8_t accumulator, tmp = 0;
    mos6507_get_register(MOS6507_REG_A, &accumulator);

    tmp = accumulator & data;

    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (data & MOS6507_STATUS_FLAG_NEGATIVE));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_OVERFLOW, (data & MOS6507_STATUS_FLAG_OVERFLOW));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));
}

/* Compare memory with Accumulator.
 * A - M
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - - |
 */
void mos6507_CMP(uint8_t data)
{
    uint8_t accumulator = 0;
    mos6507_get_register(MOS6507_REG_A, &accumulator);
    uint8_t diff = accumulator - data;

    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (accumulator >= data));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (diff & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, (accumulator == data));
}

/* Compare memory with Index X.
 * X - M
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - - |
 */
void mos6507_CPX(uint8_t data)
{
    uint8_t X = 0;
    mos6507_get_register(MOS6507_REG_X, &X);
    uint8_t diff = X - data;

    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (X >= data));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (diff & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, (X == data));
}

/* Compare memory with Index Y.
 * Y - M
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - - |
 */
void mos6507_CPY(uint8_t data)
{
    uint8_t Y = 0;
    mos6507_get_register(MOS6507_REG_Y, &Y);
    uint8_t diff = Y - data;

    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (Y >= data));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (diff & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, (Y == data));
}

/* Exclusive OR memory with Accumulator.
 * A ^ M -> A
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + - - - - |
 */
void mos6507_EOR(uint8_t data)
{
    uint8_t accumulator, tmp = 0;
    mos6507_get_register(MOS6507_REG_A, &accumulator);

    tmp = accumulator ^ data;

    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));
    mos6507_set_register(MOS6507_REG_A, tmp);
}

/* Shift right by one bit.
 * 0 -> [76543210] -> C
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | - + + - - - |
 */
void mos6507_LSR(uint8_t *data)
{
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (*data & 0x01));
    *data >>= 1;
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(*data & 0xFF));
}

/* Shift right by one bit in the Accumulator.
 * 0 -> [76543210] -> C
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | - + + - - - |
 */
void mos6507_LSR_Accumulator(void)
{
    uint8_t accumulator;
    mos6507_get_register(MOS6507_REG_A, &accumulator);
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (accumulator & 0x01));
    accumulator >>= 1;
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(accumulator & 0xFF));
    mos6507_set_register(MOS6507_REG_A, accumulator);
}

/* Logical OR memory with Accumulator.
 * A | M -> A
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + - - - - |
 */
void mos6507_ORA(uint8_t *data)
{
    uint8_t accumulator, tmp = 0;
    mos6507_get_register(MOS6507_REG_A, &accumulator);

    tmp = accumulator | *data;

    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));
    mos6507_set_register(MOS6507_REG_A, tmp);
}


// WTF
void mos6507_TSB(uint8_t *data)
{
    uint8_t accumulator, tmp = 0;
    mos6507_get_register(MOS6507_REG_A, &accumulator);

    tmp = accumulator | *data;

//    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, (*data & tmp));
    mos6507_set_register(MOS6507_REG_A, tmp);
}

/* Rotate one bit left.
 * C <- [76543210] <- C
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - - |
 */
void mos6507_ROL(uint8_t *data)
{
    uint16_t tmp = 0;

    tmp = *data << 1;
    if (mos6507_get_status_flag(MOS6507_STATUS_FLAG_CARRY)) {
        tmp |= 0x01;
    }
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (tmp & 0x0100));
    *data = (tmp & 0xFF);
}

/* Rotate Accumulator one bit left.
 * C <- [76543210] <- C
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - - |
 */
void mos6507_ROL_Accumulator(void)
{
    uint16_t tmp = 0;
    uint8_t accumulator;
    mos6507_get_register(MOS6507_REG_A, &accumulator);

    tmp = accumulator << 1;
    if (mos6507_get_status_flag(MOS6507_STATUS_FLAG_CARRY)) {
        tmp |= 0x01;
    }
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (tmp & 0x0100));
    accumulator = (tmp & 0xFF);
    mos6507_set_register(MOS6507_REG_A, accumulator);
}

/* Rotate one bit right.
 * C -> [76543210] -> C
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - - |
 */
void mos6507_ROR(uint8_t *data)
{
    uint16_t tmp = 0;
    uint8_t tmpCarry = 0;

    tmpCarry = (*data & 0x01);
    tmp = *data >> 1;
    if (mos6507_get_status_flag(MOS6507_STATUS_FLAG_CARRY)) {
        tmp |= 0x80;
    }
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, tmpCarry);
    *data = (tmp & 0xFF);
}

/* Rotate Accumulator one bit right.
 * C -> [76543210] -> C
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - - |
 */
void mos6507_ROR_Accumulator(void)
{
    uint16_t tmp, tmpCarry = 0;
    uint8_t accumulator;
    mos6507_get_register(MOS6507_REG_A, &accumulator);

    tmpCarry = accumulator & 0x01;
    tmp = accumulator >> 1;
    if (mos6507_get_status_flag(MOS6507_STATUS_FLAG_CARRY)) {
        tmp |= 0x80;
    }
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));
    mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, tmpCarry);
    accumulator = (tmp & 0xFF);
    mos6507_set_register(MOS6507_REG_A, accumulator);
}
/* Subtract memory from Accumulator with borrow.
 * A - M - C -> A
 *
 * Status flag changes (+ = conditionally modified, 1 = set, 0 = cleared):
 *
 * | N Z C I D V |
 * | + + + - - + |
 */
void mos6507_SBC(uint8_t data)
{
    uint8_t accumulator = 0;
    mos6507_get_register(MOS6507_REG_A, &accumulator);
    uint8_t borrow = mos6507_get_status_flag(MOS6507_STATUS_FLAG_CARRY) ? 0 : 1;

    if (mos6507_get_status_flag(MOS6507_STATUS_FLAG_DECIMAL)) {
        int diff = (int)accumulator - (int)data - (int)borrow;
        int al = (accumulator & 0x0F) - (data & 0x0F) - (int)borrow;
        if (al < 0) al -= 6;
        int ah = (accumulator >> 4) - (data >> 4) - (al < 0 ? 1 : 0);
        if (ah < 0) ah -= 6;

        mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (diff & 0x80));
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(diff & 0xFF));
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (diff >= 0));
        mos6507_set_status_flag(
            MOS6507_STATUS_FLAG_OVERFLOW,
            ((accumulator ^ data) & 0x80) && ((accumulator ^ diff) & 0x80)
        );
        uint8_t result = (uint8_t)(((ah & 0x0F) << 4) | (al & 0x0F));
        mos6507_set_register(MOS6507_REG_A, result);
    } else {
        int tmp = (int)accumulator - (int)data - (int)borrow;
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_NEGATIVE, (tmp & 0x80));
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_CARRY, (tmp >= 0));
        mos6507_set_status_flag(MOS6507_STATUS_FLAG_ZERO, !(tmp & 0xFF));
        mos6507_set_status_flag(
            MOS6507_STATUS_FLAG_OVERFLOW,
            ((accumulator ^ data) & 0x80) && ((accumulator ^ tmp) & 0x80)
        );
        mos6507_set_register(MOS6507_REG_A, (uint8_t)(tmp & 0xFF));
    }
}
