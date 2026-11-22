#pragma once
// ============================================================================
// Uds.h —— UDS 服务层（M2）
//
// 定位：ISO-TP 管"字节怎么搬"，这里管"搬来的字节是什么意思"。
//
// 边界（守住这几条，M2 才不会长成一团）：
//   · 只吃"一条完整的请求消息"，只吐"一条完整的响应消息"，不碰 CAN、不碰时间流逝
//   · 时间从外面传进来（nowMs），所以 S3 超时能在内存里自测，不用真等 5 秒
//   · 服务处理一律"先查长度、再查子功能、最后查会话"，顺序错了 NRC 就会给错
//
// 本阶段实现四个服务：
//   0x10  DiagnosticSessionControl   会话切换
//   0x22  ReadDataByIdentifier       读数据（DID）
//   0x2E  WriteDataByIdentifier      写数据（只在扩展会话下允许）
//   0x3E  TesterPresent              保活
// ============================================================================

#include <cstdint>
#include <vector>

namespace uds {

// 诊断会话。数值就是 UDS 里 0x10 服务的子功能号，别自己另编一套。
enum class Session : std::uint8_t {
    Default     = 0x01,
    Programming = 0x02,
    Extended    = 0x03,
    Safety      = 0x04,
};

// 负面响应码（NRC）。只列本项目真会用到的几个。
enum Nrc : std::uint8_t {
    NRC_ServiceNotSupported          = 0x11,  // 这个服务我没实现
    NRC_SubFunctionNotSupported      = 0x12,  // 服务认，子功能不认
    NRC_IncorrectLength              = 0x13,  // 长度不对
    NRC_RequestOutOfRange            = 0x31,  // 参数越界（DID 不存在 / 不可写）
    NRC_SecurityDenied               = 0x33,  // 没通过安全访问
    NRC_ServiceNotSupportedInSession = 0x7F,  // 服务存在，但当前会话不让用
};

struct UdsConfig {
    std::uint64_t s3Ms      = 5000;   // 非默认会话空闲多久自动退回默认
    std::uint16_t p2Ms      = 50;     // 写进 0x10 正响应里的 P2
    std::uint16_t p2StarMs  = 5000;   // P2*
};

class UdsServer {
public:
    explicit UdsServer(UdsConfig cfg = UdsConfig{});

    // 处理一条完整请求，返回响应（空 vector = 不响应，例如抑制正响应位）
    std::vector<std::uint8_t> handle(const std::vector<std::uint8_t>& req,
                                     std::uint64_t nowMs);

    // 空闲检查：非默认会话太久没请求就退回默认（S3 定时器）
    void tick(std::uint64_t nowMs);

    Session       session()      const { return m_session; }
    std::uint64_t requestCount() const { return m_reqCount; }

private:
    // 四个服务的实现，各自"先查长度再干活"
    std::vector<std::uint8_t> svcSessionControl(const std::vector<std::uint8_t>& req,
                                                std::uint64_t nowMs);
    std::vector<std::uint8_t> svcReadDid(const std::vector<std::uint8_t>& req);
    std::vector<std::uint8_t> svcWriteDid(const std::vector<std::uint8_t>& req);
    std::vector<std::uint8_t> svcTesterPresent(const std::vector<std::uint8_t>& req,
                                               std::uint64_t nowMs);

    void setSession(Session s, std::uint64_t nowMs);

    UdsConfig     m_cfg;
    Session       m_session   = Session::Default;
    std::uint64_t m_lastReqMs = 0;
    std::uint64_t m_reqCount  = 0;
};

} // namespace uds
