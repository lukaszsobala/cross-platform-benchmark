# Simple cross-arch CPU benchmark
# Usage: make && ./cpu-bench --help

CC ?= cc
# Assume GCC 13+ with C2x support
CFLAGS ?= -O3 -pipe -std=c2x -Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion -Wformat=2 -fno-strict-aliasing
# Avoid forcing -march=native by default for portability; allow user to add via CFLAGS
LDFLAGS ?=
LIBS := -pthread -lm

# Optional tuning (set by user):
#   make MARCH=native MTUNE=native
#   make MARCH=x86-64-v3 MTUNE=generic
ifneq ($(strip $(MARCH)),)
	CFLAGS += -march=$(MARCH)
endif
ifneq ($(strip $(MTUNE)),)
	CFLAGS += -mtune=$(MTUNE)
endif

# Build variants. Auto-vectorization and FMA contraction are translation-unit
# flags -- no pragma switches them per function -- so a binary that can measure
# more than one of them has to compile the kernels more than once. src/kernels.c
# is compiled once per row below and all four objects are linked into the one
# binary; `./cpu-bench --variants` then runs them in turn.
#
# The flags are named once, here, and reach the result document by being both
# put on the command line and passed in as -DKV_FLAGS, so a run cannot report a
# build it did not get. scalar-nofma is the cross-ISA fairness default.
VARIANTS := scalar-nofma vector-nofma scalar-fma vector-fma

KV_FLAGS_scalar-nofma := -fno-tree-vectorize -ffp-contract=off
KV_FLAGS_vector-nofma := -ftree-vectorize -ffp-contract=off
KV_FLAGS_scalar-fma   := -fno-tree-vectorize -ffp-contract=fast
KV_FLAGS_vector-fma   := -ftree-vectorize -ffp-contract=fast

KV_VEC_scalar-nofma := 0
KV_VEC_vector-nofma := 1
KV_VEC_scalar-fma   := 0
KV_VEC_vector-fma   := 1

KV_FMA_scalar-nofma := 0
KV_FMA_vector-nofma := 0
KV_FMA_scalar-fma   := 1
KV_FMA_vector-fma   := 1

# Host arch detection to make 'native' robust across platforms (esp. riscv64)
HOST_ARCH := $(shell uname -m)
NATIVE_MARCH ?= native
NATIVE_MTUNE ?= native
ifeq ($(HOST_ARCH),riscv64)
	# On some RISC-V toolchains, -march=native produces an invalid ISA string.
	# Fall back to a broadly compatible baseline (G = IMAFD).
	NATIVE_MARCH := rv64gc
	# Omit -mtune entirely unless the user provides one, as some toolchains
	# don't accept 'generic'.
	NATIVE_MTUNE :=
endif

TARGET := cpu-bench
OBJS   := src/bench.o $(addprefix src/kernels-,$(addsuffix .o,$(VARIANTS)))

.PHONY: all clean native sg2000 sg2000-xthead riscv-v FORCE

all: $(TARGET)

# Objects depend on the flags they were built with, not just on the source.
# Without this, `make` followed by `make sg2000` finds src/bench.o up to date
# and silently keeps the object built for the *previous* -march -- so an ISA
# switch appears to succeed while changing nothing, and a build that is
# supposed to have been re-targeted still runs (or crashes) as the old one.
FLAGSTAMP := .build-flags
BUILDID   := $(CC) $(CFLAGS) $(LDFLAGS) $(LIBS)

$(FLAGSTAMP): FORCE
	@[ -f $@ ] && [ "$$(cat $@)" = "$(BUILDID)" ] || printf '%s' "$(BUILDID)" > $@

FORCE:

# Convenience target for native tuning
native:
	$(MAKE) MARCH=$(NATIVE_MARCH) MTUNE=$(NATIVE_MTUNE) all

# RISC-V *ratified* vector (RVV 1.0). Only for hardware that implements 1.0 --
# see the sg2000 note below before assuming a 'V' in a datasheet means this.
riscv-v:
	$(MAKE) MARCH=rv64gcv_zicsr_zifencei all

# Sophgo SG2000 (T-Head C906): rv64imafdc + privileged CSRs/fence split.
#
# Deliberately NO 'v'. The C906 implements the *draft* 0.7.1 vector extension,
# which is not RVV 1.0 and is not binary compatible with it. GCC's 'v' means
# RVV 1.0, so -march=rv64imafdcv builds a binary that dies with SIGILL
# ("Illegal instruction") on this chip -- and it does so even with
# -fno-tree-vectorize, because the compiler also uses vector registers to
# inline memset/memcpy and struct copies. It buys nothing here anyway: the
# default build is deliberately scalar (VECTORIZE=0) for cross-ISA fairness.
sg2000:
	$(MAKE) MARCH=rv64imafdc_zicsr_zifencei all

# The 0.7.1 vector extension as T-Head hardware actually implements it. Needs
# GCC 14+ (or a T-Head toolchain) and is only worth building with VECTORIZE=1.
sg2000-xthead:
	$(MAKE) MARCH=rv64imafdc_zicsr_zifencei_xtheadvector all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

# BUILD_CC/BUILD_FLAGS are baked in so a result carries the exact toolchain and
# flags it was produced with -- an -march or a -ffast-math nobody remembers
# passing is otherwise invisible in the numbers. These are the flags every
# variant shares; the per-variant pair is baked into each kernels object below
# and the two are concatenated at report time.
src/bench.o: src/bench.c src/kernels.h $(FLAGSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DBUILD_CC='"$(CC)"' -DBUILD_FLAGS='"$(CFLAGS)"' -c -o $@ $<

# One object per variant, from the one source. KV_ID suffixes every symbol so
# the four copies coexist and a disassembly says which is which.
src/kernels-%.o: src/kernels.c src/kernels.h $(FLAGSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(KV_FLAGS_$*) -DKV_ID=_$(subst -,_,$*) -DKV_NAME='"$*"' \
	    -DKV_FLAGS='"$(KV_FLAGS_$*)"' -DKV_VECTORIZE=$(KV_VEC_$*) \
	    -DKV_FMA=$(KV_FMA_$*) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS) $(FLAGSTAMP)
