# Simple cross-arch CPU benchmark
# Usage: make && ./cpu-bench --help

CC ?= cc
# Assume GCC 13+ with C2x support
CFLAGS ?= -O3 -pipe -std=c2x -Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion -Wformat=2 -fno-strict-aliasing
# Avoid forcing -march=native by default for portability; allow user to add via CFLAGS
LDFLAGS ?=
LIBS := -pthread

# Optional tuning (set by user):
#   make MARCH=native MTUNE=native
#   make MARCH=x86-64-v3 MTUNE=generic
ifdef MARCH
	CFLAGS += -march=$(MARCH)
endif
ifdef MTUNE
	CFLAGS += -mtune=$(MTUNE)
endif

# Host arch detection to make 'native' robust across platforms (esp. riscv64)
HOST_ARCH := $(shell uname -m)
NATIVE_MARCH ?= native
NATIVE_MTUNE ?= native
ifeq ($(HOST_ARCH),riscv64)
	# On some RISC-V toolchains, -march=native produces an invalid ISA string.
	# Fall back to a broadly compatible baseline (G = IMAFD).
	NATIVE_MARCH := rv64gc
	NATIVE_MTUNE := generic
endif

TARGET := cpu-bench
SRCS := src/bench.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean native sg2000 riscv-v

all: $(TARGET)

# Convenience target for native tuning
native:
	$(MAKE) MARCH=$(NATIVE_MARCH) MTUNE=$(NATIVE_MTUNE) all

# RISC-V vector baseline (if your toolchain supports V)
riscv-v:
	$(MAKE) MARCH=rv64gcv_zicsr_zifencei MTUNE=generic all

# Sophgo SG2000: rv64imafdcv + privileged CSRs/fence split
sg2000:
	$(MAKE) MARCH=rv64imafdcv_zicsr_zifencei MTUNE=generic all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
