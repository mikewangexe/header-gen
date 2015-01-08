#include <macro.h>
#include <basic.h>
#include <cpu.h>

module_init(basic);

#if defined(LITTLE_ENDIAN)
u32 little;
#else
#error "Fix endian!"
#endif

DEFINE_ATOMIC(atom);

int __deprecated bar(int x) {
	v6_set_pte(x);
	return x & LAST_WORD_MASK(16) + inb(percpu_i);
}
