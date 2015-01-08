#ifndef __BASIC_H__
#define __BASIC_H__

#define __compiletime_error(message) __attribute__((unused))
#define declare_percpu(type, name) extern type percpu_##name
#define in_func(unit)							\
	static inline int in##unit (int port) {		\
	    return port;							\
	}

#undef __deprecated

enum log_level {
	LOG_ERR,
	LOG_WARN,
	LOG_END
};

enum ee {
	EE_START,
	EE_1,
	EE_END
};

typedef unsigned int u32;

#endif /* ! __BASIC_H__ */
