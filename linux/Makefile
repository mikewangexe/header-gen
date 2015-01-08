linux_dir = $(LINUX_DIR)
basic_paths = include arch/$(ARCH)/include
ifneq ($(BOARD),)
basic_paths += arch/$(ARCH)/mach-$(BOARD)/include
endif
generated_paths = $(basic_paths) $(addsuffix /generated,$(basic_paths))
uapi_paths = $(generated_paths) $(addsuffix /uapi,$(generated_paths))

header_paths = $(addprefix $(linux_dir)/,$(uapi_paths)) $(TOP)/lib64/clang/3.3.1/include

files = $(wildcard *.c)
directories = e1000 ixgbevf virtio bcm2708

headers = linux/kconfig.h
macros = __KERNEL__ __KERN__ __LINUX_ARM_ARCH__ KBUILD_BASENAME=\"dde\" KBUILD_MODNAME=\"dde\"
CC_FLAGS = -w $(addprefix -D,$(macros)) $(addprefix -include ,$(headers))
CC_OBJ_FLAGS = $(PLATFORM_CC_FLAGS)

composer_flags = --mode linux

include ../Makefile.inc
