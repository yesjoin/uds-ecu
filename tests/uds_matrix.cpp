// ============================================================================
// uds_matrix.cpp —— M5：200 条测试矩阵
//
// 和 selftest 的区别：
//   selftest 是"写一条、跑一条、修一条"，验证功能做没做出来；
//   matrix   是"一次性铺 200 条"，验证做出来的东西在边界上会不会塌。
//
// 每条用例 = 前置状态 + 一条请求 + 一条期望响应。
// 期望响应全部按 ISO 14229 / 项目约定手写，不从实现里反推 ——
// 否则实现错了、测试跟着错，等于没测。
// ============================================================================

#include "Uds.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
Bytes ascii(const char* s) {
    Bytes v;
    for (const char* p = s; *p != '\0'; ++p) v.push_back(static_cast<std::uint8_t>(*p));
    return v;
}

Bytes cat(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Bytes u32(std::uint32_t v) {
    return Bytes{static_cast<std::uint8_t>((v >> 24) & 0xFF),
                 static_cast<std::uint8_t>((v >> 16) & 0xFF),
                 static_cast<std::uint8_t>((v >> 8) & 0xFF),
                 static_cast<std::uint8_t>(v & 0xFF)};
}

Bytes u16(std::uint16_t v) {
    return Bytes{static_cast<std::uint8_t>((v >> 8) & 0xFF),
                 static_cast<std::uint8_t>(v & 0xFF)};
}

std::uint32_t seedOf(const Bytes& r) {
    if (r.size() < 6) return 0;
    return (static_cast<std::uint32_t>(r[2]) << 24) |
           (static_cast<std::uint32_t>(r[3]) << 16) |
           (static_cast<std::uint32_t>(r[4]) << 8) |
            static_cast<std::uint32_t>(r[5]);
}

std::string hex(const Bytes& v) {
    if (v.empty()) return "(不响应)";
    std::string s;
    char b[4];
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::snprintf(b, sizeof(b), "%02X", v[i]);
        if (i) s += " ";
        s += b;
    }
    return s;
}

// ---------------------------------------------------------------------------
// 前置状态
// ---------------------------------------------------------------------------
enum class Prep {
    None,                       // 上电默认
    Ext,                        // 10 03
    ExtUnlocked,                // 10 03 + 27 01/02
    ExtSeed,                    // 10 03 + 新鲜种子（等密钥，还没错过）
    ExtSeedWrong1,              // 错 1 次 + 又拿到新鲜种子（下次错就是第 2 次）
    ExtSeedWrong2,              // 错 2 次 + 又拿到新鲜种子（下次错就是第 3 次）
    ExtLocked,                  // 错 3 次触发锁定
    ExtClearDtc,                // 10 03 + 14 FF FF FF
    Prog,                       // 10 02
    ProgUnlocked,               // 10 02 + 解锁
    ProgBackToDefault,          // 10 02 + 解锁 + 10 01
    ProgDl100,                  // 编程会话 + 解锁 + 34(100)，一块都没传
    ProgDl100Half,              // 编程会话 + 解锁 + 34(100) + 36 01(50 字节)
    ProgDl100Full,              // 编程会话 + 解锁 + 34(100) + 36 01(50) + 36 02(50)
};

const char* prepName(Prep p) {
    switch (p) {
        case Prep::None:            return "默认会话";
        case Prep::Ext:             return "扩展会话";
        case Prep::ExtUnlocked:     return "扩展+已解锁";
        case Prep::ExtSeed:         return "扩展+已给种子";
        case Prep::ExtSeedWrong1:   return "扩展+错1次+新种子";
        case Prep::ExtSeedWrong2:   return "扩展+错2次+新种子";
        case Prep::ExtLocked:       return "扩展+已锁定";
        case Prep::ExtClearDtc:     return "扩展+已清故障";
        case Prep::Prog:            return "编程会话";
        case Prep::ProgUnlocked:    return "编程+已解锁";
        case Prep::ProgBackToDefault: return "解锁后回默认";
        case Prep::ProgDl100:       return "已申请下载100";
        case Prep::ProgDl100Half:   return "下载50/100";
        case Prep::ProgDl100Full:   return "下载100/100";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 期望类型
// ---------------------------------------------------------------------------
enum class Kind {
    Exact,     // 响应必须逐字节等于 expect
    Mute,      // 响应必须为空（抑制正响应）
    Seed,      // 种子响应：67 <sub> + 4 字节非零，且连要两次必须不同
    SelfKey,   // 请求由 runner 现场拼：27 02 + computeKey(前置拿到的种子)
    ReplayKey, // 同上，但同一把密钥连送两次，第二次必须被拒
};

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------
struct Case {
    std::string id;
    std::string svc;
    std::string cat;
    std::string desc;
    Prep        prep = Prep::None;
    Kind        kind = Kind::Exact;
    Bytes       req;
    Bytes       expect;
    bool        hasAfter = false;
    Bytes       after;
    Bytes       afterExpect;
};

std::vector<Case> g_cases;

std::size_t add(const char* svc, const char* cat, const char* desc,
                Prep prep, Kind kind, Bytes req, Bytes expect = Bytes{}) {
    Case c;
    // 32 而不是 16：%zu 是 size_t，最坏情况能打 20 位数字，"M-" + 3 位补零 + 20 位
    // 塞不进 16；gcc 15 的 -Wformat-truncation 会直接把它当错误（-Werror 下编不过）
    char id[32];
    std::snprintf(id, sizeof(id), "M-%03zu", g_cases.size() + 1);
    c.id     = id;
    c.svc    = svc;
    c.cat    = cat;
    c.desc   = desc;
    c.prep   = prep;
    c.kind   = kind;
    c.req    = req;
    c.expect = expect;
    g_cases.push_back(c);
    return g_cases.size() - 1;
}

// 闭环用例：先发 req 看 expect，紧接着再发 after 看 afterExpect
std::size_t addAfter(const char* svc, const char* cat, const char* desc,
                     Prep prep, Bytes req, Bytes expect,
                     Bytes after, Bytes afterExpect) {
    const std::size_t i = add(svc, cat, desc, prep, Kind::Exact, req, expect);
    g_cases[i].hasAfter     = true;
    g_cases[i].after        = after;
    g_cases[i].afterExpect  = afterExpect;
    return i;
}

// ---------------------------------------------------------------------------
// 前置动作
// ---------------------------------------------------------------------------
Bytes downloadReq(std::uint32_t size) {
    // 34 00 44 <地址4字节> <长度4字节>
    Bytes r{0x34, 0x00, 0x44};
    r = cat(r, u32(0x08000000u));
    r = cat(r, u32(size));
    return r;
}

void unlock(uds::UdsServer& ecu, std::uint32_t& lastSeed) {
    const Bytes s = ecu.handle(Bytes{0x27, 0x01}, 0);
    lastSeed = seedOf(s);
    Bytes k{0x27, 0x02};
    k = cat(k, u32(uds::UdsServer::computeKey(lastSeed)));
    ecu.handle(k, 0);
}

void wrongKey(uds::UdsServer& ecu) {
    ecu.handle(Bytes{0x27, 0x02, 0x00, 0x00, 0x00, 0x00}, 0);
}

void applyPrep(uds::UdsServer& ecu, Prep p, std::uint32_t& lastSeed) {
    switch (p) {
        case Prep::None:
            break;
        case Prep::Ext:
            ecu.handle(Bytes{0x10, 0x03}, 0);
            break;
        case Prep::ExtUnlocked:
            ecu.handle(Bytes{0x10, 0x03}, 0);
            unlock(ecu, lastSeed);
            break;
        case Prep::ExtSeed:
            ecu.handle(Bytes{0x10, 0x03}, 0);
            lastSeed = seedOf(ecu.handle(Bytes{0x27, 0x01}, 0));
            break;
        case Prep::ExtSeedWrong1:
            // 错一次之后，必须重新要种子才能再试 —— 种子是一次性的
            applyPrep(ecu, Prep::ExtSeed, lastSeed);
            wrongKey(ecu);
            lastSeed = seedOf(ecu.handle(Bytes{0x27, 0x01}, 0));
            break;
        case Prep::ExtSeedWrong2:
            applyPrep(ecu, Prep::ExtSeedWrong1, lastSeed);
            wrongKey(ecu);
            lastSeed = seedOf(ecu.handle(Bytes{0x27, 0x01}, 0));
            break;
        case Prep::ExtLocked:
            applyPrep(ecu, Prep::ExtSeedWrong2, lastSeed);
            wrongKey(ecu);                    // 第 3 次错 → 36 并进入冷却
            break;
        case Prep::ExtClearDtc:
            ecu.handle(Bytes{0x10, 0x03}, 0);
            ecu.handle(Bytes{0x14, 0xFF, 0xFF, 0xFF}, 0);
            break;
        case Prep::Prog:
            ecu.handle(Bytes{0x10, 0x02}, 0);
            break;
        case Prep::ProgUnlocked:
            ecu.handle(Bytes{0x10, 0x02}, 0);
            unlock(ecu, lastSeed);
            break;
        case Prep::ProgBackToDefault:
            ecu.handle(Bytes{0x10, 0x02}, 0);
            unlock(ecu, lastSeed);
            ecu.handle(Bytes{0x10, 0x01}, 0);
            break;
        case Prep::ProgDl100:
            applyPrep(ecu, Prep::ProgUnlocked, lastSeed);
            ecu.handle(downloadReq(100), 0);
            break;
        case Prep::ProgDl100Half: {
            applyPrep(ecu, Prep::ProgDl100, lastSeed);
            Bytes b{0x36, 0x01};
            for (int i = 0; i < 50; ++i) b.push_back(static_cast<std::uint8_t>(i));
            ecu.handle(b, 0);
            break;
        }
        case Prep::ProgDl100Full: {
            applyPrep(ecu, Prep::ProgDl100, lastSeed);
            Bytes b{0x36, 0x01};
            for (int i = 0; i < 50; ++i) b.push_back(static_cast<std::uint8_t>(i));
            ecu.handle(b, 0);
            Bytes c{0x36, 0x02};
            for (int i = 50; i < 100; ++i) c.push_back(static_cast<std::uint8_t>(i));
            ecu.handle(c, 0);
            break;
        }
    }
}

// -----------------------------------------------------------------============================================================================
// 一、0x10 会话控制（10 条）
// -----------------------------------------------------------------============================================================================
void buildSessionCases() {
    const char* S = "0x10";

    add(S, "正常", "切默认会话", Prep::None, Kind::Exact,
        Bytes{0x10, 0x01}, Bytes{0x50, 0x01, 0x00, 0x32, 0x13, 0x88});
    add(S, "正常", "切编程会话", Prep::None, Kind::Exact,
        Bytes{0x10, 0x02}, Bytes{0x50, 0x02, 0x00, 0x32, 0x13, 0x88});
    add(S, "正常", "切扩展会话", Prep::None, Kind::Exact,
        Bytes{0x10, 0x03}, Bytes{0x50, 0x03, 0x00, 0x32, 0x13, 0x88});
    add(S, "正常", "切安全会话", Prep::None, Kind::Exact,
        Bytes{0x10, 0x04}, Bytes{0x50, 0x04, 0x00, 0x32, 0x13, 0x88});
    add(S, "边界", "0x81 抑制正响应位（本实现忽略，仍回正响应）", Prep::None, Kind::Exact,
        Bytes{0x10, 0x81}, Bytes{0x50, 0x01, 0x00, 0x32, 0x13, 0x88});
    add(S, "异常", "子功能 0x00", Prep::None, Kind::Exact,
        Bytes{0x10, 0x00}, Bytes{0x7F, 0x10, 0x12});
    add(S, "异常", "子功能 0x05 越界", Prep::None, Kind::Exact,
        Bytes{0x10, 0x05}, Bytes{0x7F, 0x10, 0x12});
    add(S, "异常", "只有服务字，缺子功能", Prep::None, Kind::Exact,
        Bytes{0x10}, Bytes{0x7F, 0x10, 0x13});
    add(S, "异常", "多带一个字节", Prep::None, Kind::Exact,
        Bytes{0x10, 0x01, 0x00}, Bytes{0x7F, 0x10, 0x13});
    add(S, "异常", "子功能 0xFF（除掉抑制位后越界）", Prep::None, Kind::Exact,
        Bytes{0x10, 0xFF}, Bytes{0x7F, 0x10, 0x12});
}

// -----------------------------------------------------------------============================================================================
// 二、0x22 读 DID（49 条）
// -----------------------------------------------------------------============================================================================
struct DidSpec {
    std::uint16_t id;
    Bytes         data;        // 默认会话下的数据
    bool          sessionDep;  // true = F186，数据随会话变
};

void buildReadCases() {
    const char* S = "0x22";

    const DidSpec specs[] = {
        {0xF186, Bytes{0x01},                        true},
        {0xF187, ascii("1234567890A"),               false},
        {0xF188, ascii("SW01.02.03"),                false},
        {0xF18A, ascii("UDS-ECU"),                   false},
        {0xF190, ascii("LSVAA0000A1234567"),         false},
        {0xF191, ascii("HW01"),                      false},
        {0x0100, Bytes{0x00, 0x00},                  false},
        {0x0101, Bytes{0x0E, 0x74},                  false},
        {0x0200, Bytes{0x00, 0x00, 0x0F, 0xA0},      false},
    };

    const Prep  preps[]  = {Prep::None, Prep::Ext, Prep::Prog, Prep::ProgUnlocked};
    const std::uint8_t sess[] = {0x01, 0x03, 0x02, 0x02};   // 对应上面四种前置

    // 9 个已知 DID × 4 种会话前置 = 36 条
    for (std::size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); ++i) {
        for (std::size_t k = 0; k < 4; ++k) {
            Bytes expect{0x62};
            expect = cat(expect, u16(specs[i].id));
            expect = cat(expect, specs[i].sessionDep ? Bytes{sess[k]} : specs[i].data);

            char desc[128];
            std::snprintf(desc, sizeof(desc), "读 DID 0x%04X（%s）",
                          static_cast<unsigned>(specs[i].id), prepName(preps[k]));
            add(S, "正常", desc, preps[k], Kind::Exact,
                cat(Bytes{0x22}, u16(specs[i].id)), expect);
        }
    }

    // 不存在的 DID × 4 种前置 = 4 条
    for (std::size_t k = 0; k < 4; ++k) {
        char desc[128];
        std::snprintf(desc, sizeof(desc), "读不存在的 DID 0x1234（%s）", prepName(preps[k]));
        add(S, "异常", desc, preps[k], Kind::Exact,
            Bytes{0x22, 0x12, 0x34}, Bytes{0x7F, 0x22, 0x31});
    }

    // 多个 DID 顺序拼接 × 4 种前置 = 4 条
    for (std::size_t k = 0; k < 4; ++k) {
        Bytes expect{0x62, 0xF1, 0x87};
        expect = cat(expect, ascii("1234567890A"));
        expect = cat(expect, Bytes{0xF1, 0x88});
        expect = cat(expect, ascii("SW01.02.03"));

        char desc[128];
        std::snprintf(desc, sizeof(desc), "一次读 F187+F188 顺序拼接（%s）", prepName(preps[k]));
        add(S, "正常", desc, preps[k], Kind::Exact,
            Bytes{0x22, 0xF1, 0x87, 0xF1, 0x88}, expect);
    }

    // 一好一坏 → 整条失败，不许只回一半 = 2 条
    add(S, "异常", "读 F187 + 不存在的 9999 → 整条失败", Prep::None, Kind::Exact,
        Bytes{0x22, 0xF1, 0x87, 0x99, 0x99}, Bytes{0x7F, 0x22, 0x31});
    add(S, "异常", "读 9999 + F187 → 整条失败", Prep::Ext, Kind::Exact,
        Bytes{0x22, 0x99, 0x99, 0xF1, 0x87}, Bytes{0x7F, 0x22, 0x31});

    // 长度类异常 = 2 条
    add(S, "异常", "只有服务字", Prep::None, Kind::Exact,
        Bytes{0x22}, Bytes{0x7F, 0x22, 0x13});
    add(S, "异常", "DID 只给一半（奇数个字节）", Prep::None, Kind::Exact,
        Bytes{0x22, 0xF1}, Bytes{0x7F, 0x22, 0x13});

    // 重复读同一个 DID = 1 条
    add(S, "边界", "同一个 DID 连读两次（0100×2）", Prep::Ext, Kind::Exact,
        Bytes{0x22, 0x01, 0x00, 0x01, 0x00},
        Bytes{0x62, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00});
    add(S, "边界", "VIN 读两次（34 字节大响应）", Prep::Prog, Kind::Exact,
        Bytes{0x22, 0xF1, 0x90, 0xF1, 0x90},
        cat(cat(Bytes{0x62, 0xF1, 0x90}, ascii("LSVAA0000A1234567")),
            cat(Bytes{0xF1, 0x90}, ascii("LSVAA0000A1234567"))));
}

// -----------------------------------------------------------------============================================================================
// 三、0x2E 写 DID（27 条）
// -----------------------------------------------------------------============================================================================
void buildWriteCases() {
    const char* S = "0x2E";

    struct WSpec {
        std::uint16_t id;
        Bytes         data;
        Bytes         okResp;
    };
    const WSpec specs[] = {
        {0x0100, Bytes{0x00, 0x50},             Bytes{0x6E, 0x01, 0x00}},
        {0x0101, Bytes{0x0F, 0xFF},             Bytes{0x6E, 0x01, 0x01}},
        {0x0200, Bytes{0x00, 0x00, 0x1F, 0x40}, Bytes{0x6E, 0x02, 0x00}},
    };

    const Prep preps[] = {Prep::None, Prep::Ext, Prep::Prog, Prep::ExtUnlocked};

    // 3 个可写 DID × 4 种前置 = 12 条
    for (std::size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); ++i) {
        for (std::size_t k = 0; k < 4; ++k) {
            Bytes req{0x2E};
            req = cat(req, u16(specs[i].id));
            req = cat(req, specs[i].data);

            Bytes expect;
            if (preps[k] == Prep::None || preps[k] == Prep::Prog) {
                expect = Bytes{0x7F, 0x2E, 0x7F};            // 会话不许写
            } else if (!specs[i].okResp.size()) {
                expect = Bytes{0x7F, 0x2E, 0x31};
            } else if (specs[i].id == 0x0200 && preps[k] == Prep::Ext) {
                expect = Bytes{0x7F, 0x2E, 0x33};            // 受保护，没解锁
            } else {
                expect = specs[i].okResp;
            }

            char desc[128];
            std::snprintf(desc, sizeof(desc), "写 DID 0x%04X（%s）",
                          static_cast<unsigned>(specs[i].id), prepName(preps[k]));
            add(S, preps[k] == Prep::Ext || preps[k] == Prep::ExtUnlocked ? "正常" : "越权",
                desc, preps[k], Kind::Exact, req, expect);
        }
    }

    // 只读 DID = 3 条
    add(S, "越权", "写只读 DID F190（默认会话）", Prep::None, Kind::Exact,
        Bytes{0x2E, 0xF1, 0x90, 0x41}, Bytes{0x7F, 0x2E, 0x7F});
    add(S, "越权", "写只读 DID F190（扩展会话）", Prep::Ext, Kind::Exact,
        Bytes{0x2E, 0xF1, 0x90, 0x41}, Bytes{0x7F, 0x2E, 0x31});
    add(S, "越权", "写只读 DID F190（已解锁也没用）", Prep::ExtUnlocked, Kind::Exact,
        Bytes{0x2E, 0xF1, 0x90, 0x41}, Bytes{0x7F, 0x2E, 0x31});

    // 只读 DID F187 = 1 条
    add(S, "越权", "写只读 DID F187", Prep::Ext, Kind::Exact,
        Bytes{0x2E, 0xF1, 0x87, 0x41}, Bytes{0x7F, 0x2E, 0x31});

    // 不存在的 DID = 2 条
    add(S, "异常", "写不存在的 DID 0x1234（扩展）", Prep::Ext, Kind::Exact,
        Bytes{0x2E, 0x12, 0x34, 0x01}, Bytes{0x7F, 0x2E, 0x31});
    add(S, "异常", "写不存在的 DID 0x1234（已解锁）", Prep::ExtUnlocked, Kind::Exact,
        Bytes{0x2E, 0x12, 0x34, 0x01}, Bytes{0x7F, 0x2E, 0x31});

    // 长度不匹配 = 2 条
    add(S, "异常", "写 0100 只给 1 字节（要 2 字节）", Prep::Ext, Kind::Exact,
        Bytes{0x2E, 0x01, 0x00, 0x00}, Bytes{0x7F, 0x2E, 0x13});
    add(S, "异常", "写 0100 给 3 字节（要 2 字节）", Prep::Ext, Kind::Exact,
        Bytes{0x2E, 0x01, 0x00, 0x00, 0x00, 0x00}, Bytes{0x7F, 0x2E, 0x13});

    // 太短 = 2 条
    add(S, "异常", "写 DID 但没给数据", Prep::Ext, Kind::Exact,
        Bytes{0x2E, 0x01, 0x00}, Bytes{0x7F, 0x2E, 0x13});
    add(S, "异常", "只有服务字", Prep::Ext, Kind::Exact,
        Bytes{0x2E}, Bytes{0x7F, 0x2E, 0x13});

    // 解锁后写里程 = 1 条
    add(S, "正常", "解锁后写里程 8000", Prep::ExtUnlocked, Kind::Exact,
        Bytes{0x2E, 0x02, 0x00, 0x00, 0x00, 0x1F, 0x40}, Bytes{0x6E, 0x02, 0x00});

    // 闭环：写完立刻读回 = 4 条
    addAfter(S, "正常", "写电压 0F 00 → 读回 0F 00", Prep::Ext,
             Bytes{0x2E, 0x01, 0x01, 0x0F, 0x00}, Bytes{0x6E, 0x01, 0x01},
             Bytes{0x22, 0x01, 0x01}, Bytes{0x62, 0x01, 0x01, 0x0F, 0x00});
    addAfter(S, "正常", "写里程 8000 → 读回 8000", Prep::ExtUnlocked,
             Bytes{0x2E, 0x02, 0x00, 0x00, 0x00, 0x1F, 0x40}, Bytes{0x6E, 0x02, 0x00},
             Bytes{0x22, 0x02, 0x00}, Bytes{0x62, 0x02, 0x00, 0x00, 0x00, 0x1F, 0x40});
    addAfter(S, "正常", "写车速 100 → 读回 100", Prep::Ext,
             Bytes{0x2E, 0x01, 0x00, 0x00, 0x64}, Bytes{0x6E, 0x01, 0x00},
             Bytes{0x22, 0x01, 0x00}, Bytes{0x62, 0x01, 0x00, 0x00, 0x64});
    addAfter(S, "边界", "写车速 0xFFFF（全 1）→ 读回 0xFFFF", Prep::Ext,
             Bytes{0x2E, 0x01, 0x00, 0xFF, 0xFF}, Bytes{0x6E, 0x01, 0x00},
             Bytes{0x22, 0x01, 0x00}, Bytes{0x62, 0x01, 0x00, 0xFF, 0xFF});
}

// -----------------------------------------------------------------============================================================================
// 四、0x3E 保活（9 条）
// -----------------------------------------------------------------============================================================================
void buildTesterPresentCases() {
    const char* S = "0x3E";

    add(S, "正常", "3E 00 回正响应", Prep::None, Kind::Exact,
        Bytes{0x3E, 0x00}, Bytes{0x7E, 0x00});
    add(S, "正常", "3E 00 在编程会话", Prep::Prog, Kind::Exact,
        Bytes{0x3E, 0x00}, Bytes{0x7E, 0x00});
    add(S, "边界", "3E 80 抑制正响应 → 什么都不回", Prep::None, Kind::Mute,
        Bytes{0x3E, 0x80});
    add(S, "边界", "3E 80 在扩展会话也不回", Prep::Ext, Kind::Mute,
        Bytes{0x3E, 0x80});
    add(S, "异常", "子功能 01", Prep::None, Kind::Exact,
        Bytes{0x3E, 0x01}, Bytes{0x7F, 0x3E, 0x12});
    add(S, "异常", "3E 81（抑制位 + 非法子功能）", Prep::None, Kind::Exact,
        Bytes{0x3E, 0x81}, Bytes{0x7F, 0x3E, 0x12});
    add(S, "异常", "只有服务字", Prep::None, Kind::Exact,
        Bytes{0x3E}, Bytes{0x7F, 0x3E, 0x13});
    add(S, "异常", "多带一个字节", Prep::None, Kind::Exact,
        Bytes{0x3E, 0x00, 0x00}, Bytes{0x7F, 0x3E, 0x13});
    add(S, "异常", "子功能 0xFF", Prep::None, Kind::Exact,
        Bytes{0x3E, 0xFF}, Bytes{0x7F, 0x3E, 0x12});
}

// -----------------------------------------------------------------============================================================================
// 五、0x27 安全访问（25 条）
// -----------------------------------------------------------------============================================================================
void buildSecurityCases() {
    const char* S = "0x27";

    add(S, "越权", "默认会话要种子 → 整个服务禁掉", Prep::None, Kind::Exact,
        Bytes{0x27, 0x01}, Bytes{0x7F, 0x27, 0x7F});
    add(S, "越权", "默认会话要 0x03 种子", Prep::None, Kind::Exact,
        Bytes{0x27, 0x03}, Bytes{0x7F, 0x27, 0x7F});
    add(S, "越权", "默认会话直接送密钥", Prep::None, Kind::Exact,
        Bytes{0x27, 0x02, 0xAA, 0xBB, 0xCC, 0xDD}, Bytes{0x7F, 0x27, 0x7F});

    add(S, "正常", "扩展会话要种子（连要两次必须不同）", Prep::Ext, Kind::Seed,
        Bytes{0x27, 0x01}, Bytes{0x67, 0x01});
    add(S, "正常", "编程会话要种子", Prep::Prog, Kind::Seed,
        Bytes{0x27, 0x01}, Bytes{0x67, 0x01});
    add(S, "边界", "子功能 0x05 也要种子（奇数即合法）", Prep::Ext, Kind::Seed,
        Bytes{0x27, 0x05}, Bytes{0x67, 0x05});

    add(S, "边界", "已解锁再要种子 → 全零", Prep::ExtUnlocked, Kind::Exact,
        Bytes{0x27, 0x01}, Bytes{0x67, 0x01, 0x00, 0x00, 0x00, 0x00});

    add(S, "异常", "没要种子就送密钥", Prep::Ext, Kind::Exact,
        Bytes{0x27, 0x02, 0xAA, 0xBB, 0xCC, 0xDD}, Bytes{0x7F, 0x27, 0x24});
    add(S, "异常", "已解锁后再送密钥（种子已作废）", Prep::ExtUnlocked, Kind::Exact,
        Bytes{0x27, 0x02, 0xAA, 0xBB, 0xCC, 0xDD}, Bytes{0x7F, 0x27, 0x24});

    add(S, "异常", "子功能 0x00", Prep::Ext, Kind::Exact,
        Bytes{0x27, 0x00}, Bytes{0x7F, 0x27, 0x12});
    add(S, "异常", "子功能 0x07 越界", Prep::Ext, Kind::Exact,
        Bytes{0x27, 0x07}, Bytes{0x7F, 0x27, 0x12});
    add(S, "异常", "只有服务字", Prep::Ext, Kind::Exact,
        Bytes{0x27}, Bytes{0x7F, 0x27, 0x13});
    add(S, "异常", "要种子多带一个字节", Prep::Ext, Kind::Exact,
        Bytes{0x27, 0x01, 0x00}, Bytes{0x7F, 0x27, 0x13});
    add(S, "异常", "送密钥只给 3 字节", Prep::Ext, Kind::Exact,
        Bytes{0x27, 0x02, 0xAA, 0xBB, 0xCC}, Bytes{0x7F, 0x27, 0x13});
    add(S, "异常", "偶数子功能没带密钥", Prep::Ext, Kind::Exact,
        Bytes{0x27, 0x04}, Bytes{0x7F, 0x27, 0x13});
    add(S, "异常", "0x06 带 4 字节但没种子 → 顺序错", Prep::Ext, Kind::Exact,
        Bytes{0x27, 0x06, 0xAA, 0xBB, 0xCC, 0xDD}, Bytes{0x7F, 0x27, 0x24});

    add(S, "异常", "密钥错第 1 次 → 35", Prep::ExtSeed, Kind::Exact,
        Bytes{0x27, 0x02, 0x00, 0x00, 0x00, 0x00}, Bytes{0x7F, 0x27, 0x35});
    add(S, "异常", "密钥错第 2 次 → 35", Prep::ExtSeedWrong1, Kind::Exact,
        Bytes{0x27, 0x02, 0x00, 0x00, 0x00, 0x00}, Bytes{0x7F, 0x27, 0x35});
    add(S, "异常", "密钥错第 3 次 → 36 并锁定", Prep::ExtSeedWrong2, Kind::Exact,
        Bytes{0x27, 0x02, 0x00, 0x00, 0x00, 0x00}, Bytes{0x7F, 0x27, 0x36});
    add(S, "越权", "锁定后再要种子 → 37", Prep::ExtLocked, Kind::Exact,
        Bytes{0x27, 0x01}, Bytes{0x7F, 0x27, 0x37});
    add(S, "越权", "锁定后再送密钥 → 37", Prep::ExtLocked, Kind::Exact,
        Bytes{0x27, 0x02, 0xAA, 0xBB, 0xCC, 0xDD}, Bytes{0x7F, 0x27, 0x37});

    add(S, "正常", "密钥正确 → 67 02", Prep::ExtSeed, Kind::SelfKey,
        Bytes{0x27, 0x02});

    add(S, "越权", "同一把正确密钥重放第二次 → 24", Prep::ExtSeed, Kind::ReplayKey,
        Bytes{0x27, 0x02});

    add(S, "边界", "错 1 次后重新要种子仍可拿到", Prep::ExtSeedWrong1, Kind::Seed,
        Bytes{0x27, 0x01}, Bytes{0x67, 0x01});

    add(S, "越权", "解锁后切回默认会话 → 重新上锁", Prep::ProgBackToDefault, Kind::Exact,
        Bytes{0x27, 0x01}, Bytes{0x7F, 0x27, 0x7F});
}

// -----------------------------------------------------------------============================================================================
// 六、0x19 读故障码（23 条）
// -----------------------------------------------------------------============================================================================
void buildDtcCases() {
    const char* S = "0x19";

    // 出厂三条：C12001/09、0A1202/08、7A1203/01
    add(S, "正常", "报数量：掩码 FF", Prep::None, Kind::Exact,
        Bytes{0x19, 0x01, 0xFF}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x03});
    add(S, "正常", "报数量：掩码 08（已确认）", Prep::None, Kind::Exact,
        Bytes{0x19, 0x01, 0x08}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x02});
    add(S, "边界", "报数量：掩码 09（08+01）", Prep::None, Kind::Exact,
        Bytes{0x19, 0x01, 0x09}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x03});
    add(S, "边界", "报数量：掩码 01（本次失败）", Prep::None, Kind::Exact,
        Bytes{0x19, 0x01, 0x01}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x02});
    add(S, "边界", "报数量：掩码 04（pending，没有）", Prep::None, Kind::Exact,
        Bytes{0x19, 0x01, 0x04}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x00});
    add(S, "边界", "报数量：掩码 00", Prep::None, Kind::Exact,
        Bytes{0x19, 0x01, 0x00}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x00});
    add(S, "边界", "报数量：掩码 80（warning 位，没有）", Prep::None, Kind::Exact,
        Bytes{0x19, 0x01, 0x80}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x00});

    add(S, "正常", "报列表：掩码 FF（三条全出）", Prep::None, Kind::Exact,
        Bytes{0x19, 0x02, 0xFF},
        Bytes{0x59, 0x02, 0xFF,
              0xC1, 0x20, 0x01, 0x09,
              0x0A, 0x12, 0x02, 0x08,
              0x7A, 0x12, 0x03, 0x01});
    add(S, "正常", "报列表：掩码 08", Prep::None, Kind::Exact,
        Bytes{0x19, 0x02, 0x08},
        Bytes{0x59, 0x02, 0xFF, 0xC1, 0x20, 0x01, 0x09, 0x0A, 0x12, 0x02, 0x08});
    add(S, "正常", "报列表：掩码 01（状态位第 0 位：C12001 的 0x09 也命中）", Prep::None, Kind::Exact,
        Bytes{0x19, 0x02, 0x01},
        Bytes{0x59, 0x02, 0xFF, 0xC1, 0x20, 0x01, 0x09, 0x7A, 0x12, 0x03, 0x01});
    add(S, "边界", "报列表：掩码 00（空表）", Prep::None, Kind::Exact,
        Bytes{0x19, 0x02, 0x00}, Bytes{0x59, 0x02, 0xFF});
    add(S, "边界", "报列表：掩码 04（空表）", Prep::None, Kind::Exact,
        Bytes{0x19, 0x02, 0x04}, Bytes{0x59, 0x02, 0xFF});

    add(S, "异常", "子功能 03 不支持", Prep::None, Kind::Exact,
        Bytes{0x19, 0x03, 0xFF}, Bytes{0x7F, 0x19, 0x12});
    add(S, "异常", "子功能 04 不支持", Prep::None, Kind::Exact,
        Bytes{0x19, 0x04}, Bytes{0x7F, 0x19, 0x12});
    add(S, "异常", "子功能 0A 不支持", Prep::None, Kind::Exact,
        Bytes{0x19, 0x0A, 0xFF}, Bytes{0x7F, 0x19, 0x12});
    add(S, "异常", "只有服务字", Prep::None, Kind::Exact,
        Bytes{0x19}, Bytes{0x7F, 0x19, 0x13});
    add(S, "异常", "19 01 缺掩码", Prep::None, Kind::Exact,
        Bytes{0x19, 0x01}, Bytes{0x7F, 0x19, 0x13});
    add(S, "异常", "19 01 多带字节", Prep::None, Kind::Exact,
        Bytes{0x19, 0x01, 0xFF, 0x00}, Bytes{0x7F, 0x19, 0x13});
    add(S, "异常", "19 02 缺掩码", Prep::None, Kind::Exact,
        Bytes{0x19, 0x02}, Bytes{0x7F, 0x19, 0x13});
    add(S, "异常", "19 02 多带字节", Prep::None, Kind::Exact,
        Bytes{0x19, 0x02, 0xFF, 0xFF}, Bytes{0x7F, 0x19, 0x13});

    add(S, "边界", "清故障后报数量变 0", Prep::ExtClearDtc, Kind::Exact,
        Bytes{0x19, 0x01, 0xFF}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x00});
    add(S, "边界", "清故障后报列表变空", Prep::ExtClearDtc, Kind::Exact,
        Bytes{0x19, 0x02, 0xFF}, Bytes{0x59, 0x02, 0xFF});
    add(S, "正常", "会话不影响故障码读取", Prep::Ext, Kind::Exact,
        Bytes{0x19, 0x01, 0xFF}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x03});
}

// -----------------------------------------------------------------============================================================================
// 七、0x14 清故障码（8 条）
// -----------------------------------------------------------------============================================================================
void buildClearCases() {
    const char* S = "0x14";

    add(S, "正常", "清全部", Prep::Ext, Kind::Exact,
        Bytes{0x14, 0xFF, 0xFF, 0xFF}, Bytes{0x54});
    add(S, "异常", "指定组 0xFF FF 00", Prep::Ext, Kind::Exact,
        Bytes{0x14, 0xFF, 0xFF, 0x00}, Bytes{0x7F, 0x14, 0x31});
    add(S, "异常", "组 0x00 00 00", Prep::Ext, Kind::Exact,
        Bytes{0x14, 0x00, 0x00, 0x00}, Bytes{0x7F, 0x14, 0x31});
    add(S, "异常", "只给 2 字节组", Prep::Ext, Kind::Exact,
        Bytes{0x14, 0xFF, 0xFF}, Bytes{0x7F, 0x14, 0x13});
    add(S, "异常", "只有服务字", Prep::Ext, Kind::Exact,
        Bytes{0x14}, Bytes{0x7F, 0x14, 0x13});
    add(S, "异常", "多带一个字节", Prep::Ext, Kind::Exact,
        Bytes{0x14, 0xFF, 0xFF, 0xFF, 0xFF}, Bytes{0x7F, 0x14, 0x13});

    addAfter(S, "正常", "清全部 → 再读数量为 0", Prep::Ext,
             Bytes{0x14, 0xFF, 0xFF, 0xFF}, Bytes{0x54},
             Bytes{0x19, 0x01, 0xFF}, Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x00});

    add(S, "边界", "清两次都回 54", Prep::Ext, Kind::Exact,
        Bytes{0x14, 0xFF, 0xFF, 0xFF}, Bytes{0x54});
}

// -----------------------------------------------------------------============================================================================
// 八、0x34 申请下载（16 条）
// -----------------------------------------------------------------============================================================================
void buildDownloadCases() {
    const char* S = "0x34";

    add(S, "越权", "默认会话申请下载", Prep::None, Kind::Exact,
        downloadReq(100), Bytes{0x7F, 0x34, 0x7F});
    add(S, "越权", "扩展会话申请下载", Prep::Ext, Kind::Exact,
        downloadReq(100), Bytes{0x7F, 0x34, 0x7F});
    add(S, "越权", "编程会话但没解锁", Prep::Prog, Kind::Exact,
        downloadReq(100), Bytes{0x7F, 0x34, 0x33});
    add(S, "正常", "编程会话 + 已解锁", Prep::ProgUnlocked, Kind::Exact,
        downloadReq(100), Bytes{0x74, 0x02, 0x01, 0x00});

    add(S, "异常", "压缩/加密格式 0x01", Prep::ProgUnlocked, Kind::Exact,
        cat(cat(Bytes{0x34, 0x01, 0x44}, u32(0x08000000u)), u32(100)),
        Bytes{0x7F, 0x34, 0x31});

    add(S, "边界", "ALFID 0x11：地址 1 字节、长度 1 字节", Prep::ProgUnlocked, Kind::Exact,
        Bytes{0x34, 0x00, 0x11, 0x08, 0x64}, Bytes{0x74, 0x02, 0x01, 0x00});
    add(S, "边界", "ALFID 0x22：地址 2 字节、长度 2 字节", Prep::ProgUnlocked, Kind::Exact,
        Bytes{0x34, 0x00, 0x22, 0x08, 0x00, 0x00, 0x64},
        Bytes{0x74, 0x02, 0x01, 0x00});
    add(S, "异常", "ALFID 0x88：8 字节地址不支持", Prep::ProgUnlocked, Kind::Exact,
        Bytes{0x34, 0x00, 0x88, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        Bytes{0x7F, 0x34, 0x13});
    add(S, "异常", "ALFID 0x00：两个长度都 0", Prep::ProgUnlocked, Kind::Exact,
        Bytes{0x34, 0x00, 0x00}, Bytes{0x7F, 0x34, 0x13});

    add(S, "异常", "声明长度 0", Prep::ProgUnlocked, Kind::Exact,
        downloadReq(0), Bytes{0x7F, 0x34, 0x70});
    add(S, "边界", "声明长度 65536（上限内）", Prep::ProgUnlocked, Kind::Exact,
        downloadReq(65536u), Bytes{0x74, 0x02, 0x01, 0x00});
    add(S, "异常", "声明长度 65537（超上限）", Prep::ProgUnlocked, Kind::Exact,
        downloadReq(65537u), Bytes{0x7F, 0x34, 0x70});

    add(S, "异常", "请求字节数和 ALFID 不符", Prep::ProgUnlocked, Kind::Exact,
        Bytes{0x34, 0x00, 0x44, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64},
        Bytes{0x7F, 0x34, 0x13});
    add(S, "异常", "只有服务字", Prep::ProgUnlocked, Kind::Exact,
        Bytes{0x34}, Bytes{0x7F, 0x34, 0x13});
    add(S, "异常", "只有 34 00", Prep::ProgUnlocked, Kind::Exact,
        Bytes{0x34, 0x00}, Bytes{0x7F, 0x34, 0x13});
    add(S, "异常", "只有 34 00 44", Prep::ProgUnlocked, Kind::Exact,
        Bytes{0x34, 0x00, 0x44}, Bytes{0x7F, 0x34, 0x13});
}

// -----------------------------------------------------------------============================================================================
// 九、0x36 传输数据（14 条）
// -----------------------------------------------------------------============================================================================
void buildTransferCases() {
    const char* S = "0x36";

    auto block = [](std::uint8_t seq, int n, int start) {
        Bytes b{0x36, seq};
        for (int i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>((start + i) & 0xFF));
        return b;
    };

    add(S, "正常", "第 1 块 50 字节", Prep::ProgDl100, Kind::Exact,
        block(0x01, 50, 0), Bytes{0x76, 0x01});
    add(S, "边界", "第 1 块正好 100 字节（一次传满）", Prep::ProgDl100, Kind::Exact,
        block(0x01, 100, 0), Bytes{0x76, 0x01});
    add(S, "边界", "第 1 块只 1 字节", Prep::ProgDl100, Kind::Exact,
        block(0x01, 1, 0), Bytes{0x76, 0x01});

    add(S, "异常", "块序号跳到 2", Prep::ProgDl100, Kind::Exact,
        block(0x02, 50, 0), Bytes{0x7F, 0x36, 0x73});
    add(S, "异常", "块序号从 0 开始", Prep::ProgDl100, Kind::Exact,
        block(0x00, 50, 0), Bytes{0x7F, 0x36, 0x73});
    add(S, "异常", "已传完 100 还想再传第 3 块", Prep::ProgDl100Full, Kind::Exact,
        block(0x01, 10, 0), Bytes{0x7F, 0x36, 0x73});

    add(S, "异常", "单块超过声明总长（101 字节）", Prep::ProgDl100, Kind::Exact,
        block(0x01, 101, 0), Bytes{0x7F, 0x36, 0x31});
    add(S, "异常", "累计超过总长（50+60=110）", Prep::ProgDl100Half, Kind::Exact,
        block(0x02, 60, 0), Bytes{0x7F, 0x36, 0x31});

    add(S, "异常", "只有序号没数据", Prep::ProgDl100, Kind::Exact,
        Bytes{0x36, 0x01}, Bytes{0x7F, 0x36, 0x13});
    add(S, "异常", "只有服务字", Prep::ProgDl100, Kind::Exact,
        Bytes{0x36}, Bytes{0x7F, 0x36, 0x13});

    add(S, "越权", "默认会话直接传块（没申请）", Prep::None, Kind::Exact,
        block(0x01, 10, 0), Bytes{0x7F, 0x36, 0x24});
    add(S, "越权", "扩展会话直接传块（没申请）", Prep::Ext, Kind::Exact,
        block(0x01, 10, 0), Bytes{0x7F, 0x36, 0x24});
    add(S, "越权", "编程会话未解锁就传块", Prep::Prog, Kind::Exact,
        block(0x01, 10, 0), Bytes{0x7F, 0x36, 0x24});

    addAfter(S, "正常", "两块接力 50+50", Prep::ProgDl100,
             block(0x01, 50, 0), Bytes{0x76, 0x01},
             block(0x02, 50, 50), Bytes{0x76, 0x02});
}

// -----------------------------------------------------------------============================================================================
// 十、0x37 结束传输（9 条）
// -----------------------------------------------------------------============================================================================
void buildExitCases() {
    const char* S = "0x37";

    add(S, "越权", "默认会话结束传输", Prep::None, Kind::Exact,
        Bytes{0x37}, Bytes{0x7F, 0x37, 0x24});
    add(S, "越权", "编程会话未解锁就结束", Prep::Prog, Kind::Exact,
        Bytes{0x37}, Bytes{0x7F, 0x37, 0x24});

    add(S, "异常", "一块都没传就想结束", Prep::ProgDl100, Kind::Exact,
        Bytes{0x37}, Bytes{0x7F, 0x37, 0x70});
    add(S, "异常", "只传了 50/100 就想结束", Prep::ProgDl100Half, Kind::Exact,
        Bytes{0x37}, Bytes{0x7F, 0x37, 0x70});

    add(S, "正常", "收满 100 结束 → 77", Prep::ProgDl100Full, Kind::Exact,
        Bytes{0x37}, Bytes{0x77});

    add(S, "异常", "带一个多余字节", Prep::ProgDl100Full, Kind::Exact,
        Bytes{0x37, 0x00}, Bytes{0x7F, 0x37, 0x13});
    add(S, "异常", "带两个多余字节", Prep::ProgDl100Full, Kind::Exact,
        Bytes{0x37, 0x00, 0x00}, Bytes{0x7F, 0x37, 0x13});

    addAfter(S, "边界", "结束之后立刻再结束 → 24", Prep::ProgDl100Full,
             Bytes{0x37}, Bytes{0x77},
             Bytes{0x37}, Bytes{0x7F, 0x37, 0x24});
}

// -----------------------------------------------------------------============================================================================
// 十一、未知服务（10 条）
// -----------------------------------------------------------------============================================================================
void buildUnknownServiceCases() {
    const char* S = "未知";

    struct U { std::uint8_t sid; Bytes req; };
    const U list[] = {
        {0x11, Bytes{0x11, 0x01}},
        {0x2F, Bytes{0x2F, 0x01, 0x02}},
        {0x31, Bytes{0x31, 0x01, 0xFF, 0x00}},
        {0x28, Bytes{0x28, 0x01, 0x01}},
        {0x3D, Bytes{0x3D, 0x00}},
        {0x85, Bytes{0x85, 0x02}},
        {0x87, Bytes{0x87, 0x01}},
        {0x4C, Bytes{0x4C, 0x01}},
        {0xFF, Bytes{0xFF}},
        {0x00, Bytes{0x00}},
    };

    for (std::size_t i = 0; i < sizeof(list) / sizeof(list[0]); ++i) {
        char desc[96];
        std::snprintf(desc, sizeof(desc), "发未知服务 0x%02X",
                      static_cast<unsigned>(list[i].sid));
        add(S, "异常", desc, Prep::Ext, Kind::Exact,
            list[i].req, Bytes{0x7F, list[i].sid, 0x11});
    }
}

// -----------------------------------------------------------------============================================================================
// 执行
// -----------------------------------------------------------------============================================================================
struct Result {
    bool   ok = false;
    Bytes  actual;
    std::string note;
};

Result runCase(const Case& c) {
    uds::UdsServer::resetDidTable();          // 用例之间彻底隔离
    uds::UdsServer ecu;

    std::uint32_t lastSeed = 0;
    applyPrep(ecu, c.prep, lastSeed);

    Bytes req = c.req;
    if (c.kind == Kind::SelfKey || c.kind == Kind::ReplayKey) {
        req = Bytes{0x27, 0x02};
        req = cat(req, u32(uds::UdsServer::computeKey(lastSeed)));
    }

    Bytes resp = ecu.handle(req, 0);

    Result r;
    r.actual = resp;

    switch (c.kind) {
        case Kind::Exact:
            r.ok = (resp == c.expect);
            if (!r.ok) r.note = "期望 " + hex(c.expect) + "，实际 " + hex(resp);
            break;

        case Kind::Mute:
            r.ok = resp.empty();
            if (!r.ok) r.note = "本该不响应，却回了 " + hex(resp);
            break;

        case Kind::SelfKey:
            r.ok = (resp == Bytes{0x67, 0x02});
            if (!r.ok) r.note = "正确密钥却被拒：" + hex(resp);
            break;

        case Kind::ReplayKey: {
            if (resp != Bytes{0x67, 0x02}) {
                r.ok   = false;
                r.note = "第一次送正确密钥就该解锁，实际 " + hex(resp);
                break;
            }
            const Bytes r2 = ecu.handle(req, 0);   // 同一把密钥再送一次
            r.ok   = (r2 == Bytes{0x7F, 0x27, 0x24});
            r.actual = r2;
            if (!r.ok) r.note = "重放没被拒，实际回了 " + hex(r2);
            else       r.note = "重放被拒：7F 27 24";
            break;
        }

        case Kind::Seed: {
            const std::uint8_t sub = c.expect.size() > 1 ? c.expect[1] : 0x01;
            if (resp.size() != 6 || resp[0] != 0x67 || resp[1] != sub || seedOf(resp) == 0) {
                r.ok   = false;
                r.note = "种子响应格式不对：" + hex(resp);
                break;
            }
            // 连要两次：种子必须不同，否则旧密钥能被重放
            const Bytes r2 = ecu.handle(req, 0);
            if (r2.size() != 6 || seedOf(r2) == seedOf(resp)) {
                r.ok   = false;
                r.note = "两次种子相同（可重放）：" + hex(resp) + " / " + hex(r2);
            } else {
                r.ok   = true;
                r.note = "两次种子不同：" + hex(resp) + " / " + hex(r2);
            }
            break;
        }
    }

    if (r.ok && c.hasAfter) {
        const Bytes a = ecu.handle(c.after, 0);
        if (a != c.afterExpect) {
            r.ok   = false;
            r.note = "后置步骤不符：期望 " + hex(c.afterExpect) + "，实际 " + hex(a);
        } else {
            r.note = "后置步骤也对：" + hex(a);
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// 报表
// ---------------------------------------------------------------------------
void writeCsv(const std::string& path, const std::vector<Result>& rs) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "打不开 %s\n", path.c_str());
        return;
    }
    std::fputs("\xEF\xBB\xBF", f);                       // UTF-8 BOM，Excel 打开不乱码
    std::fprintf(f, "编号,服务,类别,用例,前置,请求,期望,实际,结果\n");
    for (std::size_t i = 0; i < g_cases.size(); ++i) {
        const Case& c = g_cases[i];
        std::fprintf(f, "%s,%s,%s,%s,%s,\"%s\",\"%s\",\"%s\",%s\n",
                     c.id.c_str(), c.svc.c_str(), c.cat.c_str(), c.desc.c_str(),
                     prepName(c.prep), hex(c.req).c_str(),
                     c.kind == Kind::Mute ? "(不响应)" : hex(c.expect).c_str(),
                     hex(rs[i].actual).c_str(),
                     rs[i].ok ? "PASS" : "FAIL");
    }
    std::fclose(f);
}

void writeMarkdown(const std::string& path, const std::vector<Result>& rs) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "打不开 %s\n", path.c_str());
        return;
    }

    std::size_t passed = 0;
    for (const auto& r : rs) if (r.ok) ++passed;

    // ---- 按服务、按类别统计 ----
    struct Stat { std::string key; std::size_t n = 0; std::size_t ok = 0; };
    std::vector<Stat> bySvc, byCat;
    auto bump = [](std::vector<Stat>& v, const std::string& k, bool ok) {
        for (auto& s : v) if (s.key == k) { ++s.n; if (ok) ++s.ok; return; }
        Stat s; s.key = k; s.n = 1; s.ok = ok ? 1 : 0;
        v.push_back(s);
    };
    for (std::size_t i = 0; i < g_cases.size(); ++i) {
        bump(bySvc, g_cases[i].svc, rs[i].ok);
        bump(byCat, g_cases[i].cat, rs[i].ok);
    }

    std::fprintf(f, "# UDS 诊断栈测试矩阵报告（M5）\n\n");
    std::fprintf(f, "被测对象：`uds::UdsServer`（M1 ISO-TP + M2/M3/M4 UDS 服务层）。\n");
    std::fprintf(f, "执行方式：内存内直调 `handle()`，不连 vcan、不依赖真实时间。\n");
    std::fprintf(f, "期望值来源：按 ISO 14229 与项目约定手写，不从实现反推。\n\n");

    std::fprintf(f, "## 总览\n\n");
    std::fprintf(f, "| 指标 | 数值 |\n|---|---|\n");
    std::fprintf(f, "| 用例总数 | %zu |\n", g_cases.size());
    std::fprintf(f, "| 通过 | %zu |\n", passed);
    std::fprintf(f, "| 失败 | %zu |\n", g_cases.size() - passed);
    std::fprintf(f, "| 通过率 | %.1f%% |\n\n",
                 g_cases.empty() ? 0.0 : 100.0 * static_cast<double>(passed) / g_cases.size());

    std::fprintf(f, "## 按服务分布\n\n| 服务 | 用例数 | 通过 | 状态 |\n|---|---|---|---|\n");
    for (const auto& s : bySvc) {
        std::fprintf(f, "| %s | %zu | %zu | %s |\n", s.key.c_str(), s.n, s.ok,
                     s.ok == s.n ? "全绿" : "有失败");
    }

    std::fprintf(f, "\n## 按类别分布\n\n| 类别 | 用例数 | 通过 |\n|---|---|---|\n");
    for (const auto& s : byCat) {
        std::fprintf(f, "| %s | %zu | %zu |\n", s.key.c_str(), s.n, s.ok);
    }

    std::fprintf(f, "\n## 失败清单\n\n");
    std::size_t failed = g_cases.size() - passed;
    if (failed == 0) {
        std::fprintf(f, "无。%zu 条全部通过。\n", g_cases.size());
    } else {
        for (std::size_t i = 0; i < g_cases.size(); ++i) {
            if (rs[i].ok) continue;
            std::fprintf(f, "- **%s** [%s] %s：%s\n", g_cases[i].id.c_str(),
                         g_cases[i].svc.c_str(), g_cases[i].desc.c_str(),
                         rs[i].note.c_str());
        }
    }

    std::fprintf(f, "\n## 全量矩阵\n\n");
    std::fprintf(f, "| 编号 | 服务 | 类别 | 用例 | 前置 | 请求 | 期望 | 实际 | 结果 |\n");
    std::fprintf(f, "|---|---|---|---|---|---|---|---|---|\n");
    for (std::size_t i = 0; i < g_cases.size(); ++i) {
        const Case& c = g_cases[i];
        std::fprintf(f, "| %s | %s | %s | %s | %s | `%s` | `%s` | `%s` | %s |\n",
                     c.id.c_str(), c.svc.c_str(), c.cat.c_str(), c.desc.c_str(),
                     prepName(c.prep),
                     hex(c.req).c_str(),
                     c.kind == Kind::Mute ? "(不响应)" : hex(c.expect).c_str(),
                     hex(rs[i].actual).c_str(),
                     rs[i].ok ? "PASS" : "FAIL");
    }
    std::fclose(f);
}

} // namespace

// -------------------------------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    const std::string outDir = (argc > 1) ? argv[1] : ".";

    buildSessionCases();
    buildReadCases();
    buildWriteCases();
    buildTesterPresentCases();
    buildSecurityCases();
    buildDtcCases();
    buildClearCases();
    buildDownloadCases();
    buildTransferCases();
    buildExitCases();
    buildUnknownServiceCases();

    std::printf("=== M5 测试矩阵：共 %zu 条 ===\n", g_cases.size());

    std::vector<Result> rs;
    rs.reserve(g_cases.size());
    for (std::size_t i = 0; i < g_cases.size(); ++i) {
        Result r = runCase(g_cases[i]);
        rs.push_back(r);
        if (!r.ok) {
            std::printf("[FAIL] %s %s %s\n        %s\n", g_cases[i].id.c_str(),
                        g_cases[i].svc.c_str(), g_cases[i].desc.c_str(), r.note.c_str());
        }
    }

    std::size_t passed = 0;
    for (const auto& r : rs) if (r.ok) ++passed;

    std::printf("\n--------------------------------\n");
    std::printf("通过 %zu 条，失败 %zu 条，通过率 %.1f%%\n", passed,
                g_cases.size() - passed,
                g_cases.empty() ? 0.0
                                : 100.0 * static_cast<double>(passed) / g_cases.size());

    const std::string csv = outDir + "/test_matrix.csv";
    const std::string md  = outDir + "/test_matrix_report.md";
    writeCsv(csv, rs);
    writeMarkdown(md, rs);
    std::printf("已写出：%s\n         %s\n", csv.c_str(), md.c_str());

    return (passed == g_cases.size()) ? 0 : 1;
}
