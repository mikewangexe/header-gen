#ifndef __BASIC_H__
#define __BASIC_H__

#define LITTLE_ENDIAN
#define LAST_WORD_MASK(nbits)                                \
(                                                            \
    ((nbits) % 64) ? (1UL << ((nbits) % 64)) - 1 : ~0UL      \
)

#define __compiletime_error(message) __attribute__((unused))
#define declare_percpu(type, name) extern type percpu_##name
#define in_func(unit)							\
	static inline int in##unit (int port) {		\
	    return port;							\
	}
#define module_init(mod)						\
	initcall_t __initcall_##mod##_fn

#undef __deprecated
#define __deprecated

typedef unsigned int u32;
typedef u32 (*initcall_t)(void);

typedef struct {
	int counter;
} atomic_t;
#define DEFINE_ATOMIC(name) static atomic_t name = {0}

declare_percpu(int, i);
in_func(b)

#endif /* ! __BASIC_H__ */
