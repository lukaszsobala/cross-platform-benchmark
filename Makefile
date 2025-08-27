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

TARGET := cpu-bench
SRCS := src/bench.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean native

all: $(TARGET)

# Convenience target for native tuning
native:
	$(MAKE) MARCH=native MTUNE=native all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
