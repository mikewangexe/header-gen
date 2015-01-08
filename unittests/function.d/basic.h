#ifndef __BASIC_H__
#define __BASIC_H__

#define __compiletime_error(message) __attribute__((unused))
#define declare_percpu(type, name) extern type percpu_##name
#define in_func(unit)							\
	static inline int in##unit (int port) {		\
	    return port;							\
	}

#undef __deprecated

extern void foo_2(void)
	__compiletime_error("Error!");
void foo_3(int a,
		   int b);

#endif /* ! __BASIC_H__ */
