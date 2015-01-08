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

typedef struct {
	int counter;
} atomic_t;

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

extern struct inode foo_4(struct dir *);

#endif /* ! __BASIC_H__ */
