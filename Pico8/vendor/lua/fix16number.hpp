#include <fix16.hpp>

#include <bit>
#include <compare>
#include <cstdio>
#include <limits>
#include <type_traits>

#define LUAFIX16_FN_ATTR [[gnu::always_inline, gnu::flatten]]

// adapted from fix16.hpp
struct LuaFix16 {
	fix16_t value;

	LUAFIX16_FN_ATTR constexpr LuaFix16() = default;
	LUAFIX16_FN_ATTR constexpr LuaFix16(const LuaFix16 &inValue) = default;
	LUAFIX16_FN_ATTR constexpr LuaFix16(LuaFix16 &&inValue) = default;
	//LuaFix16(const fix16_t inValue) { value = inValue;                   }
	LUAFIX16_FN_ATTR constexpr LuaFix16(const float inValue)   { value = fix16_from_float(inValue); }
	LUAFIX16_FN_ATTR constexpr LuaFix16(const double inValue)  { value = fix16_from_dbl(inValue);   }
	LUAFIX16_FN_ATTR constexpr LuaFix16(const int inValue)     { value = fix16_from_int(inValue);   }
	LUAFIX16_FN_ATTR constexpr LuaFix16(const int16_t integer_part, uint16_t decimal_part) {
		value = (uint16_t(integer_part) << 16) | decimal_part;
	}
	
	template <typename Integer> requires std::signed_integral<Integer>
	LUAFIX16_FN_ATTR constexpr LuaFix16(const Integer inValue) { value = fix16_from_int(inValue);   }
	
	template <typename UInteger> requires std::unsigned_integral<UInteger>
	LUAFIX16_FN_ATTR constexpr LuaFix16(const UInteger inValue) { value = fix16_from_int((int16_t)(uint16_t)inValue);   }

	LUAFIX16_FN_ATTR constexpr static LuaFix16 from_fix16(const fix16_t in) {
		LuaFix16 v;
		v.value = in;
		return v;
	}

	//explicit operator fix16_t() const { return value;                 }
	LUAFIX16_FN_ATTR explicit operator double()  const { return fix16_to_dbl(value);   }
	LUAFIX16_FN_ATTR explicit operator float()   const { return fix16_to_float(value); }
	
	template <typename Integer> requires std::signed_integral<Integer>
	LUAFIX16_FN_ATTR explicit operator Integer() const { return (Integer)fix16_to_int(value); }
	
	template <typename UInteger> requires std::unsigned_integral<UInteger>
	LUAFIX16_FN_ATTR explicit operator UInteger() const { return (UInteger)(uint16_t)fix16_to_int(value); }

	LUAFIX16_FN_ATTR LuaFix16 & operator=(const LuaFix16 &rhs) = default;
	LUAFIX16_FN_ATTR LuaFix16 & operator=(LuaFix16 &&rhs) = default;
	//LuaFix16 & operator=(const fix16_t rhs) { value = rhs;                   return *this; }
	LUAFIX16_FN_ATTR LuaFix16 & operator=(const double rhs)  { value = fix16_from_dbl(rhs);   return *this; }
	LUAFIX16_FN_ATTR LuaFix16 & operator=(const float rhs)   { value = fix16_from_float(rhs); return *this; }
	
	template <typename Integer, std::enable_if_t<std::is_integral<Integer>::value, bool> = true>
	LUAFIX16_FN_ATTR LuaFix16 & operator=(const Integer rhs) { value = fix16_from_int(rhs);   return *this; }

	LUAFIX16_FN_ATTR LuaFix16 & operator+=(const LuaFix16 &rhs)  { value = fix16_add(value, rhs.value); return *this; }
	LUAFIX16_FN_ATTR LuaFix16 & operator-=(const LuaFix16 &rhs)  { value = fix16_sub(value, rhs.value); return *this; }
	LUAFIX16_FN_ATTR LuaFix16 & operator*=(const LuaFix16 &rhs)  { value = fix16_mul(value, rhs.value); return *this; }
	LUAFIX16_FN_ATTR LuaFix16 & operator%=(const LuaFix16 &rhs)  { value = fix16_mod(value, rhs.value); return *this; }
	LUAFIX16_FN_ATTR LuaFix16 & operator/=(const LuaFix16 &rhs)  { value = fix16_div(value, rhs.value); return *this; }

	LUAFIX16_FN_ATTR const LuaFix16 operator+(const LuaFix16 &other) const  { LuaFix16 ret = *this; ret += other; return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 saturating_add(const LuaFix16 &other) const {
		LuaFix16 ret = *this;
		int64_t wide_sum = int64_t(value) + int64_t(other.value);

		using SignedLimits = std::numeric_limits<int32_t>;

		ret.value = int32_t(wide_sum > SignedLimits::max() ? SignedLimits::max()
		                  : wide_sum < SignedLimits::min() ? SignedLimits::min()
		                  : wide_sum);

		return ret;
	}
	LUAFIX16_FN_ATTR const LuaFix16 operator-() const { return LuaFix16(0) - *this; }
	LUAFIX16_FN_ATTR const LuaFix16 operator-(const LuaFix16 &other) const  { LuaFix16 ret = *this; ret -= other; return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 operator*(const LuaFix16 &other) const  { LuaFix16 ret = *this; ret *= other; return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 operator/(const LuaFix16 &other) const  { LuaFix16 ret = *this; ret /= other; return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 operator%(const LuaFix16 &other) const  { LuaFix16 ret = *this; ret %= other; return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 int_division(const LuaFix16 &other) const {
		if (other.value == 0) {
			return LuaFix16((value >= 0) ? 32767 : -32768);
		}

		// numidiv here is correct as the division cancels out the multiplication factor
		// of a and b, and the output type is int
		return LuaFix16(this->value / other.value);
	}

	LUAFIX16_FN_ATTR auto operator<=>(const LuaFix16& other) const = default;

	LUAFIX16_FN_ATTR const LuaFix16 operator|(const LuaFix16 &other) const { LuaFix16 ret = *this; ret.value |= other.value; return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 operator&(const LuaFix16 &other) const { LuaFix16 ret = *this; ret.value &= other.value; return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 operator^(const LuaFix16 &other) const { LuaFix16 ret = *this; ret.value ^= other.value; return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 operator~() const { LuaFix16 ret = *this; ret.value = ~ret.value; return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 operator<<(const LuaFix16 &other) const {
		const int shift_amount = int(other);
		LuaFix16 ret = *this;
		ret.value = (
			shift_amount >= 0
			? ret.value << shift_amount
			: int(unsigned(ret.value) >> unsigned(-shift_amount))
		);
		return ret;
	}
	LUAFIX16_FN_ATTR const LuaFix16 unsigned_right_shift(const LuaFix16 &other) const {
		const int shift_amount = int(other);
		LuaFix16 ret = *this;
		ret.value = (
			shift_amount >= 0
			? int(unsigned(ret.value) >> unsigned(shift_amount))
			: ret.value << -shift_amount
		);
		return ret;
	}
	LUAFIX16_FN_ATTR const LuaFix16 operator>>(const LuaFix16 &other) const {
		const int shift_amount = int(other);
		LuaFix16 ret = *this;
		ret.value = (
			shift_amount >= 0
			? ret.value >> shift_amount
			: ret.value << -shift_amount
		);
		return ret;
	}
	LUAFIX16_FN_ATTR const LuaFix16 rotate_left(const LuaFix16 &other) const { LuaFix16 ret = *this; ret.value = int32_t(std::rotl(uint32_t(ret.value), int(other))); return ret; }
	LUAFIX16_FN_ATTR const LuaFix16 rotate_right(const LuaFix16 &other) const { LuaFix16 ret = *this; ret.value = int32_t(std::rotr(uint32_t(ret.value), int(other))); return ret; }

	// FIXME: method vs public function should be normalized into one thing

	LUAFIX16_FN_ATTR LuaFix16  sin() const { return LuaFix16::from_fix16(fix16_sin(value));  }
	LUAFIX16_FN_ATTR LuaFix16  cos() const { return LuaFix16::from_fix16(fix16_cos(value));  }
	LUAFIX16_FN_ATTR LuaFix16  tan() const { return LuaFix16::from_fix16(fix16_tan(value));  }
	LUAFIX16_FN_ATTR LuaFix16 asin() const { return LuaFix16::from_fix16(fix16_asin(value)); }
	LUAFIX16_FN_ATTR LuaFix16 acos() const { return LuaFix16::from_fix16(fix16_acos(value)); }
	LUAFIX16_FN_ATTR LuaFix16 atan() const { return LuaFix16::from_fix16(fix16_atan(value)); }
	LUAFIX16_FN_ATTR LuaFix16 atan2(const LuaFix16 &inY) const { return LuaFix16::from_fix16(fix16_atan2(value, inY.value)); }
	LUAFIX16_FN_ATTR LuaFix16 sqrt() const { return LuaFix16::from_fix16(fix16_sqrt(value)); }

	LUAFIX16_FN_ATTR uint16_t unsigned_integral_bits() const { return uint32_t(value) >> 16; }
	LUAFIX16_FN_ATTR int16_t signed_integral_bits() const { return int16_t(value >> 16); }
	LUAFIX16_FN_ATTR uint16_t decimal_bits() const { return uint16_t(value & 0xFFFF); }
};

inline LuaFix16 fabs(LuaFix16 x) { return LuaFix16::from_fix16(fix16_abs(x.value)); }
inline LuaFix16 sin(LuaFix16 x) { return x.sin(); }
inline LuaFix16 floor(LuaFix16 x) { return LuaFix16::from_fix16(fix16_floor(x.value)); }
inline LuaFix16 ceil(LuaFix16 x) { return LuaFix16::from_fix16(fix16_ceil(x.value)); }
inline LuaFix16 exp(LuaFix16 x) { return LuaFix16::from_fix16(fix16_exp(x.value)); }
inline LuaFix16 log(LuaFix16 x) { return LuaFix16::from_fix16(fix16_log(x.value)); }
inline LuaFix16 pow(LuaFix16 x, LuaFix16 y) {
	// hacky, not fully correct approximation to pow that accepts negative x
	auto ret = exp(log(fabs(x)) * y);
	ret.value = (x < 0 && y < 0) ? -ret.value : ret.value; // restore sign
	return ret;
}
inline LuaFix16 ldexp(LuaFix16 x, int exp) { return x * pow(LuaFix16(2), exp); }

constexpr LuaFix16 operator ""_raw_fix16(unsigned long long x) {
	return LuaFix16::from_fix16(fix16_t(uint32_t(x)));
}