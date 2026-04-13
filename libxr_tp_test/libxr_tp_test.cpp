#include "libxr.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

using Clock = std::chrono::steady_clock;

constexpr uint32_t kNumFrames = 300;
constexpr double kPublishRateHz = 30.0;
constexpr uint32_t kSubscriberAttachRetry = 500;
constexpr uint32_t kSubscriberAttachSleepUs = 1000;
constexpr uint32_t kTopicDrainRetry = 500;
constexpr uint32_t kTopicDrainSleepMs = 10;
constexpr uint32_t kSubscriberWaitMs = 5000;

struct LatencyStats
{
  double min_us = std::numeric_limits<double>::max();
  double max_us = 0.0;
  double sum_us = 0.0;
  uint32_t count = 0;

  void Add(double us)
  {
    min_us = (us < min_us) ? us : min_us;
    max_us = (us > max_us) ? us : max_us;
    sum_us += us;
    ++count;
  }

  void PrintResult(const char* label) const
  {
    if (count == 0)
    {
      std::printf("[RESULT] %s: no samples\n", label);
      return;
    }

    std::printf("[RESULT] %s: count=%u avg=%.3f us min=%.3f us max=%.3f us\n", label, count,
                sum_us / static_cast<double>(count), min_us, max_us);
  }
};

struct ChildResult
{
  LatencyStats stats = {};
  uint32_t status = 0;
};

template <uint32_t W, uint32_t H>
struct BenchImageFrame
{
  static constexpr uint32_t WIDTH = W;
  static constexpr uint32_t HEIGHT = H;

  uint32_t width;
  uint32_t height;
  uint32_t seq;
  uint64_t pub_ns;
  uint8_t data[W * H * 3];
};

using BenchImageFrame1440 = BenchImageFrame<1440, 1080>;
using BenchImageFrame320 = BenchImageFrame<320, 240>;

template <typename Frame>
struct BenchCase
{
  const char* topic_label;
  const char* shared_topic_label;
  const char* topic_name;
  const char* domain_name;
  const char* shared_topic_name;
};

template <typename Frame>
constexpr BenchCase<Frame> MakeBenchCase(const char* topic_label,
                                         const char* shared_topic_label,
                                         const char* topic_name,
                                         const char* domain_name,
                                         const char* shared_topic_name)
{
  return {topic_label, shared_topic_label, topic_name, domain_name, shared_topic_name};
}

constexpr BenchCase<BenchImageFrame1440> kBenchCase1440 = MakeBenchCase<BenchImageFrame1440>(
    "Topic latency (Publish -> Callback) 1440x1080",
    "LinuxSharedTopic latency (Publish -> Wait OK) 1440x1080", "bench/topic_1440",
    "bench_topic_domain_1440", "bench/linux_shared_1440");

constexpr BenchCase<BenchImageFrame320> kBenchCase320 = MakeBenchCase<BenchImageFrame320>(
    "Topic latency (Publish -> Callback) 320x240",
    "LinuxSharedTopic latency (Publish -> Wait OK) 320x240", "bench/topic_320",
    "bench_topic_domain_320", "bench/linux_shared_320");

uint64_t NowNs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

bool WriteAll(int fd, const void* buffer, size_t size)
{
  const auto* bytes = static_cast<const uint8_t*>(buffer);
  size_t written_total = 0;
  while (written_total < size)
  {
    const ssize_t written = write(fd, bytes + written_total, size - written_total);
    if (written > 0)
    {
      written_total += static_cast<size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
    {
      continue;
    }
    return false;
  }
  return true;
}

bool ReadAll(int fd, void* buffer, size_t size)
{
  auto* bytes = static_cast<uint8_t*>(buffer);
  size_t read_total = 0;
  while (read_total < size)
  {
    const ssize_t read_size = read(fd, bytes + read_total, size - read_total);
    if (read_size > 0)
    {
      read_total += static_cast<size_t>(read_size);
      continue;
    }
    if (read_size < 0 && errno == EINTR)
    {
      continue;
    }
    return false;
  }
  return true;
}

template <typename TopicType>
bool WaitForSubscriberAttach(TopicType& topic)
{
  for (uint32_t retry = 0; retry < kSubscriberAttachRetry && topic.GetSubscriberNum() == 0; ++retry)
  {
    usleep(kSubscriberAttachSleepUs);
  }
  return topic.GetSubscriberNum() > 0;
}

template <typename Frame>
void FillFrame(Frame* frame, uint32_t seq)
{
  frame->width = Frame::WIDTH;
  frame->height = Frame::HEIGHT;
  frame->seq = seq;
  std::memset(frame->data, static_cast<int>(seq & 0xFFU), sizeof(frame->data));
  frame->pub_ns = NowNs();
}

template <typename Frame>
bool ValidateFrame(const Frame* frame, uint32_t expected_seq)
{
  if (frame == nullptr)
  {
    return false;
  }

  if (frame->width != Frame::WIDTH || frame->height != Frame::HEIGHT || frame->seq != expected_seq)
  {
    return false;
  }

  const uint8_t expected_byte = static_cast<uint8_t>(expected_seq & 0xFFU);
  return frame->data[0] == expected_byte &&
         frame->data[(sizeof(frame->data) / sizeof(frame->data[0])) - 1U] == expected_byte;
}

void PrintFailure(const char* label, const char* reason)
{
  std::printf("[RESULT] %s: %s\n", label, reason);
}

template <typename Frame>
void RunTopicCase(const BenchCase<Frame>& bench_case)
{
  struct CallbackContext
  {
    LatencyStats stats = {};
    std::atomic<uint32_t> received_count{0};
    std::atomic<uint32_t> status{0};
  };

  CallbackContext ctx = {};
  LibXR::Topic::Domain domain(bench_case.domain_name);
  auto topic =
      LibXR::Topic::CreateTopic<Frame>(bench_case.topic_name, &domain, false, false, false);

  auto callback = LibXR::Topic::Callback::Create(
      [](bool, CallbackContext* callback_ctx, LibXR::RawData& raw)
      {
        const auto* frame = reinterpret_cast<const Frame*>(raw.addr_);
        const uint32_t expected_seq =
            callback_ctx->received_count.load(std::memory_order_acquire);
        if (!ValidateFrame(frame, expected_seq))
        {
          callback_ctx->status.store(1, std::memory_order_release);
          return;
        }

        callback_ctx->stats.Add(static_cast<double>(NowNs() - frame->pub_ns) / 1000.0);
        callback_ctx->received_count.fetch_add(1, std::memory_order_release);
      },
      &ctx);
  topic.RegisterCallback(callback);

  const auto period = std::chrono::duration<double>(1.0 / kPublishRateHz);
  Frame frame = {};
  for (uint32_t seq = 0; seq < kNumFrames; ++seq)
  {
    FillFrame(&frame, seq);
    topic.Publish(frame);
    std::this_thread::sleep_for(period);
  }

  for (uint32_t retry = 0;
       retry < kTopicDrainRetry && ctx.status.load(std::memory_order_acquire) == 0 &&
       ctx.received_count.load(std::memory_order_acquire) < kNumFrames;
       ++retry)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(kTopicDrainSleepMs));
  }

  if (ctx.status.load(std::memory_order_acquire) != 0)
  {
    PrintFailure(bench_case.topic_label, "callback validation failed");
    return;
  }

  if (ctx.received_count.load(std::memory_order_acquire) != kNumFrames)
  {
    PrintFailure(bench_case.topic_label, "callback did not receive all frames");
    return;
  }

  ctx.stats.PrintResult(bench_case.topic_label);
}

template <typename Frame>
void RunSharedTopicCase(const BenchCase<Frame>& bench_case)
{
  using Topic = LibXR::LinuxSharedTopic<Frame>;
  using Subscriber = typename Topic::SyncSubscriber;
  using Data = typename Topic::Data;

  int ready_pipe[2] = {-1, -1};
  int stats_pipe[2] = {-1, -1};
  if (pipe(ready_pipe) != 0 || pipe(stats_pipe) != 0)
  {
    PrintFailure(bench_case.shared_topic_label, "pipe failed");
    return;
  }

  LibXR::LinuxSharedTopicConfig config = {};
  config.slot_num = 4;
  config.subscriber_num = 1;
  config.queue_num = 4;

  (void)Topic::Remove(bench_case.shared_topic_name);

  Topic publisher(bench_case.shared_topic_name, config);
  if (!publisher.Valid())
  {
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    close(stats_pipe[0]);
    close(stats_pipe[1]);
    PrintFailure(bench_case.shared_topic_label, "publisher init failed");
    return;
  }

  pid_t child = fork();
  if (child < 0)
  {
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    close(stats_pipe[0]);
    close(stats_pipe[1]);
    PrintFailure(bench_case.shared_topic_label, "fork failed");
    return;
  }

  if (child == 0)
  {
    close(ready_pipe[0]);
    close(stats_pipe[0]);

    Subscriber subscriber(bench_case.shared_topic_name);
    if (!subscriber.Valid())
    {
      _exit(2);
    }

    const uint8_t ready = 1;
    if (!WriteAll(ready_pipe[1], &ready, sizeof(ready)))
    {
      _exit(3);
    }
    close(ready_pipe[1]);

    ChildResult result = {};
    for (uint32_t expected_seq = 0; expected_seq < kNumFrames; ++expected_seq)
    {
      Data data;
      if (subscriber.Wait(data, kSubscriberWaitMs) != LibXR::ErrorCode::OK)
      {
        result.status = 1;
        break;
      }

      const Frame* frame = data.GetData();
      if (!ValidateFrame(frame, expected_seq))
      {
        result.status = 2;
        break;
      }

      result.stats.Add(static_cast<double>(NowNs() - frame->pub_ns) / 1000.0);
    }

    (void)WriteAll(stats_pipe[1], &result, sizeof(result));
    close(stats_pipe[1]);
    _exit(0);
  }

  close(ready_pipe[1]);
  close(stats_pipe[1]);

  uint8_t ready = 0;
  if (!ReadAll(ready_pipe[0], &ready, sizeof(ready)) || !WaitForSubscriberAttach(publisher))
  {
    close(ready_pipe[0]);
    close(stats_pipe[0]);
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    (void)Topic::Remove(bench_case.shared_topic_name);
    PrintFailure(bench_case.shared_topic_label, "subscriber attach failed");
    return;
  }
  close(ready_pipe[0]);

  const auto period = std::chrono::duration<double>(1.0 / kPublishRateHz);
  for (uint32_t seq = 0; seq < kNumFrames; ++seq)
  {
    Data data;
    while (publisher.CreateData(data) != LibXR::ErrorCode::OK)
    {
      std::this_thread::yield();
    }

    FillFrame(data.GetData(), seq);
    if (publisher.Publish(data) != LibXR::ErrorCode::OK)
    {
      close(stats_pipe[0]);
      kill(child, SIGKILL);
      waitpid(child, nullptr, 0);
      (void)Topic::Remove(bench_case.shared_topic_name);
      PrintFailure(bench_case.shared_topic_label, "publish failed");
      return;
    }

    std::this_thread::sleep_for(period);
  }

  ChildResult result = {};
  const bool read_ok = ReadAll(stats_pipe[0], &result, sizeof(result));
  close(stats_pipe[0]);

  int status = 0;
  waitpid(child, &status, 0);
  (void)Topic::Remove(bench_case.shared_topic_name);

  if (!read_ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || result.status != 0)
  {
    PrintFailure(bench_case.shared_topic_label, "subscriber failed");
    return;
  }

  result.stats.PrintResult(bench_case.shared_topic_label);
}

}  // namespace

int main()
{
  LibXR::PlatformInit();

  RunTopicCase(kBenchCase1440);
  RunSharedTopicCase(kBenchCase1440);
  RunTopicCase(kBenchCase320);
  RunSharedTopicCase(kBenchCase320);
  return 0;
}
