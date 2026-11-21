#include "CanBus.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <linux/can.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace uds {

CanBus::CanBus(const std::string& iface) : m_iface(iface) {
    m_fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (m_fd < 0) {
        std::perror("socket(PF_CAN)");
        return;
    }
    // 故意不设 CAN_RAW_FILTER：先收全部，过滤留给上层（M1 的 ISO-TP 按 ID 分流）。

    struct ifreq ifr {};
    std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface.c_str());

    if (::ioctl(m_fd, SIOCGIFINDEX, &ifr) < 0) {
        std::fprintf(stderr, "ioctl(SIOCGIFINDEX) %s: %s\n",
                     iface.c_str(), std::strerror(errno));
        ::close(m_fd);
        m_fd = -1;
        return;
    }

    sockaddr_can addr {};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (::bind(m_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "bind %s: %s\n", iface.c_str(), std::strerror(errno));
        ::close(m_fd);
        m_fd = -1;
        return;
    }

    std::fprintf(stderr, "CanBus 已绑定 %s\n", iface.c_str());
}

CanBus::~CanBus() {
    if (m_fd >= 0) ::close(m_fd);
}

bool CanBus::send(const Frame& f) {
    if (m_fd < 0) return false;

    struct can_frame raw {};
    raw.can_id = f.id;
    if (f.extended) raw.can_id |= CAN_EFF_FLAG;
    if (f.rtr)      raw.can_id |= CAN_RTR_FLAG;
    raw.can_dlc = f.dlc;
    std::memcpy(raw.data, f.data, sizeof(raw.data));

    const ssize_t n = ::write(m_fd, &raw, sizeof(raw));
    if (n != static_cast<ssize_t>(sizeof(raw))) {
        std::perror("write(can_frame)");
        return false;
    }
    return true;
}

bool CanBus::recv(Frame& out, int timeoutMs) {
    if (m_fd < 0) return false;

    if (timeoutMs >= 0) {
        struct pollfd pfd {};
        pfd.fd     = m_fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, timeoutMs) <= 0) return false;   // 超时 / 被信号打断
    }

    struct can_frame raw {};
    const ssize_t n = ::read(m_fd, &raw, sizeof(raw));
    if (n < 0) {
        if (errno == EINTR) return false;
        std::perror("read(can_frame)");
        return false;
    }
    if (n < static_cast<ssize_t>(sizeof(raw))) return false;  // 短读，丢弃

    // 坑 1：can_id 里混了 EFF/RTR 标志，先剥再存
    out.extended = (raw.can_id & CAN_EFF_FLAG) != 0;
    out.rtr      = (raw.can_id & CAN_RTR_FLAG) != 0;
    out.id       = raw.can_id & (out.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    out.dlc      = (raw.can_dlc > 8) ? 8 : raw.can_dlc;
    std::memcpy(out.data, raw.data, sizeof(out.data));
    return true;
}

} // namespace uds
