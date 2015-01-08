#include <basic.h>

int atomic_read(const atomic_t *v)
{
	return (*(volatile int *)&(v)->counter);
}

void test_nested(struct nested *n)
{
}

void test_forward(struct file_op *ops) {
	foo_4(0);
}
