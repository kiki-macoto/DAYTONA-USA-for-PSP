// PSP buttons to the cabinet's inputs, independent of the PSP SDK so it can
// be tested on the PC (as platform/dreamcast/game/controls.h, whose gearbox
// encoding it follows; the input bits are src/app/controls.cpp's).
//
//   analog stick, left and right only        steer (pushed more up or down than
//                                            sideways: straight ahead)
//   Cross / Square                           accelerate / brake
//   R / L                                    shift up / down (gears 1-4, on presses)
//   D-pad up, right, down, left              view buttons VR1, VR2, VR3, VR4
//   Start                                    start; with no credit, a coin too (the
//                                            game starts on a credit: one press plays).
//                                            Not with Cross held: Start + accelerator
//                                            is the game's time attack.
//   Select (pressed and let go alone)        coin
//   Select + L / Select + R (held)           test / service (the game's test menu)
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace psp {

// pspctrl.h's PSP_CTRL_* values, repeated so this header does not need the SDK.
enum Button : uint32_t {
    Select = 0x000001, Start = 0x000008,
    Up = 0x000010, Right = 0x000020, Down = 0x000040, Left = 0x000080,
    LTrigger = 0x000100, RTrigger = 0x000200,
    Triangle = 0x001000, Circle = 0x002000, Cross = 0x004000, Square = 0x008000,
};

struct Pad {
    uint32_t buttons = 0;
    int lx = 128;        // the analog stick, 0..255 (128 centred)
    bool credit = false; // the game has a credit (the frontend reads it from the backup RAM)
    int ly = 128;        // the stick's up and down (only to tell it from a sideways push)
};

struct Input { // as rt::Inputs
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20;
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;
};

class Controls {
public:
    Input sample(Pad pad) {
        Input in;
        const uint32_t b = pad.buttons;
        const uint32_t pressed = b & ~held_;
        const bool select = b & Select;
        held_ = b;
        // Select is a modifier as well as the coin button: the coin goes in
        // when Select is let go without another button having been pressed
        // with it, held for a few frames (the game looks for the edge).
        if (select && (b & ~uint32_t(Select))) select_combo_ = true;
        if (!select && select_was_) {
            if (!select_combo_) coin_frames_ = kCoinFrames;
            select_combo_ = false;
        }
        select_was_ = select;
        // Start with no credit: a coin as well (not for time attack).
        if ((pressed & Start) && !pad.credit && !(b & Cross) && !select) coin_frames_ = kCoinFrames;

        // Left and right only (reported: the wheel now and then stayed
        // turned): a stick pushed more up or down than sideways steers
        // straight, whatever its sideways part.
        const int dx = std::abs(std::clamp(pad.lx, 0, 255) - 128), dy = std::abs(std::clamp(pad.ly, 0, 255) - 128);
        const float steer = dy > dx ? 0.f : axis(pad.lx);
        in.steer = uint8_t(std::lround(128.f + 96.f * steer));
        in.accel = (b & Cross) ? 0xe0 : 0x20;
        in.brake = (b & Square) ? 0xe0 : 0x20;

        // Shift only on presses (several board frames may run per sample);
        // L and R are test and service while Select is held.
        const bool up = !select && (pressed & RTrigger), down = !select && (pressed & LTrigger);
        gear_ = std::clamp(gear_ + int(up) - int(down), 1, 4);
        constexpr uint8_t codes[] = {0, 2, 1, 6, 5}; // the gearbox, as src/app/controls.cpp
        in.in1 = uint8_t((in.in1 & ~0x70) | (codes[gear_] << 4));

        if (coin_frames_ > 0) {
            --coin_frames_;
            in.in0 &= uint8_t(~0x01); // coin
        }
        if (select && (b & LTrigger)) in.in0 &= uint8_t(~0x04); // test
        if (select && (b & RTrigger)) in.in0 &= uint8_t(~0x08); // service
        if (b & Start) in.in0 &= uint8_t(~0x10);                // start
        if (b & Up) in.in0 &= uint8_t(~0x20);                   // VR1
        if (b & Right) in.in0 &= uint8_t(~0x40);                // VR2
        if (b & Down) in.in0 &= uint8_t(~0x80);                 // VR3
        if (b & Left) in.in1 &= uint8_t(~0x01);                 // VR4
        return in;
    }
    int gear() const { return gear_; }

private:
    static constexpr int kCoinFrames = 4;
    // 0..255 to -1..1 with a dead zone (the PSP's stick rests a few steps
    // off centre).
    static float axis(int value, float deadzone = 0.2f) {
        const float x = (std::clamp(value, 0, 255) - 128) / (value < 128 ? 128.f : 127.f);
        const float magnitude = std::abs(x);
        if (magnitude <= deadzone) return 0.f;
        return std::copysign(std::min(1.f, (magnitude - deadzone) / (1.f - deadzone)), x);
    }
    uint32_t held_ = 0;
    int gear_ = 1;
    int coin_frames_ = 0;
    bool select_was_ = false, select_combo_ = false;
};

} // namespace psp
