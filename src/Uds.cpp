#include "Uds.h"

#include <algorithm>
#include <string>

namespace uds {

namespace {

using Bytes = std::vector<std::uint8_t>;

// 负响应固定三个字节：7F <服务号> <NRC>
Bytes neg(std::uint8_t sid, std::uint8_t nrc) {
    return Bytes{0x7F, sid, nrc};
}

Bytes ascii(const char* s) {
    Bytes v;
    for (const char* p = s; *p != '\0'; ++p) {
        v.push_back(static_cast<std::uint8_t>(*p));
    }
    return v;
}

struct DidSlot {
    std::uint16_t id;
    bool          writable;
    Bytes         data;
    bool          needUnlock = false;   // true = 写之前必须先过 0x27
};

// DID 表。static 只初始化一次：0x2E 写进来的值会一直留在这里，
// 于是"写进去 → 再读回来"能形成闭环验证。
std::vector<DidSlot>& dids() {
    static std::vector<DidSlot> table = {
        {0xF186, false, Bytes{0x01}},                       // 当前会话（随会话变）
        {0xF187, false, ascii("1234567890A")},              // 零件号
        {0xF188, false, ascii("SW01.02.03")},               // 软件版本
        {0xF18A, false, ascii("UDS-ECU")},                  // 供应商
        {0xF190, false, ascii("LSVAA0000A1234567")},        // VIN，17 字节
        {0xF191, false, ascii("HW01")},                     // 硬件版本
        {0x0100, true,  Bytes{0x00, 0x00}},                 // 自定义：车速 km/h
        {0x0101, true,  Bytes{0x0E, 0x74}},                 // 自定义：电压 37.00V
        {0x0200, true,  Bytes{0x00, 0x00, 0x0F, 0xA0}, true},  // 里程：写之前要解锁
    };
    return table;
}

DidSlot* findDid(std::uint16_t id) {
    for (auto& d : dids()) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

} // namespace

UdsServer::UdsServer(UdsConfig cfg) : m_cfg(cfg) {
    setSession(Session::Default, 0);

    // 出厂就带着三条故障码，方便 0x19 有东西可读
    m_dtcs = {
        {0xC12001, 0x09},   // 已确认故障 + 本循环仍失败
        {0x0A1202, 0x08},   // 已确认故障
        {0x7A1203, 0x01},   // 本次失败，还没确认
    };
}

void UdsServer::setSession(Session s, std::uint64_t nowMs) {
    m_session   = s;
    m_lastReqMs = nowMs;

    // 换会话就等于重新上锁 —— 真实 ECU 就这么干：
    // 你在扩展会话里解了锁，一切回默认，钥匙也得重新配。
    m_unlocked = false;
    m_hasSeed  = false;

    // 0xF186 是可读的"当前会话"，必须跟着会话一起变
    if (DidSlot* d = findDid(0xF186)) {
        d->data = Bytes{static_cast<std::uint8_t>(s)};
    }
}

std::uint32_t UdsServer::computeKey(std::uint32_t seed) {
    // 真实 ECU 用的是保密算法 + 保密常数，这里用一个公开的对称变换代替。
    // 对客户端的意义是一样的：拿到种子，算出唯一正确的密钥。
    std::uint32_t k = seed;
    k = ((k & 0xFFFFu) << 16) | ((k >> 16) & 0xFFFFu);   // 高低 16 位对调
    k ^= 0xA5C3F00Du;                                    // 与固定掩码异或
    k = (k << 3) | (k >> 29);                            // 循环左移 3 位
    return k;
}

std::vector<std::uint8_t> UdsServer::handle(const std::vector<std::uint8_t>& req,
                                            std::uint64_t nowMs) {
    ++m_reqCount;
    if (req.empty()) return {};

    m_lastReqMs = nowMs;                 // 有请求就算"活着"，S3 定时器往后推

    switch (req[0]) {
        case 0x10: return svcSessionControl(req, nowMs);
        case 0x22: return svcReadDid(req);
        case 0x2E: return svcWriteDid(req);
        case 0x3E: return svcTesterPresent(req, nowMs);
        case 0x27: return svcSecurityAccess(req, nowMs);
        case 0x19: return svcReadDtc(req);
        case 0x14: return svcClearDtc(req);
        default:   return neg(req[0], NRC_ServiceNotSupported);
    }
}

// ---------------------------------------------------------------------------
// 0x10 会话切换
//   请求：10 <子功能>
//   正响：50 <子功能> <P2 高> <P2 低> <P2* 高> <P2* 低>
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> UdsServer::svcSessionControl(
        const std::vector<std::uint8_t>& req, std::uint64_t nowMs) {
    if (req.size() != 2) return neg(0x10, NRC_IncorrectLength);

    const std::uint8_t sub = static_cast<std::uint8_t>(req[1] & 0x7F);
    if (sub < 1 || sub > 4) return neg(0x10, NRC_SubFunctionNotSupported);

    setSession(static_cast<Session>(sub), nowMs);

    Bytes r;
    r.push_back(0x50);
    r.push_back(sub);
    r.push_back(static_cast<std::uint8_t>((m_cfg.p2Ms >> 8) & 0xFF));
    r.push_back(static_cast<std::uint8_t>(m_cfg.p2Ms & 0xFF));
    r.push_back(static_cast<std::uint8_t>((m_cfg.p2StarMs >> 8) & 0xFF));
    r.push_back(static_cast<std::uint8_t>(m_cfg.p2StarMs & 0xFF));
    return r;
}

// ---------------------------------------------------------------------------
// 0x22 读 DID
//   请求：22 <DID1 高> <DID1 低> [<DID2 高> <DID2 低> ...]
//   正响：62 <DID 高> <DID 低> <数据...> ...（按请求顺序拼接）
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> UdsServer::svcReadDid(const std::vector<std::uint8_t>& req) {
    // 长度必须先查：1 个服务字 + 偶数个 DID 字节
    if (req.size() < 3 || ((req.size() - 1) % 2) != 0) {
        return neg(0x22, NRC_IncorrectLength);
    }

    Bytes r;
    r.push_back(0x62);

    for (std::size_t i = 1; i + 1 < req.size(); i += 2) {
        const std::uint16_t id = static_cast<std::uint16_t>((req[i] << 8) | req[i + 1]);
        DidSlot* d = findDid(id);

        // 只要有一个 DID 不认识，整条请求就失败 —— 不能只回一半
        if (d == nullptr) return neg(0x22, NRC_RequestOutOfRange);

        r.push_back(req[i]);
        r.push_back(req[i + 1]);
        r.insert(r.end(), d->data.begin(), d->data.end());
    }

    return r;
}

// ---------------------------------------------------------------------------
// 0x2E 写 DID
//   请求：2E <DID 高> <DID 低> <数据...>
//   正响：6E <DID 高> <DID 低>（只回"我收到了"，不回数据）
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> UdsServer::svcWriteDid(const std::vector<std::uint8_t>& req) {
    if (req.size() < 4) return neg(0x2E, NRC_IncorrectLength);

    // 会话门控：写数据只在扩展/安全会话下允许。
    // 这是真实 ECU 的常见策略 —— 默认会话就该只读，保护现场配置。
    if (m_session != Session::Extended && m_session != Session::Safety) {
        return neg(0x2E, NRC_ServiceNotSupportedInSession);
    }

    const std::uint16_t id = static_cast<std::uint16_t>((req[1] << 8) | req[2]);
    DidSlot* d = findDid(id);

    if (d == nullptr)    return neg(0x2E, NRC_RequestOutOfRange);  // 没这个 DID
    if (!d->writable)    return neg(0x2E, NRC_RequestOutOfRange);  // 存在但只读

    // 有些 DID 光有会话还不够，还得先过 0x27。
    // 真实零部件里"里程"这类字段就属于这一档 —— 防止随便改。
    if (d->needUnlock && !m_unlocked) {
        return neg(0x2E, NRC_SecurityDenied);
    }

    const std::size_t n = req.size() - 3;
    if (n != d->data.size()) return neg(0x2E, NRC_IncorrectLength);  // 长度必须匹配

    std::copy(req.begin() + 3, req.end(), d->data.begin());
    return Bytes{0x6E, req[1], req[2]};
}

// ---------------------------------------------------------------------------
// 0x3E 保活
//   请求：3E 00（3E 80 = 同样含义，但"别回正响应"）
//   正响：7E 00
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> UdsServer::svcTesterPresent(
        const std::vector<std::uint8_t>& req, std::uint64_t nowMs) {
    if (req.size() != 2) return neg(0x3E, NRC_IncorrectLength);

    const std::uint8_t sub  = static_cast<std::uint8_t>(req[1] & 0x7F);
    const bool         mute = (req[1] & 0x80) != 0;

    if (sub != 0x00) return neg(0x3E, NRC_SubFunctionNotSupported);

    m_lastReqMs = nowMs;      // 保活的全部意义：把 S3 往后推
    if (mute) return {};      // 抑制正响应：什么都不回
    return Bytes{0x7E, 0x00};
}

// ---------------------------------------------------------------------------
// 0x27 安全访问（种子 / 密钥）
//
//   奇数子功能 = "给我种子"（01/03/05）
//   偶数子功能 = "这是我的密钥"（02/04/06）
//
//   三道门，顺序不能乱：
//     ① 会话：默认会话不许碰钥匙
//     ② 冷却：错太多次要等一段时间
//     ③ 顺序：没要过种子就直接送密钥，那是瞎猜
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> UdsServer::svcSecurityAccess(
        const std::vector<std::uint8_t>& req, std::uint64_t nowMs) {
    if (req.size() < 2) return neg(0x27, NRC_IncorrectLength);

    if (m_session == Session::Default) {
        return neg(0x27, NRC_ServiceNotSupportedInSession);
    }

    const std::uint8_t sub = req[1];
    if (sub < 1 || sub > 6) return neg(0x27, NRC_SubFunctionNotSupported);

    // ---- 奇数：给种子 ----
    if ((sub & 0x01) == 0x01) {
        if (req.size() != 2) return neg(0x27, NRC_IncorrectLength);

        if (m_lockoutUntilMs != 0 && nowMs < m_lockoutUntilMs) {
            return neg(0x27, NRC_RequiredTimeDelayNotExpired);
        }

        // 已经解锁了：按规范回全零种子，意思是"你不用再解一次"
        if (m_unlocked) {
            return Bytes{0x67, sub, 0x00, 0x00, 0x00, 0x00};
        }

        // 种子必须每次都不同：否则重放一次旧密钥就能解锁
        ++m_seedCounter;
        std::uint32_t x = static_cast<std::uint32_t>(m_seedCounter) * 0x9E3779B9u;
        x ^= static_cast<std::uint32_t>(nowMs & 0xFFFFFFFFull);
        x ^= 0x5A5A5A5Au;
        m_lastSeed = (x == 0) ? 1u : x;
        m_hasSeed  = true;

        Bytes r;
        r.push_back(0x67);
        r.push_back(sub);
        r.push_back(static_cast<std::uint8_t>((m_lastSeed >> 24) & 0xFF));
        r.push_back(static_cast<std::uint8_t>((m_lastSeed >> 16) & 0xFF));
        r.push_back(static_cast<std::uint8_t>((m_lastSeed >> 8) & 0xFF));
        r.push_back(static_cast<std::uint8_t>(m_lastSeed & 0xFF));
        return r;
    }

    // ---- 偶数：收密钥 ----
    if (req.size() != 6) return neg(0x27, NRC_IncorrectLength);

    if (m_lockoutUntilMs != 0 && nowMs < m_lockoutUntilMs) {
        return neg(0x27, NRC_RequiredTimeDelayNotExpired);
    }
    if (!m_hasSeed) return neg(0x27, NRC_RequestSequenceError);

    std::uint32_t key = 0;
    for (std::size_t i = 2; i < 6; ++i) {
        key = (key << 8) | req[i];
    }

    if (key != computeKey(m_lastSeed)) {
        ++m_attempts;
        m_hasSeed = false;                 // 种子一次性，错了就作废，得重新要

        if (m_attempts >= 3) {
            m_lockoutUntilMs = nowMs + 10000;   // 三次错，锁 10 秒
            m_attempts       = 0;
            return neg(0x27, NRC_ExceedNumberOfAttempts);
        }
        return neg(0x27, NRC_InvalidKey);
    }

    m_unlocked = true;
    m_hasSeed  = false;
    m_attempts = 0;
    return Bytes{0x67, sub};
}

std::size_t UdsServer::countDtcWithMask(std::uint8_t mask) const {
    std::size_t n = 0;
    for (const auto& d : m_dtcs) {
        if ((d.status & mask) != 0) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// 0x19 读故障码
//   19 01 <mask>  报数量：59 01 <可用掩码> <DTC格式> <数量 2 字节>
//   19 02 <mask>  报列表：59 02 <可用掩码> (<DTC 3 字节> <状态>)*
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> UdsServer::svcReadDtc(const std::vector<std::uint8_t>& req) {
    if (req.size() < 2) return neg(0x19, NRC_IncorrectLength);

    const std::uint8_t sub = req[1];

    if (sub == 0x01) {
        if (req.size() != 3) return neg(0x19, NRC_IncorrectLength);

        const std::size_t n = countDtcWithMask(req[2]);

        Bytes r{0x59, 0x01, 0xFF, 0x01};   // 0xFF = 支持全部状态位，0x01 = ISO14229-1 格式
        r.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        r.push_back(static_cast<std::uint8_t>(n & 0xFF));
        return r;
    }

    if (sub == 0x02) {
        if (req.size() != 3) return neg(0x19, NRC_IncorrectLength);

        Bytes r{0x59, 0x02, 0xFF};
        for (const auto& d : m_dtcs) {
            if ((d.status & req[2]) == 0) continue;
            r.push_back(static_cast<std::uint8_t>((d.code >> 16) & 0xFF));
            r.push_back(static_cast<std::uint8_t>((d.code >> 8) & 0xFF));
            r.push_back(static_cast<std::uint8_t>(d.code & 0xFF));
            r.push_back(d.status);
        }
        return r;
    }

    return neg(0x19, NRC_SubFunctionNotSupported);
}

// ---------------------------------------------------------------------------
// 0x14 清故障码。本阶段只支持 14 FF FF FF（清全部）。
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> UdsServer::svcClearDtc(const std::vector<std::uint8_t>& req) {
    if (req.size() != 4) return neg(0x14, NRC_IncorrectLength);

    if (req[1] == 0xFF && req[2] == 0xFF && req[3] == 0xFF) {
        m_dtcs.clear();
        return Bytes{0x54};                // 只回"清完了"，不回数据
    }
    return neg(0x14, NRC_RequestOutOfRange);
}

void UdsServer::tick(std::uint64_t nowMs) {
    if (m_session == Session::Default) return;
    if (nowMs - m_lastReqMs > m_cfg.s3Ms) {
        setSession(Session::Default, nowMs);   // S3 到期，回默认会话
    }
}

} // namespace uds
