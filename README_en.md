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

Program `libxr_tp_test.cpp` (single executable) now contains two LibXR benchmark lines and runs both for two resolutions (1440×1080 / 320×240):

1. **Ordinary `Topic` in-process path**
   - creates an ordinary `Topic<Frame>`;
   - registers a callback subscriber in the same process;
   - publishes frames after writing `pub_ns = now()`;
   - measures **Publish -> Callback** latency.

2. **`LinuxSharedTopic` cross-process path**
   - creates a shared-memory topic;
   - the parent process acts as publisher and a forked child acts as synchronous subscriber;
   - publishes frames after writing `pub_ns = now()`;
   - measures **Publish -> Wait OK** latency in the subscriber process.

Both LibXR benchmark lines timestamp the frame **after fill completes**, so the reported latencies **exclude frame-fill cost**.

Script `auto_bench_libxr_image_latency.sh`:

- uses `pidstat -C libxr_tp_test` to capture the benchmark process set;
- aggregates `%CPU` by time slice so the reported CPU is the total cost of the whole LibXR benchmark run;
- extracts `[RESULT]` lines from the benchmark logs and summarizes latency and CPU usage.

---

## Test Configuration

ROS 2 results come from the [latest GitHub Actions run](https://github.com/Jiu-xiao/FuckingRosLatency/actions/runs/19598484409/job/56126494824). The latest dual LibXR benchmark results come from Ubuntu24:

- `/home/xiao/runs/fuck_ros_dual_bench_20260413T230047Z`

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
| Topic latency (Publish → Callback)              | count=300, avg = **0.619 µs** (min=0.201, max=2.409) |
| LinuxSharedTopic latency (Publish → Wait OK)    | count=300, avg = **93.536 µs** (min=18.556, max=135.709) |

Key points:

- For 1440×1080 frames:
  - ordinary `Topic` in-process callback latency is about **0.62 µs**;
  - cross-process `LinuxSharedTopic` latency is about **93.5 µs**.
- The pairing should be read as:
  - ordinary `Topic` vs ROS `intra-process`;
  - `LinuxSharedTopic` vs ROS multi-process.

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
| Topic latency (Publish → Callback)              | count=300, avg = **0.637 µs** (min=0.291, max=1.184) |
| LinuxSharedTopic latency (Publish → Wait OK)    | count=300, avg = **72.121 µs** (min=13.066, max=227.219) |

We can see:

- After reducing the resolution to 320×240:
  - ordinary `Topic` in-process callback latency is about **0.64 µs**;
  - `LinuxSharedTopic` latency is about **72.1 µs**.
- The same pairing still applies:
  - ordinary `Topic` for in-process comparison;
  - `LinuxSharedTopic` for cross-process comparison.

Combined LibXR CPU across the whole benchmark run:

- `libxr_bench CPU(total)`: samples=21, avg = **1.48 %**

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
   - This category is the direct peer for ordinary in-process `Topic`-style messaging.

3. **LibXR ordinary `Topic` (in-process)**
   - Latency: about **0.6 µs** at both resolutions.
   - Its direct peer is ROS 2 `intra-process`.
   - The reported number is **Publish -> Callback**, with the start timestamp taken after frame fill completes.

4. **LibXR `LinuxSharedTopic` (cross-process shared memory)**
   - Latency: about **94 µs** at 1440×1080 and **72 µs** at 320×240.
   - Its direct peer is ROS 2 multi-process.
   - It is clearly lower than ROS 2 multi-process while still preserving process isolation.
   - The reported number is **Publish -> subscriber-process Wait OK**, again with the start timestamp taken after frame fill completes.

This repository can serve as a base for further experiments, for example:

- Comparing ROS 2 multi-process / intra-process against both LibXR benchmark lines under more boundary conditions;
- Evaluating higher frame rates and larger image sizes;
- Building longer processing pipelines and measuring end-to-end behavior.
