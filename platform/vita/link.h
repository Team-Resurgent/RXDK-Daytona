#pragma once
#include "runtime/comm_board.h"
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/processmgr.h>
#include <array>
#include <cstring>
#include <string>

namespace vita {
// SceNet IPv4 transport for the same TCP byte stream used by desktop cabinets.
// Main thread only. No socket call blocks the rendering/audio producer.
class TcpLink final : public rt::LinkTransport {
public:
    TcpLink(int port, const std::string &next, int next_port) : memory_(1024 * 1024) {
        if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED) {
            if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) { error_ = "Cannot load network module"; return; }
            module_owned_ = true;
        }
        // A nonnegative result means SceNet is already initialized.
        if (sceNetShowNetstat() < 0) {
            SceNetInitParam p{memory_.data(), int(memory_.size()), 0};
            if (sceNetInit(&p) < 0) { error_ = "Cannot initialize SceNet"; return; }
            net_owned_ = true;
        }
        ctl_owned_ = sceNetCtlInit() == 0;
        remote_.sin_len = sizeof remote_;
        remote_.sin_family = SCE_NET_AF_INET;
        remote_.sin_port = sceNetHtons(uint16_t(next_port));
        if (sceNetInetPton(SCE_NET_AF_INET, next.c_str(), &remote_.sin_addr) != 1) {
            error_ = "Next cabinet must be an IPv4 address"; return;
        }
        listen_ = socket();
        if (listen_ < 0) { error_ = "Cannot create link listener"; return; }
        int one = 1;
        sceNetSetsockopt(listen_, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof one);
        SceNetSockaddrIn local{};
        local.sin_len = sizeof local; local.sin_family = SCE_NET_AF_INET;
        local.sin_port = sceNetHtons(uint16_t(port));
        if (sceNetBind(listen_, reinterpret_cast<SceNetSockaddr *>(&local), sizeof local) < 0 ||
            sceNetListen(listen_, 1) < 0) error_ = "Cannot listen on link port (already in use?)";
    }
    ~TcpLink() override {
        close_socket(rx_); close_socket(tx_); close_socket(listen_);
        if (epoll_ >= 0) sceNetEpollDestroy(epoll_);
        if (ctl_owned_) sceNetCtlTerm();
        if (net_owned_) sceNetTerm();
        if (module_owned_) sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    }
    TcpLink(const TcpLink &) = delete;
    TcpLink &operator=(const TcpLink &) = delete;
    const std::string &error() const { return error_; }
    std::string local_ip() const {
        SceNetCtlInfo info{};
        return sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) == 0
            ? info.ip_address : "WIFI NOT CONNECTED";
    }
    bool rx_open() const override { return rx_ >= 0; }
    bool tx_open() const override { return tx_ >= 0 && connected_; }
    void poll() override {
        if (!error_.empty()) return;
        if (rx_ < 0) {
            rx_ = sceNetAccept(listen_, nullptr, nullptr);
            if (rx_ >= 0 && !nonblocking(rx_)) close_socket(rx_);
        }
        const auto now = sceKernelGetProcessTimeWide();
        if (tx_ < 0 && now >= retry_) {
            tx_ = socket();
            if (tx_ >= 0) {
                int one = 1;
                sceNetSetsockopt(tx_, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof one);
                const int rc = sceNetConnect(tx_, reinterpret_cast<SceNetSockaddr *>(&remote_), sizeof remote_);
                if (rc == 0) connected_ = true;
                else if (!pending(rc)) disconnect();
                else {
                    epoll_ = sceNetEpollCreate("daytona link", 0);
                    SceNetEpollEvent ev{}; ev.events = SCE_NET_EPOLLOUT;
                    if (epoll_ < 0 || sceNetEpollControl(epoll_, SCE_NET_EPOLL_CTL_ADD, tx_, &ev) < 0) disconnect();
                    connect_deadline_ = now + 2000000;
                }
            } else retry_ = now + 500000;
        }
        if (tx_ >= 0 && !connected_) {
            SceNetEpollEvent ev{};
            const int ready = sceNetEpollWait(epoll_, &ev, 1, 0);
            if (ready > 0) {
                int error = 0; unsigned length = sizeof error;
                if ((ev.events & (SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP)) ||
                    sceNetGetsockopt(tx_, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &error, &length) < 0 || error)
                    disconnect();
                else {
                    connected_ = true;
                    sceNetEpollDestroy(epoll_); epoll_ = -1;
                }
            } else if (now >= connect_deadline_) disconnect();
        }
        flush();
    }
    int read(uint8_t *buf, int max) override {
        if (rx_ < 0) return 0;
        int n = sceNetRecv(rx_, buf, unsigned(max), 0);
        if (n > 0) return n;
        if (n < 0 && pending(n)) return 0;
        close_socket(rx_); return -1;
    }
    bool write(const uint8_t *buf, int n) override {
        if (!tx_open() || n < 0) return false;
        flush();
        if (!tx_open()) return false;
        if (size_t(n) > outgoing_.size() - (end_ - begin_)) { disconnect(); return false; }
        if (end_ + size_t(n) > outgoing_.size()) {
            std::memmove(outgoing_.data(), outgoing_.data() + begin_, end_ - begin_);
            end_ -= begin_; begin_ = 0;
        }
        if (end_ == begin_) progress_ = sceKernelGetProcessTimeWide();
        std::memcpy(outgoing_.data() + end_, buf, size_t(n)); end_ += size_t(n);
        flush();
        return tx_open();
    }
private:
    int listen_ = -1, rx_ = -1, tx_ = -1, epoll_ = -1;
    bool connected_ = false, module_owned_ = false, net_owned_ = false, ctl_owned_ = false;
    uint64_t retry_ = 0, connect_deadline_ = 0, progress_ = 0;
    SceNetSockaddrIn remote_{};
    std::string error_;
    std::vector<uint8_t> memory_;
    std::array<uint8_t, 65536> outgoing_{};
    size_t begin_ = 0, end_ = 0;
    static bool pending(int rc) {
        // SceNet returns -1 with its own errno on normal socket failures.
        const int e = rc == -1 ? *sceNetErrnoLoc() : rc;
        return e == SCE_NET_EWOULDBLOCK || e == SCE_NET_EINPROGRESS || e == SCE_NET_EALREADY ||
            e == int(SCE_NET_ERROR_EWOULDBLOCK) || e == int(SCE_NET_ERROR_EINPROGRESS) ||
            e == int(SCE_NET_ERROR_EALREADY);
    }
    static void close_socket(int &s) { if (s >= 0) sceNetSocketClose(s); s = -1; }
    static bool nonblocking(int s) {
        int one = 1;
        return sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one) == 0;
    }
    static int socket() {
        int s = sceNetSocket("daytona link", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP);
        if (s >= 0 && !nonblocking(s)) close_socket(s);
        return s;
    }
    void disconnect() {
        if (epoll_ >= 0) sceNetEpollDestroy(epoll_);
        epoll_ = -1; close_socket(tx_); connected_ = false;
        begin_ = end_ = 0; retry_ = sceKernelGetProcessTimeWide() + 500000;
    }
    void flush() {
        if (!tx_open()) return;
        while (begin_ < end_) {
            int sent = sceNetSend(tx_, outgoing_.data() + begin_, unsigned(end_ - begin_), 0);
            if (sent > 0) { begin_ += size_t(sent); progress_ = sceKernelGetProcessTimeWide(); }
            else {
                if (sent == 0 || !pending(sent) || sceKernelGetProcessTimeWide() - progress_ > 2000000)
                    disconnect();
                return;
            }
        }
        begin_ = end_ = 0;
    }
};
} // namespace vita
