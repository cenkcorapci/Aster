#include "aster/core/cbor.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>

namespace aster {
namespace cbor {
namespace {

constexpr uint8_t kMajorUnsigned = 0;
constexpr uint8_t kMajorNegative = 1;
constexpr uint8_t kMajorBytes = 2;
constexpr uint8_t kMajorText = 3;
constexpr uint8_t kMajorArray = 4;
constexpr uint8_t kMajorMap = 5;
constexpr uint8_t kMajorSimple = 7;

constexpr uint8_t kSimpleFalse = 20;
constexpr uint8_t kSimpleTrue = 21;
constexpr uint8_t kSimpleNull = 22;
constexpr uint8_t kSimpleFloat16 = 25;
constexpr uint8_t kSimpleFloat32 = 26;
constexpr uint8_t kSimpleFloat64 = 27;

void AppendU8(std::string* out, uint8_t v) {
  out->push_back(static_cast<char>(v));
}

void AppendBe(std::string* out, uint64_t v, int nbytes) {
  for (int i = nbytes - 1; i >= 0; --i) {
    AppendU8(out, static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

void EncodeHead(std::string* out, uint8_t major, uint64_t arg) {
  const uint8_t mt = static_cast<uint8_t>(major << 5);
  if (arg < 24) {
    AppendU8(out, static_cast<uint8_t>(mt | arg));
  } else if (arg <= 0xff) {
    AppendU8(out, static_cast<uint8_t>(mt | 24));
    AppendU8(out, static_cast<uint8_t>(arg));
  } else if (arg <= 0xffff) {
    AppendU8(out, static_cast<uint8_t>(mt | 25));
    AppendBe(out, arg, 2);
  } else if (arg <= 0xffffffffull) {
    AppendU8(out, static_cast<uint8_t>(mt | 26));
    AppendBe(out, arg, 4);
  } else {
    AppendU8(out, static_cast<uint8_t>(mt | 27));
    AppendBe(out, arg, 8);
  }
}

Status EncodeValue(const Value& value, std::string* out) {
  switch (value.type()) {
    case Value::Type::kNull:
      AppendU8(out, static_cast<uint8_t>((kMajorSimple << 5) | kSimpleNull));
      return Status::Ok();
    case Value::Type::kBool:
      AppendU8(out, static_cast<uint8_t>(
                        (kMajorSimple << 5) |
                        (value.as_bool() ? kSimpleTrue : kSimpleFalse)));
      return Status::Ok();
    case Value::Type::kInt: {
      const int64_t v = value.as_int();
      if (v >= 0) {
        EncodeHead(out, kMajorUnsigned, static_cast<uint64_t>(v));
      } else {
        EncodeHead(out, kMajorNegative, static_cast<uint64_t>(-1 - v));
      }
      return Status::Ok();
    }
    case Value::Type::kFloat: {
      AppendU8(out, static_cast<uint8_t>((kMajorSimple << 5) | kSimpleFloat64));
      uint64_t bits = 0;
      const double v = value.as_float();
      std::memcpy(&bits, &v, sizeof(bits));
      AppendBe(out, bits, 8);
      return Status::Ok();
    }
    case Value::Type::kString: {
      const auto& s = value.as_string();
      EncodeHead(out, kMajorText, s.size());
      out->append(s);
      return Status::Ok();
    }
    case Value::Type::kBytes: {
      const auto& s = value.as_bytes();
      EncodeHead(out, kMajorBytes, s.size());
      out->append(s);
      return Status::Ok();
    }
    case Value::Type::kArray: {
      const auto& a = value.as_array();
      EncodeHead(out, kMajorArray, a.size());
      for (const auto& el : a) {
        Status st = EncodeValue(el, out);
        if (!st.ok()) return st;
      }
      return Status::Ok();
    }
    case Value::Type::kMap: {
      const auto& m = value.as_map();
      EncodeHead(out, kMajorMap, m.size());
      for (const auto& [k, v] : m) {
        EncodeHead(out, kMajorText, k.size());
        out->append(k);
        Status st = EncodeValue(v, out);
        if (!st.ok()) return st;
      }
      return Status::Ok();
    }
  }
  return Status::InvalidArgument("unknown CBOR value type");
}

struct Decoder {
  std::string_view in;
  size_t pos = 0;

  bool Remain(size_t n) const { return pos + n <= in.size(); }

  Result<uint8_t> ReadU8() {
    if (!Remain(1)) return Status::Corruption("CBOR truncated");
    return static_cast<uint8_t>(in[pos++]);
  }

  Result<uint64_t> ReadBe(int nbytes) {
    if (!Remain(static_cast<size_t>(nbytes))) {
      return Status::Corruption("CBOR truncated integer");
    }
    uint64_t v = 0;
    for (int i = 0; i < nbytes; ++i) {
      v = (v << 8) | static_cast<uint8_t>(in[pos++]);
    }
    return v;
  }

  Result<uint64_t> ReadArg(uint8_t ai) {
    if (ai < 24) return static_cast<uint64_t>(ai);
    if (ai == 24) {
      auto b = ReadU8();
      if (!b.ok()) return b.status();
      return static_cast<uint64_t>(b.value());
    }
    if (ai == 25) return ReadBe(2);
    if (ai == 26) return ReadBe(4);
    if (ai == 27) return ReadBe(8);
    return Status::InvalidArgument("unsupported CBOR additional info");
  }

  Result<std::string> ReadBytes(uint64_t len) {
    if (len > in.size() - pos) {
      return Status::Corruption("CBOR string/bytes length past end");
    }
    std::string s(in.substr(pos, static_cast<size_t>(len)));
    pos += static_cast<size_t>(len);
    return s;
  }

  Result<Value> DecodeOne();
};

float DecodeFloat16(uint16_t half) {
  const uint32_t sign = (half >> 15) & 1;
  const uint32_t exp = (half >> 10) & 0x1f;
  const uint32_t mant = half & 0x3ff;
  uint32_t fbits = 0;
  if (exp == 0) {
    if (mant == 0) {
      fbits = sign << 31;
    } else {
      uint32_t m = mant;
      uint32_t e = 127 - 15 + 1;
      while ((m & 0x400) == 0) {
        m <<= 1;
        --e;
      }
      m &= 0x3ff;
      fbits = (sign << 31) | (e << 23) | (m << 13);
    }
  } else if (exp == 31) {
    fbits = (sign << 31) | (0xffu << 23) | (mant << 13);
  } else {
    fbits = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
  }
  float f = 0;
  std::memcpy(&f, &fbits, sizeof(f));
  return f;
}

Result<Value> Decoder::DecodeOne() {
  auto head = ReadU8();
  if (!head.ok()) return head.status();
  const uint8_t b = head.value();
  const uint8_t major = static_cast<uint8_t>(b >> 5);
  const uint8_t ai = static_cast<uint8_t>(b & 0x1f);

  switch (major) {
    case kMajorUnsigned: {
      auto arg = ReadArg(ai);
      if (!arg.ok()) return arg.status();
      if (arg.value() >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return Status::InvalidArgument("CBOR unsigned exceeds int64");
      }
      return Value::Int(static_cast<int64_t>(arg.value()));
    }
    case kMajorNegative: {
      auto arg = ReadArg(ai);
      if (!arg.ok()) return arg.status();
      if (arg.value() >=
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return Status::InvalidArgument("CBOR negative exceeds int64");
      }
      return Value::Int(-1 - static_cast<int64_t>(arg.value()));
    }
    case kMajorBytes: {
      auto arg = ReadArg(ai);
      if (!arg.ok()) return arg.status();
      auto s = ReadBytes(arg.value());
      if (!s.ok()) return s.status();
      return Value::Bytes(std::move(s.value()));
    }
    case kMajorText: {
      auto arg = ReadArg(ai);
      if (!arg.ok()) return arg.status();
      auto s = ReadBytes(arg.value());
      if (!s.ok()) return s.status();
      return Value::String(std::move(s.value()));
    }
    case kMajorArray: {
      auto arg = ReadArg(ai);
      if (!arg.ok()) return arg.status();
      Value::Array a;
      a.reserve(static_cast<size_t>(arg.value()));
      for (uint64_t i = 0; i < arg.value(); ++i) {
        auto el = DecodeOne();
        if (!el.ok()) return el.status();
        a.push_back(std::move(el.value()));
      }
      return Value::FromArray(std::move(a));
    }
    case kMajorMap: {
      auto arg = ReadArg(ai);
      if (!arg.ok()) return arg.status();
      Value::Map m;
      for (uint64_t i = 0; i < arg.value(); ++i) {
        auto key = DecodeOne();
        if (!key.ok()) return key.status();
        if (!key.value().is_string()) {
          return Status::InvalidArgument("CBOR map key must be text string");
        }
        auto val = DecodeOne();
        if (!val.ok()) return val.status();
        m.emplace(key.value().as_string(), std::move(val.value()));
      }
      return Value::FromMap(std::move(m));
    }
    case kMajorSimple: {
      if (ai == kSimpleFalse) return Value::Bool(false);
      if (ai == kSimpleTrue) return Value::Bool(true);
      if (ai == kSimpleNull) return Value::Null();
      if (ai == kSimpleFloat16) {
        auto bits = ReadBe(2);
        if (!bits.ok()) return bits.status();
        return Value::Float(static_cast<double>(
            DecodeFloat16(static_cast<uint16_t>(bits.value()))));
      }
      if (ai == kSimpleFloat32) {
        auto bits = ReadBe(4);
        if (!bits.ok()) return bits.status();
        uint32_t u = static_cast<uint32_t>(bits.value());
        float f = 0;
        std::memcpy(&f, &u, sizeof(f));
        return Value::Float(static_cast<double>(f));
      }
      if (ai == kSimpleFloat64) {
        auto bits = ReadBe(8);
        if (!bits.ok()) return bits.status();
        uint64_t u = bits.value();
        double d = 0;
        std::memcpy(&d, &u, sizeof(d));
        return Value::Float(d);
      }
      return Status::InvalidArgument("unsupported CBOR simple/float");
    }
    default:
      return Status::InvalidArgument("unsupported CBOR major type");
  }
}

size_t SkipWs(std::string_view s, size_t i) {
  while (i < s.size() &&
         std::isspace(static_cast<unsigned char>(s[i]))) {
    ++i;
  }
  return i;
}

Result<Value> ParseJsonValue(std::string_view s, size_t* i);

Result<std::string> ParseJsonString(std::string_view s, size_t* i) {
  if (*i >= s.size() || s[*i] != '"') {
    return Status::InvalidArgument("JSON string expected");
  }
  ++(*i);
  std::string out;
  while (*i < s.size()) {
    const char c = s[*i];
    if (c == '"') {
      ++(*i);
      return out;
    }
    if (c == '\\') {
      if (*i + 1 >= s.size()) {
        return Status::InvalidArgument("JSON string truncated escape");
      }
      const char e = s[*i + 1];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out.push_back(e);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (*i + 5 >= s.size()) {
            return Status::InvalidArgument("JSON \\u escape truncated");
          }
          unsigned code = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = s[*i + 2 + k];
            code <<= 4;
            if (h >= '0' && h <= '9') {
              code |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              code |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              code |= static_cast<unsigned>(h - 'A' + 10);
            } else {
              return Status::InvalidArgument("JSON \\u bad hex");
            }
          }
          if (code <= 0x7f) {
            out.push_back(static_cast<char>(code));
          } else if (code <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
          } else {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
          }
          *i += 6;
          continue;
        }
        default:
          return Status::InvalidArgument("JSON bad escape");
      }
      *i += 2;
      continue;
    }
    out.push_back(c);
    ++(*i);
  }
  return Status::InvalidArgument("JSON string not terminated");
}

Result<Value> ParseJsonNumber(std::string_view s, size_t* i) {
  const size_t start = *i;
  if (*i < s.size() && (s[*i] == '-' || s[*i] == '+')) ++(*i);
  while (*i < s.size() && std::isdigit(static_cast<unsigned char>(s[*i]))) {
    ++(*i);
  }
  bool is_float = false;
  if (*i < s.size() && s[*i] == '.') {
    is_float = true;
    ++(*i);
    while (*i < s.size() && std::isdigit(static_cast<unsigned char>(s[*i]))) {
      ++(*i);
    }
  }
  if (*i < s.size() && (s[*i] == 'e' || s[*i] == 'E')) {
    is_float = true;
    ++(*i);
    if (*i < s.size() && (s[*i] == '+' || s[*i] == '-')) ++(*i);
    while (*i < s.size() && std::isdigit(static_cast<unsigned char>(s[*i]))) {
      ++(*i);
    }
  }
  if (*i == start) return Status::InvalidArgument("JSON number expected");
  const std::string token(s.substr(start, *i - start));
  if (!is_float) {
    char* end = nullptr;
    const long long v = std::strtoll(token.c_str(), &end, 10);
    if (end == token.c_str() + token.size()) {
      return Value::Int(static_cast<int64_t>(v));
    }
  }
  char* end = nullptr;
  const double d = std::strtod(token.c_str(), &end);
  if (end != token.c_str() + token.size()) {
    return Status::InvalidArgument("JSON number parse failed");
  }
  return Value::Float(d);
}

Result<Value> ParseJsonArray(std::string_view s, size_t* i) {
  if (*i >= s.size() || s[*i] != '[') {
    return Status::InvalidArgument("JSON array expected");
  }
  ++(*i);
  Value::Array a;
  *i = SkipWs(s, *i);
  if (*i < s.size() && s[*i] == ']') {
    ++(*i);
    return Value::FromArray(std::move(a));
  }
  while (*i < s.size()) {
    auto el = ParseJsonValue(s, i);
    if (!el.ok()) return el.status();
    a.push_back(std::move(el.value()));
    *i = SkipWs(s, *i);
    if (*i < s.size() && s[*i] == ',') {
      ++(*i);
      *i = SkipWs(s, *i);
      continue;
    }
    if (*i < s.size() && s[*i] == ']') {
      ++(*i);
      return Value::FromArray(std::move(a));
    }
    return Status::InvalidArgument("JSON array syntax");
  }
  return Status::InvalidArgument("JSON array not terminated");
}

Result<Value> ParseJsonObject(std::string_view s, size_t* i) {
  if (*i >= s.size() || s[*i] != '{') {
    return Status::InvalidArgument("JSON object expected");
  }
  ++(*i);
  Value::Map m;
  *i = SkipWs(s, *i);
  if (*i < s.size() && s[*i] == '}') {
    ++(*i);
    return Value::FromMap(std::move(m));
  }
  while (*i < s.size()) {
    auto key = ParseJsonString(s, i);
    if (!key.ok()) return key.status();
    *i = SkipWs(s, *i);
    if (*i >= s.size() || s[*i] != ':') {
      return Status::InvalidArgument("JSON object expects ':'");
    }
    ++(*i);
    auto val = ParseJsonValue(s, i);
    if (!val.ok()) return val.status();
    m.emplace(std::move(key.value()), std::move(val.value()));
    *i = SkipWs(s, *i);
    if (*i < s.size() && s[*i] == ',') {
      ++(*i);
      *i = SkipWs(s, *i);
      continue;
    }
    if (*i < s.size() && s[*i] == '}') {
      ++(*i);
      return Value::FromMap(std::move(m));
    }
    return Status::InvalidArgument("JSON object syntax");
  }
  return Status::InvalidArgument("JSON object not terminated");
}

Result<Value> ParseJsonValue(std::string_view s, size_t* i) {
  *i = SkipWs(s, *i);
  if (*i >= s.size()) return Status::InvalidArgument("JSON truncated");
  const char c = s[*i];
  if (c == 'n') {
    if (s.substr(*i, 4) != "null") {
      return Status::InvalidArgument("JSON null expected");
    }
    *i += 4;
    return Value::Null();
  }
  if (c == 't') {
    if (s.substr(*i, 4) != "true") {
      return Status::InvalidArgument("JSON true expected");
    }
    *i += 4;
    return Value::Bool(true);
  }
  if (c == 'f') {
    if (s.substr(*i, 5) != "false") {
      return Status::InvalidArgument("JSON false expected");
    }
    *i += 5;
    return Value::Bool(false);
  }
  if (c == '"') {
    auto str = ParseJsonString(s, i);
    if (!str.ok()) return str.status();
    return Value::String(std::move(str.value()));
  }
  if (c == '{') return ParseJsonObject(s, i);
  if (c == '[') return ParseJsonArray(s, i);
  if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
    return ParseJsonNumber(s, i);
  }
  return Status::InvalidArgument("JSON unexpected token");
}

void AppendJsonEscaped(std::string* out, std::string_view s) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out->append("\\\"");
        break;
      case '\\':
        out->append("\\\\");
        break;
      case '\b':
        out->append("\\b");
        break;
      case '\f':
        out->append("\\f");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out->append(buf);
        } else {
          out->push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out->push_back('"');
}

void AppendJson(const Value& value, std::string* out) {
  switch (value.type()) {
    case Value::Type::kNull:
      out->append("null");
      break;
    case Value::Type::kBool:
      out->append(value.as_bool() ? "true" : "false");
      break;
    case Value::Type::kInt:
      out->append(std::to_string(value.as_int()));
      break;
    case Value::Type::kFloat: {
      std::ostringstream oss;
      oss.precision(std::numeric_limits<double>::max_digits10);
      oss << value.as_float();
      out->append(oss.str());
      break;
    }
    case Value::Type::kString:
      AppendJsonEscaped(out, value.as_string());
      break;
    case Value::Type::kBytes:
      AppendJsonEscaped(out, value.as_bytes());
      break;
    case Value::Type::kArray: {
      out->push_back('[');
      const auto& a = value.as_array();
      for (size_t i = 0; i < a.size(); ++i) {
        if (i) out->push_back(',');
        AppendJson(a[i], out);
      }
      out->push_back(']');
      break;
    }
    case Value::Type::kMap: {
      out->push_back('{');
      bool first = true;
      for (const auto& [k, v] : value.as_map()) {
        if (!first) out->push_back(',');
        first = false;
        AppendJsonEscaped(out, k);
        out->push_back(':');
        AppendJson(v, out);
      }
      out->push_back('}');
      break;
    }
  }
}

}  // namespace

Value::Type Value::type() const {
  return std::visit(
      [](const auto& v) -> Type {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) return Type::kNull;
        if constexpr (std::is_same_v<T, bool>) return Type::kBool;
        if constexpr (std::is_same_v<T, int64_t>) return Type::kInt;
        if constexpr (std::is_same_v<T, double>) return Type::kFloat;
        if constexpr (std::is_same_v<T, std::string>) return Type::kString;
        if constexpr (std::is_same_v<T, BytesTag>) return Type::kBytes;
        if constexpr (std::is_same_v<T, Array>) return Type::kArray;
        if constexpr (std::is_same_v<T, Map>) return Type::kMap;
      },
      data_);
}

bool Value::operator==(const Value& other) const {
  if (type() != other.type()) {
    if ((is_int() && other.is_float()) || (is_float() && other.is_int())) {
      const double a = is_int() ? static_cast<double>(as_int()) : as_float();
      const double b =
          other.is_int() ? static_cast<double>(other.as_int()) : other.as_float();
      return a == b;
    }
    return false;
  }
  return data_ == other.data_;
}

Result<std::string> Encode(const Value& value) {
  std::string out;
  Status st = EncodeValue(value, &out);
  if (!st.ok()) return st;
  return out;
}

Result<Value> Decode(std::string_view cbor_bytes) {
  Decoder d{cbor_bytes, 0};
  auto v = d.DecodeOne();
  if (!v.ok()) return v.status();
  if (d.pos != cbor_bytes.size()) {
    return Status::InvalidArgument("trailing bytes after CBOR value");
  }
  return v;
}

Result<Value> ParseJson(std::string_view json) {
  size_t i = 0;
  auto v = ParseJsonValue(json, &i);
  if (!v.ok()) return v.status();
  i = SkipWs(json, i);
  if (i != json.size()) {
    return Status::InvalidArgument("trailing junk after JSON value");
  }
  return v;
}

std::string ToJson(const Value& value) {
  std::string out;
  AppendJson(value, &out);
  return out;
}

Result<std::string> EncodeJson(std::string_view json) {
  auto v = ParseJson(json);
  if (!v.ok()) return v.status();
  return Encode(v.value());
}

Result<std::string> DecodeToJson(std::string_view cbor_bytes) {
  auto v = Decode(cbor_bytes);
  if (!v.ok()) return v.status();
  return ToJson(v.value());
}

}  // namespace cbor
}  // namespace aster
