/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

/*
 * The field table is Zephyr's `json_obj_descr`. Reusing it rather than inventing
 * a parallel type is what lets one schema drive both codecs: it already carries
 * every fact a serializer needs --- name, offset, width, kind, and sub-tables for
 * nested objects and arrays --- and it lets the JSON codec hand the table to
 * Zephyr unchanged. The header declares the struct without requiring
 * CONFIG_JSON_LIBRARY, so a CBOR-only image does not pull in the JSON library.
 */
#include <zephyr/data/json.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

/**
 * @file
 * @brief Declaring a struct's shape once, for every serialization format.
 *
 * A schema names fields; the wire representation of each is deduced from its C++
 * type. The same schema drives JSON (`zest/json.hpp`) and CBOR
 * (`zest/cbor.hpp`), so a payload can change format without the struct or the
 * schema changing:
 *
 * ```cpp
 * struct Reading {
 *         std::int32_t millivolts;
 *         bool charging;
 *         char label[16];
 * };
 *
 * ZEST_SCHEMA(Reading,
 *             ZEST_FIELD(Reading, millivolts, "mv"),
 *             ZEST_MEMBER(Reading, charging),
 *             ZEST_MEMBER(Reading, label));
 *
 * ZEST_TRY_ASSIGN(body, zest::serialize<zest::Format::cbor>(reading, buffer));
 * ```
 *
 * Deducing the representation from the member type is the point: a hand-written
 * token against the wrong width decodes incorrectly and silently, and a C macro
 * cannot check it.
 *
 * ### Supported member types
 *
 * | C++ member | Representation |
 * | --- | --- |
 * | `bool` | boolean |
 * | `int8/16/32_t`, `uint8/16/32_t` | integer, width taken from the member |
 * | `int64_t`, `uint64_t` | 64-bit integer |
 * | `float`, `double` | floating point |
 * | `char[N]`, `std::array<char, N>` | text, **copied** into the struct |
 * | `char *`, `const char *` | text, **borrowing** the input buffer |
 * | `enum` | its underlying integer type |
 * | a type with its own schema | nested object |
 * | array plus a `std::size_t` count | array |
 *
 * Members must be C-compatible: `std::string_view`, `std::optional` and
 * `std::span` cannot be described. Use `char[N]`, a sentinel, and an array with a
 * count.
 *
 * A field left out of the schema is not part of the mapping at all --- it is
 * never written, and an incoming key of that name is skipped. That is a
 * compile-time choice covering all values; there is no per-value omission.
 */

namespace zest
{

/**
 * The serialization schema for a type.
 *
 * Specialize with `ZEST_SCHEMA`. The primary template is deliberately undefined,
 * so a type without a schema fails with a clear unspecialized-template error
 * rather than a deep instantiation trace.
 */
template <typename T> struct Schema;

/** A type that has a schema, and so can be serialized. */
template <typename T>
concept Serializable = requires {
	{ Schema<T>::descriptors };
	{ Schema<T>::count } -> std::convertible_to<std::size_t>;
};

namespace detail
{

template <typename> inline constexpr bool always_false = false;

/** Zephyr stores the struct alignment as a shift, in two bits. */
template <typename T> consteval std::uint32_t align_shift() noexcept
{
	constexpr std::size_t alignment = alignof(T);
	static_assert(alignment == 1U || alignment == 2U || alignment == 4U || alignment == 8U,
		      "a JSON-mapped struct must have alignment 1, 2, 4 or 8");
	return alignment == 1U ? 0U : alignment == 2U ? 1U : alignment == 4U ? 2U : 3U;
}

/* Recognize both C arrays and std::array as array members. */
template <typename T> struct array_traits {
	static constexpr bool is_array = false;
};
template <typename T, std::size_t N> struct array_traits<T[N]> {
	static constexpr bool is_array = true;
	using element_type = T;
	static constexpr std::size_t extent = N;
};
template <typename T, std::size_t N> struct array_traits<std::array<T, N>> {
	static constexpr bool is_array = true;
	using element_type = T;
	static constexpr std::size_t extent = N;
};

/** True for a `char[N]` or `std::array<char, N>` member, which maps to STRING_BUF. */
template <typename T> consteval bool is_char_buffer() noexcept
{
	if constexpr (array_traits<T>::is_array) {
		return std::is_same_v<typename array_traits<T>::element_type, char>;
	}
	return false;
}

/**
 * Choose the Zephyr token for a member type.
 *
 * The mapping is symmetric: every token selected here is handled by both
 * Zephyr's decoder and its encoder, and a document `NUMBER` is accepted against
 * each numeric token.
 */
template <typename M> consteval json_tokens token_for() noexcept
{
	using T = std::remove_cv_t<M>;

	if constexpr (std::is_same_v<T, bool>) {
		/* TRUE and FALSE are interchangeable in the descriptor. */
		return JSON_TOK_TRUE;
	} else if constexpr (std::is_same_v<T, char *> || std::is_same_v<T, const char *>) {
		return JSON_TOK_STRING;
	} else if constexpr (is_char_buffer<T>()) {
		return JSON_TOK_STRING_BUF;
	} else if constexpr (std::is_same_v<T, float>) {
		return JSON_TOK_FLOAT_FP;
	} else if constexpr (std::is_same_v<T, double>) {
		return JSON_TOK_DOUBLE_FP;
	} else if constexpr (std::is_same_v<T, json_obj_token>) {
		return JSON_TOK_OPAQUE;
	} else if constexpr (std::is_enum_v<T>) {
		return token_for<std::underlying_type_t<T>>();
	} else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
		return sizeof(T) == 8U ? JSON_TOK_INT64 : JSON_TOK_INT;
	} else if constexpr (std::is_integral_v<T>) {
		return sizeof(T) == 8U ? JSON_TOK_UINT64 : JSON_TOK_UINT;
	} else if constexpr (Serializable<T>) {
		return JSON_TOK_OBJECT_START;
	} else {
		static_assert(always_false<T>,
			      "no serialization mapping for this member type. C-compatible "
			      "members only: use char[N] instead of "
			      "std::string_view, a sentinel instead of std::optional, and an "
			      "array plus a size_t count instead of std::span. A nested struct "
			      "needs its own ZEST_SCHEMA, declared first.");
		return JSON_TOK_NONE;
	}
}

/*
 * Declared and never defined. Reaching one of these in a constant expression is
 * the diagnostic: the build fails naming the function, which is what an
 * exception-free consteval function has instead of `throw`.
 */
const char *schema_member_offset_exceeds_65535();
const char *schema_field_name_must_be_1_to_127_characters();
const char *schema_array_count_offset_exceeds_65535();

/** Common structural checks shared by every field kind. */
consteval void validate_field(std::size_t offset, std::size_t name_length) noexcept
{
	/* Zephyr packs the offset into 16 bits and the name length into 7. */
	if (offset > 0xFFFFU) {
		schema_member_offset_exceeds_65535();
	}
	if (name_length == 0U || name_length > 127U) {
		schema_field_name_must_be_1_to_127_characters();
	}
}

/**
 * Build the descriptor for a primitive or nested-object member.
 *
 * The union arm is chosen by initializing it, not by assigning to it: activating
 * a union member through assignment to one of its subobjects is not permitted in
 * a constant expression.
 */
template <typename Owner, typename Member>
consteval json_obj_descr describe(const char *name, std::size_t name_length, std::size_t offset,
				  std::size_t size) noexcept
{
	validate_field(offset, name_length);

	if constexpr (Serializable<Member>) {
		return json_obj_descr{
			.field_name = name,
			.align_shift = align_shift<Owner>(),
			.field_name_len = static_cast<std::uint32_t>(name_length),
			.type = static_cast<std::uint32_t>(token_for<Member>()),
			.offset = static_cast<std::uint32_t>(offset),
			.object = {Schema<Member>::descriptors, Schema<Member>::count},
		};
	} else {
		return json_obj_descr{
			.field_name = name,
			.align_shift = align_shift<Owner>(),
			.field_name_len = static_cast<std::uint32_t>(name_length),
			.type = static_cast<std::uint32_t>(token_for<Member>()),
			.offset = static_cast<std::uint32_t>(offset),
			.field = {size},
		};
	}
}

/**
 * The element descriptor for an array member.
 *
 * Zephyr's C macro builds this with a compound literal, which is not valid C++,
 * so it lives here as a named `static constexpr` object instead. That also puts
 * it in read-only memory, which a runtime-initialized descriptor would not be.
 *
 * The element descriptor's `offset` addresses the *count* field, which is how
 * Zephyr learns how many elements were decoded.
 */
template <typename Owner, typename Element, std::size_t CountOffset, std::size_t ElementSize>
struct ArrayElement {
	static consteval json_obj_descr build() noexcept
	{
		if constexpr (Serializable<Element>) {
			return json_obj_descr{
				.field_name = nullptr,
				.align_shift = align_shift<Owner>(),
				.field_name_len = 0U,
				.type = static_cast<std::uint32_t>(JSON_TOK_OBJECT_START),
				.offset = static_cast<std::uint32_t>(CountOffset),
				.object = {Schema<Element>::descriptors, Schema<Element>::count},
			};
		} else {
			return json_obj_descr{
				.field_name = nullptr,
				.align_shift = align_shift<Owner>(),
				.field_name_len = 0U,
				.type = static_cast<std::uint32_t>(token_for<Element>()),
				.offset = static_cast<std::uint32_t>(CountOffset),
				.field = {ElementSize},
			};
		}
	}

	static constexpr json_obj_descr value = build();
};

/** Build the descriptor for an array member. */
template <typename Owner, typename ArrayMember, typename CountMember, std::size_t CountOffset>
consteval json_obj_descr describe_array(const char *name, std::size_t name_length,
					std::size_t offset) noexcept
{
	using Traits = array_traits<ArrayMember>;
	static_assert(Traits::is_array, "ZEST_ARRAY needs a C array or std::array member");
	static_assert(std::is_same_v<std::remove_cv_t<CountMember>, std::size_t>,
		      "the count member paired with a JSON array must be std::size_t, "
		      "which is the width Zephyr writes the decoded length through");

	using Element = typename Traits::element_type;
	validate_field(offset, name_length);
	if (CountOffset > 0xFFFFU) {
		schema_array_count_offset_exceeds_65535();
	}

	return json_obj_descr{
		.field_name = name,
		.align_shift = align_shift<Owner>(),
		.field_name_len = static_cast<std::uint32_t>(name_length),
		.type = static_cast<std::uint32_t>(JSON_TOK_ARRAY_START),
		.offset = static_cast<std::uint32_t>(offset),
		.array = {&ArrayElement<Owner, Element, CountOffset, sizeof(Element)>::value,
			  Traits::extent},
	};
}

} /* namespace detail */

/**
 * A parsed value, together with which of its fields the document supplied.
 *
 * Zephyr reports presence as a bitmap in declaration order. A field that was
 * absent keeps whatever the value-initialized struct held, which is
 * indistinguishable from a field that was present and zero --- so check `has()`
 * whenever "absent" and "zero" mean different things.
 */
template <Serializable T> struct Parsed {
	T value{};
	/** Bit *i* is set when the schema's *i*th field was decoded. */
	std::uint64_t present{};

	/** Whether the field at @p index in the schema was supplied. */
	[[nodiscard]] constexpr bool has(std::size_t index) const noexcept
	{
		return index < Schema<T>::count && (present & (std::uint64_t{1} << index)) != 0U;
	}

	/** Whether the named field was supplied. Unknown names are always false. */
	[[nodiscard]] constexpr bool has(std::string_view name) const noexcept
	{
		const auto index = index_of(name);
		return index < Schema<T>::count && has(index);
	}

	/** Whether every field in the schema was supplied. */
	[[nodiscard]] constexpr bool complete() const noexcept
	{
		const auto all = Schema<T>::count == 64U
					 ? ~std::uint64_t{0}
					 : (std::uint64_t{1} << Schema<T>::count) - 1U;
		return (present & all) == all;
	}

	/** Number of fields supplied. */
	[[nodiscard]] constexpr std::size_t supplied() const noexcept
	{
		std::size_t total = 0U;
		for (std::size_t i = 0; i < Schema<T>::count; ++i) {
			total += has(i) ? 1U : 0U;
		}
		return total;
	}

	/** Schema index of @p name, or `Schema<T>::count` when unknown. */
	[[nodiscard]] static constexpr std::size_t index_of(std::string_view name) noexcept
	{
		for (std::size_t i = 0; i < Schema<T>::count; ++i) {
			const auto &descriptor = Schema<T>::descriptors[i];
			if (std::string_view{descriptor.field_name, descriptor.field_name_len} ==
			    name) {
				return i;
			}
		}
		return Schema<T>::count;
	}

	[[nodiscard]] constexpr const T &operator*() const noexcept
	{
		return value;
	}
	[[nodiscard]] constexpr const T *operator->() const noexcept
	{
		return &value;
	}
};

} /* namespace zest */

/**
 * Describe a member, using a custom name on the wire.
 *
 * The representation is deduced from the member's type. Use inside `ZEST_SCHEMA`.
 */
#define ZEST_FIELD(Type, member, name)                                                             \
	::zest::detail::describe<Type, decltype(Type::member)>(                                    \
		(name), sizeof(name) - 1U, offsetof(Type, member), sizeof(Type::member))

/** Describe a member, using the C++ member name on the wire. */
#define ZEST_MEMBER(Type, member) ZEST_FIELD(Type, member, #member)

/**
 * Describe an array member paired with its `std::size_t` count member.
 *
 * The count is a sibling field, which is how the decoded length is reported:
 *
 * ```cpp
 * struct Batch {
 *         std::int32_t samples[16];
 *         std::size_t samples_count;
 * };
 * ZEST_SCHEMA(Batch, ZEST_ARRAY_FIELD(Batch, samples, samples_count, "samples"));
 * ```
 */
#define ZEST_ARRAY_FIELD(Type, member, count_member, name)                                         \
	::zest::detail::describe_array<Type, decltype(Type::member), decltype(Type::count_member), \
				       offsetof(Type, count_member)>((name), sizeof(name) - 1U,    \
								     offsetof(Type, member))

/** Describe an array member, using the C++ member name on the wire. */
#define ZEST_ARRAY(Type, member, count_member) ZEST_ARRAY_FIELD(Type, member, count_member, #member)

/**
 * Declare the schema for @p Type.
 *
 * Must appear at global scope, after @p Type and after the schemas of any nested
 * types it refers to.
 */
#define ZEST_SCHEMA(Type, ...)                                                                     \
	namespace zest                                                                             \
	{                                                                                          \
	template <> struct Schema<Type> {                                                          \
		using type = Type;                                                                 \
		static constexpr ::json_obj_descr descriptors[] = {__VA_ARGS__};                   \
		static constexpr std::size_t count = sizeof(descriptors) / sizeof(descriptors[0]); \
		static_assert(count <= 64U, "a schema is limited to 64 fields");                   \
	};                                                                                         \
	} /* namespace zest */
