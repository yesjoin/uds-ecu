#pragma once
// ============================================================================
// IsoTp.h —— ISO 15765-2 传输层（分段与重组）
//
//   职责边界（这几条守住，M1 才不会长成一团）：
//     · 只做"把 8 字节帧拼成一条消息 / 把一条消息拆成 8 字节帧"
//     · 不碰 socket、不 sleep、不发帧。要不要回流控帧由上层问 needFlowControl()
//     · 超时靠时间戳判断，不靠线程
//
//   M1a 只要求实现 feed()；segment() 留到 M1b。
// ============================================================================

#include "CanBus.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace uds {

struct IsoTpConfig {
    std::uint32_t rxId      = 0x7E0;   // 我们收：客户端 → ECU
    std::uint32_t txId      = 0x7E8;   // 我们发：ECU → 客户端
    std::uint8_t  blockSize = 8;       // 我们回流控时给的 BS
    std::uint8_t  stMinMs   = 10;      // 我们回流控时给的 STmin
    std::size_t   maxMsg    = 4095;    // 经典 CAN 硬上限：FF 的长度字段只有 12 位
    std::uint64_t nCrMs     = 1000;    // 等下一帧 CF 的超时
};

// 单调毫秒时钟（只增不减）。IsoTp 与上层必须共用一个时间基准，
// 否则 STmin / nCr 这些定时判断会因为基准不同而对不上。
std::uint64_t steadyMs();

class IsoTp {
public:
    explicit IsoTp(IsoTpConfig cfg = IsoTpConfig{});

    // 喂一帧。返回 true 表示一条完整消息已重组好，内容放在 out。
    // 不是本通道 ID、或还没收全 → false。
    bool feed(const Frame& f, std::vector<std::uint8_t>& out);

    // 需要回流控帧时返回 true。两个时机会置位：
    //   ① 刚收到首帧 —— 不马上回，对端就一直停在那儿不敢发
    //   ② 之后每收满 blockSize 个连续帧 —— 对端按块发，得逐块放行
    bool  needFlowControl() const { return m_needFc; }
    Frame makeFlowControl() const;     // 用 cfg 的 BS / STmin 生成 0x30 帧
    void  clearFlowControl() { m_needFc = false; m_cfSinceFc = 0; }

    // M1b：把一条消息拆成待发帧
    std::vector<Frame> segment(const std::vector<std::uint8_t>& msg);

    struct Stats {
        std::uint64_t sf = 0, ff = 0, cf = 0, fc = 0;
        std::uint64_t assembled = 0, dropped = 0, seqErr = 0, timeout = 0;
    };
    const Stats& stats() const { return m_st; }

private:
    void resetRx();                    // 丢弃半成品，回到初始态

    IsoTpConfig m_cfg;
    Stats       m_st;

    // ---- 接收侧状态（M1a 的全部战场都在这里）----
    bool          m_rxActive  = false;  // 正在重组中
    bool          m_needFc    = false;  // 收到 FF，等上层回流控
    std::size_t   m_rxTotal   = 0;      // FF 声明的总长度
    std::size_t   m_rxGot     = 0;      // 已收字节数
    std::uint8_t  m_rxNextSeq = 1;      // 期望的下一个 CF 序号：1..15 循环
    std::size_t   m_cfSinceFc = 0;      // 本块已经收了多少个 CF（凑满 BS 就要求回流控）
    std::uint64_t m_rxLastMs  = 0;      // 上次活动时间（超时判断）
    std::vector<std::uint8_t> m_rxBuf;  // 重组缓冲，构造时按 maxMsg 预留
};

// ============================================================================
// M1b-3：发送侧的流控状态机
//
//   为什么单独做一个类：把"下一帧能不能发"变成纯逻辑，
//   socket 收发和 sleep 全留给上层 —— 这样一堆规矩能在内存里自测，
//   不用起 vcan、不用等真实时间。
//
//   ISO 15765-2 对发送方定的三条规矩：
//     ① 首帧发完必须停下，等对端流控帧（FS=CTS 才继续）
//     ② 每发满 BS 个连续帧再停一次等下一张流控帧（BS=0 表示不用等）
//     ③ 连续帧之间至少隔 STmin
// ============================================================================
class IsoTpTx {
public:
    enum class Status {
        WaitFlowControl,   // 停住，等对端流控帧
        WaitStmin,         // 还没到点，查 nextSendMs() 再等
        SentFrame,         // out 是一帧，发出去
        Done,              // 整条消息发完
        Aborted            // 对端回 OVFLW（装不下），放弃
    };

    explicit IsoTpTx(IsoTpConfig cfg = IsoTpConfig{});

    // 装入一条消息并准备好首帧。空消息或超过 maxMsg 返回 false。
    bool begin(const std::vector<std::uint8_t>& msg);

    bool isSingleFrame() const { return m_single; }   // <=7 字节，一帧搞定
    const Frame& firstFrame() const { return m_first; }

    // 喂一张流控帧（ID 由上层先过滤好）
    void onFlowControl(const Frame& fc, std::uint64_t nowMs);

    Status poll(std::uint64_t nowMs, Frame& out);

    std::uint64_t nextSendMs()  const { return m_nextSendMs; }
    std::size_t   remaining()   const { return m_msg.size() - m_pos; }
    std::size_t   fcCount()     const { return m_fcCount; }
    std::uint8_t  blockSize()   const { return m_bs; }
    std::uint8_t  stMinMs()     const { return m_stMinMs; }

private:
    IsoTpConfig m_cfg;
    std::vector<std::uint8_t> m_msg;
    Frame         m_first;
    bool          m_single    = false;
    bool          m_waitFc    = false;   // 正在等流控帧
    bool          m_aborted   = false;
    std::size_t   m_pos       = 0;       // 下一个待发字节的偏移
    std::uint8_t  m_seq       = 1;       // 下一个 CF 的序号
    std::uint8_t  m_bs        = 0;       // 对端给的块大小（0 = 不用再等）
    std::uint8_t  m_stMinMs   = 0;       // 对端给的帧间隔
    std::size_t   m_blockSent = 0;       // 本块已发几个 CF
    std::uint64_t m_nextSendMs = 0;      // 早于这个时刻不许发
    std::size_t   m_fcCount   = 0;
};

} // namespace uds
