# FuckingRosLatency

This repository benchmarks image-message latency and CPU usage for ROS 2 and LibXR.

The comparison is intentionally split into two matched pairs:

- ROS 2 `intra-process`
  vs ordinary LibXR `Topic`
- ROS 2 multi-process pub/sub
  vs `LibXR::LinuxSharedTopic`

All benchmarks timestamp frames after frame fill completes, so the reported numbers exclude frame-fill cost.

## Layout

```text
auto_bench_image_latency.sh
auto_bench_libxr_image_latency.sh
README.md
README_en.md
src/
  image_latency_test/
  libxr_bench/
    CMakeLists.txt
    package.xml
    libxr/
    main.cpp
```

## ROS 2 Benchmarks

`src/image_latency_test/` provides two ROS 2 paths:

- multi-process pub/sub
  measures `stamp -> subscriber callback`
- `intra_process_image_latency`
  measures `stamp -> subscriber callback` in one process

Run:

```bash
source /opt/ros/humble/setup.bash
export WS=$HOME/ros2_ws
./auto_bench_image_latency.sh
```

Optional environment variables:

- `WS`
- `DURATION`
- `RATE`
- `WIDTH`
- `HEIGHT`

## LibXR Benchmarks

`src/libxr_bench/main.cpp` contains two LibXR paths:

1. ordinary `Topic`
   one-process callback subscriber, measures `Publish -> Callback`
2. `LinuxSharedTopic`
   parent publishes, child waits, measures `Publish -> Wait OK`

Run:

```bash
source /opt/ros/humble/setup.bash
export WS=$HOME/ros2_ws
./auto_bench_libxr_image_latency.sh
```

The script:

- builds or reuses `install/libxr_bench/bin/libxr_bench`
- prints `[RESULT]` lines
- aggregates total benchmark CPU with `pidstat -C libxr_bench`

## Latest Results

ROS 2 data:

- GitHub Actions
  `https://github.com/Jiu-xiao/FuckingRosLatency/actions/runs/19598484409/job/56126494824`

LibXR data:

- Ubuntu24 rerun
  `/home/xiao/runs/fuck_ros_structure_direct_20260413T234243Z`

### 1440×1080

| Path | Metric | Result |
|---|---|---|
| ROS 2 multi-process | sub latency | `1.779 ms` |
| ROS 2 `intra-process` | latency | `0.026 ms` |
| LibXR `Topic` | `Publish -> Callback` | `0.607 us` |
| LibXR `LinuxSharedTopic` | `Publish -> Wait OK` | `96.288 us` |

### 320×240

| Path | Metric | Result |
|---|---|---|
| ROS 2 multi-process | sub latency | `0.222 ms` |
| ROS 2 `intra-process` | latency | `0.024 ms` |
| LibXR `Topic` | `Publish -> Callback` | `0.551 us` |
| LibXR `LinuxSharedTopic` | `Publish -> Wait OK` | `61.550 us` |

### CPU

| Path | Result |
|---|---|
| ROS 2 multi-process 1440×1080 | pub `4.76 %`, sub `3.45 %` |
| ROS 2 `intra-process` 1440×1080 | `1.38 %` |
| ROS 2 multi-process 320×240 | pub `0.59 %`, sub `0.48 %` |
| ROS 2 `intra-process` 320×240 | `0.31 %` |
| LibXR benchmark total CPU | `1.45 %` |

## Takeaways

- For in-process messaging, ordinary `Topic` is the right LibXR peer to ROS 2 `intra-process`
- For cross-process messaging, `LinuxSharedTopic` is the right LibXR peer to ROS 2 multi-process pub/sub
- `LinuxSharedTopic` is not intended to compete with ROS 2 `intra-process`
- The two LibXR paths cover two different deployment classes: in-process and cross-process
