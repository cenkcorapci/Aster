#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "aster/core/status.h"

namespace aster {
namespace cbor {

// Structured CBOR value for Aster row metadata.
// Covers the JSON-compatible subset used by typical metadata documents:
// null, bool, int, float, UTF-8 string, byte string, array, and string-keyed map.
class Value {
 public:
  using Array = std::vector<Value>;
  using Map = std::map<std::string, Value>;

  enum class Type {
    kNull,
    kBool,
    kInt,
    kFloat,
    kString,
    kBytes,
    kArray,
    kMap,
  };

  Value() : data_(std::monostate{}) {}

  static Value Null() { return Value(std::monostate{}); }
  static Value Bool(bool v) { return Value(v); }
  static Value Int(int64_t v) { return Value(v); }
  static Value Float(double v) { return Value(v); }
  static Value String(std::string v) { return Value(std::move(v)); }
  static Value Bytes(std::string v) {
    return Value(BytesTag{std::move(v)});
  }
  static Value FromArray(Array v) { return Value(std::move(v)); }
  static Value FromMap(Map v) { return Value(std::move(v)); }

  Type type() const;

  bool is_null() const { return type() == Type::kNull; }
  bool is_bool() const { return type() == Type::kBool; }
  bool is_int() const { return type() == Type::kInt; }
  bool is_float() const { return type() == Type::kFloat; }
  bool is_string() const { return type() == Type::kString; }
  bool is_bytes() const { return type() == Type::kBytes; }
  bool is_array() const { return type() == Type::kArray; }
  bool is_map() const { return type() == Type::kMap; }

  bool as_bool() const { return std::get<bool>(data_); }
  int64_t as_int() const { return std::get<int64_t>(data_); }
  double as_float() const { return std::get<double>(data_); }
  const std::string& as_string() const { return std::get<std::string>(data_); }
  const std::string& as_bytes() const {
    return std::get<BytesTag>(data_).bytes;
  }
  const Array& as_array() const { return std::get<Array>(data_); }
  const Map& as_map() const { return std::get<Map>(data_); }

  Array& as_array() { return std::get<Array>(data_); }
  Map& as_map() { return std::get<Map>(data_); }

  bool operator==(const Value& other) const;
  bool operator!=(const Value& other) const { return !(*this == other); }

 private:
  struct BytesTag {
    std::string bytes;
    bool operator==(const BytesTag&) const = default;
  };

  using Storage = std::variant<std::monostate, bool, int64_t, double,
                               std::string, BytesTag, Array, Map>;

  explicit Value(Storage data) : data_(std::move(data)) {}

  Storage data_;
};

// Encode / decode structured values as RFC 8949 CBOR bytes.
Result<std::string> Encode(const Value& value);
Result<Value> Decode(std::string_view cbor);

// Parse a JSON document into a Value (maps, arrays, strings, ints, floats,
// bools, null). Numbers without a fractional/exponent part that fit in
// int64 become kInt; otherwise kFloat.
Result<Value> ParseJson(std::string_view json);

// Serialize a Value to a compact JSON string. Byte strings are emitted as
// JSON strings of raw bytes (for fixture readability); prefer Encode for
// storage.
std::string ToJson(const Value& value);

// Convenience: JSON fixture text ↔ CBOR bytes.
Result<std::string> EncodeJson(std::string_view json);
Result<std::string> DecodeToJson(std::string_view cbor);

}  // namespace cbor
}  // namespace aster
