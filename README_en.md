# FuckingRosLatency

This repository compares image-message latency and CPU usage for ROS 2 and LibXR.

The comparison is organized into two corresponding pairs:

- ROS 2 `intra-process`
  corresponding to ordinary LibXR `Topic`
- ROS 2 multi-process pub/sub
  corresponding to `LibXR::LinuxSharedTopic`

The default build mode is `Release`. All benchmarks timestamp frames after frame fill completes, so the reported numbers exclude frame-fill cost.

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

Script behavior:

- builds or reuses `install/libxr_bench/bin/libxr_bench` in `Release`
- prints `[RESULT]` lines
- measures total benchmark CPU usage with `pidstat -C libxr_bench`

## Latest Results

ROS 2 data:

- GitHub Actions
  `https://github.com/Jiu-xiao/FuckingRosLatency/actions/runs/24376643892/job/71191462775`

LibXR data comes from the same GitHub Actions run.

### 1440×1080

| Path | Metric | Result |
|---|---|---|
| ROS 2 multi-process | sub latency | `3.118 ms` |
| ROS 2 `intra-process` | latency | `0.027 ms` |
| LibXR `Topic` | `Publish -> Callback` | `0.718 us` |
| LibXR `LinuxSharedTopic` | `Publish -> Wait OK` | `46.010 us` |

### 320×240

| Path | Metric | Result |
|---|---|---|
| ROS 2 multi-process | sub latency | `0.253 ms` |
| ROS 2 `intra-process` | latency | `0.025 ms` |
| LibXR `Topic` | `Publish -> Callback` | `0.872 us` |
| LibXR `LinuxSharedTopic` | `Publish -> Wait OK` | `42.532 us` |

### CPU

| Path | Result |
|---|---|
| ROS 2 multi-process 1440×1080 | pub `8.76 %`, sub `4.07 %` |
| ROS 2 `intra-process` 1440×1080 | `1.07 %` |
| ROS 2 multi-process 320×240 | pub `0.62 %`, sub `0.59 %` |
| ROS 2 `intra-process` 320×240 | `0.31 %` |
| LibXR benchmark total CPU | `1.12 %` |

## Conclusions

- For in-process messaging, ordinary `Topic` shows lower latency than ROS 2 `intra-process`
- For cross-process messaging, `LinuxSharedTopic` shows lower latency than ROS 2 multi-process pub/sub
- `LinuxSharedTopic` is not intended to replace ROS 2 `intra-process`
- The two LibXR paths correspond to two different deployment classes: in-process and cross-process
