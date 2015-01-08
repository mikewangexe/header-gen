#define MAX_INT 0x7fffffff

#define VLAN_ETH_ALEN    6     /* multiline comment here
								  another line...
							   */
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

#ifndef __deprecated
#define __deprecated
#endif

#undef __deprecated
#define __deprecated

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

typedef unsigned char u8;
typedef unsigned int u32;
typedef u32 (*initcall_t)(void);

struct ttt {
	u32 i;
};
typedef struct ttt tt;

typedef struct {
	int counter;
} atomic_t;
#define DEFINE_ATOMIC(name) static atomic_t name = {0}

struct nested {
	union {
		u8 c;
		u32 i;
	};
};

struct file;
struct dir;
struct file_op {
	int (*close)(struct file *file);
};
struct file {
	u32 fd;
};
struct inode {
	u32 i;
};

// XXX If these macro-expanded decls are placed at the end of file, we can still
// not handle them well...
declare_percpu(int, i);
in_func(b)

int foo(struct ttt *x);
extern void foo_2(void)
	__compiletime_error("Error!");
void foo_3(int a,
		   int b);
extern struct inode foo_4(struct dir *);
