# Simple cross-arch CPU benchmark
# Usage: make && ./cpcpub --help

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
# -mcpu, because Apple's clang has no -march for arm64: the CPU family is the
# only tuning knob there. Kept separate from MARCH so the two never collide.
ifneq ($(strip $(MCPU)),)
	CFLAGS += -mcpu=$(MCPU)
endif

# Build variants. Auto-vectorization and FMA contraction are translation-unit
# flags -- no pragma switches them per function -- so a binary that can measure
# more than one of them has to compile the kernels more than once. src/kernels.c
# is compiled once per row below and all four objects are linked into the one
# binary; `./cpcpub --variants` then runs them in turn.
#
# The flags are named once, here, and reach the result document by being both
# put on the command line and passed in as -DKV_FLAGS, so a run cannot report a
# build it did not get. scalar-nofma is the cross-ISA fairness default.
VARIANTS := scalar-nofma vector-nofma scalar-fma vector-fma

# Clang and GCC do not mean the same thing by -f[no-]tree-vectorize, and the
# difference is the whole point of the scalar variants. GCC's switch covers both
# of its vectorizers. Clang's is an alias for -f[no-]vectorize, which is the
# *loop* vectorizer alone: its SLP vectorizer stays on at -O3 regardless, and a
# "scalar" build made with clang comes out with SIMD in it. So the SLP pass is
# named explicitly wherever the compiler will take that spelling, and the four
# variants then mean the same four things on every toolchain.
#
# This is not cosmetic on the platforms that have no choice of compiler: macOS
# and Windows-on-Arm are clang-only, and without this their scalar-nofma
# baseline -- the one cross-ISA comparisons are made against -- would be a
# partly vectorized build wearing the name of a scalar one.
KV_SLP_OFF := $(shell $(CC) -fno-slp-vectorize -E -x c /dev/null \
                  >/dev/null 2>&1 && echo -fno-slp-vectorize)
KV_SLP_ON  := $(if $(KV_SLP_OFF),-fslp-vectorize)

# $(strip) so a GCC build, where the two variables above are empty, reports the
# flags it always reported rather than the same string with a double space in
# it -- these end up in build.flags, which readers compare between documents.
KV_FLAGS_scalar-nofma := $(strip -fno-tree-vectorize $(KV_SLP_OFF) -ffp-contract=off)
KV_FLAGS_vector-nofma := $(strip -ftree-vectorize $(KV_SLP_ON) -ffp-contract=off)
KV_FLAGS_scalar-fma   := $(strip -fno-tree-vectorize $(KV_SLP_OFF) -ffp-contract=fast)
KV_FLAGS_vector-fma   := $(strip -ftree-vectorize $(KV_SLP_ON) -ffp-contract=fast)

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
HOST_OS   := $(shell uname -s)
NATIVE_MARCH ?= native
NATIVE_MTUNE ?= native
NATIVE_MCPU  ?=
ifeq ($(HOST_OS),Darwin)
	# Apple's clang rejects -march on arm64 outright and its -mcpu=native
	# support has come and gone between Xcode releases, so probe for it rather
	# than guess. Falling back to no flag is not a loss: with a deployment
	# target set, clang already tunes for the Apple core family it is on.
	NATIVE_MARCH :=
	NATIVE_MTUNE :=
	NATIVE_MCPU  := $(shell $(CC) -mcpu=native -E -x c /dev/null \
	                    >/dev/null 2>&1 && echo native)
endif
ifeq ($(HOST_ARCH),riscv64)
	# On some RISC-V toolchains, -march=native produces an invalid ISA string.
	# Fall back to a broadly compatible baseline (G = IMAFD).
	NATIVE_MARCH := rv64gc
	# Omit -mtune entirely unless the user provides one, as some toolchains
	# don't accept 'generic'.
	NATIVE_MTUNE :=
endif

# Windows, when the compiler says so. The host is no guide -- these builds are
# usually cross-compiles from Linux -- so the target triple decides. Two
# spellings of it, because the toolchains disagree: mingw-w64 gcc dumps
# x86_64-w64-mingw32, llvm-mingw's clang dumps x86_64-w64-windows-gnu. Matching
# only one would leave the other building a binary called `cpcpub` while the
# linker quietly wrote cpcpub.exe -- so make would relink on every invocation
# and `make clean` would remove nothing.
#
# PLAT_LDFLAGS rather than LDFLAGS because a `make LDFLAGS=...` on the command
# line cannot be appended to from inside a makefile, and -static is not optional
# here: without it the artifact wants libwinpthread-1.dll (and, depending on the
# toolchain, libgcc and libunwind) sitting next to it, and a published binary
# has to run on a machine with no toolchain on it.
TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
TARGET_WIN    := $(findstring mingw,$(TARGET_TRIPLE))$(findstring windows,$(TARGET_TRIPLE))
EXEEXT       :=
PLAT_LDFLAGS :=
ifneq (,$(TARGET_WIN))
	EXEEXT       := .exe
	PLAT_LDFLAGS := -static
	# --submit opens a socket, and on Windows sockets are a library rather
	# than part of libc. Still no dependency to install: ws2_32 is Windows.
	LIBS         += -lws2_32
endif

# Where `cpcpub --submit` uploads when given no URL. There is no public hub
# yet; `make HUB_URL=https://hub.example` bakes one in when there is.
#
# Deliberately not in CFLAGS: those are reported verbatim as build.flags in
# every result, and the hub a build was pointed at is a deployment detail, not
# something a reader comparing two documents needs to see. It is also the one
# entry there that would differ between builds that are otherwise identical.
HUB_DEFINE :=
ifneq ($(strip $(HUB_URL)),)
	HUB_DEFINE := -DCPCPUB_HUB_URL='"$(HUB_URL)"'
endif

# What `cpcpub --version` reports. A tree built by hand says "dev", because it
# is one: only a release build is stamped, by the workflow passing the tag it
# is building. Kept out of CFLAGS for the same reason HUB_DEFINE is -- it would
# otherwise appear in every result's build.flags, where it says nothing about
# comparability.
VERSION ?= dev
VERSION_DEFINE := -DCPCPUB_VERSION='"$(VERSION)"'

TARGET := cpcpub$(EXEEXT)
OBJS   := src/bench.o src/sha256.o src/http.o \
          $(addprefix src/kernels-,$(addsuffix .o,$(VARIANTS)))

.PHONY: all clean native sg2000 sg2000-xthead riscv-v rva23 loongarch FORCE

all: $(TARGET)

# Objects depend on the flags they were built with, not just on the source.
# Without this, `make` followed by `make sg2000` finds src/bench.o up to date
# and silently keeps the object built for the *previous* -march -- so an ISA
# switch appears to succeed while changing nothing, and a build that is
# supposed to have been re-targeted still runs (or crashes) as the old one.
FLAGSTAMP := .build-flags
# HUB_DEFINE and VERSION_DEFINE are named here even though neither is in
# CFLAGS: both change the binary, so both have to change the stamp, or
# `make HUB_URL=...` (or `make VERSION=...`) over a built tree would keep the
# object compiled without it.
BUILDID   := $(CC) $(CFLAGS) $(LDFLAGS) $(PLAT_LDFLAGS) $(LIBS) $(HUB_DEFINE) $(VERSION_DEFINE)

$(FLAGSTAMP): FORCE
	@[ -f $@ ] && [ "$$(cat $@)" = "$(BUILDID)" ] || printf '%s' "$(BUILDID)" > $@

FORCE:

# Convenience target for native tuning
native:
	$(MAKE) MARCH=$(NATIVE_MARCH) MTUNE=$(NATIVE_MTUNE) MCPU=$(NATIVE_MCPU) all

# LoongArch LA64 with the lp64d ABI. The base ISA rather than la464 or la664,
# so the binary is not tied to one core generation -- `make MARCH=la464` for a
# tuned build on hardware you know.
loongarch:
	$(MAKE) MARCH=loongarch64 all

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

# RVA23U64 -- the RISC-V profile Ubuntu 26.04 takes as its riscv64 baseline.
#
# A floor, not a tuning knob. Every extension below is *mandatory* in the
# profile, so the compiler emits V 1.0, Zba/Zbb/Zbs, Zcb, Zfa, Zicond and the
# rest freely, and the result is a binary that dies with SIGILL on anything
# older -- including every board the sg2000 targets above exist for. Build it
# for hardware that implements RVA23, not "just in case": rv64gc (the plain
# default build) is what runs everywhere.
#
# Two spellings, because toolchains disagree about profile names. LLVM 19+
# accepts `-march=rva23u64`; GCC 15 does not ("ISA string must begin with rv32
# or rv64") and needs the profile written out. The probe below prefers the name
# and falls back to the string, so one target covers both -- and the flags are
# recorded in the result either way, so a reader can see which was used.
#
# The explicit string is RVA23U64's mandatory set. Extensions with no codegen
# effect (Ziccif, Ziccrse, Za64rs and friends) are named anyway: -march is the
# only written statement of what this binary requires of a CPU, and a partial
# one invites a build that runs on less than the profile promises.
# Assembled from named groups rather than written as one 230-character word:
# a `\` continuation would not do it, since make joins those with a space and a
# space is what separates an ISA string from a syntax error.
RVA23_PROFILE := rva23u64
# No trailing comments on these: make keeps the spaces in front of a `#`, and
# they would end up inside the ISA string. RVA23_BASE is RV64GC plus vector 1.0.
RVA23_BASE     := rv64imafdcv
RVA23_CACHE    := _zic64b_zicbom_zicbop_zicboz_zicclsm_ziccamoa_ziccif_ziccrse
RVA23_COUNTERS := _zicntr_zihpm
RVA23_SCALAR   := _zicsr_zifencei_zicond_zimop_zcmop_zcb_zfa_zfhmin_zawrs_za64rs
RVA23_HINTS    := _zihintntl_zihintpause_zkt
RVA23_BITMANIP := _zba_zbb_zbs
RVA23_VECTOR   := _zvbb_zvfhmin_zvkt
RVA23_ISA := $(RVA23_BASE)$(RVA23_CACHE)$(RVA23_COUNTERS)$(RVA23_SCALAR)$(RVA23_HINTS)$(RVA23_BITMANIP)$(RVA23_VECTOR)

# Deferred (`=`, not `:=`) so the compiler is only probed when this target is
# the one being built -- every other build pays nothing for it.
RVA23_MARCH = $(shell $(CC) -march=$(RVA23_PROFILE) -E -x c /dev/null \
                  >/dev/null 2>&1 && echo $(RVA23_PROFILE) || echo $(RVA23_ISA))

rva23:
	$(MAKE) MARCH=$(RVA23_MARCH) all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(PLAT_LDFLAGS) -o $@ $(OBJS) $(LIBS)

# BUILD_CC/BUILD_FLAGS are baked in so a result carries the exact toolchain and
# flags it was produced with -- an -march or a -ffast-math nobody remembers
# passing is otherwise invisible in the numbers. These are the flags every
# variant shares; the per-variant pair is baked into each kernels object below
# and the two are concatenated at report time.
src/bench.o: src/bench.c src/platform.h src/kernels.h src/sha256.h src/http.h \
             $(FLAGSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(HUB_DEFINE) $(VERSION_DEFINE) -DBUILD_CC='"$(CC)"' \
	    -DBUILD_FLAGS='"$(CFLAGS)"' -c -o $@ $<

# Not measured code, so it gets the shared flags and none of the variant ones:
# what it hashes is the binary, and how fast it does that is nobody's result.
src/sha256.o: src/sha256.c src/sha256.h $(FLAGSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Likewise: the upload happens after the last measurement is over.
src/http.o: src/http.c src/http.h $(FLAGSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# One object per variant, from the one source. KV_ID suffixes every symbol so
# the four copies coexist and a disassembly says which is which.
src/kernels-%.o: src/kernels.c src/kernels.h $(FLAGSTAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(KV_FLAGS_$*) -DKV_ID=_$(subst -,_,$*) -DKV_NAME='"$*"' \
	    -DKV_FLAGS='"$(KV_FLAGS_$*)"' -DKV_VECTORIZE=$(KV_VEC_$*) \
	    -DKV_FMA=$(KV_FMA_$*) -c -o $@ $<

# Both spellings of the binary, not just this build's: a tree that has been
# cross-built for Windows and then built natively holds cpcpub and cpcpub.exe
# side by side, and a clean that left one of them behind would be a clean in
# name only.
clean:
	rm -f cpcpub cpcpub.exe $(OBJS) $(FLAGSTAMP)
