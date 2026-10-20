#pragma once
// Host socket adapter for exercising Vita transport logic, not a Vita SDK.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <algorithm>
#include <cstring>
inline constexpr int SCE_NET_AF_INET=AF_INET, SCE_NET_SOCK_STREAM=SOCK_STREAM, SCE_NET_IPPROTO_TCP=IPPROTO_TCP;
inline constexpr int SCE_NET_SOL_SOCKET=SOL_SOCKET, SCE_NET_SO_REUSEADDR=SO_REUSEADDR, SCE_NET_SO_ERROR=SO_ERROR;
inline constexpr int SCE_NET_SO_NBIO=0x1100, SCE_NET_TCP_NODELAY=TCP_NODELAY;
inline constexpr int SCE_NET_EWOULDBLOCK=EWOULDBLOCK, SCE_NET_EINPROGRESS=EINPROGRESS, SCE_NET_EALREADY=EALREADY;
inline constexpr int SCE_NET_ERROR_EWOULDBLOCK=-100, SCE_NET_ERROR_EINPROGRESS=-101, SCE_NET_ERROR_EALREADY=-102;
inline constexpr int SCE_NET_EPOLLOUT=EPOLLOUT, SCE_NET_EPOLLERR=EPOLLERR, SCE_NET_EPOLLHUP=EPOLLHUP;
inline constexpr int SCE_NET_EPOLL_CTL_ADD=EPOLL_CTL_ADD;
inline constexpr int SCE_SYSMODULE_NET=0, SCE_SYSMODULE_LOADED=0, SCE_NETCTL_INFO_GET_IP_ADDRESS=0;
namespace net_shim {
inline int max_send=37;
inline bool blocked=false;
inline uint64_t time_offset=0;
}
struct SceNetInitParam { void *memory; int size, flags; };
struct SceNetSockaddr {};
struct SceNetSockaddrIn { unsigned char sin_len, sin_family; uint16_t sin_port; in_addr sin_addr; char pad[8]; };
struct SceNetEpollEvent { unsigned events=0; };
struct SceNetCtlInfo { char ip_address[16]; };
inline int sceSysmoduleIsLoaded(int) { return 0; }
inline int sceSysmoduleLoadModule(int) { return 0; }
inline int sceSysmoduleUnloadModule(int) { return 0; }
inline int sceNetShowNetstat() { return 0; }
inline int sceNetInit(SceNetInitParam *) { return 0; }
inline int sceNetTerm() { return 0; }
inline int sceNetCtlInit() { return -1; }
inline void sceNetCtlTerm() {}
inline int sceNetCtlInetGetInfo(int, SceNetCtlInfo *p) { std::strcpy(p->ip_address,"127.0.0.1"); return 0; }
inline uint64_t sceKernelGetProcessTimeWide() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) + net_shim::time_offset;
}
inline int *sceNetErrnoLoc() { return &errno; }
inline uint16_t sceNetHtons(uint16_t p) { return htons(p); }
inline int sceNetInetPton(int af,const char *s,void *out) { return inet_pton(af,s,out); }
inline int sceNetSocket(const char *,int af,int type,int proto) { return socket(af,type,proto); }
inline int sceNetSocketClose(int fd) { return close(fd); }
inline int sceNetSetsockopt(int fd,int level,int opt,const void *p,unsigned n) {
    if (opt==SCE_NET_SO_NBIO) return fcntl(fd,F_SETFL,fcntl(fd,F_GETFL) | O_NONBLOCK);
    return setsockopt(fd,level,opt,p,n);
}
inline int sceNetGetsockopt(int fd,int level,int opt,void *p,unsigned *n) { return getsockopt(fd,level,opt,p,n); }
inline sockaddr_in native_address(const SceNetSockaddr *p) {
    auto *v=reinterpret_cast<const SceNetSockaddrIn *>(p);
    sockaddr_in a{}; a.sin_family=v->sin_family; a.sin_port=v->sin_port; a.sin_addr=v->sin_addr; return a;
}
inline int sceNetBind(int fd,const SceNetSockaddr *p,unsigned) { auto a=native_address(p); return bind(fd,reinterpret_cast<sockaddr *>(&a),sizeof a); }
inline int sceNetConnect(int fd,const SceNetSockaddr *p,unsigned) { auto a=native_address(p); return connect(fd,reinterpret_cast<sockaddr *>(&a),sizeof a); }
inline int sceNetListen(int fd,int n) { return listen(fd,n); }
inline int sceNetAccept(int fd,void *,void *) { return accept(fd,nullptr,nullptr); }
inline int sceNetRecv(int fd,void *p,unsigned n,int flags) { return recv(fd,p,n,flags); }
inline int sceNetSend(int fd,const void *p,unsigned n,int flags) {
    if (net_shim::blocked) { errno=EWOULDBLOCK; return -1; }
    return send(fd,p,std::min(n,unsigned(net_shim::max_send)),flags|MSG_NOSIGNAL);
}
inline int sceNetEpollCreate(const char *,int) { return epoll_create1(0); }
inline int sceNetEpollDestroy(int e) { return close(e); }
inline int sceNetEpollControl(int e,int op,int fd,SceNetEpollEvent *p) {
    epoll_event ev{}; ev.events=p->events; return epoll_ctl(e,op,fd,&ev);
}
inline int sceNetEpollWait(int e,SceNetEpollEvent *p,int,int) {
    epoll_event ev{}; int r=epoll_wait(e,&ev,1,0); p->events=ev.events; return r;
}
