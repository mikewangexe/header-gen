#define __deprecated __attribute__((deprecated))

#define CPU_ARCH v6
#define ____glue(a,b) a##b
#define __glue(a,b) ____glue(a, b)
#define cpu_set_pte __glue(CPU_ARCH, _set_pte)
