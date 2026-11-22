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
//   0x2E  WriteDataByIdentifier      写数据（扩展会话，受保护的 DID 还要先解锁）
//   0x3E  TesterPresent              保活
//   0x27  SecurityAccess             种子/密钥握手（M3）
//   0x19  ReadDTCInformation         读故障码（M3）
//   0x14  ClearDiagnosticInformation 清故障码（M3）
//   0x34  RequestDownload            申请下载（M4）
//   0x36  TransferData               传输数据块（M4）
//   0x37  RequestTransferExit        结束传输并提交（M4）
// ============================================================================

#include <cstdint>
#include <string>
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
    NRC_RequestSequenceError         = 0x24,  // 顺序错（没要种子就送密钥）
    NRC_RequestOutOfRange            = 0x31,  // 参数越界（DID 不存在 / 不可写）
    NRC_SecurityDenied               = 0x33,  // 没通过安全访问
    NRC_InvalidKey                   = 0x35,  // 密钥错
    NRC_ExceedNumberOfAttempts       = 0x36,  // 试太多次了
    NRC_RequiredTimeDelayNotExpired  = 0x37,  // 还在锁定冷却期
    NRC_UploadDownloadNotAccepted    = 0x70,  // 下载申请没通过（长度/地址不合规）
    NRC_WrongBlockSequenceCounter    = 0x73,  // 块序号不对
    NRC_ServiceNotSupportedInSession = 0x7F,  // 服务存在，但当前会话不让用
};

// 一条故障码。code 只占 24 位（3 字节），status 是状态位图：
//   0x01 testFailed  0x04 pendingDTC  0x08 confirmedDTC  0x10 warningIndicator
struct Dtc {
    std::uint32_t code   = 0;
    std::uint8_t  status = 0;
};

struct UdsConfig {
    std::uint64_t s3Ms      = 5000;   // 非默认会话空闲多久自动退回默认
    std::uint16_t p2Ms      = 50;     // 写进 0x10 正响应里的 P2
    std::uint16_t p2StarMs  = 5000;   // P2*

    // 已提交镜像存哪儿。空字符串 = 只放内存（进程一退就没了，适合自测）；
    // 给个路径 = 落盘，进程被杀掉再起来，旧镜像还在 —— 断电回滚要靠它。
    std::string   flashPath;
};

class UdsServer {
public:
    explicit UdsServer(UdsConfig cfg = UdsConfig{});

    // 处理一条完整请求，返回响应（空 vector = 不响应，例如抑制正响应位）
    std::vector<std::uint8_t> handle(const std::vector<std::uint8_t>& req,
                                     std::uint64_t nowMs);

    // 空闲检查：非默认会话太久没请求就退回默认（S3 定时器）
    void tick(std::uint64_t nowMs);

    // 0x27 的密钥推导。真实 ECU 里这是保密算法；这里公开，
    // 好处是客户端工具能直接复用它来算密钥。
    static std::uint32_t computeKey(std::uint32_t seed);

    Session       session()          const { return m_session; }
    bool          securityUnlocked() const { return m_unlocked; }
    std::size_t   dtcCount()         const { return m_dtcs.size(); }
    std::uint64_t requestCount()     const { return m_reqCount; }
    std::size_t   failedKeyCount()   const { return m_attempts; }

    // ---- 刷写相关（M4）----
    bool          downloadActive()   const { return m_dlActive; }
    std::size_t   stagedSize()       const { return m_stage.size(); }   // 暂存区已收多少
    std::size_t   imageSize()        const { return m_image.size(); }   // 已提交镜像多大
    std::uint32_t imageCrc()         const;                             // 已提交镜像的 CRC32

    // 模拟断电重启：丢弃还没提交的暂存区，已提交的镜像保持不变。
    // 真实场景里这一步由"上电自检"完成 —— 发现暂存区不完整就作废。
    void          reboot();

private:
    // 十个服务的实现，各自"先查长度再干活"
    std::vector<std::uint8_t> svcSessionControl(const std::vector<std::uint8_t>& req,
                                                std::uint64_t nowMs);
    std::vector<std::uint8_t> svcReadDid(const std::vector<std::uint8_t>& req);
    std::vector<std::uint8_t> svcWriteDid(const std::vector<std::uint8_t>& req);
    std::vector<std::uint8_t> svcTesterPresent(const std::vector<std::uint8_t>& req,
                                               std::uint64_t nowMs);
    std::vector<std::uint8_t> svcSecurityAccess(const std::vector<std::uint8_t>& req,
                                                std::uint64_t nowMs);
    std::vector<std::uint8_t> svcReadDtc(const std::vector<std::uint8_t>& req);
    std::vector<std::uint8_t> svcClearDtc(const std::vector<std::uint8_t>& req);
    std::vector<std::uint8_t> svcRequestDownload(const std::vector<std::uint8_t>& req);
    std::vector<std::uint8_t> svcTransferData(const std::vector<std::uint8_t>& req);
    std::vector<std::uint8_t> svcTransferExit(const std::vector<std::uint8_t>& req);

    void        setSession(Session s, std::uint64_t nowMs);
    std::size_t countDtcWithMask(std::uint8_t mask) const;

    // 镜像落盘（flashPath 为空时这两个是空操作）
    void        loadImage();
    void        saveImage();

    UdsConfig     m_cfg;
    Session       m_session   = Session::Default;
    std::uint64_t m_lastReqMs = 0;
    std::uint64_t m_reqCount  = 0;

    // ---- 0x27 安全访问的状态 ----
    bool          m_unlocked       = false;   // 已通过密钥校验
    bool          m_hasSeed        = false;   // 已经发过种子，等着收密钥
    std::uint32_t m_lastSeed       = 0;
    std::uint32_t m_seedCounter    = 0;       // 保证两次种子不一样
    std::uint32_t m_attempts       = 0;       // 连续错了几次密钥
    std::uint64_t m_lockoutUntilMs = 0;       // 锁定到什么时候为止

    // ---- 0x19 / 0x14 的故障码表 ----
    std::vector<Dtc> m_dtcs;

    // ---- 刷写状态（M4）----
    //   m_image：已经正式提交的镜像（掉电也不会丢的那一份）
    //   m_stage：正在收的暂存区（只有 0x37 成功才升级成 m_image）
    //   两块分开存，就是"写到一半断电不会毁掉旧版本"的全部秘密。
    bool                      m_dlActive        = false;
    std::uint32_t             m_dlAddress       = 0;
    std::uint32_t             m_dlDeclaredSize  = 0;
    std::uint8_t              m_dlNextBlock     = 1;
    std::vector<std::uint8_t> m_stage;
    std::vector<std::uint8_t> m_image;
};

} // namespace uds
