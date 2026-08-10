/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>

#include <zephyr/data/json.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

/**
 * @file
 * @brief Struct-to-JSON mapping over Zephyr's JSON library.
 *
 * Zephyr's `json_obj_parse()` and `json_obj_encode_buf()` are driven by an array
 * of `json_obj_descr`, normally built with C macros that require the caller to
 * name a `JSON_TOK_*` for every field. This layer builds those descriptors from
 * the C++ member types instead, so a schema names fields and nothing else:
 *
 * ```cpp
 * struct Reading {
 *         std::int32_t millivolts;
 *         std::int32_t centi_celsius;
 *         bool charging;
 *         char label[16];
 * };
 *
 * ZEST_JSON_SCHEMA(Reading,
 *                  ZEST_JSON_FIELD(Reading, millivolts, "mv"),
 *                  ZEST_JSON_FIELD(Reading, centi_celsius, "cc"),
 *                  ZEST_JSON_MEMBER(Reading, charging),
 *                  ZEST_JSON_MEMBER(Reading, label));
 * ```
 *
 * Then encoding and parsing are one call each:
 *
 * ```cpp
 * std::array<char, 128> buffer{};
 * ZEST_TRY_ASSIGN(text, zest::json::encode(reading, buffer));
 * ZEST_TRY(client.publish("sensor/1", text));
 *
 * ZEST_TRY_ASSIGN(parsed, zest::json::parse<Reading>(body));
 * if (parsed.has("label")) { ... }
 * ```
 *
 * What the C++ layer adds over the C macros:
 *
 * - Tokens are deduced from the member type, so `JSON_TOK_*` never appears in
 *   application code and a mismatched token cannot silently corrupt a value.
 * - A `char[N]` member maps to `JSON_TOK_STRING_BUF`, which copies. Zephyr
 *   supports that token but publishes no macro for it, so from C the only easy
 *   choice is the borrowing `char *` form.
 * - Nested objects and arrays compose from the members' own schemas.
 * - Failures are `Result<T>` rather than a bitmap that must be distinguished
 *   from a negative errno by sign.
 * - Structural limits --- 64 KB offsets, 127-character names, 64 fields --- are
 *   `static_assert`s instead of silent truncation into a bitfield.
 *
 * ### Constraints inherited from Zephyr
 *
 * These are properties of the underlying library, not of this wrapper:
 *
 * - **Parsing mutates the buffer.** `json_obj_parse()` writes NUL terminators
 *   in place. The buffer must be writable and must outlive any parsed value that
 *   borrows from it.
 * - **`char *` fields borrow.** A `const char *` member points into the parsed
 *   buffer. Use `char[N]` when the value must outlive it.
 * - **Every field is always encoded.** Zephyr's encoder walks the whole
 *   descriptor array, so there is no way to omit an absent field. Encode a
 *   sentinel, or split the schema.
 * - **Members must be C-compatible.** `std::string_view`, `std::optional` and
 *   `std::span` cannot be described. Use `char[N]`, a sentinel, and a
 *   count-plus-array pair respectively.
 *
 * Requires `CONFIG_JSON_LIBRARY=y`, and `CONFIG_JSON_LIBRARY_FP_SUPPORT=y` for
 * `float` or `double` members.
 */

namespace zest::json
{

/**
 * The JSON schema for a type.
 *
 * Specialize with `ZEST_JSON_SCHEMA`. The primary template is deliberately
 * undefined, so a type without a schema fails to compile with a clear
 * unspecialized-template error rather than a deep instantiation trace.
 */
template <typename T> struct Schema;

/** A type that has a JSON schema. */
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
			      "no JSON mapping for this member type. Zephyr's descriptors "
			      "describe C-compatible members only: use char[N] instead of "
			      "std::string_view, a sentinel instead of std::optional, and an "
			      "array plus a size_t count instead of std::span. A nested struct "
			      "needs its own ZEST_JSON_SCHEMA, declared first.");
		return JSON_TOK_NONE;
	}
}

/*
 * Declared and never defined. Reaching one of these in a constant expression is
 * the diagnostic: the build fails naming the function, which is what an
 * exception-free consteval function has instead of `throw`.
 */
const char *json_member_offset_exceeds_65535();
const char *json_field_name_must_be_1_to_127_characters();
const char *json_array_count_offset_exceeds_65535();

/** Common structural checks shared by every field kind. */
consteval void validate_field(std::size_t offset, std::size_t name_length) noexcept
{
	/* Zephyr packs the offset into 16 bits and the name length into 7. */
	if (offset > 0xFFFFU) {
		json_member_offset_exceeds_65535();
	}
	if (name_length == 0U || name_length > 127U) {
		json_field_name_must_be_1_to_127_characters();
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
	static_assert(Traits::is_array, "ZEST_JSON_ARRAY needs a C array or std::array member");
	static_assert(std::is_same_v<std::remove_cv_t<CountMember>, std::size_t>,
		      "the count member paired with a JSON array must be std::size_t, "
		      "which is the width Zephyr writes the decoded length through");

	using Element = typename Traits::element_type;
	validate_field(offset, name_length);
	if (CountOffset > 0xFFFFU) {
		json_array_count_offset_exceeds_65535();
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

/**
 * Parse a JSON object into @p T, in place.
 *
 * @p json must be writable: Zephyr terminates tokens by writing NULs into it.
 * Any `char *` member of the result points into @p json, so the buffer must
 * outlive the value. A `char[N]` member holds a copy and does not.
 */
template <Serializable T> [[nodiscard]] Result<Parsed<T>> parse(std::span<char> json) noexcept
{
	static_assert(Schema<T>::count <= 64U,
		      "Zephyr reports field presence in a 64-bit bitmap, so a schema is "
		      "limited to 64 fields; nest a sub-object to go further");

	if (json.empty()) {
		return fail(errors::invalid_argument);
	}

	Parsed<T> parsed{};
	const std::int64_t decoded = json_obj_parse(
		json.data(), json.size(), Schema<T>::descriptors, Schema<T>::count, &parsed.value);
	if (decoded < 0) {
		return fail(static_cast<int>(decoded));
	}
	parsed.present = static_cast<std::uint64_t>(decoded);
	return parsed;
}

/** Parse from a byte buffer, such as an HTTP response body. */
template <Serializable T> [[nodiscard]] Result<Parsed<T>> parse(std::span<std::byte> json) noexcept
{
	return parse<T>(std::span<char>{reinterpret_cast<char *>(json.data()), json.size()});
}

/**
 * Encode @p value as a JSON object into @p buffer.
 *
 * Returns a view of the encoded text, which is NUL-terminated within @p buffer.
 * Every field in the schema is emitted; Zephyr's encoder has no way to omit one.
 */
template <Serializable T>
[[nodiscard]] Result<std::string_view> encode(const T &value, std::span<char> buffer) noexcept
{
	if (buffer.empty()) {
		return fail(errors::invalid_argument);
	}

	const int rc = json_obj_encode_buf(Schema<T>::descriptors, Schema<T>::count, &value,
					   buffer.data(), buffer.size());
	if (rc < 0) {
		/* Zephyr reports a short buffer as -ENOMEM; say so in Zest's vocabulary. */
		return fail(rc == -ENOMEM ? errors::no_buffer_space : Error{rc});
	}
	return std::string_view{buffer.data()};
}

/** Encode into a byte buffer, for `HttpClient::post()` and similar. */
template <Serializable T>
[[nodiscard]] Result<std::span<const std::byte>> encode(const T &value,
							std::span<std::byte> buffer) noexcept
{
	ZEST_TRY_ASSIGN(text, encode(value, std::span<char>{reinterpret_cast<char *>(buffer.data()),
							    buffer.size()}));
	return std::as_bytes(std::span{text.data(), text.size()});
}

/**
 * Bytes an encoding of @p value would occupy, excluding the NUL.
 *
 * Useful for sizing a buffer before committing to one, or for rejecting a
 * payload that cannot fit before the encode attempt.
 */
template <Serializable T> [[nodiscard]] Result<std::size_t> encoded_size(const T &value) noexcept
{
	const ssize_t size =
		json_calc_encoded_len(Schema<T>::descriptors, Schema<T>::count, &value);
	if (size < 0) {
		return fail(static_cast<int>(size));
	}
	return static_cast<std::size_t>(size);
}

/**
 * Parse a top-level JSON array, such as `[{...},{...}]`.
 *
 * @p T must be a wrapper whose schema holds exactly one array field --- the shape
 * Zephyr's array API requires:
 *
 * ```cpp
 * struct Batch {
 *         Reading readings[8];
 *         std::size_t readings_count;
 * };
 * ZEST_JSON_SCHEMA(Batch, ZEST_JSON_ARRAY(Batch, readings, readings_count));
 * ```
 */
template <Serializable T> [[nodiscard]] Result<T> parse_array(std::span<char> json) noexcept
{
	static_assert(Schema<T>::count == 1U,
		      "a top-level JSON array maps to a wrapper struct whose schema has "
		      "exactly one array field");
	if (json.empty()) {
		return fail(errors::invalid_argument);
	}

	T value{};
	ZEST_TRY(check(json_arr_parse(json.data(), json.size(), Schema<T>::descriptors, &value)));
	return value;
}

template <Serializable T> [[nodiscard]] Result<T> parse_array(std::span<std::byte> json) noexcept
{
	return parse_array<T>(std::span<char>{reinterpret_cast<char *>(json.data()), json.size()});
}

/** Encode a wrapper's single array field as a top-level JSON array. */
template <Serializable T>
[[nodiscard]] Result<std::string_view> encode_array(const T &value, std::span<char> buffer) noexcept
{
	static_assert(Schema<T>::count == 1U,
		      "a top-level JSON array maps to a wrapper struct whose schema has "
		      "exactly one array field");
	if (buffer.empty()) {
		return fail(errors::invalid_argument);
	}

	const int rc =
		json_arr_encode_buf(Schema<T>::descriptors, &value, buffer.data(), buffer.size());
	if (rc < 0) {
		return fail(rc == -ENOMEM ? errors::no_buffer_space : Error{rc});
	}
	return std::string_view{buffer.data()};
}

/**
 * Escape @p text in place so it is safe as a JSON string value.
 *
 * Wraps Zephyr's `json_escape()`. @p length is updated to the escaped length.
 * Needed only when composing JSON by hand; `encode()` escapes for itself.
 */
[[nodiscard]] inline Result<std::string_view> escape(std::span<char> buffer,
						     std::size_t length) noexcept
{
	if (length > buffer.size()) {
		return fail(errors::invalid_argument);
	}
	std::size_t escaped = length;
	const ssize_t rc = json_escape(buffer.data(), &escaped, buffer.size());
	if (rc < 0) {
		return fail(static_cast<int>(rc));
	}
	return std::string_view{buffer.data(), escaped};
}

} /* namespace zest::json */

/**
 * Describe a member, using a custom JSON name.
 *
 * The token is deduced from the member's type. Use inside `ZEST_JSON_SCHEMA`.
 */
#define ZEST_JSON_FIELD(Type, member, name)                                                        \
	::zest::json::detail::describe<Type, decltype(Type::member)>(                              \
		(name), sizeof(name) - 1U, offsetof(Type, member), sizeof(Type::member))

/** Describe a member, using the C++ member name as the JSON name. */
#define ZEST_JSON_MEMBER(Type, member) ZEST_JSON_FIELD(Type, member, #member)

/**
 * Describe an array member paired with its `std::size_t` count member.
 *
 * Zephyr learns the decoded length through a sibling count field, so the two are
 * declared together:
 *
 * ```cpp
 * struct Batch {
 *         std::int32_t samples[16];
 *         std::size_t samples_count;
 * };
 * ZEST_JSON_SCHEMA(Batch, ZEST_JSON_ARRAY_FIELD(Batch, samples, samples_count, "samples"));
 * ```
 */
#define ZEST_JSON_ARRAY_FIELD(Type, member, count_member, name)                                    \
	::zest::json::detail::describe_array<Type, decltype(Type::member),                         \
					     decltype(Type::count_member),                         \
					     offsetof(Type, count_member)>(                        \
		(name), sizeof(name) - 1U, offsetof(Type, member))

/** Describe an array member, using the C++ member name as the JSON name. */
#define ZEST_JSON_ARRAY(Type, member, count_member)                                                \
	ZEST_JSON_ARRAY_FIELD(Type, member, count_member, #member)

/**
 * Declare the JSON schema for @p Type.
 *
 * Must appear at global scope, after @p Type and after the schemas of any nested
 * types it refers to.
 */
#define ZEST_JSON_SCHEMA(Type, ...)                                                                \
	namespace zest::json                                                                       \
	{                                                                                          \
	template <> struct Schema<Type> {                                                          \
		using type = Type;                                                                 \
		static constexpr ::json_obj_descr descriptors[] = {__VA_ARGS__};                   \
		static constexpr std::size_t count = sizeof(descriptors) / sizeof(descriptors[0]); \
		static_assert(count <= 64U, "a JSON schema is limited to 64 fields");              \
	};                                                                                         \
	} /* namespace zest::json */
