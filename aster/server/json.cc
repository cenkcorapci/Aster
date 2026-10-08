#include "aster/server/json.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace aster {
namespace json {
namespace {

size_t SkipWs(const std::string& s, size_t i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return i;
}

bool FindKey(const std::string& obj, const char* key, size_t* value_pos) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t i = 0;
  while (true) {
    i = obj.find(needle, i);
    if (i == std::string::npos) return false;
    size_t j = SkipWs(obj, i + needle.size());
    if (j < obj.size() && obj[j] == ':') {
      *value_pos = SkipWs(obj, j + 1);
      return true;
    }
    i += needle.size();
  }
}

void AppendUtf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) {
    *out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    *out += static_cast<char>(0xC0 | (cp >> 6));
    *out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    *out += static_cast<char>(0xE0 | (cp >> 12));
    *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    *out += static_cast<char>(0xF0 | (cp >> 18));
    *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool ParseHex4(const std::string& s, size_t i, uint32_t* out) {
  if (i + 4 > s.size()) return false;
  uint32_t v = 0;
  for (size_t k = 0; k < 4; ++k) {
    const char c = s[i + k];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
    else return false;
  }
  *out = v;
  return true;
}

// Parses a JSON string body starting just after the opening quote. Leaves
// *i on the closing quote (or at s.size() when unterminated).
std::string ParseStringBody(const std::string& s, size_t* i) {
  std::string out;
  while (*i < s.size() && s[*i] != '"') {
    if (s[*i] != '\\' || *i + 1 >= s.size()) {
      out += s[(*i)++];
      continue;
    }
    const char e = s[*i + 1];
    *i += 2;
    switch (e) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        uint32_t cp = 0;
        if (!ParseHex4(s, *i, &cp)) {
          out += 'u';
          break;
        }
        *i += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && *i + 6 <= s.size() &&
            s[*i] == '\\' && s[*i + 1] == 'u') {
          uint32_t lo = 0;
          if (ParseHex4(s, *i + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            *i += 6;
          }
        }
        AppendUtf8(&out, cp);
        break;
      }
      default: out += e; break;  // \" \\ \/ and unknown escapes
    }
  }
  return out;
}

}  // namespace

std::string Escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static const char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
          out += kHex[static_cast<unsigned char>(c) & 0xF];
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

std::optional<std::string> GetString(const std::string& obj, const char* key) {
  size_t i = 0;
  if (!FindKey(obj, key, &i)) return std::nullopt;
  if (i >= obj.size() || obj[i] != '"') return std::nullopt;
  ++i;
  return ParseStringBody(obj, &i);
}

std::optional<int64_t> GetInt(const std::string& obj, const char* key) {
  size_t i = 0;
  if (!FindKey(obj, key, &i)) return std::nullopt;
  char* end = nullptr;
  const long long v = std::strtoll(obj.c_str() + i, &end, 10);
  if (end == obj.c_str() + i) return std::nullopt;
  return static_cast<int64_t>(v);
}

std::optional<double> GetNumber(const std::string& obj, const char* key) {
  size_t i = 0;
  if (!FindKey(obj, key, &i)) return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(obj.c_str() + i, &end);
  if (end == obj.c_str() + i) return std::nullopt;
  return v;
}

std::optional<std::vector<float>> GetFloatArray(const std::string& obj,
                                                const char* key) {
  size_t i = 0;
  if (!FindKey(obj, key, &i)) return std::nullopt;
  if (i >= obj.size() || obj[i] != '[') return std::nullopt;
  ++i;
  std::vector<float> out;
  while (i < obj.size()) {
    i = SkipWs(obj, i);
    if (i < obj.size() && obj[i] == ']') return out;
    char* end = nullptr;
    const float v = std::strtof(obj.c_str() + i, &end);
    if (end == obj.c_str() + i) return std::nullopt;
    out.push_back(v);
    i = static_cast<size_t>(end - obj.c_str());
    i = SkipWs(obj, i);
    if (i < obj.size() && obj[i] == ',') {
      ++i;
      continue;
    }
    if (i < obj.size() && obj[i] == ']') return out;
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::vector<std::string>> GetStringArray(const std::string& obj,
                                                       const char* key) {
  size_t i = 0;
  if (!FindKey(obj, key, &i)) return std::nullopt;
  if (i >= obj.size() || obj[i] != '[') return std::nullopt;
  ++i;
  std::vector<std::string> out;
  while (i < obj.size()) {
    i = SkipWs(obj, i);
    if (i < obj.size() && obj[i] == ']') return out;
    if (i >= obj.size() || obj[i] != '"') return std::nullopt;
    ++i;
    std::string s = ParseStringBody(obj, &i);
    if (i >= obj.size()) return std::nullopt;
    ++i;
    out.push_back(std::move(s));
    i = SkipWs(obj, i);
    if (i < obj.size() && obj[i] == ',') {
      ++i;
      continue;
    }
    if (i < obj.size() && obj[i] == ']') return out;
    return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace json
}  // namespace aster
