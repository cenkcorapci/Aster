#include <gtest/gtest.h>

#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "aster/platform/memory_storage.h"
#include "aster/platform/posix_storage.h"
#include "aster/platform/s3_fake.h"
#include "aster/platform/s3_storage.h"

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

TEST(PosixStorage, RejectsPathTraversal) {
  const std::string root = ::testing::TempDir() + "/aster_posix_trav";
  ::mkdir(root.c_str(), 0755);
  PosixStorage store(root);
  EXPECT_FALSE(store.Put("../escape", "x").ok());
  EXPECT_FALSE(store.Put("a/../../etc/passwd", "x").ok());
  EXPECT_FALSE(store.Put("/etc/passwd", "x").ok());
  EXPECT_FALSE(store.Exists("../escape"));
  EXPECT_FALSE(store.Read("../escape").ok());
  EXPECT_FALSE(store.Remove("foo/../bar").ok());
  ASSERT_TRUE(store.Put("safe/nested.dat", "ok").ok());
  EXPECT_EQ(store.Read("safe/nested.dat").value(), "ok");
}

// Integration-style test: S3Storage ↔ in-process FakeS3 (no AWS / LocalStack).
TEST(S3Storage, PutGetListRemoveAgainstFakeS3) {
  FakeS3Server::Options opt;
  opt.bucket = "aster-test";
  FakeS3Server fake(opt);
  ASSERT_TRUE(fake.Start().ok());
  ASSERT_GT(fake.port(), 0);

  S3Config cfg;
  cfg.endpoint = fake.endpoint();
  cfg.bucket = fake.bucket();
  cfg.path_style = true;
  S3Storage store(cfg);

  ASSERT_TRUE(store.Put("a/x", "one").ok());
  ASSERT_TRUE(store.Put("a/y", "two").ok());
  ASSERT_TRUE(store.Put("b/z", "three").ok());

  EXPECT_TRUE(store.Exists("a/x"));
  EXPECT_FALSE(store.Exists("missing"));

  auto got = store.Read("a/x");
  ASSERT_TRUE(got.ok()) << got.status().message();
  EXPECT_EQ(got.value(), "one");

  auto listed = store.List("a/");
  ASSERT_TRUE(listed.ok()) << listed.status().message();
  ASSERT_EQ(listed.value().size(), 2u);
  EXPECT_EQ(listed.value()[0], "a/x");
  EXPECT_EQ(listed.value()[1], "a/y");

  ASSERT_TRUE(store.Remove("a/x").ok());
  EXPECT_FALSE(store.Exists("a/x"));
  auto missing = store.Read("a/x");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.status().code(), StatusCode::kNotFound);

  auto rm_missing = store.Remove("a/x");
  EXPECT_FALSE(rm_missing.ok());
  EXPECT_EQ(rm_missing.code(), StatusCode::kNotFound);

  // Overwrite
  ASSERT_TRUE(store.Put("a/y", "two-b").ok());
  EXPECT_EQ(store.Read("a/y").value(), "two-b");

  fake.Stop();
}

TEST(S3Storage, RejectsEmptyKeyAndBadEndpoint) {
  S3Config cfg;
  cfg.endpoint = "http://127.0.0.1:1";
  cfg.bucket = "b";
  S3Storage store(cfg);
  EXPECT_FALSE(store.Put("", "x").ok());
  EXPECT_FALSE(store.Read("").ok());

  S3Config bad;
  bad.endpoint = "not-a-url";
  bad.bucket = "b";
  S3Storage broken(bad);
  EXPECT_FALSE(broken.Put("k", "v").ok());
}

}  // namespace
}  // namespace aster
