#ifndef __BASIC_H__
#define __BASIC_H__

#define VLAN_ETH_ALEN    6     /* multiline comment here
								  another line...
							   */

#define __compiletime_error(message) __attribute__((unused))
#define declare_percpu(type, name) extern type percpu_##name
#define in_func(unit)							\
	static inline int in##unit (int port) {		\
	    return port;							\
	}

#undef __deprecated

typedef unsigned int u32;

#endif /* ! __BASIC_H__ */
