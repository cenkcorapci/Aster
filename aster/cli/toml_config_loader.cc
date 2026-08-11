#include "aster/cli/toml_config_loader.h"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "aster/core/status.h"

namespace aster {
namespace cli {

namespace {

std::string Trim(std::string_view s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return std::string(s.substr(b, e - b));
}

std::string FormatError(const std::string& source_name, int line,
                        const std::string& message) {
  std::ostringstream os;
  os << source_name << ":" << line << ": " << message;
  return os.str();
}

bool IsBareToken(std::string_view s) {
  if (s.empty()) return false;
  auto is_ident = [](unsigned char c) -> bool {
    return std::isalnum(c) || c == '_' || c == '-';
  };
  unsigned char first = static_cast<unsigned char>(s.front());
  if (!(std::isalpha(first) || first == '_')) return false;
  for (unsigned char c : s.substr(1)) {
    if (!is_ident(c)) return false;
  }
  return true;
}

struct ParsedString {
  bool ok = false;
  std::string value;
  std::string error;
};

ParsedString ParseStringLiteral(std::string_view raw) {
  if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
    return ParsedString{false, {}, "expected a double-quoted string literal"};
  }
  // Minimal TOML string handling: escapes for \" \\ \n \t \r.
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 1; i + 1 < raw.size(); ++i) {
    char c = raw[i];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= raw.size() - 1) {
      return ParsedString{false, {}, "unterminated escape sequence"};
    }
    char esc = raw[++i];
    switch (esc) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'r':
        out.push_back('\r');
        break;
      default:
        return ParsedString{false, {},
                            std::string("unsupported escape: \\") + esc};
    }
  }
  return ParsedString{true, std::move(out), {}};
}

Result<TomlConfig> ParseToml(std::string_view source_name,
                            std::string_view toml_text) {
  std::string cur_section;

  TomlConfig out;

  auto set_err = [&](int line, const std::string& msg) -> Result<TomlConfig> {
    return Result<TomlConfig>(
        Status::InvalidArgument(FormatError(std::string(source_name), line, msg)));
  };

  // Track duplicates for clearer errors.
  bool host_set = false;
  bool port_set = false;
  bool data_dir_set = false;
  bool wal_sync_set = false;
  bool memtable_flush_bytes_set = false;
  bool compaction_tier_threshold_set = false;
  bool max_segments_before_compact_set = false;

  std::istringstream iss{std::string(toml_text)};
  std::string line;
  int line_no = 0;
  while (std::getline(iss, line)) {
    line_no++;
    // Strip comments (TOML: # starts a comment unless inside a string).
    bool in_string = false;
    bool escaped = false;
    std::string no_comment;
    no_comment.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
      char c = line[i];
      if (in_string) {
        if (escaped) {
          escaped = false;
          no_comment.push_back(c);
          continue;
        }
        if (c == '\\') {
          escaped = true;
          no_comment.push_back(c);
          continue;
        }
        if (c == '"') in_string = false;
        no_comment.push_back(c);
        continue;
      }
      if (c == '"') {
        in_string = true;
        no_comment.push_back(c);
        continue;
      }
      if (c == '#') break;
      no_comment.push_back(c);
    }

    auto trimmed = Trim(no_comment);
    if (trimmed.empty()) continue;

    if (trimmed.front() == '[') {
      // Expect: [server] or [catalog]
      if (trimmed.back() != ']') {
        return set_err(line_no, "unterminated table header");
      }
      cur_section = Trim(trimmed.substr(1, trimmed.size() - 2));
      if (cur_section != "server" && cur_section != "catalog") {
        return set_err(line_no,
                        "unknown table '" + cur_section +
                            "' (expected [server] or [catalog])");
      }
      continue;
    }

    auto eq = trimmed.find('=');
    if (eq == std::string::npos) {
      return set_err(line_no, "expected key = value");
    }

    if (cur_section.empty()) {
      return set_err(line_no,
                      "key/value pair must be inside a table header");
    }

    std::string key = Trim(std::string_view(trimmed).substr(0, eq));
    std::string val = Trim(std::string_view(trimmed).substr(eq + 1));
    if (key.empty()) return set_err(line_no, "missing key before '='");

    auto parse_uint = [&](const std::string& v, const char* what)
        -> Result<uint64_t> {
      uint64_t parsed = 0;
      const char* begin = v.data();
      const char* end = v.data() + v.size();
      auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);
      if (ec != std::errc{} || ptr != end) {
        return Result<uint64_t>(Status::InvalidArgument(
            std::string("expected integer for ") + what));
      }
      return Result<uint64_t>(parsed);
    };

    auto parse_sync_policy = [&](const std::string& v) -> Result<SyncPolicy> {
      std::string token = v;
      if (!token.empty() && token.front() == '"') {
        auto ps = ParseStringLiteral(token);
        if (!ps.ok) {
          return Result<SyncPolicy>(Status::InvalidArgument(ps.error));
        }
        token = ps.value;
      }
      if (token == "always") return Result<SyncPolicy>(SyncPolicy::kAlways);
      if (token == "every_ms") return Result<SyncPolicy>(SyncPolicy::kEveryMs);
      if (token == "never") return Result<SyncPolicy>(SyncPolicy::kNever);
      return Result<SyncPolicy>(Status::InvalidArgument(
          "wal_sync must be one of: \"always\", \"every_ms\", \"never\""));
    };

    if (cur_section == "server") {
      if (key == "host") {
        if (host_set) return set_err(line_no, "duplicate key: server.host");
        auto ps = ParseStringLiteral(val);
        if (!ps.ok) {
          return set_err(line_no, "server.host: " + ps.error);
        }
        if (ps.value.empty()) {
          return set_err(line_no, "server.host must be non-empty");
        }
        out.server.host = std::move(ps.value);
        host_set = true;
        continue;
      }
      if (key == "port") {
        if (port_set) return set_err(line_no, "duplicate key: server.port");
        auto r = parse_uint(val, "server.port");
        if (!r.ok()) {
          return set_err(line_no,
                          "server.port: expected integer 0..65535");
        }
        uint64_t p = r.value();
        if (p > 65535) {
          return set_err(line_no, "server.port must be in range 0..65535");
        }
        out.server.port = static_cast<uint16_t>(p);
        port_set = true;
        continue;
      }
      return set_err(line_no, "unknown key '" + key + "' in section [server]");
    }

    if (cur_section == "catalog") {
      if (key == "data_dir") {
        if (data_dir_set)
          return set_err(line_no, "duplicate key: catalog.data_dir");
        auto ps = ParseStringLiteral(val);
        if (!ps.ok) return set_err(line_no, "catalog.data_dir: " + ps.error);
        if (ps.value.empty()) {
          return set_err(line_no, "catalog.data_dir must be non-empty");
        }
        out.catalog.data_dir = std::move(ps.value);
        data_dir_set = true;
        continue;
      }
      if (key == "wal_sync") {
        if (wal_sync_set)
          return set_err(line_no, "duplicate key: catalog.wal_sync");
        // TOML requires quoted strings in standard usage; we allow bare tokens
        // too for nicer configs, but still validate.
        if (!val.empty() && val.front() != '"') {
          if (!IsBareToken(val)) {
            return set_err(line_no,
                            "catalog.wal_sync: expected string or bare token");
          }
        }
        auto r = parse_sync_policy(val);
        if (!r.ok()) return set_err(line_no, "catalog.wal_sync: " + r.status().message());
        out.catalog.wal_sync = r.value();
        wal_sync_set = true;
        continue;
      }
      if (key == "memtable_flush_bytes") {
        if (memtable_flush_bytes_set)
          return set_err(line_no,
                          "duplicate key: catalog.memtable_flush_bytes");
        auto r = parse_uint(val, "memtable_flush_bytes");
        if (!r.ok()) return set_err(line_no, "catalog.memtable_flush_bytes: expected integer > 0");
        uint64_t v = r.value();
        if (v == 0) {
          return set_err(line_no, "catalog.memtable_flush_bytes must be > 0");
        }
        out.catalog.memtable_flush_bytes = static_cast<size_t>(v);
        memtable_flush_bytes_set = true;
        continue;
      }
      if (key == "compaction_tier_threshold") {
        if (compaction_tier_threshold_set)
          return set_err(line_no,
                          "duplicate key: catalog.compaction_tier_threshold");
        auto r = parse_uint(val, "compaction_tier_threshold");
        if (!r.ok())
          return set_err(line_no,
                          "catalog.compaction_tier_threshold: expected integer > 0");
        uint64_t v = r.value();
        if (v == 0) {
          return set_err(line_no, "catalog.compaction_tier_threshold must be > 0");
        }
        out.catalog.compaction_tier_threshold = static_cast<size_t>(v);
        compaction_tier_threshold_set = true;
        continue;
      }
      if (key == "max_segments_before_compact") {
        if (max_segments_before_compact_set)
          return set_err(line_no,
                          "duplicate key: catalog.max_segments_before_compact");
        auto r = parse_uint(val, "max_segments_before_compact");
        if (!r.ok())
          return set_err(line_no,
                          "catalog.max_segments_before_compact: expected integer > 0");
        uint64_t v = r.value();
        if (v == 0) {
          return set_err(line_no, "catalog.max_segments_before_compact must be > 0");
        }
        out.catalog.max_segments_before_compact = static_cast<size_t>(v);
        max_segments_before_compact_set = true;
        continue;
      }
      return set_err(line_no, "unknown key '" + key + "' in section [catalog]");
    }

    return set_err(line_no, "unknown parser error");
  }

  return Result<TomlConfig>(std::move(out));
}

}  // namespace

Result<TomlConfig> LoadTomlConfigText(std::string_view source_name,
                                       std::string_view toml_text) {
  // current parser is intentionally strict: it only supports the knobs we
  // actually wire into server start paths.
  return ParseToml(source_name, toml_text);
}

Result<TomlConfig> LoadTomlConfigFile(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return Result<TomlConfig>(Status::IoError("failed to open config: " + path));
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  auto res = LoadTomlConfigText(path, buffer.str());
  if (!res.ok()) return res;
  return res;
}

}  // namespace cli
}  // namespace aster

