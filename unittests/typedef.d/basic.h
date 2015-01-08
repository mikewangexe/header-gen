#ifndef __BASIC_H__
#define __BASIC_H__

#define __compiletime_error(message) __attribute__((unused))
#define declare_percpu(type, name) extern type percpu_##name
#define in_func(unit)							\
	static inline int in##unit (int port) {		\
	    return port;							\
	}

#undef __deprecated

typedef unsigned char u8;
typedef unsigned int u32;

struct ttt {
	u32 i;
};
typedef struct ttt tt;

#endif /* ! __BASIC_H__ */
