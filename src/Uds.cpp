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
        {0x0200, true,  Bytes{0x00, 0x00, 0x0F, 0xA0}},     // 自定义：里程 4000
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
}

void UdsServer::setSession(Session s, std::uint64_t nowMs) {
    m_session   = s;
    m_lastReqMs = nowMs;

    // 0xF186 是可读的"当前会话"，必须跟着会话一起变
    if (DidSlot* d = findDid(0xF186)) {
        d->data = Bytes{static_cast<std::uint8_t>(s)};
    }
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

void UdsServer::tick(std::uint64_t nowMs) {
    if (m_session == Session::Default) return;
    if (nowMs - m_lastReqMs > m_cfg.s3Ms) {
        setSession(Session::Default, nowMs);   // S3 到期，回默认会话
    }
}

} // namespace uds
