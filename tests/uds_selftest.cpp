// ============================================================================
// uds_selftest.cpp —— M2 验收夹具
//
// 和 isotp_selftest 一样：不连 vcan、不等真实时间，纯内存里发请求看响应。
// 时间全靠 nowMs 参数传进去，所以 S3 超时不用真等 5 秒。
// ============================================================================

#include "Uds.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

int gPass = 0;
int gFail = 0;

void dump(const Bytes& v) {
    for (auto b : v) std::printf(" %02X", b);
    std::printf("\n");
}

void check(bool ok, const char* name) {
    if (ok) { ++gPass; std::printf("[PASS] %s\n", name); }
    else    { ++gFail; std::printf("[FAIL] %s\n", name); }
}

// 比 check 更好用：响应是字节串，错了要能看到差在哪
void expect(const char* name, const Bytes& got, const Bytes& want) {
    if (got == want) {
        ++gPass;
        std::printf("[PASS] %s\n", name);
        return;
    }
    ++gFail;
    std::printf("[FAIL] %s\n", name);
    std::printf("      收到:"); dump(got);
    std::printf("      应为:"); dump(want);
}

Bytes ascii(const char* s) {
    Bytes v;
    for (const char* p = s; *p != '\0'; ++p) v.push_back(static_cast<std::uint8_t>(*p));
    return v;
}

// ---------------------------------------------------------------------------
// 0x10 会话切换
// ---------------------------------------------------------------------------
void testSessionControl() {
    uds::UdsServer ecu;
    expect("0x10：切扩展会话 → 50 03 + P2/P2*",
           ecu.handle(Bytes{0x10, 0x03}, 0),
           Bytes{0x50, 0x03, 0x00, 0x32, 0x13, 0x88});
    check(ecu.session() == uds::Session::Extended, "0x10：会话状态真的变了");

    expect("0x10：F186 读回当前会话 = 03",
           ecu.handle(Bytes{0x22, 0xF1, 0x86}, 0),
           Bytes{0x62, 0xF1, 0x86, 0x03});

    expect("0x10：回默认会话",
           ecu.handle(Bytes{0x10, 0x01}, 0),
           Bytes{0x50, 0x01, 0x00, 0x32, 0x13, 0x88});
    check(ecu.session() == uds::Session::Default, "0x10：回到默认会话");

    expect("0x10：子功能 05 不存在 → 7F 10 12",
           ecu.handle(Bytes{0x10, 0x05}, 0),
           Bytes{0x7F, 0x10, 0x12});

    expect("0x10：长度只有 1 字节 → 7F 10 13",
           ecu.handle(Bytes{0x10}, 0),
           Bytes{0x7F, 0x10, 0x13});
}

// ---------------------------------------------------------------------------
// 0x22 读 DID
// ---------------------------------------------------------------------------
void testReadDid() {
    uds::UdsServer ecu;

    Bytes vin = Bytes{0x62, 0xF1, 0x90};
    for (auto b : ascii("LSVAA0000A1234567")) vin.push_back(b);
    expect("0x22：读 VIN（20 字节，会走多帧）",
           ecu.handle(Bytes{0x22, 0xF1, 0x90}, 0), vin);

    Bytes two = Bytes{0x62, 0xF1, 0x88};
    for (auto b : ascii("SW01.02.03")) two.push_back(b);
    two.push_back(0xF1);
    two.push_back(0x91);
    for (auto b : ascii("HW01")) two.push_back(b);
    expect("0x22：一条请求读两个 DID，响应按顺序拼接",
           ecu.handle(Bytes{0x22, 0xF1, 0x88, 0xF1, 0x91}, 0), two);

    expect("0x22：DID 不存在 → 7F 22 31",
           ecu.handle(Bytes{0x22, 0x12, 0x34}, 0),
           Bytes{0x7F, 0x22, 0x31});

    expect("0x22：长度不是偶数个 DID 字节 → 7F 22 13",
           ecu.handle(Bytes{0x22, 0xF1}, 0),
           Bytes{0x7F, 0x22, 0x13});
}

// ---------------------------------------------------------------------------
// 0x2E 写 DID（带会话门控）
// ---------------------------------------------------------------------------
void testWriteDid() {
    uds::UdsServer ecu;

    expect("0x2E：默认会话下写 → 7F 2E 7F（会话不允许）",
           ecu.handle(Bytes{0x2E, 0x01, 0x00, 0x00, 0x50}, 0),
           Bytes{0x7F, 0x2E, 0x7F});

    ecu.handle(Bytes{0x10, 0x03}, 0);            // 先切扩展会话

    expect("0x2E：扩展会话下写车速 → 6E 01 00",
           ecu.handle(Bytes{0x2E, 0x01, 0x00, 0x00, 0x50}, 0),
           Bytes{0x6E, 0x01, 0x00});

    expect("0x2E 写入后 0x22 立刻读得到 → 00 50",
           ecu.handle(Bytes{0x22, 0x01, 0x00}, 0),
           Bytes{0x62, 0x01, 0x00, 0x00, 0x50});

    expect("0x2E：写只读 DID（VIN）→ 7F 2E 31",
           ecu.handle(Bytes{0x2E, 0xF1, 0x90, 0x41}, 0),
           Bytes{0x7F, 0x2E, 0x31});

    expect("0x2E：数据长度不匹配 → 7F 2E 13",
           ecu.handle(Bytes{0x2E, 0x01, 0x00, 0x00}, 0),
           Bytes{0x7F, 0x2E, 0x13});
}

// ---------------------------------------------------------------------------
// 0x3E 保活 + 未知服务
// ---------------------------------------------------------------------------
void testTesterPresent() {
    uds::UdsServer ecu;

    expect("0x3E：3E 00 → 7E 00", ecu.handle(Bytes{0x3E, 0x00}, 0),
           Bytes{0x7E, 0x00});

    expect("0x3E：3E 80 抑制正响应 → 什么都不回",
           ecu.handle(Bytes{0x3E, 0x80}, 0), Bytes{});

    expect("0x3E：子功能 01 不存在 → 7F 3E 12",
           ecu.handle(Bytes{0x3E, 0x01}, 0), Bytes{0x7F, 0x3E, 0x12});

    expect("未知服务 0x31 → 7F 31 11",
           ecu.handle(Bytes{0x31, 0x01, 0xFF}, 0), Bytes{0x7F, 0x31, 0x11});
}

// ---------------------------------------------------------------------------
// S3：非默认会话空闲超时自动退回
// ---------------------------------------------------------------------------
void testS3Timeout() {
    uds::UdsServer ecu;
    ecu.handle(Bytes{0x10, 0x03}, 0);              // t=0 进扩展会话

    ecu.tick(4000);                                // 4 秒，还没到 5 秒
    check(ecu.session() == uds::Session::Extended, "S3：4 秒时仍在扩展会话");

    ecu.tick(5001);                                // 超过 5 秒
    check(ecu.session() == uds::Session::Default, "S3：超 5 秒自动退回默认会话");

    expect("S3：退回后 F186 也变回 01",
           ecu.handle(Bytes{0x22, 0xF1, 0x86}, 5001),
           Bytes{0x62, 0xF1, 0x86, 0x01});

    // 保活应当把定时器推后
    ecu.handle(Bytes{0x10, 0x03}, 10000);
    ecu.handle(Bytes{0x3E, 0x00}, 14000);          // 第 4 秒保活一次
    ecu.tick(17000);                                // 距上次请求只有 3 秒
    check(ecu.session() == uds::Session::Extended, "S3：3E 保活后不会误退会话");
}

} // namespace

int main() {
    std::printf("=== UDS 服务层自测（M2）===\n\n");

    testSessionControl();
    testReadDid();
    testWriteDid();
    testTesterPresent();
    testS3Timeout();

    std::printf("\n--------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", gPass, gFail);
    if (gFail == 0) std::printf("M2 服务层全绿 —— 可以接总线给真实客户端答话了。\n");
    return (gFail == 0) ? 0 : 1;
}
