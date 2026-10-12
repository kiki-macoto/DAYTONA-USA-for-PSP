#include "runtime/enhance.h"

#include <algorithm>
#include <cstdlib>

namespace rt {

// The draw list holds at most 63 cells (0x5016c1..0x5016ff; the game uses
// 0x501700), so the widest square is 7x7 (49).
// The game's polygon budget per frame: objects stop being drawn once their
// costs pass it (0x17a78, 0x1786c); set once at boot (0x1210: 0x1388). The
// hardware's limit, not ours: more cells need a bigger budget, or they come
// last in the list and are never drawn.
constexpr uint32_t kBudgetAddr = 0x5010f4, kGameBudget = 5000;

void hook_draw_list(Cpu &c) {
    const int level = std::clamp(Enhance::draw_distance, Enhance::kDrawMin, Enhance::kDrawMax);
    static bool raised = false;
    const uint32_t budget = level > 0 ? kGameBudget * uint32_t(1 + level) : kGameBudget;
    if (level > 0 || raised) {
        c.bus->write_dword(kBudgetAddr, budget);
        raised = level > 0;
    }
    if (level == 0) return;
    const uint32_t car = c.m_r[8];
    if (car > 255) return; // not a cell: leave the game's list alone
    const int cx = int(car & 15), cy = int(car >> 4);
    uint8_t list[63];
    int n = 0;
    if (level < 0) {
        // Less: the game's own list, cut to the car's cell (-2) or one cell around it (-1).
        const int keep = level == -1 ? 1 : 0;
        const int count = std::min<int>(c.bus->read_byte(0x5016c0), 63);
        for (int i = 0; i < count; i++) {
            const int cell = c.bus->read_byte(0x5016c1 + uint32_t(i));
            const int dx = (cell & 15) - cx, dy = (cell >> 4) - cy;
            if (std::max(std::abs(dx), std::abs(dy)) <= keep) list[n++] = uint8_t(cell);
        }
    } else {
        // More: every cell within 2 (+1: the whole 5x5 the game chooses from)
        // or 3 (+2: 7x7) of the car's, nearest ring first, inside the
        // 16x16 grid (cells are bytes; no wrapping into the next row).
        const int radius = 1 + level;
        for (int r = 0; r <= radius; r++)
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || x > 15 || y < 0 || y > 15) continue;
                    list[n++] = uint8_t(x + 16 * y);
                }
    }
    c.bus->write_byte(0x5016c0, uint8_t(n));
    for (int i = 0; i < n; i++) c.bus->write_byte(0x5016c1 + uint32_t(i), list[i]);
}

} // namespace rt
