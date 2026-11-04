#include <gtest/gtest.h>

#include <string>

#include "aster/core/cbor.h"

namespace aster {
namespace cbor {
namespace {

TEST(Cbor, EncodeDecodeScalars) {
  EXPECT_EQ(Decode(Encode(Value::Null()).value()).value(), Value::Null());
  EXPECT_EQ(Decode(Encode(Value::Bool(true)).value()).value(),
            Value::Bool(true));
  EXPECT_EQ(Decode(Encode(Value::Bool(false)).value()).value(),
            Value::Bool(false));
  EXPECT_EQ(Decode(Encode(Value::Int(0)).value()).value(), Value::Int(0));
  EXPECT_EQ(Decode(Encode(Value::Int(23)).value()).value(), Value::Int(23));
  EXPECT_EQ(Decode(Encode(Value::Int(24)).value()).value(), Value::Int(24));
  EXPECT_EQ(Decode(Encode(Value::Int(-1)).value()).value(), Value::Int(-1));
  EXPECT_EQ(Decode(Encode(Value::Int(-1000)).value()).value(),
            Value::Int(-1000));
  EXPECT_EQ(Decode(Encode(Value::Float(1.5)).value()).value(),
            Value::Float(1.5));
  EXPECT_EQ(Decode(Encode(Value::String("aster")).value()).value(),
            Value::String("aster"));
}

TEST(Cbor, KnownByteSequences) {
  // RFC 8949 examples.
  EXPECT_EQ(Encode(Value::Int(0)).value(), std::string("\x00", 1));
  EXPECT_EQ(Encode(Value::Int(1)).value(), std::string("\x01", 1));
  EXPECT_EQ(Encode(Value::Int(23)).value(), std::string("\x17", 1));
  EXPECT_EQ(Encode(Value::Int(24)).value(), std::string("\x18\x18", 2));
  EXPECT_EQ(Encode(Value::Bool(false)).value(), std::string("\xf4", 1));
  EXPECT_EQ(Encode(Value::Bool(true)).value(), std::string("\xf5", 1));
  EXPECT_EQ(Encode(Value::Null()).value(), std::string("\xf6", 1));
  EXPECT_EQ(Encode(Value::String("a")).value(), std::string("\x61\x61", 2));
  EXPECT_EQ(Encode(Value::FromArray({})).value(), std::string("\x80", 1));
  EXPECT_EQ(Encode(Value::FromMap({})).value(), std::string("\xa0", 1));
}

TEST(Cbor, NestedMapAndArrayRoundTrip) {
  Value::Map nested;
  nested.emplace("x", Value::Int(1));
  nested.emplace("y", Value::FromArray({Value::Bool(true), Value::Null(),
                                        Value::String("z")}));

  Value::Map root;
  root.emplace("id", Value::String("row-1"));
  root.emplace("score", Value::Float(0.25));
  root.emplace("tags",
               Value::FromArray({Value::String("a"), Value::String("b")}));
  root.emplace("attrs", Value::FromMap(std::move(nested)));
  root.emplace("active", Value::Bool(true));

  const Value original = Value::FromMap(std::move(root));
  auto encoded = Encode(original);
  ASSERT_TRUE(encoded.ok()) << encoded.status().message();
  auto decoded = Decode(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  EXPECT_EQ(decoded.value(), original);
}

TEST(Cbor, JsonFixtureRoundTripNested) {
  constexpr const char* kFixture = R"json(
{
  "title": "widget",
  "count": 42,
  "price": 9.99,
  "on_sale": false,
  "note": null,
  "tags": ["red", "large", "sale"],
  "dims": {"w": 10, "h": 20, "nested": {"ok": true, "vals": [1, 2, 3]}}
}
)json";

  auto parsed = ParseJson(kFixture);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();

  auto cbor_bytes = EncodeJson(kFixture);
  ASSERT_TRUE(cbor_bytes.ok()) << cbor_bytes.status().message();
  EXPECT_FALSE(cbor_bytes.value().empty());

  auto back = Decode(cbor_bytes.value());
  ASSERT_TRUE(back.ok()) << back.status().message();
  EXPECT_EQ(back.value(), parsed.value());

  auto json_out = DecodeToJson(cbor_bytes.value());
  ASSERT_TRUE(json_out.ok()) << json_out.status().message();
  auto reparsed = ParseJson(json_out.value());
  ASSERT_TRUE(reparsed.ok()) << reparsed.status().message();
  EXPECT_EQ(reparsed.value(), parsed.value());
}

TEST(Cbor, JsonFixtureArrayRoot) {
  constexpr const char* kFixture =
      R"json([{"k":1},{"k":2,"xs":[true,false,null,"s"]}])json";

  auto cbor_bytes = EncodeJson(kFixture);
  ASSERT_TRUE(cbor_bytes.ok()) << cbor_bytes.status().message();
  auto back = Decode(cbor_bytes.value());
  ASSERT_TRUE(back.ok()) << back.status().message();
  auto expected = ParseJson(kFixture);
  ASSERT_TRUE(expected.ok());
  EXPECT_EQ(back.value(), expected.value());
}

TEST(Cbor, RejectsTruncatedAndTrailing) {
  EXPECT_FALSE(Decode(std::string_view("\xa1", 1)).ok());  // map len 1, empty
  auto one = Encode(Value::Int(1));
  ASSERT_TRUE(one.ok());
  std::string trailing = one.value();
  trailing.push_back('\x00');
  EXPECT_FALSE(Decode(trailing).ok());
}

TEST(Cbor, EmptyMetadataObject) {
  auto bytes = EncodeJson("{}");
  ASSERT_TRUE(bytes.ok());
  EXPECT_EQ(bytes.value(), std::string("\xa0", 1));
  auto json = DecodeToJson(bytes.value());
  ASSERT_TRUE(json.ok());
  EXPECT_EQ(json.value(), "{}");
}

TEST(Cbor, LargeIntAndByteString) {
  const int64_t big = 1'000'000'000'000ll;
  auto enc = Encode(Value::Int(big));
  ASSERT_TRUE(enc.ok());
  auto dec = Decode(enc.value());
  ASSERT_TRUE(dec.ok());
  EXPECT_EQ(dec.value().as_int(), big);

  const std::string payload(300, 'x');
  Value::Map m;
  m.emplace("blob", Value::Bytes(payload));
  auto enc_map = Encode(Value::FromMap(std::move(m)));
  ASSERT_TRUE(enc_map.ok());
  auto dec_map = Decode(enc_map.value());
  ASSERT_TRUE(dec_map.ok());
  EXPECT_EQ(dec_map.value().as_map().at("blob").as_bytes(), payload);
}

TEST(Cbor, JsonParseErrors) {
  EXPECT_FALSE(ParseJson("").ok());
  EXPECT_FALSE(ParseJson("{").ok());
  EXPECT_FALSE(ParseJson("{\"a\":}").ok());
  EXPECT_FALSE(ParseJson("[1,]").ok());
}

TEST(Cbor, Float32Encoding) {
  auto enc = Encode(Value::Float(3.14f));
  ASSERT_TRUE(enc.ok());
  auto dec = Decode(enc.value());
  ASSERT_TRUE(dec.ok());
  EXPECT_NEAR(dec.value().as_float(), 3.14f, 1e-5f);
}

TEST(Cbor, DecodeInvalidMajorTypes) {
  EXPECT_FALSE(Decode(std::string("\x7f", 1)).ok());  // undefined major
  EXPECT_FALSE(Decode(std::string("\xc0", 1)).ok());  // invalid simple
}

TEST(Cbor, EncodeLongStringAndArray) {
  std::string long_s(300, 'z');
  auto enc = Encode(Value::String(long_s));
  ASSERT_TRUE(enc.ok());
  auto dec = Decode(enc.value());
  ASSERT_TRUE(dec.ok());
  EXPECT_EQ(dec.value().as_string(), long_s);

  Value::Array arr;
  for (int i = 0; i < 30; ++i) {
    arr.push_back(Value::Int(i));
  }
  enc = Encode(Value::FromArray(std::move(arr)));
  ASSERT_TRUE(enc.ok());
  dec = Decode(enc.value());
  ASSERT_TRUE(dec.ok());
  EXPECT_EQ(dec.value().as_array().size(), 30u);
}

TEST(Cbor, JsonScientificAndLiterals) {
  auto parsed = ParseJson(R"({"n":1.5e2,"neg":-3,"t":true,"f":false,"z":null})");
  ASSERT_TRUE(parsed.ok());
  const auto& m = parsed.value().as_map();
  EXPECT_TRUE(m.at("t").as_bool());
  EXPECT_FALSE(m.at("f").as_bool());
  EXPECT_TRUE(m.at("z").is_null());
  EXPECT_NEAR(m.at("n").as_float(), 150.0, 1e-6);
  EXPECT_EQ(m.at("neg").as_int(), -3);
}

TEST(Cbor, JsonParseInvalidLiterals) {
  EXPECT_FALSE(ParseJson("nul").ok());
  EXPECT_FALSE(ParseJson("tru").ok());
  EXPECT_FALSE(ParseJson("fals").ok());
  EXPECT_FALSE(ParseJson("?").ok());
  EXPECT_FALSE(ParseJson("[1,2").ok());
  EXPECT_FALSE(ParseJson("{\"a\":1,}").ok());
}

TEST(Cbor, DecodeToJsonEscapesControlChars) {
  Value::Map m;
  m.emplace("c", Value::String(std::string("\x01\b\f", 3)));
  m.emplace("bytes", Value::Bytes("raw"));
  auto enc = Encode(Value::FromMap(std::move(m)));
  ASSERT_TRUE(enc.ok());
  auto json = DecodeToJson(enc.value());
  ASSERT_TRUE(json.ok());
  EXPECT_NE(json.value().find("\\u"), std::string::npos);
}

}  // namespace
}  // namespace cbor
}  // namespace aster
