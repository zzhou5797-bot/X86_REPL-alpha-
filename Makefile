# Makefile — asmrepl userspace build
#
# Targets:
#   make           build asmrepl binary
#   make kmod      build kernel module (requires kernel headers)
#   make clean     remove build artifacts
#   make install   install asmrepl to /usr/local/bin (requires root)

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic \
            -O2 -g \
            -I. \
            -D_GNU_SOURCE
LDFLAGS := -ledit

TARGET  := asmrepl

SRCS    := asmrepl_main.c \
           hw_regs.c      \
           ptrace_exec.c  \
           asm_encode.c

OBJS    := $(SRCS:.c=.o)

.PHONY: all kmod clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Object-level dependencies
asmrepl_main.o: asmrepl_main.c asm_encode.h ptrace_exec.h hw_regs.h include/asmrepl_ioctl.h
hw_regs.o:      hw_regs.c hw_regs.h include/asmrepl_ioctl.h
ptrace_exec.o:  ptrace_exec.c ptrace_exec.h hw_regs.h
asm_encode.o:   asm_encode.c asm_encode.h

kmod:
	$(MAKE) -C kmod all

clean:
	rm -f $(OBJS) $(TARGET)
	$(MAKE) -C kmod clean 2>/dev/null || true

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "Installed /usr/local/bin/$(TARGET)"
	@echo "To enable CR/DR/MSR: cd kmod && make install"
