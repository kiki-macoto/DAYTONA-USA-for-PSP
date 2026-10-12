// gen_psp.h's helpers: exactly what the inline code they replace did, in the
// same order (the IP, the count the runtime sees, the epoch before and after
// the access), around the same Cpu and Bus calls.
#include "gen_psp.h"

#include <utility>

namespace gen {

namespace {
template <typename Access> inline auto around(Env &e, uint32_t ip, uint32_t n, Access access) {
    e.c.m_IP = ip;
    e.ls.pending = n; // the runtime reads ls.count + ls.pending (Lockstep::now)
    const uint32_t epoch = e.ls.epoch;
    const auto v = access();
    return std::pair(v, e.ls.epoch != epoch);
}
uint64_t pack(uint32_t v, bool moved) { return uint64_t(v) | uint64_t(moved) << 32; }
} // namespace

uint64_t psp_ld32(Env &e, uint32_t addr, uint32_t ip, uint32_t n) {
    const auto [v, moved] = around(e, ip, n, [&] { return e.c.i960_read_dword_unaligned(addr); });
    return pack(v, moved);
}
uint64_t psp_ld16(Env &e, uint32_t addr, uint32_t ip, uint32_t n) {
    const auto [v, moved] = around(e, ip, n, [&] { return e.c.i960_read_word_unaligned(addr); });
    return pack(v, moved);
}
uint64_t psp_ld8(Env &e, uint32_t addr, uint32_t ip, uint32_t n) {
    const auto [v, moved] = around(e, ip, n, [&] { return e.c.bus->read_byte(addr); });
    return pack(v, moved);
}
bool psp_st32(Env &e, uint32_t addr, uint32_t value, uint32_t ip, uint32_t n) {
    return around(e, ip, n, [&] { e.c.i960_write_dword_unaligned(addr, value); return 0; }).second;
}
bool psp_st16(Env &e, uint32_t addr, uint16_t value, uint32_t ip, uint32_t n) {
    return around(e, ip, n, [&] { e.c.i960_write_word_unaligned(addr, value); return 0; }).second;
}
bool psp_st8(Env &e, uint32_t addr, uint8_t value, uint32_t ip, uint32_t n) {
    return around(e, ip, n, [&] { e.c.bus->write_byte(addr, value); return 0; }).second;
}
bool psp_call(Env &e, uint32_t target, uint32_t sp, uint32_t ip, uint32_t n) {
    return around(e, ip, n, [&] { e.c.do_call(target, 0, sp); return 0; }).second;
}
bool psp_ret(Env &e, uint32_t ip, uint32_t n) {
    return around(e, ip, n, [&] { e.c.do_ret(); return 0; }).second;
}

} // namespace gen
