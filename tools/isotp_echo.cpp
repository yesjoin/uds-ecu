// ============================================================================
// isotp_echo.cpp —— M1b-3：带流控握手的真实双向收发
//
// 用法（两个终端，第三个可选）：
//   T1: ./build/isotp-echo --recv --bs 8 --stmin 0
//   T2: ./build/isotp-echo --send
//   T3: candump vcan0            # 看总线上的原始字节（含 0x7E0 的流控帧）
//
// 和 M1b-2 的本质区别：
//   M1b-2 里发送端一股脑把 572 帧倒出去；
//   M1b-3 里发送端发完首帧就停住，等对端给一张流控帧才动，
//         之后每发满 BS 个连续帧再停一次。
//
// 谁决定节奏？**接收方**。
// 这就是 ISO-TP 的设计意图：收不动的那个有发言权。
// ============================================================================

#include "CanBus.h"
#include "IsoTp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t TESTER_TX = 0x7E0;   // 测试仪 → ECU（请求 / 流控）
constexpr std::uint32_t ECU_TX    = 0x7E8;   // ECU → 测试仪（响应）
constexpr std::size_t   MSG_LEN   = 4000;

std::vector<std::uint8_t> makePattern(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::uint8_t>(i & 0xFF);
    }
    return v;
}

std::uint8_t checksum(const std::vector<std::uint8_t>& v) {
    std::uint32_t s = 0;
    for (auto b : v) s += static_cast<std::uint32_t>(b);
    return static_cast<std::uint8_t>(s & 0xFF);
}

void dumpFrame(const char* tag, const uds::Frame& f) {
    std::printf("  %s id=0x%03X dlc=%u  ", tag, f.id, static_cast<unsigned>(f.dlc));
    for (std::uint8_t i = 0; i < f.dlc; ++i) {
        std::printf("%02X ", f.data[i]);
    }
    std::printf("\n");
}

// 睡到指定时刻。
//
// sleep_for 在虚拟机里粒度约 15ms：请求 10ms 实际睡 15ms。
// 所以只在"还早得很"（>20ms）时才睡，剩下的全部自旋补齐 ——
// STmin 这种 10ms 级的间隔，一睡就超标。
void waitUntil(std::chrono::steady_clock::time_point target) {
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= target) return;

        const auto remainUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                  target - now).count();
        if (remainUs > 20000) {
            std::this_thread::sleep_for(std::chrono::microseconds(remainUs - 3000));
        }
        // 剩余 <=20ms：不睡，空转，精度到微秒
    }
}

void waitUntilMs(std::uint64_t targetMs) {
    const std::uint64_t nowMs = uds::steadyMs();
    if (nowMs >= targetMs) return;
    waitUntil(std::chrono::steady_clock::now() +
              std::chrono::milliseconds(static_cast<long long>(targetMs - nowMs)));
}

// ---------------------------------------------------------------------------
// 发送侧：扮演 ECU，往 0x7E8 发；节奏完全听对端流控帧的
// ---------------------------------------------------------------------------
int runSend(const std::string& iface) {
    uds::CanBus bus(iface);
    if (!bus.ok()) {
        std::fprintf(stderr, "[send] 打不开 %s\n", iface.c_str());
        return 1;
    }

    uds::IsoTpConfig cfg;
    cfg.rxId = TESTER_TX;    // 流控帧从这个 ID 来
    cfg.txId = ECU_TX;       // 我们发出去用这个 ID

    uds::IsoTpTx tx(cfg);
    const auto msg = makePattern(MSG_LEN);
    if (!tx.begin(msg)) {
        std::fprintf(stderr, "[send] begin 失败\n");
        return 1;
    }

    std::printf("[send] 消息 %zu 字节，校验和 0x%02X\n",
                msg.size(), static_cast<unsigned>(checksum(msg)));
    if (tx.isSingleFrame()) {
        std::printf("[send] 短消息，单帧搞定，不需要流控\n");
    }

    const auto t0 = std::chrono::steady_clock::now();

    if (!bus.send(tx.firstFrame())) {
        std::fprintf(stderr, "[send] 首帧发送失败\n");
        return 1;
    }

    std::size_t sentCf       = 0;
    bool        printedFirstFc = false;
    std::uint64_t waitFcSince  = 0;
    int         rc             = 1;

    while (true) {
        // ① 先把已经到手的流控帧喂进状态机（非阻塞，不能漏）
        uds::Frame in;
        while (bus.recv(in, 0)) {
            if (in.id != cfg.rxId) continue;
            if ((in.data[0] >> 4) != 0x3) continue;
            tx.onFlowControl(in, uds::steadyMs());
            waitFcSince = 0;
            if (!printedFirstFc) {
                std::printf("[send] 收到第 1 张流控：FS=CTS BS=%u STmin=%u ms\n",
                            static_cast<unsigned>(tx.blockSize()),
                            static_cast<unsigned>(tx.stMinMs()));
                printedFirstFc = true;
            }
        }

        // ② 问状态机：现在该干什么
        uds::Frame out;
        const auto st = tx.poll(uds::steadyMs(), out);

        if (st == uds::IsoTpTx::Status::SentFrame) {
            if (!bus.send(out)) {
                std::fprintf(stderr, "[send] 连续帧发送失败\n");
                rc = 1;
                break;
            }
            ++sentCf;
            continue;
        }
        if (st == uds::IsoTpTx::Status::WaitStmin) {
            waitUntilMs(tx.nextSendMs());
            continue;
        }
        if (st == uds::IsoTpTx::Status::Done) {
            rc = 0;
            break;
        }
        if (st == uds::IsoTpTx::Status::Aborted) {
            std::printf("[send] 对端回了 OVFLW，放弃\n");
            rc = 1;
            break;
        }

        // WaitFlowControl：等，但别无限等
        const std::uint64_t now = uds::steadyMs();
        if (waitFcSince == 0) {
            waitFcSince = now;
        } else if (now - waitFcSince > 300) {
            std::printf("[send] 等流控帧超时（300ms）\n");
            rc = 1;
            break;
        }
        std::this_thread::yield();
    }

    const auto ms = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    const long long total = static_cast<long long>(sentCf) + (tx.isSingleFrame() ? 0 : 1);

    std::printf("[send] 发出 %zu 个连续帧，收到 %zu 张流控，耗时 %lld ms\n",
                sentCf, tx.fcCount(), ms);
    if (ms > 0 && total > 0) {
        std::printf("[send] 全程 %lld 帧 → 平均 %lld 帧/秒\n",
                    total, total * 1000 / ms);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// 接收侧：扮演测试仪，从 0x7E8 收；节奏由它自己给（BS / STmin）
// ---------------------------------------------------------------------------
int runRecv(const std::string& iface, int waitSec, int bs, int stmin) {
    uds::CanBus bus(iface);
    if (!bus.ok()) {
        std::fprintf(stderr, "[recv] 打不开 %s\n", iface.c_str());
        return 1;
    }

    uds::IsoTpConfig cfg;
    cfg.rxId      = ECU_TX;
    cfg.txId      = TESTER_TX;
    cfg.blockSize = static_cast<std::uint8_t>(bs);
    cfg.stMinMs   = static_cast<std::uint8_t>(stmin);

    uds::IsoTp tp(cfg);

    const auto expect    = makePattern(MSG_LEN);
    const auto expectSum = checksum(expect);

    std::printf("[recv] 在 %s 上等 0x%03X（最多 %d 秒）\n",
                iface.c_str(), ECU_TX, waitSec);
    std::printf("[recv] 我给的流控参数：BS=%d, STmin=%d ms "
                "（BS=0 表示不要求对端分块）\n", bs, stmin);
    std::printf("[recv] 期望 %zu 字节，校验和 0x%02X\n",
                expect.size(), static_cast<unsigned>(expectSum));

    std::vector<std::uint8_t> out;
    bool got = false;

    std::size_t   nRead   = 0;
    std::size_t   nFc     = 0;
    unsigned      lastSeq = 0;
    std::uint64_t lastMs  = 0;

    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(waitSec);

    while (std::chrono::steady_clock::now() < deadline) {
        uds::Frame f;
        if (!bus.recv(f, 100)) continue;
        ++nRead;
        if (f.id != cfg.rxId) continue;
        if ((f.data[0] >> 4) == 0x2) lastSeq = f.data[0] & 0x0F;
        lastMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());

        if (tp.feed(f, out)) {
            got = true;
            break;
        }

        // 该回流控帧了就回 —— 首帧之后一次，之后每满一块再一次
        if (tp.needFlowControl()) {
            if (!bus.send(tp.makeFlowControl())) {
                std::fprintf(stderr, "[recv] 流控帧发送失败\n");
            }
            tp.clearFlowControl();
            ++nFc;
        }
    }

    if (!got) {
        const auto& st = tp.stats();
        std::printf("[recv] 没收到完整消息。sf=%llu ff=%llu cf=%llu "
                    "seqErr=%llu timeout=%llu\n",
                    static_cast<unsigned long long>(st.sf),
                    static_cast<unsigned long long>(st.ff),
                    static_cast<unsigned long long>(st.cf),
                    static_cast<unsigned long long>(st.seqErr),
                    static_cast<unsigned long long>(st.timeout));
        std::printf("[recv] 诊断：socket 共交出 %zu 帧，回出 %zu 张流控，"
                    "最后一个 CF 序号=%u，最后一帧到达于 %llu ms\n",
                    nRead, nFc, lastSeq,
                    static_cast<unsigned long long>(lastMs));
        return 1;
    }

    const auto sum = checksum(out);
    std::printf("[recv] 收到 %zu 字节，校验和 0x%02X，回出 %zu 张流控\n",
                out.size(), static_cast<unsigned>(sum), nFc);

    if (!out.empty()) {
        std::printf("[recv] 首 4 字节: %02X %02X %02X %02X\n",
                    out[0], out[1], out[2], out[3]);
        const std::size_t n = out.size();
        std::printf("[recv] 末 4 字节: %02X %02X %02X %02X\n",
                    out[n - 4], out[n - 3], out[n - 2], out[n - 1]);
    }

    const bool lenOk     = out.size() == expect.size();
    const bool sumOk     = sum == expectSum;
    const bool contentOk = out == expect;

    std::printf("[recv] 长度 %s / 校验和 %s / 逐字节 %s\n",
                lenOk ? "OK" : "错", sumOk ? "OK" : "错",
                contentOk ? "OK" : "错");

    if (!contentOk) {
        const std::size_t n = std::min(out.size(), expect.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (out[i] != expect[i]) {
                std::printf("[recv] 第 %zu 个字节不符：收到 %02X，应为 %02X\n",
                            i, out[i], expect[i]);
                break;
            }
        }
    } else {
        std::printf("[recv] PASS —— %zu 字节跨总线完整还原\n", out.size());
    }

    return contentOk ? 0 : 1;
}

void usage() {
    std::printf(
        "用法：\n"
        "  isotp-echo --recv [--iface vcan0] [--wait-s 10] [--bs 8] [--stmin 0]\n"
        "  isotp-echo --send [--iface vcan0]\n"
        "\n"
        "  --bs    接收方要求的块大小（0 = 不要求分块，对端可一口气发完）\n"
        "  --stmin 接收方要求的帧间隔毫秒数\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string iface  = "vcan0";
    bool        wantSend = false;
    bool        wantRecv = false;
    int         waitSec  = 10;
    int         bs       = 8;
    int         stmin    = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--send") {
            wantSend = true;
        } else if (a == "--recv") {
            wantRecv = true;
        } else if (a == "--iface" && i + 1 < argc) {
            iface = argv[++i];
        } else if (a == "--wait-s" && i + 1 < argc) {
            waitSec = std::atoi(argv[++i]);
        } else if (a == "--bs" && i + 1 < argc) {
            bs = std::atoi(argv[++i]);
        } else if (a == "--stmin" && i + 1 < argc) {
            stmin = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "未知参数：%s\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (wantSend == wantRecv) {
        usage();
        return 2;
    }

    return wantSend ? runSend(iface) : runRecv(iface, waitSec, bs, stmin);
}
