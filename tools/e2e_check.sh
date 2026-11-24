#!/usr/bin/env bash
# =============================================================================
# e2e_check.sh —— 真 vcan0 上的一次完整对话（CI 和本地共用）
#
# 用法： bash tools/e2e_check.sh [iface]       默认 vcan0
#
# 验两件事：
#   ① 22 F1 90 读 VIN —— 20 字节响应，必须走 ISO-TP 多帧（FF + CF + 流控全链路）
#   ② 3E 00 心跳 —— 2 字节响应，单帧；证明短请求也没被多帧逻辑带坏
#
# 前置：vcan0 已 up（bash tools/vcanup.sh），build/ 已构建。
# =============================================================================
set -u

IFACE=${1:-vcan0}
HERE=$(cd "$(dirname "$0")/.." && pwd)
ECU="$HERE/build/uds-demo"

pass=0; fail=0
ok()  { echo "[PASS] $1"; pass=$((pass+1)); }
bad() { echo "[FAIL] $1"; fail=$((fail+1)); }

cleanup() { pkill -x uds-demo 2>/dev/null; }
trap cleanup EXIT

[ -x "$ECU" ] || { echo "[SKIP] 没有 $ECU（先 cmake --build build）"; exit 0; }
[ -e "/sys/class/net/$IFACE" ] || { echo "[SKIP] 没有 $IFACE（先 bash tools/vcanup.sh）"; exit 0; }

cleanup
echo "=== 起虚拟 ECU（$IFACE），12 秒后自动退出 ==="
"$ECU" --ecu --iface "$IFACE" --wait-s 12 > /tmp/e2e_ecu.log 2>&1 &
ECU_PID=$!
sleep 1.5

# ---- ① 多帧：读 VIN ----
echo
echo "=== ① 22 F1 90（20 字节，多帧）==="
timeout 15 "$ECU" --tester "22 F1 90" --iface "$IFACE" --wait-s 5 > /tmp/e2e_vin.log 2>&1
grep -q '62 F1 90 4C 53 56 41 41' /tmp/e2e_vin.log && ok "读到 VIN（62 F1 90 4C 53 56 41 41…）" || { bad "读 VIN 失败"; tail -5 /tmp/e2e_vin.log; }

# ---- ② 单帧：心跳 ----
echo
echo "=== ② 3E 00（2 字节，单帧）==="
timeout 15 "$ECU" --tester "3E 00" --iface "$IFACE" --wait-s 5 > /tmp/e2e_alive.log 2>&1
grep -q '7E 00' /tmp/e2e_alive.log && ok "3E 00 → 7E 00" || { bad "心跳失败"; tail -5 /tmp/e2e_alive.log; }

kill $ECU_PID 2>/dev/null
wait $ECU_PID 2>/dev/null

echo
echo "--------------------------------"
echo "通过 $pass 项，失败 $fail 项"
[ "$fail" -eq 0 ] || exit 1
