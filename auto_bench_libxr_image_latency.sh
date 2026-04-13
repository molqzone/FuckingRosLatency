#!/usr/bin/env bash
set -eo pipefail

WS="${WS:-$HOME/ros2_ws}"
LOG_DIR="$WS/logs"
BIN_PATH="$WS/build/libxr_tp_test/libxr_tp_test"
BIN_NAME="$(basename "$BIN_PATH")"

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

  echo "[INFO] 开始 colcon build libxr_tp_test ..."
  colcon build --packages-select libxr_tp_test
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

  cd "$WS"
  cleanup_processes

  local ts
  ts=$(date +%F_%H%M%S)

  local bench_log="$LOG_DIR/libxr_${ts}.log"
  local cpu_log="$LOG_DIR/cpu_libxr_${ts}.log"

  echo "[INFO] libxr log: $bench_log"

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

  echo
  echo "------ LibXR CPU ------"
  print_total_cpu "$cpu_log" "libxr_bench" "$BIN_NAME"

  echo
  echo "Logs:"
  echo "  $bench_log"
  echo "  $cpu_log"
}

main
