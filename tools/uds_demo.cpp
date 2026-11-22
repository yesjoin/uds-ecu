// ============================================================================
// uds_demo.cpp —— M2：把 UDS 服务层架到 vcan0 上
//
// 用法（两个终端）：
//   T1: ./build/uds-demo --ecu --wait-s 60          # 虚拟 ECU 待命
//   T2: ./build/uds-demo --tester "10 03"           # 切扩展会话
//       ./build/uds-demo --tester "22 F1 90"        # 读 VIN（20 字节，走多帧）
//       ./build/uds-demo --tester "2E 01 00 00 50"  # 写车速（默认会话下会被拒）
//       ./build/uds-demo --tester "3E 00"           # 保活
//   T3: candump vcan0                               # 看总线
//
// 这一层之后诊断终于闭环：
//   tester 发请求 → ISO-TP 拆帧 → ECU 收齐 → UDS 解释 → 生成响应
//   → ISO-TP 再拆帧 → tester 收齐 → 打印并解读
// ============================================================================

#include "CanBus.h"
#include "IsoTp.h"
#include "Uds.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t TESTER_TX = 0x7E0;   // 请求方向
constexpr std::uint32_t ECU_TX    = 0x7E8;   // 响应方向

void printHex(const char* tag, const Bytes& v) {
    std::printf("%s (%zu 字节):", tag, v.size());
    for (auto b : v) std::printf(" %02X", b);
    std::printf("\n");
}

// "10 03" / "1003" / "10,03" 都认
Bytes parseHex(const std::string& s) {
    Bytes out;
    int   hi = -1;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) continue;
        const int v = std::isdigit(static_cast<unsigned char>(c))
                          ? c - '0'
                          : std::tolower(c) - 'a' + 10;
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<std::uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    return out;
}

const char* nrcName(std::uint8_t nrc) {
    switch (nrc) {
        case 0x11: return "服务不支持";
        case 0x12: return "子功能不支持";
        case 0x13: return "长度不对";
        case 0x22: return "条件不满足";
        case 0x31: return "参数越界";
        case 0x33: return "安全访问未通过";
        case 0x7F: return "当前会话不允许";
        default:   return "未知";
    }
}

void waitUntilMs(std::uint64_t targetMs) {
    while (true) {
        const std::uint64_t now = uds::steadyMs();
        if (now >= targetMs) return;
        const std::uint64_t remain = targetMs - now;
        if (remain > 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<long long>(remain - 3)));
        }
    }
}

enum class Pump { Continue, Done, Aborted };

// 把发送状态机往前推一步：该发就发，该等就等
Pump pumpTx(uds::CanBus& bus, uds::IsoTpTx& tx) {
    uds::Frame out;
    switch (tx.poll(uds::steadyMs(), out)) {
        case uds::IsoTpTx::Status::SentFrame:
            bus.send(out);
            return Pump::Continue;
        case uds::IsoTpTx::Status::WaitStmin:
            waitUntilMs(tx.nextSendMs());
            return Pump::Continue;
        case uds::IsoTpTx::Status::Done:
            return Pump::Done;
        case uds::IsoTpTx::Status::Aborted:
            return Pump::Aborted;
        default:
            return Pump::Continue;   // WaitFlowControl：等下一轮
    }
}

// ---------------------------------------------------------------------------
// ECU 侧：等请求、答响应
// ---------------------------------------------------------------------------
int runEcu(const std::string& iface, int waitSec) {
    uds::CanBus bus(iface);
    if (!bus.ok()) {
        std::fprintf(stderr, "[ecu] 打不开 %s\n", iface.c_str());
        return 1;
    }

    uds::IsoTpConfig cfg;
    cfg.rxId = TESTER_TX;
    cfg.txId = ECU_TX;

    uds::IsoTp    rx(cfg);
    uds::IsoTpTx  tx(cfg);
    uds::UdsServer ecu;

    std::printf("[ecu] 就绪：监听 0x%03X，应答用 0x%03X，待命 %d 秒\n",
                TESTER_TX, ECU_TX, waitSec);
    std::printf("[ecu] 服务：0x10 会话 / 0x22 读DID / 0x2E 写DID / 0x3E 保活\n");

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(waitSec);

    bool  txActive = false;
    Bytes msg;

    while (std::chrono::steady_clock::now() < deadline) {
        uds::Frame f;

        if (bus.recv(f, 20)) {
            const bool isFc = (f.id == cfg.rxId) && ((f.data[0] >> 4) == 0x3);

            if (isFc) {
                tx.onFlowControl(f, uds::steadyMs());   // 客户端在管我们的节奏
            } else if (rx.feed(f, msg)) {
                printHex("[ecu] <- 请求", msg);

                const Bytes resp = ecu.handle(msg, uds::steadyMs());
                if (resp.empty()) {
                    std::printf("[ecu] -> 不响应（抑制正响应）\n");
                } else {
                    printHex("[ecu] -> 响应", resp);
                    if (tx.begin(resp)) {
                        bus.send(tx.firstFrame());
                        txActive = true;
                    }
                }
            }

            if (rx.needFlowControl()) {
                bus.send(rx.makeFlowControl());
                rx.clearFlowControl();
            }
        }

        if (txActive) {
            const Pump p = pumpTx(bus, tx);
            if (p != Pump::Continue) {
                if (p == Pump::Aborted) std::printf("[ecu] 客户端回了 OVFLW\n");
                txActive = false;
            }
        }

        ecu.tick(uds::steadyMs());    // S3：空闲太久自动退回默认会话
    }

    std::printf("\n[ecu] 待命结束：共处理 %llu 条请求，结束时会话 = 0x%02X\n",
                static_cast<unsigned long long>(ecu.requestCount()),
                static_cast<unsigned>(ecu.session()));
    return 0;
}

// ---------------------------------------------------------------------------
// 测试仪侧：发一条请求，等响应，然后解读
// ---------------------------------------------------------------------------
int runTester(const std::string& iface, const Bytes& request, int waitSec) {
    uds::CanBus bus(iface);
    if (!bus.ok()) {
        std::fprintf(stderr, "[tester] 打不开 %s\n", iface.c_str());
        return 1;
    }

    uds::IsoTpConfig cfg;
    cfg.rxId      = ECU_TX;
    cfg.txId      = TESTER_TX;
    cfg.blockSize = 8;      // 我们给 ECU 的流控：每 8 帧放行一次
    cfg.stMinMs   = 0;      // 不额外限速

    uds::IsoTp   rx(cfg);
    uds::IsoTpTx tx(cfg);

    printHex("[tester] -> 请求", request);

    if (!tx.begin(request)) {
        std::fprintf(stderr, "[tester] 请求无效（空或超过 4095 字节）\n");
        return 2;
    }
    bus.send(tx.firstFrame());

    bool  txActive = true;
    bool  gotResp  = false;
    Bytes msg;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(waitSec);

    while (std::chrono::steady_clock::now() < deadline) {
        uds::Frame f;

        if (bus.recv(f, 20)) {
            const bool isFc = (f.id == cfg.rxId) && ((f.data[0] >> 4) == 0x3);

            if (isFc) {
                tx.onFlowControl(f, uds::steadyMs());
            } else if (rx.feed(f, msg)) {
                gotResp = true;
                break;
            }

            if (rx.needFlowControl()) {
                bus.send(rx.makeFlowControl());
                rx.clearFlowControl();
            }
        }

        if (txActive) {
            const Pump p = pumpTx(bus, tx);
            if (p != Pump::Continue) txActive = false;
        }
    }

    if (!gotResp) {
        std::printf("[tester] %.1f 秒内没等到响应\n", static_cast<double>(waitSec));
        return 1;
    }

    printHex("[tester] <- 响应", msg);

    if (msg.size() >= 3 && msg[0] == 0x7F) {
        std::printf("[tester] 负面响应：服务 0x%02X 被拒，NRC=0x%02X（%s）\n",
                    msg[1], msg[2], nrcName(msg[2]));
    } else if (!msg.empty()) {
        std::printf("[tester] 正响应：服务号 0x%02X = 请求 0x%02X + 0x40\n",
                    msg[0], static_cast<unsigned>(msg[0] - 0x40));
    }
    return 0;
}

void usage() {
    std::printf(
        "用法：\n"
        "  uds-demo --ecu [--iface vcan0] [--wait-s 60]\n"
        "  uds-demo --tester \"10 03\" [--iface vcan0] [--wait-s 5]\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string iface   = "vcan0";
    std::string reqStr;
    bool        ecuMode = false;
    int         waitSec = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ecu") {
            ecuMode = true;
        } else if (a == "--tester" && i + 1 < argc) {
            reqStr = argv[++i];
        } else if (a == "--iface" && i + 1 < argc) {
            iface = argv[++i];
        } else if (a == "--wait-s" && i + 1 < argc) {
            waitSec = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "未知参数：%s\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (ecuMode && !reqStr.empty()) {     // --ecu 不接受请求字符串
        usage();
        return 2;
    }
    if (!ecuMode && reqStr.empty()) {     // --tester 必须给请求
        usage();
        return 2;
    }

    if (ecuMode) {
        return runEcu(iface, waitSec > 0 ? waitSec : 60);
    }
    return runTester(iface, parseHex(reqStr), waitSec > 0 ? waitSec : 5);
}
