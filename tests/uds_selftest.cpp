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

// ---------------------------------------------------------------------------
// M3：0x27 安全访问
// ---------------------------------------------------------------------------
std::uint32_t seedOf(const Bytes& r) {
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

void testSecurityAccess() {
    uds::UdsServer ecu;

    // 注意是 0x7F 不是 0x7E：
    //   0x7E = 子功能不支持，0x7F = 服务不支持。
    //   整个 0x27 在默认会话都不许用，所以给 7F。
    expect("0x27：默认会话要种子 → 7F 27 7F",
           ecu.handle(Bytes{0x27, 0x01}, 0), Bytes{0x7F, 0x27, 0x7F});

    ecu.handle(Bytes{0x10, 0x03}, 100);            // 进扩展会话

    const Bytes sf1 = ecu.handle(Bytes{0x27, 0x01}, 100);
    check(sf1.size() == 6 && sf1[0] == 0x67 && sf1[1] == 0x01,
          "0x27：请求种子 → 67 01 + 4 字节种子");

    // 再要一次：种子必须变，否则旧密钥可以重放
    const Bytes sf2 = ecu.handle(Bytes{0x27, 0x01}, 100);
    check(sf2.size() == 6 && seedOf(sf1) != seedOf(sf2),
          "0x27：两次种子不同（防重放）");

    expect("0x27：密钥错 → 7F 27 35",
           ecu.handle(keyReq(0x00000000u), 100), Bytes{0x7F, 0x27, 0x35});

    expect("0x27：种子作废后再送密钥 → 7F 27 24（顺序错）",
           ecu.handle(keyReq(0x00000000u), 100), Bytes{0x7F, 0x27, 0x24});

    const Bytes sf3 = ecu.handle(Bytes{0x27, 0x01}, 100);
    const std::uint32_t key = uds::UdsServer::computeKey(seedOf(sf3));
    expect("0x27：正确密钥 → 67 02",
           ecu.handle(keyReq(key), 100), Bytes{0x67, 0x02});
    check(ecu.securityUnlocked(), "0x27：解锁标志置位");

    expect("0x27：已解锁再要种子 → 全零（不用再解）",
           ecu.handle(Bytes{0x27, 0x01}, 100),
           Bytes{0x67, 0x01, 0x00, 0x00, 0x00, 0x00});
}

void testSecurityLockout() {
    uds::UdsServer ecu;
    ecu.handle(Bytes{0x10, 0x03}, 0);

    Bytes last;
    for (int i = 0; i < 3; ++i) {
        ecu.handle(Bytes{0x27, 0x01}, 0);                  // 每次都得重新要种子
        last = ecu.handle(keyReq(0x11111111u), 0);         // 每次都给错的
    }
    expect("0x27：连错 3 次 → 7F 27 36（超次数）", last, Bytes{0x7F, 0x27, 0x36});

    expect("0x27：锁定期内要种子 → 7F 27 37",
           ecu.handle(Bytes{0x27, 0x01}, 500), Bytes{0x7F, 0x27, 0x37});

    check(ecu.handle(Bytes{0x27, 0x01}, 10500).size() == 6,
          "0x27：冷却 10 秒后可以重新要种子");
}

// ---------------------------------------------------------------------------
// M3：受保护 DID 的写入门槛
// ---------------------------------------------------------------------------
void testProtectedWrite() {
    uds::UdsServer ecu;
    ecu.handle(Bytes{0x10, 0x03}, 0);

    expect("0x2E：里程 DID 未解锁 → 7F 2E 33",
           ecu.handle(Bytes{0x2E, 0x02, 0x00, 0x00, 0x00, 0x27, 0x10}, 0),
           Bytes{0x7F, 0x2E, 0x33});

    const Bytes sf = ecu.handle(Bytes{0x27, 0x01}, 0);
    ecu.handle(keyReq(uds::UdsServer::computeKey(seedOf(sf))), 0);

    expect("0x2E：解锁后写里程 → 6E 02 00",
           ecu.handle(Bytes{0x2E, 0x02, 0x00, 0x00, 0x00, 0x27, 0x10}, 0),
           Bytes{0x6E, 0x02, 0x00});

    expect("0x2E：读回里程 = 00 00 27 10（10000）",
           ecu.handle(Bytes{0x22, 0x02, 0x00}, 0),
           Bytes{0x62, 0x02, 0x00, 0x00, 0x00, 0x27, 0x10});

    // 切回默认再进扩展：会话回来了，但锁不回来
    ecu.handle(Bytes{0x10, 0x01}, 0);
    ecu.handle(Bytes{0x10, 0x03}, 0);
    expect("0x27：换会话后重新上锁 → 7F 2E 33",
           ecu.handle(Bytes{0x2E, 0x02, 0x00, 0x00, 0x00, 0x00, 0x10}, 0),
           Bytes{0x7F, 0x2E, 0x33});
}

// ---------------------------------------------------------------------------
// M3：0x19 读故障码 / 0x14 清故障码
// ---------------------------------------------------------------------------
void testDtc() {
    uds::UdsServer ecu;

    expect("0x19：按 0xFF 掩码报数量 = 3 条",
           ecu.handle(Bytes{0x19, 0x01, 0xFF}, 0),
           Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x03});

    expect("0x19：列出全部 DTC（码 + 状态）",
           ecu.handle(Bytes{0x19, 0x02, 0xFF}, 0),
           Bytes{0x59, 0x02, 0xFF,
                 0xC1, 0x20, 0x01, 0x09,
                 0x0A, 0x12, 0x02, 0x08,
                 0x7A, 0x12, 0x03, 0x01});

    expect("0x19：只查已确认（0x08）→ 2 条",
           ecu.handle(Bytes{0x19, 0x02, 0x08}, 0),
           Bytes{0x59, 0x02, 0xFF,
                 0xC1, 0x20, 0x01, 0x09,
                 0x0A, 0x12, 0x02, 0x08});

    expect("0x19：数量也按 0x08 过滤 = 2",
           ecu.handle(Bytes{0x19, 0x01, 0x08}, 0),
           Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x02});

    expect("0x14：清全部 → 54",
           ecu.handle(Bytes{0x14, 0xFF, 0xFF, 0xFF}, 0), Bytes{0x54});
    check(ecu.dtcCount() == 0, "0x14：清完之后故障码表空了");

    expect("0x19：清完再查数量 = 0",
           ecu.handle(Bytes{0x19, 0x01, 0xFF}, 0),
           Bytes{0x59, 0x01, 0xFF, 0x01, 0x00, 0x00});

    expect("0x19：清完后列表只剩表头",
           ecu.handle(Bytes{0x19, 0x02, 0xFF}, 0), Bytes{0x59, 0x02, 0xFF});

    expect("0x19：子功能 0x03 不支持 → 7F 19 12",
           ecu.handle(Bytes{0x19, 0x03, 0xFF}, 0), Bytes{0x7F, 0x19, 0x12});
}

} // namespace

int main() {
    std::printf("=== UDS 服务层自测（M2 + M3）===\n\n");

    testSessionControl();
    testReadDid();
    testWriteDid();
    testTesterPresent();
    testS3Timeout();
    testSecurityAccess();
    testSecurityLockout();
    testProtectedWrite();
    testDtc();

    std::printf("\n--------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", gPass, gFail);
    if (gFail == 0) std::printf("M3 全绿 —— 服务层从能读能写，进到有门槛了。\n");
    return (gFail == 0) ? 0 : 1;
}
