#include "nr_guide_region.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

int main() {
    struct Case { const char* name; uint32_t aw, ah, x, y, w, h, ex, ey, ew, eh; };
    const Case cases[] = {
        {"whole",3840,2160,0,0,0,0,0,0,3840,2160},
        {"explicit",3840,2160,0,0,1920,1080,0,0,1920,1080},
        {"offset explicit",3840,2160,1920,1080,1920,1080,1920,1080,1920,1080},
        {"offset remainder",3840,2160,1920,1080,0,0,1920,1080,1920,1080},
        {"clipped",3840,2160,1920,1080,3840,2160,1920,1080,1920,1080},
        {"outside x",3840,2160,3840,0,1920,1080,0,0,3840,2160},
        {"outside y",3840,2160,0,2160,1920,1080,0,0,3840,2160},
        {"one texel",3840,2160,3839,2159,0,0,3839,2159,1,1},
        {"overflow extent",3840,2160,1920,1080,UINT32_MAX,UINT32_MAX,1920,1080,1920,1080},
        {"empty",0,0,0,0,0,0,0,0,0,0},
    };
    unsigned checked=0;
    for(const auto& c:cases) {
        const auto r=nr::resolve_guide_region(c.aw,c.ah,c.x,c.y,c.w,c.h);
        assert(r.x==c.ex && r.y==c.ey && r.width==c.ew && r.height==c.eh);
        for(float scale: {0.f,1.f,-1.f,0.5f,2.f}) {
            const float sx=nr::normalized_guide_motion(scale,r.width);
            const float sy=nr::normalized_guide_motion(scale,r.height);
            assert(sx==(c.ew ? scale/float(c.ew):scale));
            assert(sy==(c.eh ? scale/float(c.eh):scale));
            ++checked;
        }
    }
    // A moving stream using an offset region with implicit extent. Exact
    // backward displacement is +8 region pixels; UV is relative to that region.
    // The reference is geometry, not the NR model or a NVIDIA GPU output.
    const auto r=nr::resolve_guide_region(3840,2160,1920,1080,0,0);
    double old_error=0,new_error=0;
    for(int frame=0;frame<32;++frame) {
        const float backward=frame==0 ? 0.f:8.f;
        const float old_px=backward/3840.f*1920.f;
        const float new_px=backward*nr::normalized_guide_motion(1.f,r.width)*1920.f;
        old_error+=std::abs(old_px-backward);
        new_error+=std::abs(new_px-backward);
        assert(std::abs(new_px-backward)<1e-6f);
    }
    assert(nr::normalized_guide_motion(0.f,r.width)==0.f);
    std::cout<<"PASS: "<<checked<<" region/scale cases; 32-frame coordinate replay. "
             <<"Mean error before="<<old_error/32<<" after="<<new_error/32<<" active-region pixels\n";
}
