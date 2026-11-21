#include "CanBus.h"
#include "RingBuffer.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

std::atomic<bool> gStop{false};
void onSignal(int) { gStop.store(true, std::memory_order_relaxed); }

void printFrame(const uds::Frame& f) {
    std::printf("RX id=0x%03X ext=%u dlc=%u data=",
                f.id, f.extended ? 1u : 0u,
                static_cast<unsigned>(f.dlc));
    for (unsigned i = 0; i < f.dlc; ++i) std::printf("%02X ", f.data[i]);
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const char* iface = (argc > 1) ? argv[1] : "vcan0";
    // 第二参数 --quiet：只计数不打印。用来把"丢帧"和"printf 太慢"区分开。
    const bool  quiet = (argc > 2) && std::strcmp(argv[2], "--quiet") == 0;

    uds::CanBus bus(iface);
    if (!bus.ok()) {
        std::fprintf(stderr, "打不开 %s，先跑 sudo ip link set up %s\n", iface, iface);
        return 1;
    }

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // 收帧线程与业务解耦：Rx 只往队列里塞，绝不在这里 printf
    RingBuffer<uds::Frame, 256> rxq;
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> dropped{0};

    std::thread rxThread([&] {
        uds::Frame f;
        while (!gStop.load(std::memory_order_relaxed)) {
            if (!bus.recv(f, 100)) continue;          // 100ms 顺手检查一次 stop
            received.fetch_add(1, std::memory_order_relaxed);
            if (!rxq.push(f)) dropped.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread workerThread([&] {
        uds::Frame batch[32];
        while (!gStop.load(std::memory_order_relaxed)) {
            const std::size_t n = rxq.popBatch(batch, 32);
            for (std::size_t i = 0; i < n; ++i) {
                if (!quiet) printFrame(batch[i]);
            }
        }
    });

    rxThread.join();
    workerThread.join();

    std::printf("\n共收 %llu 帧，丢 %llu 帧\n",
                static_cast<unsigned long long>(received.load()),
                static_cast<unsigned long long>(dropped.load()));
    return 0;
}
