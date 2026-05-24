#include <cstdint>
#include <functional>
#include <stdexcept>

#include <rclcpp/rclcpp.hpp>
#include <builtin_interfaces/msg/time.hpp>

#include "image_latency_test/msg/bench_image1440.hpp"
#include "image_latency_test/msg/bench_image320.hpp"

namespace
{
constexpr int kWidth1440 = 1440;
constexpr int kHeight1440 = 1080;
constexpr int kWidth320 = 320;
constexpr int kHeight320 = 240;

enum class BenchMode
{
  Mode1440,
  Mode320
};

BenchMode resolve_mode(int width, int height)
{
  if (width == kWidth1440 && height == kHeight1440) {
    return BenchMode::Mode1440;
  }
  if (width == kWidth320 && height == kHeight320) {
    return BenchMode::Mode320;
  }
  throw std::runtime_error("unsupported resolution");
}
}  // namespace

class MultiImageSubscriberNode : public rclcpp::Node
{
public:
  MultiImageSubscriberNode()
  : rclcpp::Node("image_subscriber_node"),
    width_(kWidth1440),
    height_(kHeight1440),
    count_(0),
    latency_sum_ms_(0.0)
  {
    this->declare_parameter<int>("width", width_);
    this->declare_parameter<int>("height", height_);
    this->get_parameter("width", width_);
    this->get_parameter("height", height_);

    try {
      mode_ = resolve_mode(width_, height_);
    } catch (const std::exception &) {
      RCLCPP_FATAL(
        this->get_logger(),
        "[multi_sub] unsupported size %dx%d, only %dx%d and %dx%d are supported for fixed-size benchmark",
        width_, height_, kWidth1440, kHeight1440, kWidth320, kHeight320);
      throw;
    }

    auto qos = rclcpp::SensorDataQoS();
    if (mode_ == BenchMode::Mode1440) {
      subscription_1440_ = this->create_subscription<image_latency_test::msg::BenchImage1440>(
        "test_image",
        qos,
        std::bind(&MultiImageSubscriberNode::on_image_1440, this, std::placeholders::_1));
    } else {
      subscription_320_ = this->create_subscription<image_latency_test::msg::BenchImage320>(
        "test_image",
        qos,
        std::bind(&MultiImageSubscriberNode::on_image_320, this, std::placeholders::_1));
    }

    RCLCPP_INFO(
      this->get_logger(),
      "[multi_sub] fixed-size+loaned mode listening on test_image (%dx%d)",
      width_, height_);
  }

private:
  void update_latency(const builtin_interfaces::msg::Time & stamp)
  {
    auto now = this->now();
    auto dt = now - rclcpp::Time(stamp);
    double latency_ms = dt.seconds() * 1000.0;

    ++count_;
    latency_sum_ms_ += latency_ms;

    if (count_ % 100 == 0) {
      double avg = latency_sum_ms_ / static_cast<double>(count_);
      RCLCPP_INFO(
        this->get_logger(),
        "[multi_sub] frames=%llu avg=%.3f ms last=%.3f ms",
        static_cast<unsigned long long>(count_),
        avg,
        latency_ms);
    }
  }

  void on_image_1440(const image_latency_test::msg::BenchImage1440::ConstSharedPtr msg)
  {
    update_latency(msg->stamp);
  }

  void on_image_320(const image_latency_test::msg::BenchImage320::ConstSharedPtr msg)
  {
    update_latency(msg->stamp);
  }

  int width_;
  int height_;
  BenchMode mode_;
  rclcpp::Subscription<image_latency_test::msg::BenchImage1440>::SharedPtr subscription_1440_;
  rclcpp::Subscription<image_latency_test::msg::BenchImage320>::SharedPtr subscription_320_;
  std::uint64_t count_;
  double latency_sum_ms_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<MultiImageSubscriberNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
