// Packed texture helper against the original scalar address/nibble equations.
#include "runtime/raster_texel.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
static uint32_t original(uint32_t bx,uint32_t by,int x,int y,const uint32_t *sheet) {
    int x2=int(bx)+x,y2=int(by)+y;
    if(x2>=1024){x2-=1024;y2^=1024;}
    uint32_t offset=uint32_t(((y2/2)*512)+(x2/2));
    uint32_t texel=sheet[(offset>>1)&0x7ffff];
    if(offset&1)texel>>=16;
    if((y&1)==0)texel>>=8;
    if((x&1)==0)texel>>=4;
    return (texel&15)<<4;
}
int main(){
    std::mt19937 rng(0x5400);
    std::vector<uint32_t> sheet(0x80000);for(auto &v:sheet)v=rng();
    const std::array<uint32_t,16> edges{0,1,2,3,30,31,32,127,128,511,512,1023,1024,2047,2048,4095};
    for(unsigned i=0;i<2000000;++i){
        const uint32_t bx=(i<100000)?edges[(i/16)%16]%2048:rng()%2048;
        const uint32_t by=(i<100000)?edges[i%16]%1024:rng()%1024;
        uint32_t u0=rng()%4096,v0=rng()%4096,u1=(u0+1)%4096,v1=(v0+1)%4096;
        if(i%3==0)u1=rng()%4096;
        if(i%5==0)v1=rng()%4096;
        const auto q=rt::read_texel_quad(bx,by,u0,u1,v0,v1,sheet.data());
        const uint32_t expected[4]{original(bx,by,int(u0),int(v0),sheet.data()),original(bx,by,int(u1),int(v0),sheet.data()),
            original(bx,by,int(u0),int(v1),sheet.data()),original(bx,by,int(u1),int(v1),sheet.data())};
        if(q.t00!=expected[0]||q.t01!=expected[1]||q.t10!=expected[2]||q.t11!=expected[3]){
            std::fprintf(stderr,"packed texel mismatch case %u\n",i);return 1;
        }
    }
    std::puts("PASS: 2,000,000 packed-texel footprints / 8,000,000 scalar texel comparisons");
}
