#!/usr/bin/env bash
# ============================================================================
# vcanup.sh —— 把 vcan0 拉起来（虚拟机重启后 vcan 模块会丢，每次开机都要做一次）
#
# 用法：
#   bash ~/uds-ecu/tools/vcanup.sh            # 这次开机先用起来
#   bash ~/uds-ecu/tools/vcanup.sh --persist  # 顺便设成开机自动创建（推荐，只做一次）
#
# 会提示输入 sudo 密码 —— vcan 是内核虚拟网卡，必须 root 才能创建。
# ============================================================================
set -e

echo "[1/3] 加载 vcan 内核模块..."
sudo modprobe vcan

echo "[2/3] 创建 vcan0..."
if ip link show vcan0 >/dev/null 2>&1; then
    echo "      vcan0 已存在，跳过创建"
else
    sudo ip link add dev vcan0 type vcan
fi

echo "[3/3] 启用 vcan0..."
sudo ip link set up vcan0

ip -br link show vcan0
echo "OK：vcan0 已就绪，可以跑 candump / isotp-echo / uds-demo 了。"

# ---------------------------------------------------------------------------
if [ "$1" = "--persist" ]; then
    echo
    echo "设置为开机自动创建（只需做一次）..."

    echo vcan | sudo tee /etc/modules-load.d/vcan.conf >/dev/null

    sudo tee /etc/systemd/system/vcan0.service >/dev/null <<'EOF'
[Unit]
Description=Create and bring up vcan0
After=network.target

[Service]
Type=oneshot
ExecStart=/sbin/modprobe vcan
ExecStart=/sbin/ip link add dev vcan0 type vcan
ExecStart=/sbin/ip link set up vcan0
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

    sudo systemctl daemon-reload
    sudo systemctl enable vcan0.service
    echo "完成：以后开机 vcan0 自动出现，不用再手动折腾。"
fi
