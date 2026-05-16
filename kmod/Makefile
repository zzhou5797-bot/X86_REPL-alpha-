# kmod/Makefile — build the asmrepl kernel module
#
# Usage:
#   make              build asmrepl.ko against running kernel
#   make install      insmod the module (requires root)
#   make uninstall    rmmod the module (requires root)
#   make clean        remove build artifacts
#
# The module source needs the shared ioctl header at ../include/asmrepl_ioctl.h.
# We pass that path via ccflags-y.

KDIR   ?= /lib/modules/$(shell uname -r)/build
PWD    := $(shell pwd)

# Tell the kernel build system which object to build
obj-m := asmrepl.o

# Extra include path so asmrepl.c can #include "asmrepl_ioctl.h"
ccflags-y := -I$(PWD)/../include

.PHONY: all install uninstall clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install: all
	sudo insmod asmrepl.ko || true
	@echo "Module loaded. /dev/asmrepl should appear shortly."

uninstall:
	sudo rmmod asmrepl || true

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
