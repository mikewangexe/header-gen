#include <basic.h>

struct ttt global;

int main(int argc, char *argv[]) {
	global.i = MAX_INT;
	return foo(&global);
}
