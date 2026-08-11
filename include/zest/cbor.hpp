/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <zest/error.hpp>
#include <zest/schema.hpp>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

/**
 * @file
 * @brief The CBOR codec for `zest::Schema`.
 *
 * The same schema that drives JSON drives this, so switching a payload between
 * the two formats changes neither the struct nor its schema. Encoding and
 * decoding are delegated to zcbor, the CBOR library Zephyr already ships and uses
 * for MCUmgr, LwM2M SenML-CBOR and CoAP.
 *
 * CBOR is usually the better choice where both ends are yours: roughly half the
 * bytes of the equivalent JSON, no text parsing, and none of the JSON codec's
 * constraints. In particular:
 *
 * - **Decoding does not modify the input**, so the payload may be `const` and a
 *   received buffer can be decoded more than once.
 * - **Text is length-prefixed**, so nothing depends on NUL termination.
 *
 * One asymmetry is worth knowing. A `const char *` member can be *encoded* --- the
 * string is copied into the payload --- but cannot be *decoded*, because CBOR text
 * is not NUL-terminated in the buffer and pointing a C string at it would run past
 * the value into whatever follows. Decoding into `char[N]`, which copies, is the
 * supported form and is what a schema should use if it is decoded at all.
 *
 * Requires `CONFIG_ZCBOR=y`.
 */

namespace zest::cbor
{

namespace detail
{

/** Nesting budget for zcbor's backup states: objects and arrays within a map. */
inline constexpr std::size_t kMaxNesting = 8U;

/** Length of a fixed-width text member, stopping at the first NUL. */
[[nodiscard]] inline std::size_t buffer_length(const char *text, std::size_t capacity) noexcept
{
	std::size_t length = 0U;
	while (length < capacity && text[length] != '\0') {
		++length;
	}
	return length;
}

[[nodiscard]] inline zcbor_string as_zcbor_string(const char *text, std::size_t length) noexcept
{
	return zcbor_string{reinterpret_cast<const std::uint8_t *>(text), length};
}

/**
 * Whether a schema contains an array of objects, at any depth.
 *
 * Zephyr's descriptor records an object array's element *descriptor* but not its
 * element *stride* --- Zephyr recomputes that internally from member alignments.
 * Rather than replicate that arithmetic and risk striding through the array
 * incorrectly, the CBOR codec rejects the shape at compile time. The JSON codec
 * still supports it, because Zephyr does the walking there.
 */
consteval bool has_object_array(const json_obj_descr *descriptors, std::size_t count) noexcept
{
	for (std::size_t i = 0; i < count; ++i) {
		const auto type = static_cast<json_tokens>(descriptors[i].type);
		if (type == JSON_TOK_ARRAY_START &&
		    static_cast<json_tokens>(descriptors[i].array.element_descr->type) ==
			    JSON_TOK_OBJECT_START) {
			return true;
		}
		if (type == JSON_TOK_OBJECT_START &&
		    has_object_array(descriptors[i].object.sub_descr,
				     descriptors[i].object.sub_descr_len)) {
			return true;
		}
	}
	return false;
}

bool encode_fields(zcbor_state_t *state, const json_obj_descr *descriptors, std::size_t count,
		   const void *value) noexcept;

/** Encode one field's value, dispatching on the descriptor's declared type. */
inline bool encode_value(zcbor_state_t *state, const json_obj_descr &descriptor,
			 const void *field) noexcept
{
	switch (static_cast<json_tokens>(descriptor.type)) {
	case JSON_TOK_TRUE:
	case JSON_TOK_FALSE:
		return zcbor_bool_put(state, *static_cast<const bool *>(field));
	case JSON_TOK_INT:
	case JSON_TOK_NUMBER:
		return zcbor_int_encode(state, field, descriptor.field.size);
	case JSON_TOK_UINT:
		return zcbor_uint_encode(state, field, descriptor.field.size);
	case JSON_TOK_INT64:
		return zcbor_int64_put(state, *static_cast<const std::int64_t *>(field));
	case JSON_TOK_UINT64:
		return zcbor_uint64_put(state, *static_cast<const std::uint64_t *>(field));
	case JSON_TOK_FLOAT_FP:
		return zcbor_float32_put(state, *static_cast<const float *>(field));
	case JSON_TOK_DOUBLE_FP:
		return zcbor_float64_put(state, *static_cast<const double *>(field));
	case JSON_TOK_STRING: {
		const char *text = *static_cast<const char *const *>(field);
		if (text == nullptr) {
			return zcbor_nil_put(state, nullptr);
		}
		const auto string = as_zcbor_string(text, std::strlen(text));
		return zcbor_tstr_encode(state, &string);
	}
	case JSON_TOK_STRING_BUF: {
		const char *text = static_cast<const char *>(field);
		const auto string =
			as_zcbor_string(text, buffer_length(text, descriptor.field.size));
		return zcbor_tstr_encode(state, &string);
	}
	case JSON_TOK_OBJECT_START:
		return encode_fields(state, descriptor.object.sub_descr,
				     descriptor.object.sub_descr_len, field);
	case JSON_TOK_ARRAY_START: {
		const auto &element = *descriptor.array.element_descr;
		/* The element descriptor's offset addresses the sibling count field. */
		const auto *base = static_cast<const std::byte *>(field) - descriptor.offset;
		const auto count = *reinterpret_cast<const std::size_t *>(base + element.offset);
		const std::size_t capped =
			count > descriptor.array.n_elements ? descriptor.array.n_elements : count;

		if (!zcbor_list_start_encode(state, capped)) {
			return false;
		}
		for (std::size_t i = 0; i < capped; ++i) {
			const void *item =
				static_cast<const std::byte *>(field) + i * element.field.size;
			if (!encode_value(state, element, item)) {
				return false;
			}
		}
		return zcbor_list_end_encode(state, capped);
	}
	default:
		break;
	}
	return false;
}

inline bool encode_fields(zcbor_state_t *state, const json_obj_descr *descriptors,
			  std::size_t count, const void *value) noexcept
{
	if (!zcbor_map_start_encode(state, count)) {
		return false;
	}
	for (std::size_t i = 0; i < count; ++i) {
		const auto &descriptor = descriptors[i];
		const auto key = as_zcbor_string(descriptor.field_name, descriptor.field_name_len);
		if (!zcbor_tstr_encode(state, &key)) {
			return false;
		}
		const void *field = static_cast<const std::byte *>(value) + descriptor.offset;
		if (!encode_value(state, descriptor, field)) {
			return false;
		}
	}
	return zcbor_map_end_encode(state, count);
}

bool decode_fields(zcbor_state_t *state, const json_obj_descr *descriptors, std::size_t count,
		   void *value, std::uint64_t &present) noexcept;

/** Decode one field's value into @p field. */
inline bool decode_value(zcbor_state_t *state, const json_obj_descr &descriptor,
			 void *field) noexcept
{
	switch (static_cast<json_tokens>(descriptor.type)) {
	case JSON_TOK_TRUE:
	case JSON_TOK_FALSE:
		return zcbor_bool_decode(state, static_cast<bool *>(field));
	case JSON_TOK_INT:
	case JSON_TOK_NUMBER:
		return zcbor_int_decode(state, field, descriptor.field.size);
	case JSON_TOK_UINT:
		return zcbor_uint_decode(state, field, descriptor.field.size);
	case JSON_TOK_INT64:
		return zcbor_int64_decode(state, static_cast<std::int64_t *>(field));
	case JSON_TOK_UINT64:
		return zcbor_uint64_decode(state, static_cast<std::uint64_t *>(field));
	case JSON_TOK_FLOAT_FP:
		return zcbor_float32_decode(state, static_cast<float *>(field));
	case JSON_TOK_DOUBLE_FP:
		return zcbor_float64_decode(state, static_cast<double *>(field));
	case JSON_TOK_STRING_BUF: {
		zcbor_string text{};
		if (!zcbor_tstr_decode(state, &text)) {
			return false;
		}
		/* Leave room for the terminator the C string form needs. */
		if (text.len >= descriptor.field.size) {
			return false;
		}
		auto *destination = static_cast<char *>(field);
		std::memcpy(destination, text.value, text.len);
		destination[text.len] = '\0';
		return true;
	}
	case JSON_TOK_STRING:
		/*
		 * CBOR text is length-prefixed, not terminated, so a char* member
		 * would have no way to know where the value ends. Refuse rather than
		 * hand back a pointer that reads into the following item.
		 */
		return false;
	case JSON_TOK_OBJECT_START: {
		std::uint64_t nested = 0U;
		return decode_fields(state, descriptor.object.sub_descr,
				     descriptor.object.sub_descr_len, field, nested);
	}
	case JSON_TOK_ARRAY_START: {
		const auto &element = *descriptor.array.element_descr;
		if (!zcbor_list_start_decode(state)) {
			return false;
		}
		std::size_t decoded = 0U;
		while (!zcbor_array_at_end(state)) {
			if (decoded >= descriptor.array.n_elements) {
				return false;
			}
			void *item = static_cast<std::byte *>(field) + decoded * element.field.size;
			if (!decode_value(state, element, item)) {
				return false;
			}
			++decoded;
		}
		if (!zcbor_list_end_decode(state)) {
			return false;
		}
		/* Report the length through the sibling count field. */
		*reinterpret_cast<std::size_t *>(static_cast<std::byte *>(field) -
						 descriptor.offset + element.offset) = decoded;
		return true;
	}
	default:
		break;
	}
	return false;
}

inline bool decode_fields(zcbor_state_t *state, const json_obj_descr *descriptors,
			  std::size_t count, void *value, std::uint64_t &present) noexcept
{
	if (!zcbor_map_start_decode(state)) {
		return false;
	}

	while (!zcbor_array_at_end(state)) {
		zcbor_string key{};
		if (!zcbor_tstr_decode(state, &key)) {
			return false;
		}

		std::size_t index = count;
		for (std::size_t i = 0; i < count; ++i) {
			if (descriptors[i].field_name_len == key.len &&
			    std::memcmp(descriptors[i].field_name, key.value, key.len) == 0) {
				index = i;
				break;
			}
		}

		if (index == count) {
			/* Unknown key: step over its value so a sender may add fields. */
			if (!zcbor_any_skip(state, nullptr)) {
				return false;
			}
			continue;
		}

		void *field = static_cast<std::byte *>(value) + descriptors[index].offset;
		if (!decode_value(state, descriptors[index], field)) {
			return false;
		}
		present |= std::uint64_t{1} << index;
	}

	return zcbor_map_end_decode(state);
}

} /* namespace detail */

/**
 * Encode @p value as a CBOR map into @p buffer.
 *
 * Returns a view of the encoded bytes, ready for `MqttClient::publish()` or
 * `HttpClient::post()`.
 */
template <Serializable T>
[[nodiscard]] Result<std::span<const std::byte>> encode(const T &value,
							std::span<std::byte> buffer) noexcept
{
	static_assert(!detail::has_object_array(Schema<T>::descriptors, Schema<T>::count),
		      "the CBOR codec does not support arrays of objects: Zephyr's descriptor "
		      "records the element descriptor but not the element stride. Use an array "
		      "of primitives, or the JSON codec, which lets Zephyr do the walking.");

	if (buffer.empty()) {
		return fail(errors::invalid_argument);
	}

	ZCBOR_STATE_E(state, detail::kMaxNesting, reinterpret_cast<std::uint8_t *>(buffer.data()),
		      buffer.size(), 1);

	if (!detail::encode_fields(state, Schema<T>::descriptors, Schema<T>::count, &value)) {
		/* zcbor reports a full buffer and a malformed schema separately. */
		return fail(zcbor_peek_error(state) == ZCBOR_ERR_NO_PAYLOAD
				    ? errors::no_buffer_space
				    : errors::invalid_argument);
	}

	const auto written = static_cast<std::size_t>(
		state->payload - reinterpret_cast<const std::uint8_t *>(buffer.data()));
	return std::span<const std::byte>{buffer.data(), written};
}

/**
 * Decode a CBOR map into @p T.
 *
 * The payload is not modified, so it may be `const` and may be decoded again.
 * Keys with no matching field are skipped.
 */
template <Serializable T>
[[nodiscard]] Result<Parsed<T>> parse(std::span<const std::byte> payload) noexcept
{
	static_assert(Schema<T>::count <= 64U,
		      "field presence is reported in a 64-bit bitmap, so a schema is limited "
		      "to 64 fields; nest a sub-object to go further");
	static_assert(!detail::has_object_array(Schema<T>::descriptors, Schema<T>::count),
		      "the CBOR codec does not support arrays of objects: Zephyr's descriptor "
		      "records the element descriptor but not the element stride. Use an array "
		      "of primitives, or the JSON codec, which lets Zephyr do the walking.");

	if (payload.empty()) {
		return fail(errors::invalid_argument);
	}

	ZCBOR_STATE_D(state, detail::kMaxNesting,
		      reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size(), 1, 0);

	Parsed<T> parsed{};
	if (!detail::decode_fields(state, Schema<T>::descriptors, Schema<T>::count, &parsed.value,
				   parsed.present)) {
		return fail(errors::bad_message);
	}
	return parsed;
}

/** Decode from a mutable buffer, for symmetry with the JSON codec. */
template <Serializable T>
[[nodiscard]] Result<Parsed<T>> parse(std::span<std::byte> payload) noexcept
{
	return parse<T>(std::span<const std::byte>{payload});
}

} /* namespace zest::cbor */
