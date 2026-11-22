// ============================================================================
// isotp_selftest.cpp —— M1a 的验收夹具
//
// 它不连 vcan，纯内存里喂帧。你每实现一条分支，就有对应的 [FAIL] 变 [PASS]。
// 现在编译能过、跑起来大部分是 FAIL，这是正常的——先红后绿。
//
// 注意：buildFrames() 是"测试夹具"，模拟对端客户端发来的帧序列。
//       它不是 segment() 的参考答案，别照着它写 segment。
// ============================================================================

#include "IsoTp.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int gPass = 0;
int gFail = 0;

void check(bool ok, const char* name) {
    if (ok) { ++gPass; std::printf("[PASS] %s\n", name); }
    else    { ++gFail; std::printf("[FAIL] %s\n", name); }
}

uds::Frame mk(std::uint32_t id, const std::uint8_t* d, std::uint8_t n) {
    uds::Frame f;
    f.id  = id;
    f.dlc = n;
    std::memcpy(f.data, d, n);
    return f;
}

// 模拟对端把一条消息拆成帧（仅供测试喂数据用）
std::vector<uds::Frame> buildFrames(std::uint32_t id,
                                    const std::vector<std::uint8_t>& msg) {
    std::vector<uds::Frame> out;
    const std::size_t n = msg.size();

    if (n <= 7) {
        std::uint8_t b[8] = {};
        b[0] = static_cast<std::uint8_t>(n);
        std::memcpy(&b[1], msg.data(), n);
        out.push_back(mk(id, b, 8));
        return out;
    }

    {
        std::uint8_t b[8] = {};
        b[0] = static_cast<std::uint8_t>(0x10 | ((n >> 8) & 0x0F));
        b[1] = static_cast<std::uint8_t>(n & 0xFF);
        std::memcpy(&b[2], msg.data(), 6);
        out.push_back(mk(id, b, 8));
    }

    std::size_t off = 6;
    std::uint8_t seq = 1;
    while (off < n) {
        std::uint8_t b[8] = {};
        const std::size_t take = std::min<std::size_t>(7, n - off);
        b[0] = static_cast<std::uint8_t>(0x20 | seq);
        std::memcpy(&b[1], msg.data() + off, take);
        out.push_back(mk(id, b, static_cast<std::uint8_t>(take + 1)));
        off += take;
        seq = (seq == 15) ? 1 : static_cast<std::uint8_t>(seq + 1);
    }
    return out;
}

std::vector<std::uint8_t> makeMsg(std::size_t n, std::uint8_t seed = 1) {
    std::vector<std::uint8_t> m(n);
    for (std::size_t i = 0; i < n; ++i)
        m[i] = static_cast<std::uint8_t>((i * seed + 0x11) & 0xFF);
    return m;
}

// ---------------------------------------------------------------------------
void testSingleFrame() {
    uds::IsoTp tp;
    const std::vector<std::uint8_t> msg{0x10, 0x03};
    const auto f = buildFrames(0x7E0, msg)[0];
    std::vector<std::uint8_t> out;
    check(tp.feed(f, out) && out == msg, "SF：2 字节单帧");
}

void testSingleFrameMax() {
    uds::IsoTp tp;
    const auto msg = makeMsg(7, 5);           // SF 的极限：7 字节
    const auto f = buildFrames(0x7E0, msg)[0];
    std::vector<std::uint8_t> out;
    check(tp.feed(f, out) && out == msg, "SF：7 字节边界");
}

void testMultiFrame() {
    uds::IsoTp tp;
    const auto msg = makeMsg(20);
    const auto frames = buildFrames(0x7E0, msg);
    std::vector<std::uint8_t> out;
    bool done = false;
    for (const auto& f : frames) {
        if (tp.feed(f, out)) { done = true; break; }
    }
    check(done && out == msg, "FF+CF：20 字节（FF + 2 个 CF）");
}

void testBigMessage() {
    uds::IsoTp tp;
    const auto msg = makeMsg(4000, 3);        // 经典 CAN 天花板 4095，取 4000 测序号循环
    const auto frames = buildFrames(0x7E0, msg);
    std::vector<std::uint8_t> out;
    bool done = false;
    for (const auto& f : frames) {
        if (tp.feed(f, out)) { done = true; break; }
    }
    check(done && out == msg && out.size() == 4000,
          "大消息：4000 字节（571 个 CF，跨 38 轮序号循环）");
}

void testSeqError() {
    uds::IsoTp tp;
    const auto msg = makeMsg(20);
    auto frames = buildFrames(0x7E0, msg);
    frames[1].data[0] = 0x22;                 // 期望序号 1，实际来了 2
    std::vector<std::uint8_t> out;
    bool done = false;
    for (const auto& f : frames) {
        if (tp.feed(f, out)) done = true;
    }
    check(!done && tp.stats().seqErr >= 1, "序号错：丢整包并计 seqErr");
}

void testTruncated() {
    uds::IsoTp tp;
    const auto msg = makeMsg(20);
    const auto frames = buildFrames(0x7E0, msg);
    std::vector<std::uint8_t> out;
    bool done = false;
    for (std::size_t i = 0; i < 2 && i < frames.size(); ++i) {   // 只喂 FF + 1 个 CF
        if (tp.feed(frames[i], out)) done = true;
    }
    check(!done, "只来了 13/20 字节：不应完成");
}

void testTimeout() {
    uds::IsoTp tp;
    const auto msg = makeMsg(20);
    const auto frames = buildFrames(0x7E0, msg);
    std::vector<std::uint8_t> out;
    tp.feed(frames[0], out);                                    // 先给 FF
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // 超过 nCr = 1000ms
    bool done = false;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        if (tp.feed(frames[i], out)) done = true;
    }
    check(!done && tp.stats().timeout >= 1, "FF 后 1.2s 才来 CF：超时丢弃");
}

void testWrongId() {
    uds::IsoTp tp;
    auto f = buildFrames(0x7E0, {0x10, 0x03})[0];
    f.id = 0x123;
    std::vector<std::uint8_t> out;
    check(!tp.feed(f, out), "非本通道 ID：应忽略");
}

void testFlowControlFrame() {
    uds::IsoTp tp;
    const auto msg = makeMsg(20);
    const auto frames = buildFrames(0x7E0, msg);
    std::vector<std::uint8_t> out;
    tp.feed(frames[0], out);                                    // FF
    check(tp.needFlowControl(), "收到 FF：needFlowControl() 应为 true");

    const auto fc = tp.makeFlowControl();
    check(fc.id == 0x7E8 && fc.dlc == 3 &&
          fc.data[0] == 0x30 && fc.data[1] == 8 && fc.data[2] == 10,
          "FC 帧格式：0x7E8 / 0x30 / BS=8 / STmin=10");

    tp.clearFlowControl();
    check(!tp.needFlowControl(), "clearFlowControl() 后应复位");
}

// ---------------------------------------------------------------------------
// M1b-1：发送侧分块（segment）—— 纯函数，不碰 socket
// ---------------------------------------------------------------------------
void testSegmentSingle() {
    uds::IsoTp tp;
    const auto msg = makeMsg(5, 7);
    const auto frames = tp.segment(msg);
    check(frames.size() == 1 && frames[0].dlc == 6,
          "segment：<=7 字节 → 1 个单帧，dlc = 长度+1");
}

void testSegmentCount() {
    uds::IsoTp tp;
    const auto msg = makeMsg(4000, 3);
    const auto frames = tp.segment(msg);
    check(frames.size() == 572, "segment：4000 字节 → 1 FF + 571 CF = 572 帧");

    if (frames.size() >= 2) {
        const auto& ff   = frames.front();
        const auto& last = frames.back();
        const std::size_t hi = (msg.size() >> 8) & 0x0F;
        const std::size_t lo = msg.size() & 0xFF;
        check((ff.data[0] >> 4) == 0x1 &&
              (ff.data[0] & 0x0F) == hi && ff.data[1] == lo,
              "segment：首帧长度字段 = 4000");
        check(last.dlc == 5,
              "segment：最后一个 CF 应为 dlc=5（1 字节 PCI + 4 字节数据）");
    }
}

void testSegmentRoundTrip() {
    uds::IsoTp tp;
    const auto msg = makeMsg(4000, 3);
    auto frames = tp.segment(msg);
    std::vector<std::uint8_t> out;
    bool done = false;
    for (auto& f : frames) {
        f.id = 0x7E0;                     // 冒充成"收到的帧"，才能喂回同一个实例
        if (tp.feed(f, out)) { done = true; break; }
    }
    check(done && out == msg, "segment → feed 往返：4000 字节原样还原");
}

// ---------------------------------------------------------------------------
// M1b-3：发送侧流控状态机（IsoTpTx）
// ---------------------------------------------------------------------------
void testTxSingleFrame() {
    uds::IsoTpTx tx;
    const auto msg = makeMsg(5, 11);
    check(tx.begin(msg) && tx.isSingleFrame(), "tx：短消息 begin 后是单帧");
    check(tx.firstFrame().dlc == 6, "tx：单帧 dlc = 长度+1");

    uds::Frame f;
    check(tx.poll(0, f) == uds::IsoTpTx::Status::Done,
          "tx：单帧不需要流控，poll 直接 Done");
}

void testTxWaitFlowControl() {
    uds::IsoTpTx tx;
    const auto msg = makeMsg(4000, 12);
    check(tx.begin(msg) && !tx.isSingleFrame(), "tx：长消息 begin 后不是单帧");
    check(tx.firstFrame().dlc == 8, "tx：首帧 dlc 必须是 8");

    uds::Frame f;
    check(tx.poll(0, f) == uds::IsoTpTx::Status::WaitFlowControl,
          "tx：首帧之后必须停下等流控帧");
}

void testTxBlockSize() {
    uds::IsoTpTx tx;
    check(tx.begin(makeMsg(4000, 13)), "tx：begin 成功");

    uds::Frame fc;
    fc.id = 0x7E0; fc.dlc = 3;
    fc.data[0] = 0x30; fc.data[1] = 4; fc.data[2] = 0;    // BS=4, STmin=0
    tx.onFlowControl(fc, 0);

    check(tx.blockSize() == 4, "tx：FC 里的 BS 被记住");

    uds::Frame f;
    std::size_t sent = 0;
    while (tx.poll(0, f) == uds::IsoTpTx::Status::SentFrame) ++sent;
    check(sent == 4, "tx：BS=4 → 发满 4 个连续帧就停下");
    check(tx.poll(0, f) == uds::IsoTpTx::Status::WaitFlowControl,
          "tx：发满一块后回到等流控");
}

void testTxStmin() {
    uds::IsoTpTx tx;
    check(tx.begin(makeMsg(4000, 14)), "tx：begin 成功");

    uds::Frame fc;
    fc.id = 0x7E0; fc.dlc = 3;
    fc.data[0] = 0x30; fc.data[1] = 0; fc.data[2] = 10;   // BS=0, STmin=10ms
    tx.onFlowControl(fc, 0);

    check(tx.stMinMs() == 10, "tx：FC 里的 STmin 被记住");

    uds::Frame f;
    check(tx.poll(0, f) == uds::IsoTpTx::Status::SentFrame, "tx：第 1 帧立即发");
    check(tx.poll(5, f) == uds::IsoTpTx::Status::WaitStmin,
          "tx：STmin=10 时，5ms 还不能发第 2 帧");
    check(tx.poll(10, f) == uds::IsoTpTx::Status::SentFrame,
          "tx：到 10ms 就能发第 2 帧");
}

void testTxComplete() {
    uds::IsoTpTx tx;
    const auto msg = makeMsg(4000, 15);
    check(tx.begin(msg), "tx：begin 成功");

    uds::Frame fc;
    fc.id = 0x7E0; fc.dlc = 3;
    fc.data[0] = 0x30; fc.data[1] = 0; fc.data[2] = 0;    // BS=0 → 一口气发完
    tx.onFlowControl(fc, 0);

    std::vector<uds::Frame> frames;
    frames.push_back(tx.firstFrame());
    uds::Frame f;
    while (tx.poll(0, f) == uds::IsoTpTx::Status::SentFrame) {
        frames.push_back(f);
    }

    check(frames.size() == 572, "tx：4000 字节应发出 1 FF + 571 CF = 572 帧");
    check(tx.poll(0, f) == uds::IsoTpTx::Status::Done, "tx：发完回到 Done");

    // 拆出来的帧和 segment() 的结果应当能喂回 feed() 还原
    uds::IsoTp rx;
    std::vector<std::uint8_t> out;
    bool done = false;
    for (auto& fr : frames) {
        fr.id = 0x7E0;
        if (rx.feed(fr, out)) { done = true; break; }
    }
    check(done && out == msg, "tx → 接收端往返：4000 字节原样还原");
}

void testTxOverflow() {
    uds::IsoTpTx tx;
    check(tx.begin(makeMsg(4000, 16)), "tx：begin 成功");

    uds::Frame fc;
    fc.id = 0x7E0; fc.dlc = 3;
    fc.data[0] = 0x32;                                    // FS=OVFLW
    tx.onFlowControl(fc, 0);

    uds::Frame f;
    check(tx.poll(0, f) == uds::IsoTpTx::Status::Aborted,
          "tx：对端回 OVFLW → 放弃发送");
}

// ---------------------------------------------------------------------------
// M1b-3：接收侧每收满一块要回流控
// ---------------------------------------------------------------------------
void testRxBlockFlowControl() {
    uds::IsoTp tp;                       // 默认 blockSize = 8
    const auto msg    = makeMsg(4000, 17);
    const auto frames = buildFrames(0x7E0, msg);
    std::vector<std::uint8_t> out;

    tp.feed(frames[0], out);             // 首帧
    check(tp.needFlowControl(), "rx：收到首帧就该回流控");
    tp.clearFlowControl();
    check(!tp.needFlowControl(), "rx：clearFlowControl() 后复位");

    for (std::size_t i = 1; i <= 7; ++i) tp.feed(frames[i], out);
    check(!tp.needFlowControl(), "rx：只收了 7 个连续帧，还不该回流控");

    tp.feed(frames[8], out);             // 第 8 个 → 满一块
    check(tp.needFlowControl(), "rx：收满 8 个连续帧要再回一张流控");
}

} // namespace

int main() {
    std::printf("=== ISO-TP 自测（M1a / M1b-1 / M1b-3）===\n\n");

    testSingleFrame();
    testSingleFrameMax();
    testMultiFrame();
    testBigMessage();
    testSeqError();
    testTruncated();
    testTimeout();
    testWrongId();
    testFlowControlFrame();
    testSegmentSingle();
    testSegmentCount();
    testSegmentRoundTrip();
    testTxSingleFrame();
    testTxWaitFlowControl();
    testTxBlockSize();
    testTxStmin();
    testTxComplete();
    testTxOverflow();
    testRxBlockFlowControl();

    std::printf("\n--------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", gPass, gFail);
    if (gFail == 0) std::printf("M1 全绿 —— ISO-TP 收发两侧的规矩都立住了。\n");
    return (gFail == 0) ? 0 : 1;
}
