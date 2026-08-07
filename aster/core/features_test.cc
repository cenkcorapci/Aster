#include <gtest/gtest.h>

#include "aster/core/features.h"

namespace aster {
namespace {

TEST(Features, ActiveProfileIsValid) {
  const Profile p = ActiveProfile();
  EXPECT_TRUE(p == Profile::kTiny || p == Profile::kEdge ||
              p == Profile::kServer);
}

TEST(Features, TinyDisablesHnsw) {
#if defined(ASTER_PROFILE_TINY)
  EXPECT_EQ(ActiveProfile(), Profile::kTiny);
  EXPECT_FALSE(HnswEnabled());
  EXPECT_FALSE(GossipEnabled());
  EXPECT_FALSE(PrometheusEnabled());
  EXPECT_FALSE(ReplicationEnabled());
  EXPECT_FALSE(CompressionEnabled());
#else
  EXPECT_TRUE(HnswEnabled());
  EXPECT_TRUE(GossipEnabled());
  EXPECT_TRUE(PrometheusEnabled());
  EXPECT_TRUE(ReplicationEnabled());
  EXPECT_TRUE(CompressionEnabled());
#endif
}

TEST(Features, DefaultIsServerWhenNoConfig) {
#if !defined(ASTER_PROFILE_TINY) && !defined(ASTER_PROFILE_EDGE)
  EXPECT_EQ(ActiveProfile(), Profile::kServer);
#endif
}

}  // namespace
}  // namespace aster
