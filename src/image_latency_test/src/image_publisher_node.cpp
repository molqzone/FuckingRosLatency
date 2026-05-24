#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>

#include <rclcpp/rclcpp.hpp>

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

class MultiImagePublisherNode : public rclcpp::Node
{
public:
  MultiImagePublisherNode()
  : rclcpp::Node("image_publisher_node"),
    width_(kWidth1440),
    height_(kHeight1440),
    publish_rate_(30.0),
    seq_(0)
  {
    this->declare_parameter<int>("width", width_);
    this->declare_parameter<int>("height", height_);
    this->declare_parameter<double>("publish_rate", publish_rate_);

    this->get_parameter("width", width_);
    this->get_parameter("height", height_);
    this->get_parameter("publish_rate", publish_rate_);

    try {
      mode_ = resolve_mode(width_, height_);
    } catch (const std::exception &) {
      RCLCPP_FATAL(
        this->get_logger(),
        "[multi_pub] unsupported size %dx%d, only %dx%d and %dx%d are supported for fixed-size benchmark",
        width_, height_, kWidth1440, kHeight1440, kWidth320, kHeight320);
      throw;
    }

    auto qos = rclcpp::SensorDataQoS();
    if (mode_ == BenchMode::Mode1440) {
      publisher_1440_ =
        this->create_publisher<image_latency_test::msg::BenchImage1440>("test_image", qos);
    } else {
      publisher_320_ =
        this->create_publisher<image_latency_test::msg::BenchImage320>("test_image", qos);
    }

    auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / publish_rate_));

    timer_ = this->create_wall_timer(
      period,
      std::bind(&MultiImagePublisherNode::on_timer, this));

    RCLCPP_INFO(
      this->get_logger(),
      "[multi_pub] fixed-size+loaned mode width=%d height=%d rate=%.2f Hz",
      width_, height_, publish_rate_);
  }

private:
  template<typename MessageT>
  void publish_loaned_message(rclcpp::Publisher<MessageT> & publisher)
  {
    auto loaned = publisher.borrow_loaned_message();
    auto & msg = loaned.get();

    msg.seq = seq_;
    std::memset(msg.data.data(), static_cast<int>(seq_ & 0xFFU), msg.data.size());
    msg.stamp = this->now();

    publisher.publish(std::move(loaned));
    ++seq_;
  }

  void on_timer()
  {
    if (mode_ == BenchMode::Mode1440) {
      publish_loaned_message(*publisher_1440_);
    } else {
      publish_loaned_message(*publisher_320_);
    }
  }

  int width_;
  int height_;
  double publish_rate_;
  BenchMode mode_;
  std::uint32_t seq_;

  rclcpp::Publisher<image_latency_test::msg::BenchImage1440>::SharedPtr publisher_1440_;
  rclcpp::Publisher<image_latency_test::msg::BenchImage320>::SharedPtr publisher_320_;
  rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<MultiImagePublisherNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
