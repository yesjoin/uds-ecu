#include "IsoTp.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace uds {

namespace {
std::uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
} // namespace

std::uint64_t steadyMs() { return nowMs(); }

IsoTp::IsoTp(IsoTpConfig cfg) : m_cfg(cfg) {
    m_rxBuf.assign(m_cfg.maxMsg, 0);
}

void IsoTp::resetRx() {
    m_rxActive  = false;
    m_needFc    = false;
    m_rxTotal   = 0;
    m_rxGot     = 0;
    m_rxNextSeq = 1;
    m_cfSinceFc = 0;
}

Frame IsoTp::makeFlowControl() const {
    Frame f;
    f.id  = m_cfg.txId;
    f.dlc = 3;
    f.data[0] = 0x30;                  // 0x3 = FC，FS = 0 表示"继续发送"
    f.data[1] = m_cfg.blockSize;
    f.data[2] = m_cfg.stMinMs;
    return f;
}

// ============================================================================
// M1a —— 接收侧重组（完整）
//
//   四条分支 + 一个前置检查。顺序不能乱：
//     ① ID 过滤  →  ② 结算旧账（超时）→ ③ 记录活动时间  →  ④ 按 PCI 分流
// ============================================================================
bool IsoTp::feed(const Frame& f, std::vector<std::uint8_t>& out) {
    // ① 不是本通道的帧，完全不关我们的事
    if (f.id != m_cfg.rxId) return false;

    const std::uint64_t now = nowMs();

    // ② 先结算旧账：上一条半成品等不到后续帧了，就地作废
    //    必须放在分发之前，否则新帧会被当成旧消息的续篇
    if (m_rxActive && (now - m_rxLastMs) > m_cfg.nCrMs) {
        ++m_st.timeout;
        resetRx();
    }
    m_rxLastMs = now;

    const std::uint8_t pci = f.data[0] >> 4;

    // ---- 0x0 单帧：整条消息就在这一帧里 ----
    if (pci == 0x0) {
        const std::uint8_t len = f.data[0] & 0x0F;
        if (len == 0 || len > 7) { ++m_st.dropped; return false; }
        out.assign(f.data + 1, f.data + 1 + len);
        ++m_st.sf;
        ++m_st.assembled;
        return true;
    }

    // ---- 0x1 首帧：开缓冲，等连续帧 ----
    if (pci == 0x1) {
        const std::size_t total =
            (static_cast<std::size_t>(f.data[0] & 0x0F) << 8) | f.data[1];
        if (total < 8 || total > m_cfg.maxMsg) {
            ++m_st.dropped;
            resetRx();
            return false;
        }
        std::memcpy(m_rxBuf.data(), f.data + 2, 6);   // 首帧只有 6 个数据字节
        m_rxGot     = 6;
        m_rxActive  = true;
        m_needFc    = true;
        m_rxNextSeq = 1;
        m_rxTotal   = total;
        ++m_st.ff;
        return false;
    }

    // ---- 0x2 连续帧：往缓冲里续 ----
    if (pci == 0x2) {
        if (!m_rxActive) return false;                // 没在重组中：别人的尾巴

        const std::uint8_t seq = f.data[0] & 0x0F;
        if (seq != m_rxNextSeq) {                     // 序号错 → 整包作废
            ++m_st.seqErr;
            resetRx();
            return false;
        }

        const std::size_t take =
            std::min<std::size_t>(7, m_rxTotal - m_rxGot);   // 最后一帧可能不满 7
        std::memcpy(m_rxBuf.data() + m_rxGot, f.data + 1, take);
        m_rxGot += take;

        // 15 之后回到 1 —— 序号只有 4 位
        m_rxNextSeq = (m_rxNextSeq == 15) ? 1 : m_rxNextSeq + 1;
        ++m_st.cf;

        // 收满一块（BS 个）就让上层再回一张流控帧放行下一块。
        // 消息没凑完才需要；凑完了就直接交货，不必再多说一句。
        if (m_cfg.blockSize > 0 && m_rxGot < m_rxTotal) {
            ++m_cfSinceFc;
            if (m_cfSinceFc >= m_cfg.blockSize) {
                m_needFc    = true;
                m_cfSinceFc = 0;
            }
        }

        if (m_rxGot >= m_rxTotal) {                   // 凑满了才交货
            out.assign(m_rxBuf.begin(), m_rxBuf.begin() + m_rxTotal);
            ++m_st.assembled;
            resetRx();
            return true;
        }
        return false;
    }

    // ---- 0x3 流控帧：对端在管我们的发送节奏，M1a 只计数 ----
    if (pci == 0x3) {
        ++m_st.fc;
        return false;
    }

    return false;
}

// ============================================================================
// M1b-1 —— 发送侧分块：把一条消息切成 CAN 帧
//
//   和 feed() 正好是镜像：
//     feed()： 一堆帧  →  一条消息
//     segment()：一条消息  →  一堆帧
//
//   规则只有三条：
//     ① <=7 字节：塞进一个单帧，PCI 低 4 位 = 长度
//     ② >7 字节：先发首帧（2 字节 PCI + 6 字节数据），长度写 12 位
//     ③ 剩下的每帧装 7 字节，PCI = 0x2 | 序号，序号 1..15 循环
//
//   两个细节容易错：
//     · 每帧的 dlc 必须真实（= PCI 字节数 + 数据字节数），
//       最后一帧不满 7 时 dlc 要跟着小，否则会把垃圾字节发上总线。
//     · 用 txId 还是 rxId 取决于"谁在说话"。segment() 是我们自己发，
//       所以用 txId（0x7E8）。
// ============================================================================
std::vector<Frame> IsoTp::segment(const std::vector<std::uint8_t>& msg) {
    std::vector<Frame> frames;
    if (msg.empty()) return frames;

    const std::size_t n = msg.size();

    // ---- ① 短消息：一个单帧 ----
    if (n <= 7) {
        Frame f;
        f.id      = m_cfg.txId;
        f.dlc     = static_cast<std::uint8_t>(n + 1);   // 1 字节 PCI + n 字节数据
        f.data[0] = static_cast<std::uint8_t>(n);       // PCI = 0x0，低 4 位是长度
        std::memcpy(f.data + 1, msg.data(), n);
        frames.push_back(f);
        return frames;
    }

    // ---- ② 长消息：首帧。长度是 12 位，拆成高 4 位 + 低 8 位 ----
    Frame ff;
    ff.id      = m_cfg.txId;
    ff.dlc     = 8;                                     // 首帧永远是满的
    ff.data[0] = static_cast<std::uint8_t>(0x10 | ((n >> 8) & 0x0F));
    ff.data[1] = static_cast<std::uint8_t>(n & 0xFF);
    std::memcpy(ff.data + 2, msg.data(), 6);            // 首帧只装 6 字节
    frames.push_back(ff);

    // ---- ③ 剩下的按每帧 7 字节切 ----
    std::size_t  pos = 6;                               // 前 6 字节已经被首帧拿走
    std::uint8_t seq = 1;                               // 连续帧序号从 1 开始

    while (pos < n) {
        const std::size_t take = std::min<std::size_t>(7, n - pos);  // 末帧可能不满

        Frame cf;
        cf.id      = m_cfg.txId;
        cf.dlc     = static_cast<std::uint8_t>(take + 1);
        cf.data[0] = static_cast<std::uint8_t>(0x20 | seq);
        std::memcpy(cf.data + 1, msg.data() + pos, take);
        frames.push_back(cf);

        pos += take;
        seq  = (seq == 15) ? 1 : static_cast<std::uint8_t>(seq + 1);
    }

    return frames;
}

// ============================================================================
// M1b-3 —— 发送侧流控状态机
// ============================================================================
IsoTpTx::IsoTpTx(IsoTpConfig cfg) : m_cfg(cfg) {}

bool IsoTpTx::begin(const std::vector<std::uint8_t>& msg) {
    m_msg.clear();
    m_msg.assign(msg.begin(), msg.end());

    m_first       = Frame{};
    m_single      = false;
    m_waitFc      = false;
    m_aborted     = false;
    m_pos         = 0;
    m_seq         = 1;
    m_bs          = 0;
    m_stMinMs     = 0;
    m_blockSent   = 0;
    m_nextSendMs  = 0;
    m_fcCount     = 0;

    if (msg.empty() || msg.size() > m_cfg.maxMsg) return false;

    // ---- 短消息：一帧搞定，不需要流控 ----
    if (msg.size() <= 7) {
        m_single       = true;
        m_first.id     = m_cfg.txId;
        m_first.dlc    = static_cast<std::uint8_t>(msg.size() + 1);
        m_first.data[0] = static_cast<std::uint8_t>(msg.size());
        std::memcpy(m_first.data + 1, msg.data(), msg.size());
        m_pos = msg.size();
        return true;
    }

    // ---- 长消息：首帧 ----
    m_first.id      = m_cfg.txId;
    m_first.dlc     = 8;
    m_first.data[0] = static_cast<std::uint8_t>(0x10 | ((msg.size() >> 8) & 0x0F));
    m_first.data[1] = static_cast<std::uint8_t>(msg.size() & 0xFF);
    std::memcpy(m_first.data + 2, msg.data(), 6);
    m_pos    = 6;
    m_waitFc = true;      // 规矩①：首帧发完必须停下等流控
    return true;
}

void IsoTpTx::onFlowControl(const Frame& fc, std::uint64_t nowMs) {
    if (fc.dlc < 3) return;

    const std::uint8_t fs = fc.data[0] & 0x0F;
    ++m_fcCount;

    if (fs == 0x0) {                    // CTS：允许继续
        m_bs = fc.data[1];

        // STmin 编码：0x00..0x7F 是毫秒；0xF1..0xF9 是 100..900 微秒。
        // 本机定时精度到毫秒，微秒档统一按 1ms 处理（诚实降级，不是偷懒）。
        const std::uint8_t raw = fc.data[2];
        m_stMinMs = (raw <= 0x7F) ? raw : static_cast<std::uint8_t>(1);

        m_blockSent  = 0;
        m_waitFc     = false;

        // 注意：这里不能把时间闸门清零。
        // 上一帧的 STmin 还没走完就得接着等 —— 否则每过一个块的边界，
        // 都会白偷一个 STmin（72 个块就能偷掉 700 多毫秒，实测过）。
        if (m_nextSendMs < nowMs) m_nextSendMs = nowMs;
    } else if (fs == 0x1) {             // WAIT：对端还没准备好，继续等
        // 什么都不做，m_waitFc 保持 true
    } else if (fs == 0x2) {             // OVFLW：对端装不下，放弃
        m_aborted = true;
        m_waitFc  = true;
    }
}

IsoTpTx::Status IsoTpTx::poll(std::uint64_t nowMs, Frame& out) {
    if (m_aborted)                 return Status::Aborted;
    if (m_single)                  return Status::Done;    // 单帧在 begin 里就备好了
    if (m_pos >= m_msg.size())     return Status::Done;
    if (m_waitFc)                  return Status::WaitFlowControl;
    if (nowMs < m_nextSendMs)      return Status::WaitStmin;

    const std::size_t take = std::min<std::size_t>(7, m_msg.size() - m_pos);

    out        = Frame{};
    out.id     = m_cfg.txId;
    out.dlc    = static_cast<std::uint8_t>(take + 1);
    out.data[0] = static_cast<std::uint8_t>(0x20 | m_seq);
    std::memcpy(out.data + 1, m_msg.data() + m_pos, take);

    m_pos += take;
    m_seq  = (m_seq == 15) ? 1 : static_cast<std::uint8_t>(m_seq + 1);
    ++m_blockSent;
    m_nextSendMs = nowMs + m_stMinMs;

    // 规矩②：发满一块就停下等下一张流控帧（BS=0 表示对端不要求分块）
    if (m_bs > 0 && m_blockSent >= m_bs && m_pos < m_msg.size()) {
        m_waitFc = true;
    }

    return Status::SentFrame;
}

} // namespace uds
