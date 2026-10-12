// game/controls.h on the PC: the PSP buttons' cabinet inputs.
//   c++ -std=c++20 -I platform/psp/game platform/psp/tools/test_controls.cpp -o test_controls && ./test_controls
#include "controls.h"

#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void expect(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}
int coins(psp::Controls &c, int frames, bool credit = false) {
    int n = 0;
    for (int i = 0; i < frames; ++i) n += !(c.sample({0, 128, credit}).in0 & 0x01);
    return n;
}
} // namespace

int main() {
    using namespace psp;
    {
        Controls c;
        const Input in = c.sample({});
        expect(in.steer == 0x80 && in.accel == 0x20 && in.brake == 0x20, "idle: centred, pedals up");
        expect(in.in0 == 0xff && in.in1 == ((0x8f & ~0x70) | (2 << 4)), "idle: no buttons, gear 1");
    }
    {
        Controls c;
        expect(c.sample({0, 0}).steer == 128 - 96, "stick full left");
        expect(c.sample({0, 255}).steer == 128 + 96, "stick full right");
        expect(c.sample({0, 140}).steer == 0x80, "stick in the dead zone");
        expect(c.sample({Right, 128}).steer == 0x80, "D-pad right does not steer");
        expect(c.sample({0, 200, false, 0}).steer == 0x80, "stick up and a little right: straight");
        expect(c.sample({0, 60, false, 250}).steer == 0x80, "stick down and a little left: straight");
        expect(c.sample({0, 255, false, 60}).steer == 128 + 96, "stick right and a little up: full right");
        expect(c.sample({0, 0, false, 200}).steer == 128 - 96, "stick left and a little down: full left");
        const Input in = c.sample({Cross | Square, 128});
        expect(in.accel == 0xe0 && in.brake == 0xe0, "Cross accelerates, Square brakes");
    }
    {
        Controls c;
        c.sample({RTrigger, 128});
        expect(c.gear() == 2, "R shifts up");
        c.sample({RTrigger, 128});
        expect(c.gear() == 2, "held R does not shift again");
        c.sample({0, 128});
        c.sample({Up, 128});
        expect(c.gear() == 2, "D-pad up does not shift");
        c.sample({LTrigger, 128});
        expect(c.gear() == 1, "L shifts down");
        for (int i = 0; i < 12; ++i) c.sample({i & 1 ? 0u : uint32_t(RTrigger), 128});
        expect(c.gear() == 4, "gears stop at 4");
    }
    {
        Controls c;
        expect(c.sample({Select, 128}).in0 & 0x01, "no coin while Select is held");
        expect(coins(c, 10) == 4, "Select let go alone: a coin for 4 frames");
    }
    {
        Controls c;
        const Input in = c.sample({Select | LTrigger, 128});
        expect(!(in.in0 & 0x04) && c.gear() == 1, "Select + L: test, no shift");
        expect(!(c.sample({Select | RTrigger, 128}).in0 & 0x08), "Select + R: service");
        expect(coins(c, 10) == 0, "Select used with another button: no coin");
    }
    {
        Controls c;
        const Input in = c.sample({Start, 128, false});
        expect(!(in.in0 & 0x10) && !(in.in0 & 0x01), "Start with no credit: start and a coin");
        expect(coins(c, 10) == 3, "  the coin held 4 frames in all");
        expect(c.sample({Start, 128, true}).in0 & 0x01, "Start with a credit: no coin");
        c.sample({0, 128});
        expect(c.sample({Start | Cross, 128, false}).in0 & 0x01, "Start + Cross (time attack): no coin");
        c.sample({0, 128});
        c.sample({Start, 128, false});
        c.sample({Start, 128, false});
        expect(coins(c, 10) == 2, "Start held: one coin, not one a frame");
    }
    {
        Controls c;
        expect(!(c.sample({Up, 128}).in0 & 0x20), "D-pad up: VR1");
        expect(!(c.sample({Right, 128}).in0 & 0x40), "D-pad right: VR2");
        expect(!(c.sample({Down, 128}).in0 & 0x80), "D-pad down: VR3");
        expect(!(c.sample({Left, 128}).in1 & 0x01), "D-pad left: VR4");
        expect(c.sample({Triangle | Circle, 128}).in0 == 0xff, "Triangle, Circle: nothing");
    }
    std::printf(failures ? "test_controls: %d failed\n" : "test_controls: all passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
