#include "../platform/vita/link.h"
#include "app/link_socket.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
int reserve_port() {
    int s=::socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(::bind(s,reinterpret_cast<sockaddr *>(&a),sizeof a)==0);
    socklen_t size=sizeof a; CHECK(getsockname(s,reinterpret_cast<sockaddr *>(&a),&size)==0);
    int port=ntohs(a.sin_port); close(s); return port;
}
int main() {
    const int vp=reserve_port(), dp=reserve_port();
    vita::TcpLink bad(vp,"not-an-ip",dp); CHECK(!bad.error().empty());
    vita::TcpLink vita(vp,"127.0.0.1",dp); CHECK(vita.error().empty());
    auto desktop=std::make_unique<app::TcpLink>(uint16_t(dp),"127.0.0.1:"+std::to_string(vp));
    CHECK(desktop->error().empty());
    for (int i=0;i<2000 && !(vita.tx_open()&&vita.rx_open()&&desktop->tx_open()&&desktop->rx_open());++i) {
        vita.poll(); desktop->poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(vita.tx_open()&&vita.rx_open()&&desktop->tx_open()&&desktop->rx_open());
    std::array<uint8_t,0x4000> a{},b{};
    rt::CommBoard master(a.data()),slave(b.data());
    master.set_transport(&vita); slave.set_transport(desktop.get());
    master.fg_w(1); slave.fg_w(0); master.cn_w(1); slave.cn_w(1);
    for(int i=0;i<1000 && (master.count()!=2 || slave.count()!=2);++i) {
        master.vblank(); slave.vblank(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(master.id()==1 && slave.id()==2 && master.count()==2 && slave.count()==2);
    for(int i=0;i<64;++i) { a[0x2000+i]=uint8_t(i+1); b[0x2000+i]=uint8_t(i+80); }
    for(int i=0;i<30;++i) { master.vblank(); slave.vblank(); master.fg_r(); slave.fg_r(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    for(int i=0;i<64;++i) { CHECK(a[0x21c0+i]==uint8_t(i+80)); CHECK(b[0x21c0+i]==uint8_t(i+1)); }
    net_shim::blocked=true;
    uint8_t data[4096]{};
    CHECK(vita.write(data,sizeof data)); // queued, no blocking send
    net_shim::time_offset+=2100000;
    vita.poll(); CHECK(!vita.tx_open()); // bounded stall becomes loss
    master.vblank(); CHECK(master.link()==rt::CommBoard::Link::Lost);
    net_shim::blocked=false;
    desktop.reset();
    vita.read(data,sizeof data); CHECK(!vita.rx_open());
    desktop=std::make_unique<app::TcpLink>(uint16_t(dp),"127.0.0.1:"+std::to_string(vp));
    CHECK(desktop->error().empty());
    for(int i=0;i<2000 && !(vita.tx_open()&&vita.rx_open()&&desktop->tx_open()&&desktop->rx_open());++i) {
        vita.poll(); desktop->poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(vita.tx_open()&&vita.rx_open()&&desktop->tx_open()&&desktop->rx_open());
    net_shim::blocked=true;
    for(int i=0;i<16;++i) CHECK(vita.write(data,sizeof data));
    CHECK(!vita.write(data,1)); // hard queue bound, not unbounded allocation
    CHECK(!vita.tx_open());
    net_shim::blocked=false;
    std::puts("Vita transport/desktop TCP ring, fragmented sends, board data and stalled-peer loss passed");
}
