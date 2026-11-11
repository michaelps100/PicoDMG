#include "fix16.hpp"
#include <stdbool.h>
#include <lobject.hpp>
#ifndef FIXMATH_NO_CTYPE
#include <ctype.h>
#else
static inline int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

static inline int isspace(int c)
{
    return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\v' || c == '\f';
}
#endif

static const uint32_t scales[8] = {
    /* 5 decimals is enough for full fix16_t precision */
    1, 10, 100, 1000, 10000, 100000, 100000, 100000
};

static char *itoa_loop(char *buf, uint32_t scale, uint32_t value, bool skip)
{
    while (scale)
    {
        unsigned digit = (value / scale);
    
        if (!skip || digit || scale == 1)
        {
            skip = false;
            *buf++ = '0' + digit;
            value %= scale;
        }
        
        scale /= 10;
    }
    return buf;
}

int fix16_to_str(fix16_t value, char *buf, int decimals)
{
    char *const beg = buf;

    uint32_t uvalue = (value >= 0) ? value : -value;
    if (value < 0)
        *buf++ = '-';

    /* Separate the integer and decimal parts of the value */
    unsigned intpart = uvalue >> 16;
    uint32_t fracpart = uvalue & 0xFFFF;
    uint32_t scale = scales[decimals & 7];
    fracpart = fix16_mul(fracpart, scale);
    
    if (fracpart >= scale)
    {
        /* Handle carry from decimal part */
        intpart++;
        fracpart -= scale;    
    }
    
    /* Format integer part */
    buf = itoa_loop(buf, 10000, intpart, true);
    
    /* Format decimal part (if any) */
    if (scale != 1)
    {
        *buf++ = '.';
        buf = itoa_loop(buf, scale / 10, fracpart, false);
    }
    
    *buf = '\0';

    /* yocto-8 HACK: walk back n 0 digits */
    if (scale != 1)
    {
        while (*(buf - 1) == '0')
        {
            --buf;
            *buf = '\0';
        }

        if (*(buf - 1) == '.')
        {
            --buf;
            *buf = '\0';
        }
    }

    return buf - beg;
}

fix16_t strtofix16(const char *buf, char **end, int parse_mask)
{
    // parse as 32:32 fixed point for this logic to preserve precision when
    // doing stuff like scientific notation
    // downconvert to 16:16 at the end

    const char *start = buf;

    while (isspace(*buf))
        buf++;
    
    /* Decode the sign */
    bool negative = (*buf == '-');
    if (*buf == '+' || *buf == '-')
        buf++;

    /* Decode the integer part */
    int digit_count = 0;
    uint32_t intpart = 0;
    while (isdigit(*buf))
    {
        intpart *= 10;
        intpart += *buf++ - '0';
        ++digit_count;
    }
    
    uint64_t value = uint64_t(intpart) << 32;

    /* Decode the decimal part */
    if (*buf == '.')
    {
        buf++;
        
        uint64_t fracpart = 0;
        uint64_t scale = 1;
        while (isdigit(*buf) && scale < 100000000) {
            scale *= 10;
            fracpart *= 10;
            fracpart += *buf++ - '0';
            ++digit_count;
        }
        value += (uint64_t(fracpart) << 32) / scale;
    }

    // a single dot is not a proper number
    // it needs to be either e.g. `6.` or `.4` to be valid
    if (digit_count == 0) {
        if (end != nullptr) {
            *end = const_cast<char*>(start);
        }
        return 0;
    }

    // keep parsing extra unnecessary precision digits
    // stop whenever anything else is encountered
    while (*buf != '\0' && isdigit(*buf)) {
        buf++;
    }

    // non-digit character encountered
    // is it the exponent part of the number? e.g. 1.23e-6
    // check if the the caller wants us to parse it?
    if ((parse_mask & LPARSE_ALLOW_EXPONENT) != 0 && *buf == 'e') {
        buf++;
        bool negative_exponent = (*buf == '-');
        if (*buf == '+' || *buf == '-') {
            buf++;
        }

        int exponent = 0;
        while (isdigit(*buf)) {
            exponent *= 10;
            exponent += *buf++ - '0';
        }

        exponent = negative_exponent ? -exponent : exponent;

        // override value if an exponent is known
        if (exponent > 0) {
            for (int i = 0; i < exponent; ++i) {
                value *= 10;
            }
        }
        else if (exponent < 0) {
            for (int i = 0; i < exponent; ++i) {
                value /= 10;
            }
        }
    }

    // update end position
    if (end != nullptr) {
        *end = const_cast<char*>(buf);
    }

    value >>= 16;

    if ((parse_mask & LPARSE_SHIFT) != 0) {
        value >>= 16;
    }

    return negative ? -fix16_t(value) : fix16_t(value);
}

fix16_t fix16_to_str(const char* buf)
{
    return strtofix16(buf, nullptr, 0);
}
