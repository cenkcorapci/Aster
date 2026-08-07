#include "aster/server/json.h"

#include <cctype>
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
        out += c;
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
  std::string out;
  while (i < obj.size() && obj[i] != '"') {
    if (obj[i] == '\\' && i + 1 < obj.size()) {
      out += obj[i + 1];
      i += 2;
      continue;
    }
    out += obj[i++];
  }
  return out;
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
    std::string s;
    while (i < obj.size() && obj[i] != '"') {
      if (obj[i] == '\\' && i + 1 < obj.size()) {
        s += obj[i + 1];
        i += 2;
        continue;
      }
      s += obj[i++];
    }
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
