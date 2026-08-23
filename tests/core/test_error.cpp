#include "optronic/core/expected.hpp"

#include <gtest/gtest.h>

using namespace optronic;

namespace {

expected<int> parse_gain(int raw) {
  if (raw < 0 || raw > 4095)
    return fail(Code::cfg_range, "parse_gain");
  return raw;
}

expected<int> apply_gain(int gain) {
  if (gain == 0)
    return fail(Code::not_ready, "apply_gain");
  return gain * 2;
}

} // namespace

TEST(Error, DomainIsEncodedInTheCode) {
  EXPECT_EQ(domain_of(Code::cfg_range), 0x0200u);
  EXPECT_EQ(domain_of(Code::hal_map), 0x0300u);
  EXPECT_EQ(domain_of(Code::mqtt_connect), 0x0600u);
}

TEST(Expected, PropagatesThroughTwoLayers) {
  auto ok = parse_gain(256).and_then(apply_gain);
  ASSERT_TRUE(ok);
  EXPECT_EQ(*ok, 512);

  auto bad = parse_gain(9999).and_then(apply_gain);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().code, Code::cfg_range);
  EXPECT_EQ(bad.error().where, "parse_gain"); // the inner frame, not the outer
}

TEST(Expected, TransformKeepsTheError) {
  auto r = parse_gain(-1).transform([](int v) { return v + 1; });
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, Code::cfg_range);
}
