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

}  // namespace
}  // namespace cbor
}  // namespace aster
