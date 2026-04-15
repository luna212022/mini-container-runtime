# Makefile — Mini Container Runtime
#
# Targets:
#   make          — build user-space engine and workload programs
#   make module   — build the kernel module
#   make all      — build everything (engine + module)
#   make clean    — remove all build artifacts
#   make load     — insert the kernel module (requires root)
#   make unload   — remove the kernel module (requires root)

# ── User-space build settings ─────────────────────────────────────────────
CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -pthread
LDFLAGS := -pthread

# Binaries
ENGINE   := engine
CPU_TEST := cpu_test
MEM_TEST := memory_test

# Sources
ENGINE_SRC   := engine.c
CPU_SRC      := cpu_test.c
MEM_SRC      := memory_test.c

# ── Kernel module build settings ──────────────────────────────────────────
MODULE_NAME  := monitor
MODULE_SRCS  := monitor.c
KDIR         := /lib/modules/$(shell uname -r)/build
PWD          := $(shell pwd)

# ── Default target: user-space only ───────────────────────────────────────
.PHONY: default
default: $(ENGINE) $(CPU_TEST) $(MEM_TEST)
	@echo "==> User-space binaries built. Run 'make module' to build the kernel module."

# Build everything
.PHONY: all
all: default module

# ── engine ────────────────────────────────────────────────────────────────
$(ENGINE): $(ENGINE_SRC) monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "==> Built: $(ENGINE)"

# ── Workload programs ─────────────────────────────────────────────────────
$(CPU_TEST): $(CPU_SRC)
	$(CC) $(CFLAGS) -O2 -o $@ $<
	@echo "==> Built: $(CPU_TEST)"

$(MEM_TEST): $(MEM_SRC)
	$(CC) $(CFLAGS) -O0 -o $@ $<
	@echo "==> Built: $(MEM_TEST)"

# ── Kernel module ─────────────────────────────────────────────────────────
obj-m += $(MODULE_NAME).o

.PHONY: module
module:
	@echo "==> Building kernel module $(MODULE_NAME).ko ..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@echo "==> Kernel module built: $(MODULE_NAME).ko"

# ── Load / unload ─────────────────────────────────────────────────────────
.PHONY: load
load: module
	@echo "==> Loading kernel module ..."
	sudo insmod $(MODULE_NAME).ko
	@echo "==> Module loaded. Check: ls -l /dev/container_monitor"
	@dmesg | tail -5

.PHONY: unload
unload:
	@echo "==> Unloading kernel module ..."
	sudo rmmod $(MODULE_NAME)
	@echo "==> Module unloaded."
	@dmesg | tail -5

# ── Clean ─────────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	@echo "==> Cleaning user-space artifacts ..."
	rm -f $(ENGINE) $(CPU_TEST) $(MEM_TEST)
	@echo "==> Cleaning kernel module artifacts ..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f *.o *.ko *.mod *.mod.c *.symvers *.order .*.cmd
	rm -rf .tmp_versions
	@echo "==> Clean done."

# ── Help ──────────────────────────────────────────────────────────────────
.PHONY: help
help:
	@echo "Targets:"
	@echo "  make          Build engine + workload programs (user-space)"
	@echo "  make module   Build kernel module monitor.ko"
	@echo "  make all      Build everything"
	@echo "  make load     sudo insmod monitor.ko"
	@echo "  make unload   sudo rmmod monitor"
	@echo "  make clean    Remove all build artifacts"
