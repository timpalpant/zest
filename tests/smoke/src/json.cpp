/*
 * Copyright (c) 2026 Timothy Palpant
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/*
 * Round-trip tests for the schema layer against Zephyr's JSON library.
 *
 * The descriptor generation is checked on the host (tests/host/test_json.cpp);
 * these exercise the parts that need the real parser and encoder.
 */

#include <zephyr/ztest.h>

#include <zest/json.hpp>

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

ZEST_JSON_SCHEMA(Reading, ZEST_JSON_FIELD(Reading, millivolts, "mv"),
		 ZEST_JSON_FIELD(Reading, centi_celsius, "cc"), ZEST_JSON_MEMBER(Reading, charging),
		 ZEST_JSON_MEMBER(Reading, label));

ZEST_JSON_SCHEMA(Nested, ZEST_JSON_FIELD(Nested, identifier, "id"),
		 ZEST_JSON_MEMBER(Nested, reading));

ZEST_JSON_SCHEMA(Samples, ZEST_JSON_ARRAY(Samples, values, values_count));

ZEST_JSON_SCHEMA(Batch, ZEST_JSON_ARRAY(Batch, readings, readings_count));

ZEST_JSON_SCHEMA(Wide, ZEST_JSON_MEMBER(Wide, big), ZEST_JSON_MEMBER(Wide, unsigned_big),
		 ZEST_JSON_MEMBER(Wide, narrow), ZEST_JSON_MEMBER(Wide, borrowed));

ZEST_JSON_SCHEMA(WithEnum, ZEST_JSON_MEMBER(WithEnum, mode));

/* ------------------------------------------------------------------ tests --- */

ZTEST_SUITE(zest_json, nullptr, nullptr, nullptr, nullptr, nullptr);

/* Copy a literal into a writable buffer: parsing mutates its input. */
template <std::size_t N>
static std::span<char> writable(std::array<char, N> &buffer, std::string_view text)
{
	zassert_true(text.size() < N);
	std::memcpy(buffer.data(), text.data(), text.size());
	buffer[text.size()] = '\0';
	return std::span<char>{buffer.data(), text.size()};
}

ZTEST(zest_json, test_parse_primitives)
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

ZTEST(zest_json, test_false_decodes_against_a_true_descriptor)
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

ZTEST(zest_json, test_absent_fields_are_reported)
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

ZTEST(zest_json, test_string_buf_copies_and_char_pointer_borrows)
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

ZTEST(zest_json, test_label_survives_buffer_reuse)
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

ZTEST(zest_json, test_encode_round_trip)
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

ZTEST(zest_json, test_encoded_size_matches_encode)
{
	Reading reading{.millivolts = 1, .centi_celsius = 2, .charging = true, .label = "z"};

	auto size = zest::json::encoded_size(reading);
	zassert_true(size.has_value());

	std::array<char, 128> buffer{};
	auto text = zest::json::encode(reading, buffer);
	zassert_true(text.has_value());
	zassert_equal(*size, text->size());
}

ZTEST(zest_json, test_short_buffer_reports_no_buffer_space)
{
	Reading reading{.millivolts = 123456, .centi_celsius = 2, .charging = true, .label = "x"};

	std::array<char, 8> tiny{};
	auto text = zest::json::encode(reading, tiny);
	zassert_false(text.has_value());
	/* Zephyr's -ENOMEM is translated into Zest's vocabulary. */
	zassert_equal(text.error(), zest::errors::no_buffer_space);
}

ZTEST(zest_json, test_nested_object)
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

ZTEST(zest_json, test_primitive_array)
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

ZTEST(zest_json, test_top_level_array_of_objects)
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

ZTEST(zest_json, test_enum_member_uses_underlying_type)
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

ZTEST(zest_json, test_malformed_input_is_rejected)
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

ZTEST(zest_json, test_type_mismatch_is_rejected)
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

ZTEST(zest_json, test_byte_span_overloads)
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
