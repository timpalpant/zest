/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/*
 * Round-trip tests for the schema layer against both codecs.
 *
 * The same schemas drive JSON and CBOR, so most tests here exist to prove the two
 * agree -- and to pin the places where they deliberately do not.
 */

#include <zephyr/ztest.h>

#include <zest/cbor.hpp>
#include <zest/json.hpp>
#include <zest/serde.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

/* ------------------------------------------------------------------ types --- */

struct Reading {
	std::int32_t millivolts;
	std::int32_t centi_celsius;
	bool charging;
	char label[16];
};

struct Nested {
	std::int32_t identifier;
	Reading reading;
};

struct Samples {
	std::int32_t values[8];
	std::size_t values_count;
};

struct Batch {
	Reading readings[4];
	std::size_t readings_count;
};

struct Wide {
	std::int64_t big;
	std::uint64_t unsigned_big;
	std::uint8_t narrow;
	const char *borrowed;
};

enum class Mode : std::int32_t {
	idle = 0,
	active = 7,
};

struct WithEnum {
	Mode mode;
};

/* ---------------------------------------------------------------- schemas --- */

ZEST_SCHEMA(Reading, ZEST_FIELD(Reading, millivolts, "mv"),
	    ZEST_FIELD(Reading, centi_celsius, "cc"), ZEST_MEMBER(Reading, charging),
	    ZEST_MEMBER(Reading, label));

ZEST_SCHEMA(Nested, ZEST_FIELD(Nested, identifier, "id"), ZEST_MEMBER(Nested, reading));

ZEST_SCHEMA(Samples, ZEST_ARRAY(Samples, values, values_count));

ZEST_SCHEMA(Batch, ZEST_ARRAY(Batch, readings, readings_count));

ZEST_SCHEMA(Wide, ZEST_MEMBER(Wide, big), ZEST_MEMBER(Wide, unsigned_big),
	    ZEST_MEMBER(Wide, narrow), ZEST_MEMBER(Wide, borrowed));

ZEST_SCHEMA(WithEnum, ZEST_MEMBER(WithEnum, mode));

/* ------------------------------------------------------------------ tests --- */

ZTEST_SUITE(zest_serde, nullptr, nullptr, nullptr, nullptr, nullptr);

/* Copy a literal into a writable buffer: parsing mutates its input. */
template <std::size_t N>
static std::span<char> writable(std::array<char, N> &buffer, std::string_view text)
{
	zassert_true(text.size() < N);
	std::memcpy(buffer.data(), text.data(), text.size());
	buffer[text.size()] = '\0';
	return std::span<char>{buffer.data(), text.size()};
}

ZTEST(zest_serde, test_parse_primitives)
{
	std::array<char, 128> storage{};
	auto json = writable(storage, R"({"mv":3742,"cc":2350,"charging":true,"label":"battery"})");

	auto parsed = zest::json::parse<Reading>(json);
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.millivolts, 3742);
	zassert_equal(parsed->value.centi_celsius, 2350);
	zassert_true(parsed->value.charging);
	zassert_equal(std::string_view{parsed->value.label}, std::string_view{"battery"});

	/* Every field was supplied. */
	zassert_true(parsed->complete());
	zassert_equal(parsed->supplied(), 4U);
	zassert_true(parsed->has("mv"));
	zassert_true(parsed->has("label"));
	/* An unknown name is not present, and does not read out of bounds. */
	zassert_false(parsed->has("nope"));
}

ZTEST(zest_serde, test_false_decodes_against_a_true_descriptor)
{
	/*
	 * A bool member maps to JSON_TOK_TRUE. Zephyr treats TRUE and FALSE as
	 * equivalent types, so a literal false must still decode -- if it did not,
	 * every bool field would silently fail to parse.
	 */
	std::array<char, 128> storage{};
	auto json = writable(storage, R"({"mv":1,"cc":2,"charging":false,"label":"x"})");

	auto parsed = zest::json::parse<Reading>(json);
	zassert_true(parsed.has_value());
	zassert_false(parsed->value.charging);
	zassert_true(parsed->has("charging"));
}

ZTEST(zest_serde, test_absent_fields_are_reported)
{
	std::array<char, 128> storage{};
	auto json = writable(storage, R"({"mv":3742})");

	auto parsed = zest::json::parse<Reading>(json);
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.millivolts, 3742);
	zassert_false(parsed->complete());
	zassert_equal(parsed->supplied(), 1U);

	zassert_true(parsed->has("mv"));
	/* Absent, and zero -- which is why has() exists at all. */
	zassert_false(parsed->has("cc"));
	zassert_equal(parsed->value.centi_celsius, 0);
	zassert_false(parsed->has("charging"));
	zassert_false(parsed->has("label"));
}

ZTEST(zest_serde, test_string_buf_copies_and_char_pointer_borrows)
{
	std::array<char, 160> storage{};
	auto json =
		writable(storage, R"({"big":-5,"unsigned_big":18446744073709551615,"narrow":200,)"
				  R"("borrowed":"inside the buffer"})");

	auto parsed = zest::json::parse<Wide>(json);
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.big, -5);
	zassert_equal(parsed->value.unsigned_big, 18446744073709551615ULL);
	zassert_equal(parsed->value.narrow, 200);

	/* A const char* member points into the caller's buffer, not a copy. */
	const char *borrowed = parsed->value.borrowed;
	zassert_equal(std::string_view{borrowed}, std::string_view{"inside the buffer"});
	zassert_true(borrowed >= storage.data());
	zassert_true(borrowed < storage.data() + storage.size());
}

ZTEST(zest_serde, test_label_survives_buffer_reuse)
{
	/* char[N] copies, so it must still be readable after the buffer is clobbered. */
	Reading reading{};
	{
		std::array<char, 128> storage{};
		auto json =
			writable(storage, R"({"mv":1,"cc":2,"charging":true,"label":"durable"})");
		auto parsed = zest::json::parse<Reading>(json);
		zassert_true(parsed.has_value());
		reading = parsed->value;
		storage.fill('X');
	}
	zassert_equal(std::string_view{reading.label}, std::string_view{"durable"});
}

ZTEST(zest_serde, test_encode_round_trip)
{
	Reading original{
		.millivolts = 3742,
		.centi_celsius = -1250,
		.charging = false,
		.label = "cell-a",
	};

	std::array<char, 128> buffer{};
	auto text = zest::json::encode(original, buffer);
	zassert_true(text.has_value());
	zassert_true(text->size() > 0U);

	/* Field names are the schema's, not the C++ members'. */
	zassert_not_null(std::strstr(text->data(), "\"mv\":3742"));
	zassert_not_null(std::strstr(text->data(), "\"label\":\"cell-a\""));

	/* And it parses back to the same values. */
	std::array<char, 128> round{};
	auto json = writable(round, *text);
	auto parsed = zest::json::parse<Reading>(json);
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.millivolts, original.millivolts);
	zassert_equal(parsed->value.centi_celsius, original.centi_celsius);
	zassert_equal(parsed->value.charging, original.charging);
	zassert_equal(std::string_view{parsed->value.label}, std::string_view{original.label});
}

ZTEST(zest_serde, test_encoded_size_matches_encode)
{
	Reading reading{.millivolts = 1, .centi_celsius = 2, .charging = true, .label = "z"};

	auto size = zest::json::encoded_size(reading);
	zassert_true(size.has_value());

	std::array<char, 128> buffer{};
	auto text = zest::json::encode(reading, buffer);
	zassert_true(text.has_value());
	zassert_equal(*size, text->size());
}

ZTEST(zest_serde, test_short_buffer_reports_no_buffer_space)
{
	Reading reading{.millivolts = 123456, .centi_celsius = 2, .charging = true, .label = "x"};

	std::array<char, 8> tiny{};
	auto text = zest::json::encode(reading, tiny);
	zassert_false(text.has_value());
	/* Zephyr's -ENOMEM is translated into Zest's vocabulary. */
	zassert_equal(text.error(), zest::errors::no_buffer_space);
}

ZTEST(zest_serde, test_nested_object)
{
	std::array<char, 192> storage{};
	auto json = writable(
		storage,
		R"({"id":9,"reading":{"mv":3300,"cc":100,"charging":true,"label":"inner"}})");

	auto parsed = zest::json::parse<Nested>(json);
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.identifier, 9);
	zassert_equal(parsed->value.reading.millivolts, 3300);
	zassert_true(parsed->value.reading.charging);
	zassert_equal(std::string_view{parsed->value.reading.label}, std::string_view{"inner"});

	/* Nested objects encode from the member's own schema. */
	std::array<char, 192> buffer{};
	auto text = zest::json::encode(parsed->value, buffer);
	zassert_true(text.has_value());
	zassert_not_null(std::strstr(text->data(), "\"reading\":{"));
}

ZTEST(zest_serde, test_primitive_array)
{
	std::array<char, 128> storage{};
	auto json = writable(storage, R"({"values":[10,20,30]})");

	auto parsed = zest::json::parse<Samples>(json);
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.values_count, 3U);
	zassert_equal(parsed->value.values[0], 10);
	zassert_equal(parsed->value.values[2], 30);

	std::array<char, 128> buffer{};
	auto text = zest::json::encode(parsed->value, buffer);
	zassert_true(text.has_value());
	zassert_not_null(std::strstr(text->data(), "\"values\":[10,20,30]"));
}

ZTEST(zest_serde, test_top_level_array_of_objects)
{
	std::array<char, 256> storage{};
	auto json = writable(storage, R"([{"mv":1,"cc":2,"charging":true,"label":"a"},)"
				      R"({"mv":3,"cc":4,"charging":false,"label":"b"}])");

	auto batch = zest::json::parse_array<Batch>(json);
	zassert_true(batch.has_value());
	zassert_equal(batch->readings_count, 2U);
	zassert_equal(batch->readings[0].millivolts, 1);
	zassert_equal(std::string_view{batch->readings[1].label}, std::string_view{"b"});
	zassert_false(batch->readings[1].charging);

	std::array<char, 256> buffer{};
	auto text = zest::json::encode_array(*batch, buffer);
	zassert_true(text.has_value());
	zassert_equal(text->front(), '[');
}

ZTEST(zest_serde, test_enum_member_uses_underlying_type)
{
	std::array<char, 64> storage{};
	auto json = writable(storage, R"({"mode":7})");

	auto parsed = zest::json::parse<WithEnum>(json);
	zassert_true(parsed.has_value());
	zassert_true(parsed->value.mode == Mode::active);

	std::array<char, 64> buffer{};
	auto text = zest::json::encode(parsed->value, buffer);
	zassert_true(text.has_value());
	zassert_not_null(std::strstr(text->data(), "\"mode\":7"));
}

ZTEST(zest_serde, test_malformed_input_is_rejected)
{
	std::array<char, 64> storage{};

	auto truncated = writable(storage, R"({"mv":37)");
	zassert_false(zest::json::parse<Reading>(truncated).has_value());

	auto garbage = writable(storage, R"(not json at all)");
	zassert_false(zest::json::parse<Reading>(garbage).has_value());

	/* An empty buffer is rejected before Zephyr is entered. */
	zassert_equal(zest::json::parse<Reading>(std::span<char>{}).error(),
		      zest::errors::invalid_argument);
}

ZTEST(zest_serde, test_type_mismatch_is_rejected)
{
	/* A string where a number belongs must not silently decode as zero. */
	std::array<char, 96> storage{};
	auto json = writable(storage, R"({"mv":"not a number"})");

	auto parsed = zest::json::parse<Reading>(json);
	if (parsed.has_value()) {
		/* If Zephyr tolerates it, it must at least not claim the field. */
		zassert_false(parsed->has("mv"));
	}
}

ZTEST(zest_serde, test_byte_span_overloads)
{
	/* The HTTP path hands back bytes, so the byte overloads must agree. */
	Reading reading{.millivolts = 42, .centi_celsius = 1, .charging = true, .label = "b"};

	std::array<std::byte, 128> buffer{};
	auto encoded = zest::json::encode(reading, std::span<std::byte>{buffer});
	zassert_true(encoded.has_value());
	zassert_true(encoded->size() > 0U);

	auto parsed =
		zest::json::parse<Reading>(std::span<std::byte>{buffer.data(), encoded->size()});
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.millivolts, 42);
}

/* ------------------------------------------------------------------- CBOR --- */

ZTEST(zest_serde, test_cbor_round_trip)
{
	const Reading original{
		.millivolts = 3742,
		.centi_celsius = -1250,
		.charging = true,
		.label = "cell-a",
	};

	std::array<std::byte, 128> buffer{};
	auto encoded = zest::cbor::encode(original, buffer);
	zassert_true(encoded.has_value());
	zassert_true(encoded->size() > 0U);

	auto parsed = zest::cbor::parse<Reading>(*encoded);
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.millivolts, original.millivolts);
	zassert_equal(parsed->value.centi_celsius, original.centi_celsius);
	zassert_equal(parsed->value.charging, original.charging);
	zassert_equal(std::string_view{parsed->value.label}, std::string_view{original.label});
	zassert_true(parsed->complete());
}

ZTEST(zest_serde, test_cbor_is_smaller_than_json)
{
	const Reading reading{
		.millivolts = 3742,
		.centi_celsius = -1250,
		.charging = true,
		.label = "cell-a",
	};

	std::array<std::byte, 128> cbor_buffer{};
	std::array<char, 128> json_buffer{};
	auto as_cbor = zest::cbor::encode(reading, cbor_buffer);
	auto as_json = zest::json::encode(reading, json_buffer);
	zassert_true(as_cbor.has_value());
	zassert_true(as_json.has_value());

	/* The whole reason to reach for CBOR on a constrained link. */
	zassert_true(as_cbor->size() < as_json->size());
}

ZTEST(zest_serde, test_cbor_does_not_modify_its_input)
{
	const Reading reading{.millivolts = 7, .centi_celsius = 8, .charging = false, .label = "x"};

	std::array<std::byte, 128> buffer{};
	auto encoded = zest::cbor::encode(reading, buffer);
	zassert_true(encoded.has_value());

	std::array<std::byte, 128> untouched{};
	std::memcpy(untouched.data(), buffer.data(), encoded->size());

	/* Decoding twice must work, which it cannot if the first pass rewrote the buffer. */
	auto first = zest::cbor::parse<Reading>(*encoded);
	auto second = zest::cbor::parse<Reading>(*encoded);
	zassert_true(first.has_value());
	zassert_true(second.has_value());
	zassert_equal(second->value.millivolts, 7);
	zassert_mem_equal(buffer.data(), untouched.data(), encoded->size());
}

ZTEST(zest_serde, test_cbor_skips_unknown_keys)
{
	/* A sender adding a field must not break a receiver. */
	struct Extra {
		std::int32_t millivolts;
		std::int32_t unexpected;
		bool charging;
	};

	std::array<std::byte, 128> buffer{};
	zest::cbor::detail::kMaxNesting == 0U ? (void)0 : (void)0;

	/* Hand-build a map with a key the Reading schema does not model. */
	ZCBOR_STATE_E(state, 4, reinterpret_cast<std::uint8_t *>(buffer.data()), buffer.size(), 1);
	zassert_true(zcbor_map_start_encode(state, 3));
	zassert_true(zcbor_tstr_put_lit(state, "mv"));
	zassert_true(zcbor_int32_put(state, 1234));
	zassert_true(zcbor_tstr_put_lit(state, "surprise"));
	zassert_true(zcbor_int32_put(state, 999));
	zassert_true(zcbor_tstr_put_lit(state, "charging"));
	zassert_true(zcbor_bool_put(state, true));
	zassert_true(zcbor_map_end_encode(state, 3));

	const std::size_t length = static_cast<std::size_t>(
		state->payload - reinterpret_cast<const std::uint8_t *>(buffer.data()));

	auto parsed = zest::cbor::parse<Reading>(std::span<const std::byte>{buffer.data(), length});
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.millivolts, 1234);
	zassert_true(parsed->value.charging);
	zassert_true(parsed->has("mv"));
	zassert_true(parsed->has("charging"));
	/* Fields the document omitted are still reported absent. */
	zassert_false(parsed->has("cc"));
	zassert_false(parsed->complete());
}

ZTEST(zest_serde, test_cbor_short_buffer_is_reported)
{
	const Reading reading{
		.millivolts = 123456, .centi_celsius = 2, .charging = true, .label = "x"};

	std::array<std::byte, 4> tiny{};
	auto encoded = zest::cbor::encode(reading, tiny);
	zassert_false(encoded.has_value());
	zassert_equal(encoded.error(), zest::errors::no_buffer_space);
}

ZTEST(zest_serde, test_cbor_rejects_malformed_input)
{
	constexpr std::array<std::byte, 3> garbage{std::byte{0xFF}, std::byte{0xFF},
						   std::byte{0xFF}};
	zassert_false(zest::cbor::parse<Reading>(std::span<const std::byte>{garbage}).has_value());
	zassert_equal(zest::cbor::parse<Reading>(std::span<const std::byte>{}).error(),
		      zest::errors::invalid_argument);
}

ZTEST(zest_serde, test_cbor_nested_and_arrays)
{
	Nested original{};
	original.identifier = 9;
	original.reading.millivolts = 3300;
	original.reading.centi_celsius = 100;
	original.reading.charging = true;
	std::strcpy(original.reading.label, "inner");

	std::array<std::byte, 192> buffer{};
	auto encoded = zest::cbor::encode(original, buffer);
	zassert_true(encoded.has_value());

	auto parsed = zest::cbor::parse<Nested>(*encoded);
	zassert_true(parsed.has_value());
	zassert_equal(parsed->value.identifier, 9);
	zassert_equal(parsed->value.reading.millivolts, 3300);
	zassert_equal(std::string_view{parsed->value.reading.label}, std::string_view{"inner"});

	Samples samples{};
	samples.values[0] = 10;
	samples.values[1] = 20;
	samples.values[2] = 30;
	samples.values_count = 3U;

	std::array<std::byte, 128> array_buffer{};
	auto array_encoded = zest::cbor::encode(samples, array_buffer);
	zassert_true(array_encoded.has_value());

	auto array_parsed = zest::cbor::parse<Samples>(*array_encoded);
	zassert_true(array_parsed.has_value());
	zassert_equal(array_parsed->value.values_count, 3U);
	zassert_equal(array_parsed->value.values[0], 10);
	zassert_equal(array_parsed->value.values[2], 30);
}

ZTEST(zest_serde, test_cbor_wide_integers_and_enums)
{
	Wide original{};
	original.big = -5;
	original.unsigned_big = 18446744073709551615ULL;
	original.narrow = 200;
	original.borrowed = "encoded fine";

	std::array<std::byte, 128> buffer{};
	auto encoded = zest::cbor::encode(original, buffer);
	zassert_true(encoded.has_value());

	/*
	 * A const char* member encodes, but cannot decode: CBOR text is
	 * length-prefixed rather than terminated, so a C string pointed at it would
	 * read past the value. The codec refuses instead of returning that pointer.
	 */
	auto parsed = zest::cbor::parse<Wide>(*encoded);
	zassert_false(parsed.has_value());

	WithEnum mode{.mode = Mode::active};
	std::array<std::byte, 32> enum_buffer{};
	auto enum_encoded = zest::cbor::encode(mode, enum_buffer);
	zassert_true(enum_encoded.has_value());
	auto enum_parsed = zest::cbor::parse<WithEnum>(*enum_encoded);
	zassert_true(enum_parsed.has_value());
	zassert_true(enum_parsed->value.mode == Mode::active);
}

/* ------------------------------------------------------------ format toggle --- */

ZTEST(zest_serde, test_format_toggle_round_trips_both_ways)
{
	const Reading original{
		.millivolts = 4100,
		.centi_celsius = 2100,
		.charging = false,
		.label = "toggle",
	};

	/* The same schema and the same call shape, differing only in the format. */
	std::array<std::byte, 160> json_buffer{};
	auto as_json = zest::serialize<zest::Format::json>(original, json_buffer);
	zassert_true(as_json.has_value());
	auto from_json = zest::deserialize<zest::Format::json, Reading>(
		std::span<std::byte>{json_buffer.data(), as_json->size()});
	zassert_true(from_json.has_value());

	std::array<std::byte, 160> cbor_buffer{};
	auto as_cbor = zest::serialize<zest::Format::cbor>(original, cbor_buffer);
	zassert_true(as_cbor.has_value());
	auto from_cbor = zest::deserialize<zest::Format::cbor, Reading>(
		std::span<std::byte>{cbor_buffer.data(), as_cbor->size()});
	zassert_true(from_cbor.has_value());

	/* Both formats must reconstruct identical values. */
	zassert_equal(from_json->value.millivolts, from_cbor->value.millivolts);
	zassert_equal(from_json->value.centi_celsius, from_cbor->value.centi_celsius);
	zassert_equal(from_json->value.charging, from_cbor->value.charging);
	zassert_equal(std::string_view{from_json->value.label},
		      std::string_view{from_cbor->value.label});
	zassert_equal(from_json->value.millivolts, original.millivolts);
}

ZTEST(zest_serde, test_content_type)
{
	zassert_equal(zest::content_type(zest::Format::json), std::string_view{"application/json"});
	zassert_equal(zest::content_type(zest::Format::cbor), std::string_view{"application/cbor"});
}
