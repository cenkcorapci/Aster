#include <gtest/gtest.h>

#include "aster/cli/toml_config_loader.h"

namespace aster {
namespace cli {
namespace {

TEST(TomlConfigLoader, ParsesServerAndCatalogKnobs) {
  const std::string text = R"([server]
host = "127.0.0.1"
port = 9091

[catalog]
data_dir = "/tmp/aster"
wal_sync = "every_ms"
memtable_flush_bytes = 67108864
compaction_tier_threshold = 4
max_segments_before_compact = 8
)";

  auto res = LoadTomlConfigText("test.toml", text);
  ASSERT_TRUE(res.ok()) << res.status().message();

  ASSERT_TRUE(res.value().server.host.has_value());
  EXPECT_EQ(*res.value().server.host, "127.0.0.1");
  ASSERT_TRUE(res.value().server.port.has_value());
  EXPECT_EQ(*res.value().server.port, 9091);

  ASSERT_TRUE(res.value().catalog.data_dir.has_value());
  EXPECT_EQ(*res.value().catalog.data_dir, "/tmp/aster");

  ASSERT_TRUE(res.value().catalog.wal_sync.has_value());
  EXPECT_EQ(*res.value().catalog.wal_sync, SyncPolicy::kEveryMs);

  ASSERT_TRUE(res.value().catalog.memtable_flush_bytes.has_value());
  EXPECT_EQ(*res.value().catalog.memtable_flush_bytes, 64u << 20);
  ASSERT_TRUE(res.value().catalog.compaction_tier_threshold.has_value());
  EXPECT_EQ(*res.value().catalog.compaction_tier_threshold, 4u);
  ASSERT_TRUE(res.value().catalog.max_segments_before_compact.has_value());
  EXPECT_EQ(*res.value().catalog.max_segments_before_compact, 8u);
}

TEST(TomlConfigLoader, UnknownSectionReturnsClearError) {
  const std::string text = R"([nope]
h = 1
)";
  auto res = LoadTomlConfigText("test.toml", text);
  ASSERT_FALSE(res.ok());
  EXPECT_NE(res.status().message().find("test.toml:1:"), std::string::npos);
  EXPECT_NE(res.status().message().find("unknown table"), std::string::npos);
}

TEST(TomlConfigLoader, UnknownKeyReturnsClearErrorWithLine) {
  const std::string text = R"([server]
foo = "bar"
)";
  auto res = LoadTomlConfigText("test.toml", text);
  ASSERT_FALSE(res.ok());
  EXPECT_NE(res.status().message().find("test.toml:2:"), std::string::npos);
  EXPECT_NE(res.status().message().find("unknown key 'foo'"), std::string::npos);
}

TEST(TomlConfigLoader, BadPortRangeReturnsClearError) {
  const std::string text = R"([server]
port = 70000
)";
  auto res = LoadTomlConfigText("test.toml", text);
  ASSERT_FALSE(res.ok());
  EXPECT_NE(res.status().message().find("server.port"), std::string::npos);
  EXPECT_NE(res.status().message().find("0..65535"), std::string::npos);
}

TEST(TomlConfigLoader, MalformedKeyValueReturnsClearError) {
  const std::string text = R"([server]
host
)";
  auto res = LoadTomlConfigText("test.toml", text);
  ASSERT_FALSE(res.ok());
  EXPECT_NE(res.status().message().find("expected key = value"),
            std::string::npos);
}

TEST(TomlConfigLoader, UnterminatedStringReturnsClearError) {
  const std::string text = R"([server]
host = "127.0.0.1
)";
  auto res = LoadTomlConfigText("test.toml", text);
  ASSERT_FALSE(res.ok());
  EXPECT_NE(res.status().message().find("server.host"),
            std::string::npos);
  EXPECT_NE(res.status().message().find("double-quoted"), std::string::npos);
}

TEST(TomlConfigLoader, InvalidWalSyncReturnsClearError) {
  const std::string text = R"([catalog]
data_dir = "/tmp/aster"
wal_sync = "nope"
)";
  auto res = LoadTomlConfigText("test.toml", text);
  ASSERT_FALSE(res.ok());
  EXPECT_NE(res.status().message().find("wal_sync must be one of"),
            std::string::npos);
}

}  // namespace
}  // namespace cli
}  // namespace aster

