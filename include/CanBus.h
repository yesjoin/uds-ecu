#pragma once
// ============================================================================
// CanBus.h —— SocketCAN 的薄封装
//
//   1. 只做三件事：开 socket、收发一帧、把 id 里的标志位剥干净。
//      协议逻辑一律不放在这里。
//   2. Frame.id 是"干净 ID"（已剥掉 EFF/RTR 标志）。上层比较 ID 时
//      不需要再 & CAN_EFF_MASK —— 坑在下面就一次性埋掉。
//   3. 单 ECU 不需要 epoll。一个 socket 配一个阻塞 recv 线程足够；
//      epoll 留给同时监听多个接口的场景（双通道网关）。
//   4. vcan 上永远不会有错误帧和总线关闭，别在这儿找 ERR_FILTER 的活。
// ============================================================================

#include <cstdint>
#include <string>

namespace uds {

struct Frame {
    std::uint32_t id       = 0;      // 干净 ID（11 位或 29 位）
    bool          extended = false;
    bool          rtr      = false;
    std::uint8_t  dlc      = 0;      // 0..8
    std::uint8_t  data[8]  = {};
};

class CanBus {
public:
    explicit CanBus(const std::string& iface);
    ~CanBus();

    CanBus(const CanBus&)            = delete;
    CanBus& operator=(const CanBus&) = delete;

    bool ok() const { return m_fd >= 0; }

    bool send(const Frame& f);

    // 有帧返回 true；timeoutMs 内无帧返回 false。<0 表示无限等待。
    bool recv(Frame& out, int timeoutMs = 100);

    int fd() const { return m_fd; }

private:
    int         m_fd = -1;
    std::string m_iface;
};

} // namespace uds
