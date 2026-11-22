// ============================================================================
// uds_demo.cpp —— M2/M3/M4：把 UDS 服务层架到 vcan0 上
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
//       ./build/uds-demo --flash 400                   # 刷 400 字节（34/36/37 全链）
//
//   T3: candump vcan0                                  # 看总线
// ============================================================================

#include "CanBus.h"
#include "IsoTp.h"
#include "Uds.h"

#include <csignal>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
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
int runEcu(const std::string& iface, int waitSec,
           const std::string& flashPath, int dieAfterBlock) {
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

    uds::UdsConfig ucfg;
    ucfg.flashPath = flashPath;                 // 空 = 只放内存
    uds::UdsServer ecu(ucfg);

    std::printf("[ecu] 就绪：监听 0x%03X，应答 0x%03X，待命 %d 秒\n",
                TESTER_TX, ECU_TX, waitSec);
    std::printf("[ecu] 服务：10 会话 / 22 读DID / 2E 写DID / 27 安全访问 / "
                "19 读故障码 / 14 清故障码 / 3E 保活 / 34+36+37 刷写\n");
    std::printf("[ecu] 镜像：%s，当前 %zu 字节\n",
                flashPath.empty() ? "内存（进程一退就没了）" : flashPath.c_str(),
                ecu.imageSize());

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(waitSec);

    bool  txActive = false;
    Bytes msg;
    int   blocksSeen = 0;

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

                // 拔电源模拟：处理到第 N 个 0x36 之后立刻死掉，
                // 缓冲不刷新、退出码 9，跟真断电一样不留遗言
                if (!msg.empty() && msg[0] == 0x36) {
                    ++blocksSeen;
                    if (dieAfterBlock > 0 && blocksSeen >= dieAfterBlock) {
                        std::printf("[ecu] *** 拔电源 *** 收到第 %d 块就断，"
                                    "镜像没提交\n", blocksSeen);
                        std::fflush(stdout);
                        ::kill(::getpid(), SIGKILL);
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
                "故障码 %zu 条，镜像 %zu 字节\n",
                static_cast<unsigned long long>(ecu.requestCount()),
                static_cast<unsigned>(ecu.session()),
                ecu.securityUnlocked() ? "已解锁" : "已锁定",
                ecu.dtcCount(),
                ecu.imageSize());
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

// ---------------------------------------------------------------------------
// 刷写：10 02 → 27 解锁 → 34 申请 → 36 分块传 → 37 提交
//   total 字节的数据用 64 字节一块发出去，正好能看清 ISO-TP 多帧 + 流控
// ---------------------------------------------------------------------------
int runFlash(const std::string& iface, std::uint32_t total, int waitSec) {
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

    // 发一条请求、等一条正响应（首字节 = 请求 SID + 0x40）
    auto send = [&](const Bytes& req, Bytes& resp, const char* what) -> bool {
        printHex("[tester] -> 请求", req);
        if (!transact(bus, rx, tx, cfg.rxId, req, resp,
                      static_cast<std::uint64_t>(waitSec) * 1000)) {
            std::printf("[tester] %s 没响应（%d 秒超时）\n", what, waitSec);
            return false;
        }
        printHex("[tester] <- 响应", resp);
        if (resp.empty() ||
            resp[0] != static_cast<std::uint8_t>(req[0] + 0x40)) {
            interpret(resp);
            std::printf("[tester] %s 被拒\n", what);
            return false;
        }
        return true;
    };

    Bytes resp;

    // 1) 进编程会话 —— 刷写只在 0x02 会话里允许
    if (!send(Bytes{0x10, 0x02}, resp, "切编程会话")) return 1;
    std::printf("[tester] 已进入编程会话\n\n");

    // 2) 安全访问
    if (!send(Bytes{0x27, 0x01}, resp, "要种子")) return 1;
    const std::uint32_t seed = seedOf(resp);
    const std::uint32_t key  = uds::UdsServer::computeKey(seed);
    std::printf("[tester] 种子 0x%08X → 密钥 0x%08X\n", seed, key);
    if (!send(keyReq(key), resp, "送密钥")) return 1;
    std::printf("[tester] 已解锁\n\n");

    // 3) 0x34 申请下载：34 00 44 <地址 4 字节> <长度 4 字节>
    Bytes dl{0x34, 0x00, 0x44,
             0x08, 0x00, 0x00, 0x00,
             static_cast<std::uint8_t>((total >> 24) & 0xFF),
             static_cast<std::uint8_t>((total >> 16) & 0xFF),
             static_cast<std::uint8_t>((total >> 8) & 0xFF),
             static_cast<std::uint8_t>(total & 0xFF)};
    if (!send(dl, resp, "申请下载")) return 1;
    std::printf("[tester] 申请下载 %u 字节成功\n\n", total);

    // 4) 0x36 分块传
    std::vector<std::uint8_t> pattern(total);
    for (std::uint32_t i = 0; i < total; ++i) {
        pattern[i] = static_cast<std::uint8_t>((i * 7u + 3u) & 0xFF);
    }

    const std::uint32_t chunk = 64;
    std::uint8_t  seq  = 1;
    std::uint32_t sent = 0;
    const std::uint64_t t0 = uds::steadyMs();

    while (sent < total) {
        const std::uint32_t left = total - sent;
        const std::uint32_t n    = (left < chunk) ? left : chunk;

        Bytes blk{0x36, seq};
        blk.insert(blk.end(), pattern.begin() + sent, pattern.begin() + sent + n);

        if (!send(blk, resp, "传数据块")) return 1;
        std::printf("[tester]   块 %02X 传 %u 字节（累计 %u/%u）\n", seq, n, sent + n, total);

        sent += n;
        seq = static_cast<std::uint8_t>(seq + 1);   // 0xFF 之后自然回到 0x00
    }

    const std::uint64_t dt = uds::steadyMs() - t0;
    std::printf("[tester] 全部传完，耗时 %llu ms\n\n", static_cast<unsigned long long>(dt));

    // 5) 0x37 结束并提交
    if (!send(Bytes{0x37}, resp, "结束传输")) return 1;
    std::printf("[tester] 提交成功 —— 新镜像落定，旧镜像被替换\n");
    return 0;
}

void usage() {
    std::printf(
        "用法：\n"
        "  uds-demo --ecu [--iface vcan0] [--wait-s 120] [--flash-file <路径>] [--die-after N]\n"
        "  uds-demo --tester \"22 F1 90\" [--iface vcan0] [--wait-s 5]\n"
        "  uds-demo --unlock [--iface vcan0] [--wait-s 5]\n"
        "  uds-demo --flash 400 [--iface vcan0] [--wait-s 5]\n"
        "\n"
        "  0x27 安全访问在默认会话下不给种子，--unlock 会自动先切扩展会话。\n"
        "  --flash N 走完整刷写链：10 02 → 27 → 34 → 36×N → 37，N 是总字节数。\n"
        "  --flash-file 让 ECU 把已提交的镜像落盘；--die-after N 模拟收到第 N 块就断电。\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string iface    = "vcan0";
    std::string reqStr;
    bool        ecuMode  = false;
    bool        unlockMode = false;
    int         waitSec  = 0;
    int         flashBytes = 0;
    std::string flashFile;
    int         dieAfter = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ecu") {
            ecuMode = true;
        } else if (a == "--unlock") {
            unlockMode = true;
        } else if (a == "--flash" && i + 1 < argc) {
            flashBytes = std::atoi(argv[++i]);
        } else if (a == "--flash-file" && i + 1 < argc) {
            flashFile = argv[++i];
        } else if (a == "--die-after" && i + 1 < argc) {
            dieAfter = std::atoi(argv[++i]);
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

    const int modes = (ecuMode ? 1 : 0) + (unlockMode ? 1 : 0) +
                      (reqStr.empty() ? 0 : 1) + (flashBytes > 0 ? 1 : 0);
    if (modes != 1) {
        usage();
        return 2;
    }

    if (ecuMode) {
        return runEcu(iface, waitSec > 0 ? waitSec : 120, flashFile, dieAfter);
    }
    if (unlockMode) return runUnlock(iface, waitSec > 0 ? waitSec : 5);
    if (flashBytes > 0) {
        return runFlash(iface, static_cast<std::uint32_t>(flashBytes),
                        waitSec > 0 ? waitSec : 5);
    }
    return runTester(iface, parseHex(reqStr), waitSec > 0 ? waitSec : 5);
}
