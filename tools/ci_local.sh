#!/usr/bin/env bash
# =============================================================================
# ci_local.sh —— 推之前先在本机把 CI 要干的事全干一遍
#
# 用法： bash tools/ci_local.sh [gcc|clang]
#
# 5 步：编译器 → 配置（干净目录）→ 构建（-Werror + -O2）→ ctest → 真 vcan 端到端
# 缺 vcan0 的第 5 步打 [SKIP]，不算红；前 4 步任何一步红都不许推。
# =============================================================================
set -u

MODE=${1:-gcc}
if [ "$MODE" = "clang" ]; then
    CC=${CC:-clang};   CXX=${CXX:-clang++};   WERROR=0
else
    CC=${CC:-gcc};     CXX=${CXX:-g++};       WERROR=1   # CI 的 gcc 腿开 -Werror
fi
export CC CXX

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1
BUILD="build"
LOG="/tmp/uds_ci"
mkdir -p "$LOG"

pass=0; fail=0
ok()  { echo "[PASS] $1"; pass=$((pass+1)); }
bad() { echo "[FAIL] $1"; fail=$((fail+1)); }
step() { printf '\n=== %s ===\n' "$1"; }

# ---------------------------------------------------------------------------
step "1/5 编译器"
command -v "$CXX" >/dev/null 2>&1 || { echo "[SKIP] 这台机器上没有 $CXX。"; exit 0; }
$CXX --version | head -1 | sed 's/^/      /'

# ---------------------------------------------------------------------------
step "2/5 配置（干净目录，等价于 CI 全新克隆）"
rm -rf "$BUILD"
EXTRA=""
[ "$WERROR" -eq 1 ] && EXTRA="-DUDS_WERROR=ON"
# -O2 是故意的：有的警告只在优化后才出现，-O0 下压根不报
echo "      cmake -S . -B $BUILD -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-O2 $EXTRA"
if cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-O2 $EXTRA > "$LOG/cfg.log" 2>&1; then
    ok "cmake 配置成功"
else
    bad "cmake 配置失败"; tail -n 12 "$LOG/cfg.log" | sed 's/^/      /'
fi

# ---------------------------------------------------------------------------
step "3/5 构建 -Wall -Wextra -Wpedantic $([ "$WERROR" -eq 1 ] && echo '+ -Werror')"
if cmake --build "$BUILD" -j"$(nproc)" > "$LOG/build.log" 2>&1; then
    W=$(grep -c 'warning:' "$LOG/build.log" || true)
    [ "$W" -eq 0 ] && ok "构建成功，零警告" || bad "构建成功但有 $W 条警告"
else
    bad "构建失败"; grep -E 'error|warning:' "$LOG/build.log" | head -12 | sed 's/^/      /'
fi

# ---------------------------------------------------------------------------
step "4/5 自测（ctest：ISO-TP + UDS + 测试矩阵）"
if (cd "$BUILD" && ctest --output-on-failure > "$LOG/ctest.log" 2>&1); then
    ok "ctest 全绿"; grep -E 'tests passed' "$LOG/ctest.log" | sed 's/^/      /'
else
    bad "ctest 红"; grep -E 'Failed|failed' "$LOG/ctest.log" | head -8 | sed 's/^/      /'
fi

# ---------------------------------------------------------------------------
step "5/5 端到端：真 vcan0（ECU ↔ 诊断仪）"
if [ -e /sys/class/net/vcan0 ] && [ -x "$BUILD/uds-demo" ]; then
    if bash tools/e2e_check.sh vcan0 > "$LOG/e2e.log" 2>&1; then
        ok "真总线端到端：多帧读 VIN + 单帧心跳"
    else
        bad "真总线端到端失败"; tail -n 15 "$LOG/e2e.log" | sed 's/^/      /'
    fi
    grep -E '^[^ ]?\[PASS\]' "$LOG/e2e.log" | sed 's/^/      /'
else
    echo "[SKIP] 没有 vcan0（bash tools/vcanup.sh），跳过真总线"
fi

# ---------------------------------------------------------------------------
echo
echo "--------------------------------"
echo "通过 $pass 项，失败 $fail 项"
[ "$fail" -eq 0 ] && echo "本地这条腿全绿。" || echo "先修红的，再推 Git。"
[ "$fail" -eq 0 ] || exit 1
