#!/usr/bin/env bash
set -eo pipefail

WS="${WS:-$HOME/ros2_ws}"
LOG_DIR="$WS/logs"
BIN_PATH="$WS/install/libxr_bench/bin/libxr_bench"
BIN_NAME="$(basename "$BIN_PATH")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLOT_TOOL="${PLOT_TOOL:-$SCRIPT_DIR/tools/boxplot_svg.py}"

mkdir -p "$LOG_DIR"

echo "Workspace: $WS"
echo "Log dir  : $LOG_DIR"

ensure_pidstat() {
  if command -v pidstat >/dev/null 2>&1; then
    return
  fi

  echo "[INFO] pidstat 未找到，自动安装 sysstat..."
  apt-get update -y
  apt-get install -y sysstat
}

ensure_binary() {
  if [ -x "$BIN_PATH" ]; then
    return
  fi

  if [ ! -f /opt/ros/humble/setup.bash ]; then
    echo "[ERROR] 缺少可执行文件 $BIN_PATH，且找不到 /opt/ros/humble/setup.bash。"
    exit 1
  fi

  # shellcheck disable=SC1091
  source /opt/ros/humble/setup.bash

  echo "[INFO] 开始 colcon build libxr_bench (Release) ..."
  colcon build --packages-select libxr_bench --cmake-args -DCMAKE_BUILD_TYPE=Release
}

cleanup_processes() {
  pkill -x "$BIN_NAME" || true
  pkill -f "pidstat -u" || true
}

print_total_cpu() {
  local file="$1"
  local label="$2"
  local cmd_name="$3"

  if [ ! -f "$file" ]; then
    echo "[RESULT] $label CPU(total): no data"
    return
  fi

  awk -v label="$label" -v cmd_name="$cmd_name" '
    $1 == "Linux" || $1 == "#" || $1 == "Average:" || NF < 4 { next }
    $NF != cmd_name { next }
    {
      key = $1
      if ($2 == "AM" || $2 == "PM") {
        key = $1 " " $2
      }
      sample_sum[key] += $(NF - 2)
    }
    END {
      n = 0
      sum = 0
      for (k in sample_sum) {
        sum += sample_sum[k]
        n++
      }
      if (n > 0) {
        printf("[RESULT] %s CPU(total): samples=%d avg=%.2f%%\n", label, n, sum / n)
      } else {
        printf("[RESULT] %s CPU(total): no data\n", label)
      }
    }
  ' "$file"
}

main() {
  ensure_pidstat
  ensure_binary

  if [ ! -x "$PLOT_TOOL" ]; then
    echo "[ERROR] 箱线图工具不可执行: $PLOT_TOOL"
    exit 1
  fi

  cd "$WS"
  cleanup_processes

  local ts
  ts=$(date +%F_%H%M%S)

  local bench_log="$LOG_DIR/libxr_${ts}.log"
  local cpu_log="$LOG_DIR/cpu_libxr_${ts}.log"
  local sample_dir="$LOG_DIR/libxr_samples_${ts}"
  local plot_dir="$LOG_DIR/libxr_boxplots_${ts}"

  echo "[INFO] libxr log: $bench_log"
  mkdir -p "$sample_dir" "$plot_dir"

  "$BIN_PATH" >"$bench_log" 2>&1 &
  local bench_pid=$!
  echo "[INFO] ${BIN_NAME} PID=${bench_pid}"

  sleep 1

  pidstat -u -h -C "$BIN_NAME" 1 >"$cpu_log" &
  local pidstat_pid=$!

  wait "$bench_pid" || true
  kill "$pidstat_pid" 2>/dev/null || true

  echo
  echo "------ LibXR Benchmark Results ------"
  if grep -q "\[RESULT\]" "$bench_log"; then
    grep "\[RESULT\]" "$bench_log"
  else
    echo "[RESULT] libxr_bench: no result lines"
  fi

  awk '
    /^\[SAMPLE\]/ {
      key = ""
      value = ""
      for (i = 1; i <= NF; ++i) {
        if ($i ~ /^key=/) {
          split($i, a, "=")
          key = a[2]
        } else if ($i ~ /^value_us=/) {
          split($i, a, "=")
          value = a[2]
        }
      }
      if (key != "" && value != "") {
        print value >> "'"$sample_dir"'/" key ".csv"
      }
    }
  ' "$bench_log"

  echo
  echo "------ LibXR CPU ------"
  print_total_cpu "$cpu_log" "libxr_bench" "$BIN_NAME"
  awk '
    $1 == "Linux" || $1 == "#" || $1 == "Average:" || NF < 4 { next }
    $NF != "'"$BIN_NAME"'" { next }
    {
      key = $1
      if ($2 == "AM" || $2 == "PM") {
        key = $1 " " $2
      }
      sample_sum[key] += $(NF - 2)
    }
    END {
      for (k in sample_sum) {
        print sample_sum[k]
      }
    }
  ' "$cpu_log" >"$sample_dir/libxr_bench_cpu_total.csv"

  local sample_file
  for sample_file in "$sample_dir"/*.csv; do
    [ -f "$sample_file" ] || continue
    local stem
    stem="$(basename "$sample_file" .csv)"
    local title
    local ylabel
    case "$stem" in
      topic_1440x1080)
        title="LibXR Topic 1440x1080"
        ylabel="Latency (us)"
        ;;
      linux_shared_topic_1440x1080)
        title="LibXR LinuxSharedTopic 1440x1080"
        ylabel="Latency (us)"
        ;;
      topic_320x240)
        title="LibXR Topic 320x240"
        ylabel="Latency (us)"
        ;;
      linux_shared_topic_320x240)
        title="LibXR LinuxSharedTopic 320x240"
        ylabel="Latency (us)"
        ;;
      libxr_bench_cpu_total)
        title="LibXR Benchmark CPU"
        ylabel="CPU (%)"
        ;;
      *)
        title="$stem"
        ylabel="Value"
        ;;
    esac
    python3 "$PLOT_TOOL" --input "$sample_file" --output "$plot_dir/$stem.svg" --title "$title" --ylabel "$ylabel"
    echo "[RESULT] $stem boxplot: $plot_dir/$stem.svg"
  done

  echo
  echo "Logs:"
  echo "  $bench_log"
  echo "  $cpu_log"
  echo "  $sample_dir"
  echo "  $plot_dir"
}

main
