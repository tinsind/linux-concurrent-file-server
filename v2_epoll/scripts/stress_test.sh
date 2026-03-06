#!/usr/bin/env bash
set -u

usage() {
    cat <<'EOF'
用法:
  ./scripts/stress_test.sh <server_ip> <port> [并发数] [每客户端循环次数] [文件大小KB]

示例:
  ./scripts/stress_test.sh 127.0.0.1 9000
  ./scripts/stress_test.sh 127.0.0.1 9000 8 5 128

说明:
  - 客户端默认使用 ./client/client_host
  - 每个并发客户端执行: list -> put -> get -> quit
  - 每个客户端在独立临时目录运行，避免文件互相覆盖
    - 默认: 成功自动清理临时目录，失败保留日志用于排查

可选环境变量:
    CLIENT_BIN=./client/client_host      # 自定义客户端路径
    KEEP_LOG_ON_FAIL=1                   # 失败保留日志(1保留,0删除)
    KEEP_RUN_DIR_ON_PASS=0               # 成功后是否保留临时目录(1保留,0删除)
    PER_CLIENT_TIMEOUT=0                 # 单客户端超时秒数(0表示不超时)
EOF
}

if [[ $# -lt 2 || $# -gt 5 ]]; then
    usage
    exit 1
fi

SERVER_IP="$1"
PORT="$2"
CONCURRENCY="${3:-5}"
LOOPS="${4:-3}"
FILE_SIZE_KB="${5:-64}"

CLIENT_BIN="${CLIENT_BIN:-./client/client_host}"
KEEP_LOG_ON_FAIL="${KEEP_LOG_ON_FAIL:-1}"
KEEP_RUN_DIR_ON_PASS="${KEEP_RUN_DIR_ON_PASS:-0}"
PER_CLIENT_TIMEOUT="${PER_CLIENT_TIMEOUT:-0}"

if [[ ! -x "$CLIENT_BIN" ]]; then
    echo "[ERROR] 客户端不存在或不可执行: $CLIENT_BIN"
    echo "先执行: make client_host"
    exit 1
fi

CLIENT_BIN_ABS="$(realpath "$CLIENT_BIN")"

if ! [[ "$PORT" =~ ^[0-9]+$ ]] || (( PORT < 1 || PORT > 65535 )); then
    echo "[ERROR] 端口非法: $PORT"
    exit 1
fi

if ! [[ "$CONCURRENCY" =~ ^[0-9]+$ ]] || (( CONCURRENCY < 1 )); then
    echo "[ERROR] 并发数非法: $CONCURRENCY"
    exit 1
fi

if ! [[ "$LOOPS" =~ ^[0-9]+$ ]] || (( LOOPS < 1 )); then
    echo "[ERROR] 循环次数非法: $LOOPS"
    exit 1
fi

if ! [[ "$FILE_SIZE_KB" =~ ^[0-9]+$ ]] || (( FILE_SIZE_KB < 1 )); then
    echo "[ERROR] 文件大小KB非法: $FILE_SIZE_KB"
    exit 1
fi

if ! [[ "$KEEP_LOG_ON_FAIL" =~ ^[01]$ ]]; then
    echo "[ERROR] KEEP_LOG_ON_FAIL 只能是 0 或 1"
    exit 1
fi

if ! [[ "$KEEP_RUN_DIR_ON_PASS" =~ ^[01]$ ]]; then
    echo "[ERROR] KEEP_RUN_DIR_ON_PASS 只能是 0 或 1"
    exit 1
fi

if ! [[ "$PER_CLIENT_TIMEOUT" =~ ^[0-9]+$ ]]; then
    echo "[ERROR] PER_CLIENT_TIMEOUT 必须是非负整数秒"
    exit 1
fi

if ! command -v sha256sum >/dev/null 2>&1; then
    echo "[ERROR] 未找到 sha256sum，请先安装 coreutils"
    exit 1
fi

if (( PER_CLIENT_TIMEOUT > 0 )) && ! command -v timeout >/dev/null 2>&1; then
    echo "[ERROR] PER_CLIENT_TIMEOUT>0 需要 timeout 命令(coreutils)"
    exit 1
fi

RUN_DIR="$(mktemp -d -t cfs_stress_XXXXXX)"
LOG_DIR="$RUN_DIR/logs"
mkdir -p "$LOG_DIR"

START_TS="$(date +%s)"

cleanup() {
    local ec=$?

    if [[ -z "${RUN_DIR:-}" || ! -d "$RUN_DIR" ]]; then
        return
    fi

    if (( ec == 0 )); then
        if (( KEEP_RUN_DIR_ON_PASS == 1 )); then
            echo "[INFO] 压测成功，保留目录: $RUN_DIR"
            return
        fi
        rm -rf "$RUN_DIR"
        return
    fi

    if (( KEEP_LOG_ON_FAIL == 1 )); then
        echo "[INFO] 压测失败，保留日志目录: $LOG_DIR"
        return
    fi

    rm -rf "$RUN_DIR"
}
trap cleanup EXIT

echo "[INFO] server=$SERVER_IP:$PORT"
echo "[INFO] concurrency=$CONCURRENCY loops=$LOOPS payload=${FILE_SIZE_KB}KB"
echo "[INFO] run_dir=$RUN_DIR"
if (( PER_CLIENT_TIMEOUT > 0 )); then
    echo "[INFO] per_client_timeout=${PER_CLIENT_TIMEOUT}s"
fi

pids=()

run_one_client() {
    local idx="$1"
    local cdir="$RUN_DIR/client_$idx"
    local clog="$LOG_DIR/client_$idx.log"
    local script_file="$cdir/commands.txt"

    mkdir -p "$cdir"

    (
        cd "$cdir" || exit 100

        : > "$script_file"
        for ((round=1; round<=LOOPS; round++)); do
            local fname="put_c${idx}_r${round}.bin"
            dd if=/dev/urandom of="$fname" bs=1024 count="$FILE_SIZE_KB" status=none || exit 101
            cp "$fname" "$fname.src" || exit 102
            printf 'list\n' >> "$script_file"
            printf 'put %s\n' "$fname" >> "$script_file"
            printf 'get %s\n' "$fname" >> "$script_file"
        done
        printf 'quit\n' >> "$script_file"

        if (( PER_CLIENT_TIMEOUT > 0 )); then
            timeout "$PER_CLIENT_TIMEOUT" "$CLIENT_BIN_ABS" "$SERVER_IP" "$PORT" < "$script_file" > "$clog" 2>&1 || exit $?
        else
            "$CLIENT_BIN_ABS" "$SERVER_IP" "$PORT" < "$script_file" > "$clog" 2>&1 || exit $?
        fi

        for ((round=1; round<=LOOPS; round++)); do
            local fname="put_c${idx}_r${round}.bin"
            local old_hash
            local new_hash

            old_hash="$(sha256sum "$fname.src" | awk '{print $1}')" || exit 103
            new_hash="$(sha256sum "$fname" | awk '{print $1}')" || exit 104
            if [[ "$old_hash" != "$new_hash" ]]; then
                echo "[ERROR] checksum mismatch: $fname" >> "$clog"
                exit 105
            fi
        done
    )
}

for ((i=1; i<=CONCURRENCY; i++)); do
    run_one_client "$i" &
    pids+=("$!")
done

fail_count=0
timeout_count=0
for pid in "${pids[@]}"; do
    if wait "$pid"; then
        :
    else
        rc=$?
        if (( rc == 124 )); then
            ((timeout_count++))
        fi
        ((fail_count++))
    fi
done

total_sessions="$CONCURRENCY"
expected_ops=$((CONCURRENCY * LOOPS))

put_ok=$(grep -h "PUT ok:" "$LOG_DIR"/*.log 2>/dev/null | wc -l | tr -d ' ')
get_ok=$(grep -h "GET ok:" "$LOG_DIR"/*.log 2>/dev/null | wc -l | tr -d ' ')
err_lines=$(grep -h "\[ERROR\]" "$LOG_DIR"/*.log 2>/dev/null | wc -l | tr -d ' ')

END_TS="$(date +%s)"
elapsed=$((END_TS - START_TS))
if (( elapsed <= 0 )); then
    elapsed=1
fi

total_bytes=$((expected_ops * FILE_SIZE_KB * 1024 * 2))
throughput_mib_s=$(awk -v b="$total_bytes" -v t="$elapsed" 'BEGIN { printf "%.2f", (b / 1024 / 1024) / t }')
ops_per_sec=$(awk -v o="$expected_ops" -v t="$elapsed" 'BEGIN { printf "%.2f", o / t }')

echo
echo "========== 压测结果 =========="
echo "会话总数          : $total_sessions"
echo "会话失败数        : $fail_count"
echo "超时会话数        : $timeout_count"
echo "PUT 成功(期望 $expected_ops): $put_ok"
echo "GET 成功(期望 $expected_ops): $get_ok"
echo "ERROR 行数        : $err_lines"
echo "总耗时(秒)        : $elapsed"
echo "估算吞吐(MiB/s)   : $throughput_mib_s"
echo "请求速率(op/s)    : $ops_per_sec"
echo "日志目录          : $LOG_DIR"

if (( fail_count == 0 && put_ok == expected_ops && get_ok == expected_ops && err_lines == 0 )); then
    echo "[PASS] 并发压测通过"
    exit 0
fi

echo "[FAIL] 并发压测未完全通过，建议查看: $LOG_DIR"
exit 2
