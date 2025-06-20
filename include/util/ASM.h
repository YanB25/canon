#pragma once

namespace util::asms
{
__attribute__((always_inline)) inline void cpu_relax()
{
    asm volatile("pause\n" : : : "memory");
}
}  // namespace util::asms