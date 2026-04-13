#include "libxr.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
using Micro = std::chrono::duration<double, std::micro>;

static constexpr uint32_t NUM_FRAMES = 300;
static constexpr double PUBLISH_RATE_HZ = 30.0;

struct LatencyStats
{
  double min_us = std::numeric_limits<double>::max();
  double max_us = 0.0;
  double sum_us = 0.0;
  uint32_t count = 0;

  void Add(double us)
  {
    if (us < min_us)
    {
      min_us = us;
    }
    if (us > max_us)
    {
      max_us = us;
    }
    sum_us += us;
    ++count;
  }

  void Log(const char* label) const
  {
    if (count == 0)
    {
      std::printf("[RESULT] %s: no samples\n", label);
      return;
    }

    const double avg_us = sum_us / static_cast<double>(count);
    std::printf("[RESULT] %s: count=%u avg=%.3f us min=%.3f us max=%.3f us\n", label, count,
                avg_us, min_us, max_us);
  }
};

struct ChildResult
{
  LatencyStats stats = {};
  uint32_t status = 0;
};

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
  for (int retry = 0; retry < 500 && topic.GetSubscriberNum() == 0; ++retry)
  {
    usleep(1000);
  }
  return topic.GetSubscriberNum() > 0;
}

template <uint32_t W, uint32_t H>
struct BenchImageFrameT
{
  static constexpr uint32_t WIDTH = W;
  static constexpr uint32_t HEIGHT = H;

  uint32_t width;
  uint32_t height;
  uint32_t seq;
  uint64_t pub_ns;
  uint8_t data[W * H * 3];
};

using BenchImageFrame1440 = BenchImageFrameT<1440, 1080>;
using BenchImageFrame320 = BenchImageFrameT<320, 240>;

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

template <typename Frame>
void RunTopicCase(const char* label, const char* topic_name, const char* domain_name)
{
  struct CallbackContext
  {
    LatencyStats stats = {};
    std::atomic<uint32_t> received_count{0};
    uint32_t status = 0;
  };

  CallbackContext ctx = {};
  LibXR::Topic::Domain domain(domain_name);
  auto topic =
      LibXR::Topic::CreateTopic<Frame>(topic_name, &domain, false, false, false);

  auto callback = LibXR::Topic::Callback::Create(
      [](bool, CallbackContext* cb_ctx, LibXR::RawData& raw)
      {
        const auto* frame = reinterpret_cast<const Frame*>(raw.addr_);
        const uint32_t received = cb_ctx->received_count.load(std::memory_order_acquire);
        if (!ValidateFrame(frame, received))
        {
          cb_ctx->status = 1;
          return;
        }

        cb_ctx->stats.Add(static_cast<double>(NowNs() - frame->pub_ns) / 1000.0);
        cb_ctx->received_count.fetch_add(1, std::memory_order_release);
      },
      &ctx);
  topic.RegisterCallback(callback);

  const auto period = std::chrono::duration<double>(1.0 / PUBLISH_RATE_HZ);
  Frame frame = {};
  for (uint32_t seq = 0; seq < NUM_FRAMES; ++seq)
  {
    FillFrame(&frame, seq);
    topic.Publish(frame);
    std::this_thread::sleep_for(period);
  }

  for (int retry = 0; retry < 500 &&
                      ctx.received_count.load(std::memory_order_acquire) < NUM_FRAMES &&
                      ctx.status == 0;
       ++retry)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (ctx.status != 0 ||
      ctx.received_count.load(std::memory_order_acquire) != NUM_FRAMES)
  {
    std::printf("[RESULT] %s: callback failed count=%u status=%u\n", label,
                ctx.received_count.load(std::memory_order_acquire), ctx.status);
    return;
  }

  ctx.stats.Log(label);
}

template <typename Frame>
void RunSharedTopicCase(const char* label, const char* topic_name)
{
  using Topic = LibXR::LinuxSharedTopic<Frame>;
  using Subscriber = typename Topic::SyncSubscriber;
  using Data = typename Topic::Data;

  int ready_pipe[2] = {-1, -1};
  int stats_pipe[2] = {-1, -1};
  if (pipe(ready_pipe) != 0 || pipe(stats_pipe) != 0)
  {
    std::printf("[RESULT] %s: pipe failed\n", label);
    return;
  }

  LibXR::LinuxSharedTopicConfig config = {};
  config.slot_num = 4;
  config.subscriber_num = 1;
  config.queue_num = 4;

  (void)Topic::Remove(topic_name);

  Topic publisher(topic_name, config);
  if (!publisher.Valid())
  {
    std::printf("[RESULT] %s: publisher init failed\n", label);
    return;
  }

  pid_t child = fork();
  if (child < 0)
  {
    std::printf("[RESULT] %s: fork failed\n", label);
    return;
  }

  if (child == 0)
  {
    close(ready_pipe[0]);
    close(stats_pipe[0]);

    Subscriber subscriber(topic_name);
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
    for (uint32_t expected_seq = 0; expected_seq < NUM_FRAMES; ++expected_seq)
    {
      Data data;
      if (subscriber.Wait(data, 5000) != LibXR::ErrorCode::OK)
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
    std::printf("[RESULT] %s: subscriber attach failed\n", label);
    return;
  }
  close(ready_pipe[0]);

  const auto period = std::chrono::duration<double>(1.0 / PUBLISH_RATE_HZ);
  for (uint32_t seq = 0; seq < NUM_FRAMES; ++seq)
  {
    Data data;
    while (publisher.CreateData(data) != LibXR::ErrorCode::OK)
    {
      std::this_thread::yield();
    }

    FillFrame(data.GetData(), seq);

    if (publisher.Publish(data) != LibXR::ErrorCode::OK)
    {
      std::printf("[RESULT] %s: publish failed at seq=%u\n", label, seq);
      kill(child, SIGKILL);
      waitpid(child, nullptr, 0);
      close(stats_pipe[0]);
      (void)Topic::Remove(topic_name);
      return;
    }

    std::this_thread::sleep_for(period);
  }

  ChildResult result = {};
  const bool read_ok = ReadAll(stats_pipe[0], &result, sizeof(result));
  close(stats_pipe[0]);

  int status = 0;
  waitpid(child, &status, 0);
  if (!read_ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || result.status != 0)
  {
    std::printf("[RESULT] %s: subscriber failed status=%u exit=%d\n", label, result.status,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    (void)Topic::Remove(topic_name);
    return;
  }

  result.stats.Log(label);
  (void)Topic::Remove(topic_name);
}

int main()
{
  LibXR::PlatformInit();

  RunTopicCase<BenchImageFrame1440>("Topic latency (Publish -> Callback) 1440x1080",
                                    "bench/topic_1440", "bench_topic_domain");
  RunSharedTopicCase<BenchImageFrame1440>(
      "LinuxSharedTopic latency (Publish -> Wait OK) 1440x1080", "bench/linux_shared_1440");
  RunTopicCase<BenchImageFrame320>("Topic latency (Publish -> Callback) 320x240",
                                   "bench/topic_320", "bench_topic_domain");
  RunSharedTopicCase<BenchImageFrame320>(
      "LinuxSharedTopic latency (Publish -> Wait OK) 320x240", "bench/linux_shared_320");
  return 0;
}
