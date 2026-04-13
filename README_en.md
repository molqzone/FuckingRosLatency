# FuckingRosLatency

Compare the **latency** and **CPU usage** of the ROS 2 image transport pipeline and `LibXR::LinuxSharedTopic` shared-memory IPC.  
All tests use RGB image frames as payload, with a publish rate of 30 Hz, compiled with `-O3`.

---

## Repository Layout

Assume the workspace root is `ros2_ws`:

```text
ros2_ws/
├── auto_bench_image_latency.sh        # ROS 2 image latency benchmark script
├── auto_bench_libxr_image_latency.sh  # LibXR image latency benchmark script
├── build/                             # colcon build output
├── install/                           # colcon install space
├── src/
│   └── image_latency_test/            # ROS 2 test package
└── libxr_tp_test/
    ├── CMakeLists.txt                 # LibXR test package
    ├── libxr/                         # LibXR source (cloned from upstream repo)
    └── libxr_tp_test.cpp              # LibXR LinuxSharedTopic image benchmark program
```

---

## Environment & Build

### Dependencies

Reference environment (CI):

- Ubuntu 22.04
- ROS 2 Humble (`ros-humble-ros-base`)
- `python3-colcon-common-extensions`
- `build-essential`, `cmake`
- `sysstat` (for `pidstat`)
- `pkg-config`
- `libudev-dev`, `libwpa-client-dev`, `libnm-dev` (LibXR dependencies)

Example installation (simplified):

```bash
sudo apt-get update
sudo apt-get install -y curl gnupg2 lsb-release

# ROS 2 apt repository
sudo curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key   -o /usr/share/keyrings/ros-archive-keyring.gpg

echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] http://packages.ros.org/ros2/ubuntu $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/ros2.list > /dev/null

sudo apt-get update
sudo apt-get install -y   ros-humble-ros-base   python3-colcon-common-extensions   build-essential   cmake   sysstat   pkg-config   libudev-dev   libwpa-client-dev   libnm-dev
```

LibXR source must be cloned into `libxr_tp_test/libxr`:

```bash
cd ros2_ws/libxr_tp_test
git clone --depth 1 https://github.com/Jiu-xiao/libxr.git libxr
```

### Build

```bash
cd ros2_ws
source /opt/ros/humble/setup.bash

colcon build
# or:
# colcon build --packages-select image_latency_test libxr_tp_test
```

After a successful build you should have:

- ROS 2 nodes: `install/image_latency_test/lib/image_latency_test/`
- LibXR benchmark: `build/libxr_tp_test/libxr_tp_test`

---

## Running the Benchmarks

Both scripts use the following environment variables:

- `WS`: workspace path (default `$HOME/ros2_ws`)
- `DURATION`: runtime for each mode in seconds (default 30)
- `RATE` / `WIDTH` / `HEIGHT`: publish rate and resolution (only affects ROS 2 tests)

Typical usage:

```bash
cd ros2_ws
source /opt/ros/humble/setup.bash
export WS=$(pwd)

# ROS 2: 1440x1080 @ 30 Hz
./auto_bench_image_latency.sh

# ROS 2: 320x240 @ 30 Hz
WIDTH=320 HEIGHT=240 ./auto_bench_image_latency.sh

# LibXR: internally tests 1440x1080 / 320x240 in sequence
./auto_bench_libxr_image_latency.sh
```

All logs are written to `ros2_ws/logs/`. The scripts print `[RESULT]` summary lines to the console.

---

## Implementation Overview

### ROS 2

Package `image_latency_test`:

- `image_publisher_node`
  - Publishes `sensor_msgs/msg/Image` (`rgb8`) on topic `test_image`.
  - On startup, pre-allocates a read-only image buffer (for the target resolution) and reuses it every cycle.
  - In the timer callback:
    1. Constructs a `sensor_msgs::msg::Image` and copies one frame from the pre-generated buffer into `msg.data`;
    2. **After the copy finishes**, sets `msg.header.stamp = now()`;
    3. Immediately calls `publisher_->publish(std::move(msg))`.
  - Therefore, the `now() - msg.header.stamp` computed on the subscriber side only covers:
    - ROS 2 communication path (multi-process / intra-process) and scheduling overhead,
    - and **does not include** the data copy time from the pre-generated buffer into the message.

- `image_subscriber_node`
  - Subscribes to `test_image`.
  - In the callback, reads `msg.header.stamp`, computes `now() - msg.header.stamp`, and periodically prints the average latency.
  - This latency reflects only the time **from timestamping on the publisher side to subscriber callback execution**.

- `intra_process_image_latency`
  - Publisher and subscriber live in the same process.
  - Explicitly enables intra-process communication.
  - The publisher uses `std::unique_ptr<Image>` and the subscriber uses `Image::UniquePtr`.
  - The timestamping order is kept identical to the multi-process version: **copy first, then stamp, then publish**.  
    As a result, the measured latency is the cost of the intra-process path itself and still excludes the frame copy time.

Script `auto_bench_image_latency.sh`:

- Runs the modes in order: multi-process pub/sub → intra-process single process.
- Uses `pidstat -u 1 -p <PID>` to record CPU usage of the pub, sub, and intra processes/instances separately.

---

### LibXR

Program `libxr_tp_test.cpp` (single executable) directly benchmarks the cross-process path of `LibXR::LinuxSharedTopic<Frame>` for two resolutions in sequence:

- For each frame type (1440×1080 / 320×240), it creates one shared topic;
- the parent process acts as publisher, and a forked child process acts as synchronous subscriber;
- the publisher sends `NUM_FRAMES = 300` frames at 30 Hz, and for each frame:
  1. calls `CreateData()` to allocate one shared payload slot;
  2. fills RGB888 image data for the target resolution;
  3. writes `pub_ns = now()` **after the fill is complete**;
  4. calls `Publish()`.

Therefore, the measured **Publish -> Wait OK** latency **excludes the frame-fill cost**.

In the subscriber child process:

- it loops on `subscriber.Wait(data)`;
- validates the frame sequence, resolution, and first/last payload bytes;
- computes one-way latency as `now() - pub_ns`.

Script `auto_bench_libxr_image_latency.sh`:

- uses `pidstat -C libxr_tp_test` to capture all same-name parent/child processes;
- aggregates `%CPU` by time slice so the reported CPU is the total cost of the whole shared-topic pipeline;
- extracts `[RESULT]` lines from the benchmark logs and summarizes latency and CPU usage.

---

## Test Configuration

ROS 2 results come from the [latest GitHub Actions run](https://github.com/Jiu-xiao/FuckingRosLatency/actions/runs/19598484409/job/56126494824). LibXR LinuxSharedTopic results come from the latest Ubuntu24 rerun:

- `/home/xiao/runs/fuck_ros_shared_topic_20260413T223805Z`

Unified configuration:

- OS: Ubuntu 22.04 (GitHub-hosted runner)
- ROS: Humble
- Frame rate: 30 Hz
- Runtime per mode: 30 s
- Frames per resolution:
  - ROS 2: ~900 frames (30 Hz × 30 s)
  - LibXR: fixed 300 frames / resolution
- Resolutions:
  - 1440×1080
  - 320×240

---

## Results & Comparison

### Resolution 1440×1080

#### ROS 2

Units: latency in milliseconds (ms), CPU in percent.

| Mode                  | Metric                   | Value                                         |
| --------------------- | ------------------------ | --------------------------------------------- |
| Multi-process pub/sub | sub latency (900 frames) | avg = **1.779 ms** (min ≈ 1.551, max ≈ 2.021) |
|                       | pub CPU (29 samples)     | avg = **4.76 %**                              |
|                       | sub CPU (29 samples)     | avg = **3.45 %**                              |
| Intra-process single  | latency (900 frames)     | avg = **0.026 ms** (min ≈ 0.019, max ≈ 0.036) |
|                       | CPU (29 samples)         | avg = **1.38 %**                              |

Observations:

- For large image frames, the multi-process pipeline includes serialization/deserialization, IPC, and scheduling overhead, resulting in an average latency of about 1.8 ms.
- Enabling intra-process reduces latency to ~26 µs and noticeably lowers CPU usage.

#### LibXR

Units: latency in microseconds (µs), CPU in percent.

| Metric                                          | Value |
| ----------------------------------------------- | ----- |
| LinuxSharedTopic latency (Publish → Wait OK)    | count=300, avg = **89.090 µs** (min=21.675, max=127.630) |

Key points:

- For 1440×1080 frames, cross-process `LinuxSharedTopic` delivery is about **89 µs**.
- This is much lower than ROS 2 multi-process (**1.779 ms**).
- The direct peer here is ROS 2 **multi-process**, because both paths keep process isolation.
- ROS 2 `intra-process` numbers remain in the table as a reference baseline, but that category maps to ordinary in-process `Topic`-style messaging rather than to this shared-topic benchmark.

---

### Resolution 320×240

#### ROS 2

| Mode                  | Metric                   | Value                                         |
| --------------------- | ------------------------ | --------------------------------------------- |
| Multi-process pub/sub | sub latency (900 frames) | avg = **0.222 ms** (min ≈ 0.184, max ≈ 0.331) |
|                       | pub CPU (29 samples)     | avg = **0.59 %**                              |
|                       | sub CPU (29 samples)     | avg = **0.48 %**                              |
| Intra-process single  | latency (900 frames)     | avg = **0.024 ms** (min ≈ 0.020, max ≈ 0.046) |
|                       | CPU (29 samples)         | avg = **0.31 %**                              |

Compared to 1440×1080:

- With smaller images, both latency and CPU usage in multi-process mode decrease significantly.
- Intra-process latency at both resolutions is similar, around ~25 µs, indicating that its main cost is a single copy, only weakly dependent on resolution.

#### LibXR

| Metric                                          | Value |
| ----------------------------------------------- | ----- |
| LinuxSharedTopic latency (Publish → Wait OK)    | count=300, avg = **73.584 µs** (min=18.503, max=117.085) |

We can see:

- After reducing the resolution to 320×240, `LinuxSharedTopic` latency drops to **73.6 µs**.
- It remains lower than ROS 2 multi-process (**0.222 ms**).
- This is still a cross-process path, so it is not presented here as a same-category comparison against ROS 2 `intra-process`.

Combined LibXR CPU across the whole run (publisher parent + subscriber child):

- `libxr_linux_shared_topic CPU(total)`: samples=10, avg = **1.80 %**

---

### Overall Comparison & Conclusions

By communication mode, we can roughly summarize:

1. **ROS 2 multi-process**
   - Latency: from ~0.22 ms (320×240) to ~1.78 ms (1440×1080), strongly dependent on resolution.
   - CPU: for large images, pub/sub together use around 8%; for small images, under 1%.
   - Suitable when process isolation is required and millisecond-level latency and higher CPU cost are acceptable.

2. **ROS 2 intra-process**
   - Latency: ~0.02–0.03 ms at both resolutions.
   - CPU: ~0.31% for 320×240; ~1.38% for 1440×1080.
   - Eliminates IPC and extra scheduling overhead; this is the in-process low-latency option within the ROS 2 framework.
   - This category is a better peer for ordinary in-process `Topic`-style messaging, not for `LinuxSharedTopic`.

3. **LibXR LinuxSharedTopic (cross-process shared memory)**
   - Latency: about **89 µs** at 1440×1080 and **74 µs** at 320×240.
   - Its direct peer is ROS 2 multi-process, not ROS 2 intra-process.
   - It is clearly lower than ROS 2 multi-process while still preserving process isolation.
   - CPU: about **1.80 %** total for publisher + subscriber across the whole benchmark run.
   - The reported number is **Publish -> subscriber-process Wait OK**, with the start timestamp taken after frame fill completes.

This repository can serve as a base for further experiments, for example:

- Comparing ROS 2 multi-process / intra-process against shared-memory IPC under more boundary conditions;
- Evaluating higher frame rates and larger image sizes;
- Building longer processing pipelines and measuring end-to-end behavior.
