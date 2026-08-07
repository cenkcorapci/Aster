#include <gtest/gtest.h>

#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "aster/platform/memory_storage.h"
#include "aster/platform/posix_storage.h"

namespace aster {
namespace {

TEST(MemoryStorage, PutGetListRemove) {
  MemoryStorage store;
  ASSERT_TRUE(store.Put("a/x", "one").ok());
  ASSERT_TRUE(store.Put("a/y", "two").ok());
  ASSERT_TRUE(store.Put("b/z", "three").ok());
  EXPECT_TRUE(store.Exists("a/x"));
  auto got = store.Read("a/x");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), "one");
  auto listed = store.List("a/");
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed.value().size(), 2u);
  ASSERT_TRUE(store.Remove("a/x").ok());
  EXPECT_FALSE(store.Exists("a/x"));
}

TEST(PosixStorage, PutGetRoundTrip) {
  const std::string root = ::testing::TempDir() + "/aster_posix_root";
  ::mkdir(root.c_str(), 0755);
  PosixStorage store(root);
  ASSERT_TRUE(store.Put("segments/seg1.ast", "payload").ok());
  EXPECT_TRUE(store.Exists("segments/seg1.ast"));
  auto got = store.Read("segments/seg1.ast");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), "payload");
  ASSERT_TRUE(store.Remove("segments/seg1.ast").ok());
}

}  // namespace
}  // namespace aster
