#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aster {
namespace json {

// Minimal JSON helpers for the HTTP API (no external dependency).

std::string Escape(const std::string& s);

std::optional<std::string> GetString(const std::string& obj, const char* key);
std::optional<int64_t> GetInt(const std::string& obj, const char* key);
std::optional<double> GetNumber(const std::string& obj, const char* key);
std::optional<std::vector<float>> GetFloatArray(const std::string& obj,
                                                const char* key);
std::optional<std::vector<std::string>> GetStringArray(const std::string& obj,
                                                       const char* key);

}  // namespace json
}  // namespace aster
