#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
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

class IntraImagePipelineNode : public rclcpp::Node
{
public:
  explicit IntraImagePipelineNode(const rclcpp::NodeOptions & options)
  : rclcpp::Node("intra_process_image_latency", options),
    width_(kWidth1440),
    height_(kHeight1440),
    publish_rate_(30.0),
    seq_(0),
    count_(0),
    latency_sum_ms_(0.0)
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
        "[intra] unsupported size %dx%d, only %dx%d and %dx%d are supported for fixed-size benchmark",
        width_, height_, kWidth1440, kHeight1440, kWidth320, kHeight320);
      throw;
    }

    auto qos = rclcpp::SensorDataQoS();
    rclcpp::PublisherOptionsWithAllocator<std::allocator<void>> pub_options;
    pub_options.use_intra_process_comm = rclcpp::IntraProcessSetting::Enable;

    if (mode_ == BenchMode::Mode1440) {
      publisher_1440_ = this->create_publisher<image_latency_test::msg::BenchImage1440>(
        "test_image_intra", qos, pub_options);
      subscription_1440_ = this->create_subscription<image_latency_test::msg::BenchImage1440>(
        "test_image_intra", qos,
        std::bind(&IntraImagePipelineNode::on_image_1440, this, std::placeholders::_1));
    } else {
      publisher_320_ = this->create_publisher<image_latency_test::msg::BenchImage320>(
        "test_image_intra", qos, pub_options);
      subscription_320_ = this->create_subscription<image_latency_test::msg::BenchImage320>(
        "test_image_intra", qos,
        std::bind(&IntraImagePipelineNode::on_image_320, this, std::placeholders::_1));
    }

    auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / publish_rate_));
    timer_ = this->create_wall_timer(period, std::bind(&IntraImagePipelineNode::on_timer, this));

    RCLCPP_INFO(
      this->get_logger(),
      "[intra] fixed-size+unique_ptr mode width=%d height=%d rate=%.2f Hz",
      width_, height_, publish_rate_);
  }

private:
  template<typename MessageT>
  void publish_unique_message(rclcpp::Publisher<MessageT> & publisher)
  {
    auto msg = std::make_unique<MessageT>();

    msg->seq = seq_;
    std::memset(msg->data.data(), static_cast<int>(seq_ & 0xFFU), msg->data.size());
    msg->stamp = this->now();

    publisher.publish(std::move(msg));
    ++seq_;
  }

  void on_timer()
  {
    if (mode_ == BenchMode::Mode1440) {
      publish_unique_message(*publisher_1440_);
    } else {
      publish_unique_message(*publisher_320_);
    }
  }

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
        "[intra] frames=%llu avg=%.3f ms last=%.3f ms",
        static_cast<unsigned long long>(count_), avg, latency_ms);
    }
  }

  void on_image_1440(image_latency_test::msg::BenchImage1440::UniquePtr msg)
  {
    update_latency(msg->stamp);
  }

  void on_image_320(image_latency_test::msg::BenchImage320::UniquePtr msg)
  {
    update_latency(msg->stamp);
  }

  int width_;
  int height_;
  double publish_rate_;
  BenchMode mode_;
  std::uint32_t seq_;
  std::uint64_t count_;
  double latency_sum_ms_;

  rclcpp::Publisher<image_latency_test::msg::BenchImage1440>::SharedPtr publisher_1440_;
  rclcpp::Publisher<image_latency_test::msg::BenchImage320>::SharedPtr publisher_320_;
  rclcpp::Subscription<image_latency_test::msg::BenchImage1440>::SharedPtr subscription_1440_;
  rclcpp::Subscription<image_latency_test::msg::BenchImage320>::SharedPtr subscription_320_;
  rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  rclcpp::NodeOptions options;
  options.use_intra_process_comms(true);

  auto node = std::make_shared<IntraImagePipelineNode>(options);
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}
