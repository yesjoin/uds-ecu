# uds-ecu

[![CI](https://github.com/yesjoin/uds-ecu/actions/workflows/ci.yml/badge.svg)](https://github.com/yesjoin/uds-ecu/actions/workflows/ci.yml)

一个跑在 Linux 虚拟 CAN 上的 **UDS 诊断栈**：SocketCAN → ISO-TP（15765-2）→ UDS（14229-1）→ 虚拟 ECU + 测试仪。
纯 C++17，不依赖第三方库；一条 `vcan0` 就能把"诊断仪 ↔ ECU"整条链路跑起来。

```bash
git clone git@github.com:yesjoin/uds-ecu.git && cd uds-ecu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUDS_WERROR=ON && cmake --build build -j4
(cd build && ctest)          # 3 个套件：41 + 77 + 200 条断言
```

真总线部分要先建 `vcan0`（内核虚拟 CAN，需要 sudo，重启后要重建）：

```bash
bash tools/vcanup.sh            # 这次开机用起来
bash tools/vcanup.sh --persist  # 顺便设成开机自动创建（推荐，只做一次）
```

---

## 它解决什么问题

诊断是车上最"看不见"的一层：一个 `22 F1 90` 下去，回来的是一串十六进制。
这一层出问题，现象永远是"超时"，而原因可能在四个地方——CAN 帧没收全、ISO-TP 序号错了、
UDS 会话没切过去、安全访问没解锁。**分不清是哪一层，就只能靠猜。**

所以这个工程的骨架就是"一层一个坑"：每层单独自测，再串起来做端到端。

| 层 | 干什么 | 关键点 |
|---|---|---|
| SocketCAN | 收发 CAN 帧 | 收发解耦（收线程 + 环形缓冲），1000 fps 零丢帧 |
| ISO-TP | 把 8 字节的 CAN 帧拼成任意长度的报文 | SF/FF/CF + 流控（BS / STmin）、序号校验、超时 |
| UDS | 诊断服务 | 0x10 会话 / 0x22 读 / 0x2E 写 / 0x3E 心跳 / 0x27 安全访问 / 0x19+0x14 故障码 / 0x34+0x36+0x37 刷写 |
| 虚拟 ECU | 扮演被测节点 | 7E0 收、7E8 发；VIN/硬件号在 DID 表里 |
| 测试仪 | 扮演诊断仪 | 一条命令发一个请求，把响应的字节数、十六进制都打出来 |

---

## 里程碑

| | 内容 | 验证 |
|---|---|---|
| M0 | SocketCAN 收帧 + 收发解耦 | 1000 fps 零丢帧 |
| M1 | ISO-TP 收发 + 流控握手 | 41 项自测 + vcan 实测（`isotp-echo` 两端对话） |
| M2 | UDS 服务层（0x10 / 0x22 / 0x2E / 0x3E） | 自测 + vcan 端到端 |
| M3 | 0x27 安全访问 + 0x19 / 0x14 故障码 | 48 项自测 + 总线握手 |
| M4 | 0x34 / 0x36 / 0x37 刷写 + 掉电回滚 | 77 项自测 + 真断电验证（tmp → rename 原子提交） |
| M5 | 200 条测试矩阵 + 报告导出 | 通过率 100%，`test_matrix.csv` / `test_matrix_report.md` |

---

## 怎么用

**① ISO-TP 对打**（两个终端，一个收一个发）：

```bash
./build/isotp-echo --recv --wait-s 10        # 终端 1：接收侧
./build/isotp-echo --send                    # 终端 2：发送侧（多帧 + 流控）
```

**② UDS 对话**（一个当 ECU，另一个当诊断仪）：

```bash
./build/uds-demo --ecu --wait-s 120                      # 终端 1：虚拟 ECU
./build/uds-demo --tester "22 F1 90"                     # 终端 2：读 VIN（20 字节，走多帧）
./build/uds-demo --tester "10 03"                        # 切扩展会话
./build/uds-demo --tester "2E 02 00 00 00 27 10"         # 写里程（必须先 0x27 解锁）
./build/uds-demo --tester "19 02 FF"                     # 列出全部故障码
./build/uds-demo --tester "14 FF FF FF"                  # 清故障码
```

**③ 刷写与掉电回滚**（最值得看的一条）：

```bash
./build/uds-demo --flash 400                             # 10 02 → 27 → 34 → 36×N → 37
./build/uds-demo --ecu --flash-file /tmp/fw.bin --die-after 5   # 收到第 5 块就"断电"
```

`--die-after` 是把"刷一半断电会不会把 ECU 刷成砖"这件事真跑一遍：镜像先写临时文件，
**全部块收齐才 rename 提交**，中途断电则旧镜像完好。这不是模拟，是拿真实进程退出做的。

---

## 它和另外两个项目的关系

| 项目 | 角色 |
|---|---|
| [`uds-ecu`](https://github.com/yesjoin/uds-ecu)（本仓库） | 造帧的一方：被诊断的 ECU |
| [`dbc-toolkit`](https://github.com/yesjoin/dbc-toolkit) | 读帧的一方：把 CAN 报文翻译成人看得懂的信号 |
| [`doip-gw`](https://github.com/yesjoin/doip-gw) | 桥接的一方：把以太网 DoIP 请求打到 CAN 上，多 ECU 走路由表 |

三个项目互为验收工具：`doip-gw` 的端到端测试里，CAN 侧那台 ECU 就是本仓库的 `uds-demo`。

---

## 已知边界

- CAN ID 是源码常量（`tools/uds_demo.cpp` 里 `0x7E0` / `0x7E8`），不支持命令行改 —— 需要"第二台 ECU 换 ID"的场景，
  是用 `doip-gw` 里的 `stub-ecu` 做的。
- 只做 11 位标准帧，不支持 29 位扩展 ID、不支持 CAN FD。
- 功能寻址（`7DF`）没做，只做物理寻址。
- 0x27 是"种子-密钥"的简化版（固定算法），不是 OEM 的真实算法。
- 刷写只做 0x34/0x36/0x37 的最小可用链，没有 0x31 例程校验、没有压缩/加密镜像。
- 单 ECU、单诊断仪；多诊断仪并发隔离是 `doip-gw` 里做的（共享总线 + 一把锁串行）。
- 依赖 Linux SocketCAN，Windows / macOS 上只能跑三个纯内存自测（`ctest` 全绿，但没有真总线）。
