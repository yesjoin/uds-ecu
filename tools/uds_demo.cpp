// ============================================================================
// uds_demo.cpp —— M2/M3：把 UDS 服务层架到 vcan0 上
//
// 用法（两个终端）：
//   T1: ./build/uds-demo --ecu --wait-s 120            # 虚拟 ECU 待命
//
//   T2: ./build/uds-demo --tester "10 03"              # 切扩展会话
//       ./build/uds-demo --tester "22 F1 90"           # 读 VIN（20 字节，走多帧）
//       ./build/uds-demo --unlock                      # 自动做完整的安全访问
//       ./build/uds-demo --tester "2E 02 00 00 00 27 10"  # 写里程（必须先解锁）
//       ./build/uds-demo --tester "19 02 FF"           # 列出全部故障码
//       ./build/uds-demo --tester "19 01 08"           # 只数"已确认"的故障码
//       ./build/uds-demo --tester "14 FF FF FF"        # 清故障码
//
//   T3: candump vcan0                                  # 看总线
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
        case 0x24: return "请求顺序错误";
        case 0x31: return "参数越界";
        case 0x33: return "安全访问未通过";
        case 0x35: return "密钥无效";
        case 0x36: return "尝试次数超限";
        case 0x37: return "冷却时间未到";
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
            return Pump::Continue;   // WaitFlowControl
    }
}

// 一问一答：发一条请求，等一条响应。超时返回 false。
bool transact(uds::CanBus& bus, uds::IsoTp& rx, uds::IsoTpTx& tx,
              std::uint32_t expectId, const Bytes& req, Bytes& resp, int waitMs) {
    resp.clear();
    if (!tx.begin(req)) return false;
    if (!bus.send(tx.firstFrame())) return false;

    bool txActive = true;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(waitMs);

    while (std::chrono::steady_clock::now() < deadline) {
        uds::Frame f;

        if (bus.recv(f, 10)) {
            if (f.id == expectId && ((f.data[0] >> 4) == 0x3)) {
                tx.onFlowControl(f, uds::steadyMs());    // 对端的流控帧
            } else if (rx.feed(f, resp)) {
                return true;                             // 收到完整响应
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
    return false;
}

std::uint32_t seedOf(const Bytes& r) {
    if (r.size() < 6) return 0;
    return (static_cast<std::uint32_t>(r[2]) << 24) |
           (static_cast<std::uint32_t>(r[3]) << 16) |
           (static_cast<std::uint32_t>(r[4]) << 8) |
            static_cast<std::uint32_t>(r[5]);
}

Bytes keyReq(std::uint32_t key) {
    return Bytes{0x27, 0x02,
                 static_cast<std::uint8_t>((key >> 24) & 0xFF),
                 static_cast<std::uint8_t>((key >> 16) & 0xFF),
                 static_cast<std::uint8_t>((key >> 8) & 0xFF),
                 static_cast<std::uint8_t>(key & 0xFF)};
}

// 把响应翻译成人话
void interpret(const Bytes& msg) {
    if (msg.size() >= 3 && msg[0] == 0x7F) {
        std::printf("[tester] 负面响应：服务 0x%02X 被拒，NRC=0x%02X（%s）\n",
                    msg[1], msg[2], nrcName(msg[2]));
        return;
    }
    if (msg.size() >= 2 && msg[0] == 0x67 && msg[1] == 0x01) {
        std::printf("[tester] 拿到种子 0x%08X\n", seedOf(msg));
        return;
    }
    if (msg.size() >= 2 && msg[0] == 0x67 && msg[1] == 0x02) {
        std::printf("[tester] 密钥通过，安全访问已解锁\n");
        return;
    }
    if (!msg.empty()) {
        std::printf("[tester] 正响应：服务号 0x%02X = 请求 0x%02X + 0x40\n",
                    msg[0], static_cast<unsigned>(msg[0] - 0x40));
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

    std::printf("[ecu] 就绪：监听 0x%03X，应答 0x%03X，待命 %d 秒\n",
                TESTER_TX, ECU_TX, waitSec);
    std::printf("[ecu] 服务：10 会话 / 22 读DID / 2E 写DID / 27 安全访问 / "
                "19 读故障码 / 14 清故障码 / 3E 保活\n");

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(waitSec);

    bool  txActive = false;
    Bytes msg;

    while (std::chrono::steady_clock::now() < deadline) {
        uds::Frame f;

        if (bus.recv(f, 20)) {
            const bool isFc = (f.id == cfg.rxId) && ((f.data[0] >> 4) == 0x3);

            if (isFc) {
                tx.onFlowControl(f, uds::steadyMs());
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

        ecu.tick(uds::steadyMs());
    }

    std::printf("\n[ecu] 待命结束：处理 %llu 条请求，会话 = 0x%02X，解锁状态 = %s，"
                "故障码 %zu 条\n",
                static_cast<unsigned long long>(ecu.requestCount()),
                static_cast<unsigned>(ecu.session()),
                ecu.securityUnlocked() ? "已解锁" : "已锁定",
                ecu.dtcCount());
    return 0;
}

// ---------------------------------------------------------------------------
// 测试仪侧
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
    cfg.blockSize = 8;
    cfg.stMinMs   = 0;

    uds::IsoTp   rx(cfg);
    uds::IsoTpTx tx(cfg);

    printHex("[tester] -> 请求", request);

    Bytes resp;
    if (!transact(bus, rx, tx, cfg.rxId, request, resp, waitSec * 1000)) {
        std::printf("[tester] %d 秒内没等到响应\n", waitSec);
        return 1;
    }

    printHex("[tester] <- 响应", resp);
    interpret(resp);
    return 0;
}

// 安全访问三步走：切会话 → 要种子 → 算密钥送过去
int runUnlock(const std::string& iface, int waitSec) {
    uds::CanBus bus(iface);
    if (!bus.ok()) {
        std::fprintf(stderr, "[tester] 打不开 %s\n", iface.c_str());
        return 1;
    }

    uds::IsoTpConfig cfg;
    cfg.rxId      = ECU_TX;
    cfg.txId      = TESTER_TX;
    cfg.blockSize = 8;
    cfg.stMinMs   = 0;

    uds::IsoTp   rx(cfg);
    uds::IsoTpTx tx(cfg);

    Bytes resp;

    // 第 1 步：0x27 在默认会话不给种子，必须先切扩展会话
    Bytes step1{0x10, 0x03};
    printHex("[tester] -> 请求", step1);
    if (!transact(bus, rx, tx, cfg.rxId, step1, resp, waitSec * 1000)) {
        std::printf("[tester] 切会话没响应\n");
        return 1;
    }
    printHex("[tester] <- 响应", resp);
    if (resp.size() < 2 || resp[0] != 0x50) {
        interpret(resp);
        return 1;
    }

    // 第 2 步：要种子
    Bytes step2{0x27, 0x01};
    printHex("[tester] -> 请求", step2);
    if (!transact(bus, rx, tx, cfg.rxId, step2, resp, waitSec * 1000)) {
        std::printf("[tester] 要种子没响应\n");
        return 1;
    }
    printHex("[tester] <- 响应", resp);
    if (resp.size() < 6 || resp[0] != 0x67) {
        interpret(resp);
        return 1;
    }

    const std::uint32_t seed = seedOf(resp);
    const std::uint32_t key  = uds::UdsServer::computeKey(seed);
    std::printf("[tester] 种子 0x%08X → 算出密钥 0x%08X\n", seed, key);

    // 第 3 步：送密钥
    const Bytes step3 = keyReq(key);
    printHex("[tester] -> 请求", step3);
    if (!transact(bus, rx, tx, cfg.rxId, step3, resp, waitSec * 1000)) {
        std::printf("[tester] 送密钥没响应\n");
        return 1;
    }
    printHex("[tester] <- 响应", resp);
    interpret(resp);
    return (resp.size() >= 2 && resp[0] == 0x67 && resp[1] == 0x02) ? 0 : 1;
}

void usage() {
    std::printf(
        "用法：\n"
        "  uds-demo --ecu [--iface vcan0] [--wait-s 120]\n"
        "  uds-demo --tester \"22 F1 90\" [--iface vcan0] [--wait-s 5]\n"
        "  uds-demo --unlock [--iface vcan0] [--wait-s 5]\n"
        "\n"
        "  0x27 安全访问在默认会话下不给种子，--unlock 会自动先切扩展会话。\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string iface    = "vcan0";
    std::string reqStr;
    bool        ecuMode  = false;
    bool        unlockMode = false;
    int         waitSec  = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ecu") {
            ecuMode = true;
        } else if (a == "--unlock") {
            unlockMode = true;
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

    const int modes = (ecuMode ? 1 : 0) + (unlockMode ? 1 : 0) + (reqStr.empty() ? 0 : 1);
    if (modes != 1) {
        usage();
        return 2;
    }

    if (ecuMode)    return runEcu(iface, waitSec > 0 ? waitSec : 120);
    if (unlockMode) return runUnlock(iface, waitSec > 0 ? waitSec : 5);
    return runTester(iface, parseHex(reqStr), waitSec > 0 ? waitSec : 5);
}
