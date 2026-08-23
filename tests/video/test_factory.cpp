// The graph a configuration produces is checked as text. No GStreamer, no
// hardware, no display - so these run in CI on any machine and a change to the
// element choice shows up as a diff instead of as a field surprise.

#include "optronic/video/spec.hpp"

#include <gtest/gtest.h>

using namespace optronic::video;

TEST(Factory, HostGraphIsTheExpectedText) {
  PipelineSpec s;
  s.source.width = 1280;
  s.source.height = 720;
  s.source.fps = 30;

  EXPECT_EQ(launch_string(s),
            "videotestsrc is-live=true pattern=0 ! "
            "video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 ! "
            "queue leaky=downstream max-size-buffers=4 max-size-bytes=0 max-size-time=0 ! "
            "appsink name=out sync=false max-buffers=1 drop=true emit-signals=false"
            "  "
            "appsrc name=in is-live=true format=time block=false "
            "caps=video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 ! "
            "queue leaky=downstream max-size-buffers=4 max-size-bytes=0 max-size-time=0 ! "
            "x264enc bitrate=4000 key-int-max=30 tune=zerolatency speed-preset=ultrafast ! "
            "h264parse ! "
            "rtph264pay config-interval=1 pt=96 ! udpsink host=127.0.0.1 port=5600 sync=false "
            "async=false");
}

TEST(Factory, EncoderIsAConfigurationChoiceNotAnIfdef) {
  EXPECT_EQ(encoder_element(EncoderKind::h264_sw), "x264enc");
  EXPECT_EQ(encoder_element(EncoderKind::h264_omx), "omxh264enc");
  EXPECT_EQ(encoder_element(EncoderKind::h264_vcu), "vvas_xvcuenc");
}

// The hardware encoders take bit/s where x264enc takes kbit/s. Getting this
// wrong is a factor of 1000 that only shows on the target.
TEST(Factory, HardwareEncoderBitrateIsConvertedToBitsPerSecond) {
  PipelineSpec s;
  s.encoder.kind = EncoderKind::h264_vcu;
  s.encoder.bitrate_kbps = 4000;

  const std::string g = launch_string(s);
  EXPECT_NE(g.find("vvas_xvcuenc target-bitrate=4000000"), std::string::npos) << g;
  EXPECT_EQ(g.find("bitrate=4000 "), std::string::npos) << g;
}

TEST(Factory, V4l2SourceCarriesTheDeviceAndDropsIsLive) {
  PipelineSpec s;
  s.source.kind = SourceKind::v4l2;
  s.source.device = "/dev/video2";

  const std::string g = launch_string(s);
  EXPECT_NE(g.find("v4l2src device=/dev/video2"), std::string::npos) << g;
  EXPECT_EQ(g.find("videotestsrc"), std::string::npos) << g;
}

// A camera negotiates its own format; the graph converts rather than demanding
// NV12 from a sensor that cannot produce it.
TEST(Factory, V4l2SourceConvertsToTheTargetFormat) {
  PipelineSpec s;
  s.source.kind = SourceKind::v4l2;

  const std::string g = launch_string(s);
  const auto src = g.find("v4l2src");
  const auto conv = g.find("videoconvert");
  ASSERT_NE(conv, std::string::npos) << g;
  EXPECT_LT(conv, g.find("appsink")) << "convert must precede the sink";

  // Whatever caps the camera is asked for must not name a pixel format: a
  // sensor delivers YUY2 or MJPEG and would simply fail to negotiate NV12.
  const std::string negotiated = g.substr(src, conv - src);
  EXPECT_EQ(negotiated.find("format="), std::string::npos) << negotiated;
  EXPECT_NE(negotiated.find("width=1280"), std::string::npos) << negotiated;
}

TEST(Factory, TestPatternIsSelectable) {
  PipelineSpec s;
  s.source.pattern = 18; // moving ball
  EXPECT_NE(launch_string(s).find("pattern=18"), std::string::npos);
}

TEST(Factory, NonLeakyQueueIsRequestable) {
  PipelineSpec s;
  s.queues.leaky = false;
  s.queues.max_buffers = 16;

  const std::string g = launch_string(s);
  EXPECT_NE(g.find("queue leaky=no max-size-buffers=16"), std::string::npos) << g;
}

TEST(Factory, SameSpecProducesTheSameGraph) {
  const PipelineSpec a;
  const PipelineSpec b;
  EXPECT_EQ(launch_string(a), launch_string(b));
}
