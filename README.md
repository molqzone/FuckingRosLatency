# FuckingRosLatency

这个仓库只做一件事：对比 ROS 2 与 LibXR 在图像消息链路上的延迟和 CPU。

当前口径是两组一一对应：

- ROS 2 `intra-process`
  对标 LibXR 普通 `Topic`
- ROS 2 多进程 pub/sub
  对标 `LibXR::LinuxSharedTopic`

所有测试都在图像帧填充完成后再打时间戳，所以结果不包含帧填充时间。

## 目录

```text
auto_bench_image_latency.sh
auto_bench_libxr_image_latency.sh
README.md
README_en.md
libxr_tp_test/
  CMakeLists.txt
  libxr/
  libxr_tp_test.cpp
src/
  image_latency_test/
```

## ROS 2 基准

`src/image_latency_test/` 提供两条 ROS 2 基准线：

- 多进程 pub/sub
  发布节点和订阅节点分开运行，统计 `stamp -> subscriber callback`
- `intra_process_image_latency`
  发布和订阅在同一进程，统计 `stamp -> subscriber callback`

运行脚本：

```bash
source /opt/ros/humble/setup.bash
export WS=$HOME/ros2_ws
./auto_bench_image_latency.sh
```

可选环境变量：

- `WS`
- `DURATION`
- `RATE`
- `WIDTH`
- `HEIGHT`

## LibXR 基准

`libxr_tp_test/libxr_tp_test.cpp` 同时包含两条 LibXR 基准线：

1. 普通 `Topic`
   同进程 callback subscriber，统计 `Publish -> Callback`
2. `LinuxSharedTopic`
   父进程 publish，子进程 `Wait(data)`，统计 `Publish -> Wait OK`

运行脚本：

```bash
source /opt/ros/humble/setup.bash
export WS=$HOME/ros2_ws
./auto_bench_libxr_image_latency.sh
```

脚本会：

- 构建或复用 `build/libxr_tp_test/libxr_tp_test`
- 抽取 `[RESULT]` 行
- 用 `pidstat -C libxr_tp_test` 聚合整个 LibXR benchmark 运行期 CPU

## 最新结果

ROS 2 数据：

- GitHub Actions
  `https://github.com/Jiu-xiao/FuckingRosLatency/actions/runs/19598484409/job/56126494824`

LibXR 数据：

- Ubuntu24 实跑
  `/home/xiao/runs/fuck_ros_cleanup_20260413T232545Z`

### 1440×1080

| 路径 | 指标 | 结果 |
|---|---|---|
| ROS 2 多进程 | sub latency | `1.779 ms` |
| ROS 2 `intra-process` | latency | `0.026 ms` |
| LibXR `Topic` | `Publish -> Callback` | `0.641 us` |
| LibXR `LinuxSharedTopic` | `Publish -> Wait OK` | `93.983 us` |

### 320×240

| 路径 | 指标 | 结果 |
|---|---|---|
| ROS 2 多进程 | sub latency | `0.222 ms` |
| ROS 2 `intra-process` | latency | `0.024 ms` |
| LibXR `Topic` | `Publish -> Callback` | `0.563 us` |
| LibXR `LinuxSharedTopic` | `Publish -> Wait OK` | `62.151 us` |

### CPU

| 路径 | 结果 |
|---|---|
| ROS 2 多进程 1440×1080 | pub `4.76 %`, sub `3.45 %` |
| ROS 2 `intra-process` 1440×1080 | `1.38 %` |
| ROS 2 多进程 320×240 | pub `0.59 %`, sub `0.48 %` |
| ROS 2 `intra-process` 320×240 | `0.31 %` |
| LibXR benchmark 总 CPU | `1.52 %` |

## 结论

- 进程内对比里，普通 `Topic` 明显快于 ROS 2 `intra-process`
- 跨进程对比里，`LinuxSharedTopic` 明显快于 ROS 2 多进程链路
- `LinuxSharedTopic` 的目标不是对标 ROS 2 `intra-process`
- 普通 `Topic` 和 `LinuxSharedTopic` 分别覆盖进程内、跨进程两类场景
