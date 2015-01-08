
#define DDE_WEAK __attribute__((weak))

#define dde_dummy_printf(...)
#define dde_printf(...) dde_dummy_printf(__VA_ARGS__)

