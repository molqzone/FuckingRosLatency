# FuckingRosLatency

对比 ROS 2 图像传输链路与 `LibXR::LinuxSharedTopic` 共享内存 IPC 在 **延迟** 和 **CPU 占用** 上的表现。  
所有测试均以 RGB 图像帧为载荷，发布频率 30 Hz，编译使用 `-O3`。

---

## 仓库结构

假定工作空间根目录为 `ros2_ws`：

```text
ros2_ws/
├── auto_bench_image_latency.sh        # ROS 2 图像延迟基准脚本
├── auto_bench_libxr_image_latency.sh  # LibXR 图像延迟基准脚本
├── build/                             # colcon 构建输出
├── install/                           # colcon 安装空间
├── src/
│   └── image_latency_test/            # ROS 2 测试包
└── libxr_tp_test/
    ├── CMakeLists.txt                 # LibXR 测试包
    ├── libxr/                         # LibXR 源码（上游仓库克隆）
    └── libxr_tp_test.cpp              # LibXR LinuxSharedTopic 图像基准程序
```

---

## 环境与构建

### 依赖

参考环境（CI）：

- Ubuntu 22.04
- ROS 2 Humble（`ros-humble-ros-base`）
- `python3-colcon-common-extensions`
- `build-essential`, `cmake`
- `sysstat`（用于 `pidstat`）
- `pkg-config`
- `libudev-dev`, `libwpa-client-dev`, `libnm-dev`（LibXR 依赖）

安装示例（简化）：

```bash
sudo apt-get update
sudo apt-get install -y curl gnupg2 lsb-release

# ROS 2 源
sudo curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key \
  -o /usr/share/keyrings/ros-archive-keyring.gpg

echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] \
http://packages.ros.org/ros2/ubuntu $(lsb_release -cs) main" \
| sudo tee /etc/apt/sources.list.d/ros2.list > /dev/null

sudo apt-get update
sudo apt-get install -y \
  ros-humble-ros-base \
  python3-colcon-common-extensions \
  build-essential \
  cmake \
  sysstat \
  pkg-config \
  libudev-dev \
  libwpa-client-dev \
  libnm-dev
```

LibXR 源码需克隆到 `libxr_tp_test/libxr`：

```bash
cd ros2_ws/libxr_tp_test
git clone --depth 1 https://github.com/Jiu-xiao/libxr.git libxr
```

### 构建

```bash
cd ros2_ws
source /opt/ros/humble/setup.bash

colcon build
# 或：
# colcon build --packages-select image_latency_test libxr_tp_test
```

编译完成后：

- ROS 2 节点：`install/image_latency_test/lib/image_latency_test/`
- LibXR 基准：`build/libxr_tp_test/libxr_tp_test`

---

## 运行基准

脚本统一使用环境变量：

- `WS`：工作空间路径（默认 `$HOME/ros2_ws`）
- `DURATION`：每种模式运行时长（秒，默认 30）
- `RATE` / `WIDTH` / `HEIGHT`：发布帧率和分辨率（只影响 ROS 2 测试）

典型调用：

```bash
cd ros2_ws
source /opt/ros/humble/setup.bash
export WS=$(pwd)

# ROS 2：1440x1080 @ 30 Hz
./auto_bench_image_latency.sh

# ROS 2：320x240 @ 30 Hz
WIDTH=320 HEIGHT=240 ./auto_bench_image_latency.sh

# LibXR：内部依次测试 1440x1080 / 320x240
./auto_bench_libxr_image_latency.sh
```

所有日志输出到 `ros2_ws/logs/`，脚本会在控制台打印 `[RESULT]` 汇总行。

---

## 实现概览

### ROS 2

包 `image_latency_test`：

- `image_publisher_node`
  - 发布 `sensor_msgs/msg/Image`（`rgb8`），话题 `test_image`。
  - 启动时预生成一块只读图像缓冲区（对应目标分辨率），后续周期复用。
  - 在定时器回调中：
    1. 构造 `sensor_msgs::msg::Image`，从预生成缓冲区拷贝一帧到 `msg.data`；
    2. **完成拷贝后**，执行 `msg.header.stamp = now()`；
    3. 紧接着调用 `publisher_->publish(std::move(msg))`。
  - 因此订阅端计算的 `now() - msg.header.stamp` 只覆盖：
    - ROS 2 通信路径（多进程 / intra-process）和调度开销，
    - **不包含** 从预生成缓冲到消息的数据拷贝时间。

- `image_subscriber_node`
  - 订阅 `test_image`。
  - 回调中读取 `msg.header.stamp`，计算 `now() - msg.header.stamp`，周期性打印平均延迟。
  - 该延迟仅反映 **消息从发布端打时间戳到订阅回调触发** 的时间。

- `intra_process_image_latency`
  - 发布 / 订阅在同一进程中。
  - 显式启用 intra-process 通信。
  - 发布端使用 `std::unique_ptr<Image>`，订阅端使用 `Image::UniquePtr`。
  - 时间戳打点顺序与多进程版本保持一致：**先拷贝，后 stamp，再 publish**，因此测得的是 intra-process 路径本身的开销，同样不含帧拷贝时间。

脚本 `auto_bench_image_latency.sh`：

- 按“多进程 pub/sub → intra-process 单进程”顺序运行。
- 用 `pidstat -u 1 -p <PID>` 分别记录 pub、sub、intra 三个进程/实例的 CPU 占用。

---

### LibXR

程序 `libxr_tp_test.cpp`（单一可执行）直接测试 `LibXR::LinuxSharedTopic<Frame>` 的跨进程路径，对两个分辨率依次执行：

- 对每种帧类型（1440×1080 / 320×240）创建一个共享 topic；
- 父进程作为 publisher，子进程作为 synchronous subscriber；
- 主进程以 30 Hz 发送 `NUM_FRAMES = 300` 帧，每帧流程为：
  1. 调用 `CreateData()` 申请共享 payload；
  2. 填充对应分辨率的 RGB888 数据；
  3. **完成帧填充后** 写入 `pub_ns = now()`；
  4. 调用 `Publish()`。

因此，统计的 **Publish -> Wait OK** 延迟同样**不包含帧填充时间**。

子进程中：

- 循环调用 `subscriber.Wait(data)`；
- 校验帧号、分辨率和首尾像素；
- 用 `now() - pub_ns` 计算 **发布 → 订阅进程 Wait 成功** 的一程延迟。

脚本 `auto_bench_libxr_image_latency.sh`：

- 启动基准程序的同时，用 `pidstat -C libxr_tp_test` 统计同名父/子进程；
- 以时间片为单位聚合父、子两边的 `%CPU`，给出整个共享 topic 链路的总 CPU 占用；
- 从程序日志中抽取 `[RESULT]` 行，并汇总延迟和 CPU。

---

## 测试配置

ROS 2 结果来自 GitHub Actions 上[最近一次运行](https://github.com/Jiu-xiao/FuckingRosLatency/actions/runs/19598484409/job/56126494824)；LibXR LinuxSharedTopic 结果来自 Ubuntu24 主机 `MD-063744` 上的最新实跑：

- `/home/xiao/runs/fuck_ros_shared_topic_20260413T223805Z`

统一配置如下：

- OS：Ubuntu 22.04（GitHub 托管 runner）
- ROS：Humble
- 帧率：30 Hz
- 每种模式运行时长：30 s
- 每种分辨率测试帧数：
  - ROS 2：约 900 帧（30 Hz × 30 s）
  - LibXR：固定 300 帧 / 分辨率
- 分辨率：
  - 1440×1080
  - 320×240

---

## 结果与对比

### 1440×1080 分辨率

#### ROS 2

单位：延迟为毫秒（ms），CPU 为百分比。

| 模式                | 指标                          | 数值                            |
|---------------------|-------------------------------|---------------------------------|
| 多进程 pub/sub      | sub 延迟（900 帧）           | avg = **1.779 ms** (min ≈ 1.551, max ≈ 2.021) |
|                     | pub CPU（29 样本）           | avg = **4.76 %**                |
|                     | sub CPU（29 样本）           | avg = **3.45 %**                |
| intra-process 单进程 | 延迟（900 帧）               | avg = **0.026 ms** (min ≈ 0.019, max ≈ 0.036) |
|                     | CPU（29 样本）               | avg = **1.38 %**                |

观察：

- 对大图像帧，多进程模式的链路包含序列化 / 反序列化、进程间通信和调度开销，平均延迟约 1.8 ms。
- 启用 intra-process 后，延迟下降到 ~26 µs 量级，CPU 也明显降低。

#### LibXR

单位：延迟为微秒（µs），CPU 为百分比。

| 指标                                            | 数值 |
|-------------------------------------------------|------|
| LinuxSharedTopic latency (Publish -> Wait OK)   | count=300，avg = **89.090 µs** (min=21.675, max=127.630) |

关键点：

- 对 1440×1080 大图像，`LinuxSharedTopic` 的跨进程同步接收延迟约 **89 µs**。
- 这个数字明显低于 ROS 2 多进程的 **1.779 ms**；
- 这里的直接对标对象就是 ROS 2 **多进程**链路，因为两者都保留进程隔离。
- ROS 2 `intra-process` 结果仍然保留在表里，但它对应的是普通 `Topic` 这类进程内通信路径，不是当前共享 topic 基准的直接目标。

---

### 320×240 分辨率

#### ROS 2

| 模式                | 指标                          | 数值                            |
|---------------------|-------------------------------|---------------------------------|
| 多进程 pub/sub      | sub 延迟（900 帧）           | avg = **0.222 ms** (min ≈ 0.184, max ≈ 0.331) |
|                     | pub CPU（29 样本）           | avg = **0.59 %**                |
|                     | sub CPU（29 样本）           | avg = **0.48 %**                |
| intra-process 单进程 | 延迟（900 帧）               | avg = **0.024 ms** (min ≈ 0.020, max ≈ 0.046) |
|                     | CPU（29 样本）               | avg = **0.31 %**                |

与 1440×1080 对比：

- 图像更小后，多进程模式延迟和 CPU 占用都显著下降。
- intra-process 延迟在两种分辨率下接近，都在 ~25 µs 量级，说明其主要成本是一次数据拷贝，受分辨率影响不大。

#### LibXR

| 指标                                            | 数值 |
|-------------------------------------------------|------|
| LinuxSharedTopic latency (Publish -> Wait OK)   | count=300，avg = **73.584 µs** (min=18.503, max=117.085) |

可以看到：

- 分辨率降到 320×240 后，`LinuxSharedTopic` 的跨进程延迟降到 **73.6 µs**；
- 相比 ROS 2 多进程的 **0.222 ms** 仍然更低；
- 这里仍然是**跨进程**路径，因此不把它和 ROS 2 `intra-process` 作为同一类直接比较。

LibXR 整体 CPU（父进程 + subscriber 子进程，整次运行聚合）：

- `libxr_linux_shared_topic CPU(total)`: samples=10，avg = **1.80 %**

---

### 综合对比与结论

按链路模式划分，可以粗略归纳为：

1. **ROS 2 多进程**
   - 延迟：从 ~0.22 ms（320×240）到 ~1.78 ms（1440×1080），随分辨率显著变化。
   - CPU：大图像下 pub/sub 合计约 8% 左右，小图像时降至 1% 以内。
   - 适合需要进程隔离、但能接受毫秒级延迟和较高 CPU 开销的场景。

2. **ROS 2 intra-process**
   - 延迟：两种分辨率都在 ~0.02–0.03 ms。
   - CPU：320×240 时约 0.31%，1440×1080 时约 1.38%。
   - 避免了 IPC 和额外调度开销，是 ROS 2 框架下的进程内低延迟方案。
   - 这一类更适合与普通 `Topic` 这类进程内消息路径对标。

3. **LibXR LinuxSharedTopic（跨进程共享内存）**
   - 延迟：1440×1080 约 **89 µs**，320×240 约 **74 µs**。
   - 它的直接对标对象是 ROS 2 多进程，而不是 ROS 2 intra-process。
   - 相比 ROS 2 多进程显著更低，同时仍保留进程隔离。
   - CPU：父进程 + subscriber 子进程总占用约 **1.80 %**。
   - 这里测到的是 **Publish -> 订阅进程 Wait 成功** 的端到端数字，且起点在帧填充完成之后。

该仓库可以作为后续实验的基础，例如：

- 继续比较 ROS 2 多进程 / intra-process 与共享内存 IPC 的边界条件；
- 评估更高帧率、更大图像尺寸下的行为；
- 组合成更长的处理流水线，测量端到端行为。
