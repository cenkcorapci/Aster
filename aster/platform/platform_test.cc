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

TEST(MemoryStorage, MissingReadAndRemove) {
  MemoryStorage store;
  EXPECT_FALSE(store.Exists("nope"));
  auto got = store.Read("nope");
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.status().code(), StatusCode::kNotFound);
  auto rm = store.Remove("nope");
  EXPECT_FALSE(rm.ok());
  EXPECT_EQ(rm.code(), StatusCode::kNotFound);
}

TEST(MemoryStorage, OverwriteAndEmptyList) {
  MemoryStorage store;
  ASSERT_TRUE(store.Put("k", "v1").ok());
  ASSERT_TRUE(store.Put("k", "v2").ok());
  EXPECT_EQ(store.Read("k").value(), "v2");
  auto listed = store.List("zzz");
  ASSERT_TRUE(listed.ok());
  EXPECT_TRUE(listed.value().empty());
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

TEST(PosixStorage, MissingAndListAndOverwrite) {
  const std::string root = ::testing::TempDir() + "/aster_posix_root2";
  ::mkdir(root.c_str(), 0755);
  PosixStorage store(root);
  EXPECT_FALSE(store.Exists("missing"));
  EXPECT_FALSE(store.Read("missing").ok());
  EXPECT_FALSE(store.Remove("missing").ok());

  ASSERT_TRUE(store.Put("a.dat", "1").ok());
  ASSERT_TRUE(store.Put("a.dat", "2").ok());
  EXPECT_EQ(store.Read("a.dat").value(), "2");
  ASSERT_TRUE(store.Put("b.dat", "b").ok());
  auto listed = store.List("a");
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value().size(), 1u);
  EXPECT_EQ(listed.value()[0], "a.dat");
}

TEST(PosixStorage, TrailingSlashRoot) {
  const std::string root = ::testing::TempDir() + "/aster_posix_slash/";
  ::mkdir(root.c_str(), 0755);
  PosixStorage store(root);
  ASSERT_TRUE(store.Put("x", "y").ok());
  EXPECT_TRUE(store.Exists("x"));
}

}  // namespace
}  // namespace aster
