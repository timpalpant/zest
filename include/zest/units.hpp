/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <compare>
#include <cstdint>
#include <ratio>
#include <type_traits>

namespace zest
{

/**
 * A physical quantity carrying its unit and scale in the type.
 *
 * A representation, a tag saying *what* is measured, and a `std::ratio` saying at
 * what scale --- the same shape as `std::chrono::duration`. Mixing millivolts with
 * volts, or degrees with thousandths, stops compiling rather than producing a
 * reading that is wrong by a factor of a thousand.
 *
 * Conversions that cannot lose information are implicit; the rest need an
 * explicit `quantity_cast`, exactly as with `std::chrono`:
 *
 * ```cpp
 * zest::Millivolts reading{3742};
 * zest::Microvolts fine = reading;                          // implicit, exact
 * auto coarse = zest::quantity_cast<zest::Volts>(reading);  // explicit, truncates
 * ```
 *
 * The representation is whatever the caller chooses, so nothing here forces
 * floating point onto a part without an FPU.
 */
template <typename Rep, typename Tag, typename Ratio = std::ratio<1>> class Quantity
{
      public:
	using rep = Rep;
	using tag = Tag;
	using ratio = Ratio;

	static_assert(std::is_arithmetic_v<Rep>, "a quantity needs an arithmetic representation");
	static_assert(Ratio::num > 0, "a quantity scale must be positive");

	constexpr Quantity() noexcept = default;

	/** Wrap a raw count. Explicit, so a bare number never becomes a quantity. */
	constexpr explicit Quantity(Rep count) noexcept : count_{count}
	{
	}

	/**
	 * Convert from the same tag at a finer or equal scale.
	 *
	 * Only available when the conversion is exact for integral representations,
	 * mirroring `std::chrono::duration`'s rule.
	 */
	template <typename OtherRep, typename OtherRatio>
		requires(std::ratio_divide<OtherRatio, Ratio>::den == 1 ||
			 std::is_floating_point_v<Rep>)
	constexpr Quantity(const Quantity<OtherRep, Tag, OtherRatio> &other) noexcept
		: count_{convert_count<OtherRep, OtherRatio>(other.count())}
	{
	}

	[[nodiscard]] constexpr Rep count() const noexcept
	{
		return count_;
	}

	constexpr Quantity &operator+=(const Quantity &other) noexcept
	{
		count_ += other.count_;
		return *this;
	}
	constexpr Quantity &operator-=(const Quantity &other) noexcept
	{
		count_ -= other.count_;
		return *this;
	}
	constexpr Quantity &operator*=(Rep scalar) noexcept
	{
		count_ *= scalar;
		return *this;
	}
	constexpr Quantity &operator/=(Rep scalar) noexcept
	{
		count_ /= scalar;
		return *this;
	}

	[[nodiscard]] constexpr Quantity operator-() const noexcept
	{
		return Quantity{static_cast<Rep>(-count_)};
	}
	[[nodiscard]] constexpr Quantity operator+() const noexcept
	{
		return *this;
	}

	[[nodiscard]] friend constexpr Quantity operator+(Quantity lhs, Quantity rhs) noexcept
	{
		return Quantity{static_cast<Rep>(lhs.count_ + rhs.count_)};
	}
	[[nodiscard]] friend constexpr Quantity operator-(Quantity lhs, Quantity rhs) noexcept
	{
		return Quantity{static_cast<Rep>(lhs.count_ - rhs.count_)};
	}
	[[nodiscard]] friend constexpr Quantity operator*(Quantity value, Rep scalar) noexcept
	{
		return Quantity{static_cast<Rep>(value.count_ * scalar)};
	}
	[[nodiscard]] friend constexpr Quantity operator*(Rep scalar, Quantity value) noexcept
	{
		return value * scalar;
	}
	[[nodiscard]] friend constexpr Quantity operator/(Quantity value, Rep scalar) noexcept
	{
		return Quantity{static_cast<Rep>(value.count_ / scalar)};
	}
	/** Dividing like by like yields a dimensionless ratio. */
	[[nodiscard]] friend constexpr Rep operator/(Quantity lhs, Quantity rhs) noexcept
	{
		return static_cast<Rep>(lhs.count_ / rhs.count_);
	}

	[[nodiscard]] friend constexpr bool operator==(Quantity, Quantity) noexcept = default;
	[[nodiscard]] friend constexpr auto operator<=>(Quantity, Quantity) noexcept = default;

      private:
	template <typename OtherRep, typename OtherRatio>
	[[nodiscard]] static constexpr Rep convert_count(OtherRep value) noexcept
	{
		using Factor = std::ratio_divide<OtherRatio, Ratio>;
		if constexpr (std::is_floating_point_v<Rep>) {
			return static_cast<Rep>(static_cast<Rep>(value) *
						static_cast<Rep>(Factor::num) /
						static_cast<Rep>(Factor::den));
		} else {
			return static_cast<Rep>(static_cast<std::int64_t>(value) * Factor::num /
						Factor::den);
		}
	}

	Rep count_{};
};

/** Convert between scales of the same quantity, truncating toward zero. */
template <typename To, typename Rep, typename Tag, typename Ratio>
	requires std::is_same_v<typename To::tag, Tag>
[[nodiscard]] constexpr To quantity_cast(const Quantity<Rep, Tag, Ratio> &from) noexcept
{
	using Factor = std::ratio_divide<Ratio, typename To::ratio>;
	using ToRep = typename To::rep;
	if constexpr (std::is_floating_point_v<ToRep>) {
		return To{static_cast<ToRep>(static_cast<ToRep>(from.count()) *
					     static_cast<ToRep>(Factor::num) /
					     static_cast<ToRep>(Factor::den))};
	} else {
		return To{static_cast<ToRep>(static_cast<std::int64_t>(from.count()) * Factor::num /
					     Factor::den)};
	}
}

/** Tags naming what is being measured. */
namespace tags
{
struct Voltage {
};
struct Current {
};
struct Resistance {
};
struct Power {
};
struct Charge {
};
struct Temperature {
};
struct Pressure {
};
struct Frequency {
};
} /* namespace tags */

/* Voltage. The base scale is the volt; readings are usually millivolts. */
using Microvolts = Quantity<std::int32_t, tags::Voltage, std::micro>;
using Millivolts = Quantity<std::int32_t, tags::Voltage, std::milli>;
using Volts = Quantity<std::int32_t, tags::Voltage>;
using VoltsF = Quantity<float, tags::Voltage>;

/* Current. */
using Microamps = Quantity<std::int32_t, tags::Current, std::micro>;
using Milliamps = Quantity<std::int32_t, tags::Current, std::milli>;
using Amps = Quantity<std::int32_t, tags::Current>;

/* Resistance, as devicetree divider nodes report it. */
using Milliohms = Quantity<std::int32_t, tags::Resistance, std::milli>;
using Ohms = Quantity<std::int32_t, tags::Resistance>;
using Kilohms = Quantity<std::int32_t, tags::Resistance, std::kilo>;

/* Power. */
using Microwatts = Quantity<std::int32_t, tags::Power, std::micro>;
using Milliwatts = Quantity<std::int32_t, tags::Power, std::milli>;
using Watts = Quantity<std::int32_t, tags::Power>;

/* Battery charge. */
using MilliampHours = Quantity<std::int32_t, tags::Charge, std::milli>;

/**
 * Temperature.
 *
 * These are Celsius readings on a fixed scale, which is what sensors report.
 * Subtracting two of them yields a difference on the same scale; the type does
 * not model the affine distinction between a reading and a delta, so treat a
 * sum of two absolute temperatures as the meaningless value it is.
 */
using MilliCelsius = Quantity<std::int32_t, tags::Temperature, std::milli>;
using DeciCelsius = Quantity<std::int32_t, tags::Temperature, std::deci>;
using Celsius = Quantity<std::int32_t, tags::Temperature>;
using CelsiusF = Quantity<float, tags::Temperature>;

/* Pressure, at the scales barometric sensors use. */
using Pascals = Quantity<std::int32_t, tags::Pressure>;
using Hectopascals = Quantity<std::int32_t, tags::Pressure, std::hecto>;
using Kilopascals = Quantity<std::int32_t, tags::Pressure, std::kilo>;

/* Frequency. */
using Hertz = Quantity<std::uint32_t, tags::Frequency>;
using Kilohertz = Quantity<std::uint32_t, tags::Frequency, std::kilo>;

/**
 * Reconstruct the input voltage of a resistive divider.
 *
 * `full` is the total series resistance and `measured` the part the ADC sees, as
 * the devicetree `voltage-divider` binding reports them. Evaluated in 64-bit
 * integer arithmetic, so no FPU is involved. The result carries the same scale as
 * the measurement, so a microvolt read reconstructs to microvolts and a millivolt
 * read to millivolts --- the caller then converts between the two with the usual
 * implicit / `quantity_cast` rules rather than the divider rounding to a single
 * unit for everyone.
 */
template <typename Output> requires std::is_same_v<typename Output::tag, tags::Voltage>
[[nodiscard]] constexpr Output divider_input(Output output, Ohms measured,
					     Ohms full) noexcept
{
	if (measured.count() <= 0) {
		return Output{0};
	}
	return Output{static_cast<typename Output::rep>(
		static_cast<std::int64_t>(output.count()) * full.count() / measured.count())};
}

/** User-defined literals for the common sensing scales. */
namespace literals
{

[[nodiscard]] constexpr Microvolts operator""_uV(unsigned long long value) noexcept
{
	return Microvolts{static_cast<std::int32_t>(value)};
}
[[nodiscard]] constexpr Millivolts operator""_mV(unsigned long long value) noexcept
{
	return Millivolts{static_cast<std::int32_t>(value)};
}
[[nodiscard]] constexpr Volts operator""_V(unsigned long long value) noexcept
{
	return Volts{static_cast<std::int32_t>(value)};
}
[[nodiscard]] constexpr Microamps operator""_uA(unsigned long long value) noexcept
{
	return Microamps{static_cast<std::int32_t>(value)};
}
[[nodiscard]] constexpr Milliamps operator""_mA(unsigned long long value) noexcept
{
	return Milliamps{static_cast<std::int32_t>(value)};
}
[[nodiscard]] constexpr Ohms operator""_ohm(unsigned long long value) noexcept
{
	return Ohms{static_cast<std::int32_t>(value)};
}
[[nodiscard]] constexpr Kilohms operator""_kohm(unsigned long long value) noexcept
{
	return Kilohms{static_cast<std::int32_t>(value)};
}
[[nodiscard]] constexpr MilliCelsius operator""_mdegC(unsigned long long value) noexcept
{
	return MilliCelsius{static_cast<std::int32_t>(value)};
}
[[nodiscard]] constexpr Celsius operator""_degC(unsigned long long value) noexcept
{
	return Celsius{static_cast<std::int32_t>(value)};
}
[[nodiscard]] constexpr Hertz operator""_Hz(unsigned long long value) noexcept
{
	return Hertz{static_cast<std::uint32_t>(value)};
}
[[nodiscard]] constexpr Kilohertz operator""_kHz(unsigned long long value) noexcept
{
	return Kilohertz{static_cast<std::uint32_t>(value)};
}

} /* namespace literals */

} /* namespace zest */
