# cpcpub: the benchmark (bench/) and the results hub (web/).
#
#   make            build the benchmark          -> bench/cpcpub
#   make check      build it, then run the contract and hub tests
#   make testdata   regenerate testdata/full-run.json on this machine
#   make serve      run the hub on http://127.0.0.1:8080
#   make submit     build, measure, and upload the result to a hub
#
# Build tuning lives in bench/Makefile and passes straight through:
#   make native / make loongarch / make sg2000 / make MARCH=x86-64-v3
#
# The vectorization and FMA toggles are no longer build-time: one binary carries
# all four combinations. Pick one with `cpcpub --variant NAME`, or run them
# all with `cpcpub --variants`.

PYTHON ?= python3
# .exe when this tree was built for Windows. Deferred (`=`, not `:=`) so it is
# answered when a recipe runs rather than when this file is read -- the targets
# below build the binary first, and on a tree that has never been built there
# would be nothing to look at yet.
BENCH   = $(shell [ -x bench/cpcpub.exe ] && echo bench/cpcpub.exe \
                                          || echo bench/cpcpub)

.PHONY: all bench check contract test testdata serve submit clean \
        native riscv-v rva23 loongarch sg2000 sg2000-xthead

all: bench

bench:
	@$(MAKE) -C bench

# The tuning targets, forwarded so the top level is a complete entry point.
native riscv-v rva23 loongarch sg2000 sg2000-xthead:
	@$(MAKE) -C bench $@

# The two halves are only correct together: the benchmark's JSON is the hub's
# only input, and nothing else in the tree checks that they still agree. So
# `check` runs the contract test against the binary it just built, not against
# the committed sample alone.
check: contract test

contract: bench
	@CPCPUB_BIN=$(abspath $(BENCH)) $(PYTHON) tests/test_contract.py

test:
	@$(PYTHON) web/test_server.py

# A committed sample of real output: the fixture the contract test uses when no
# binary is built, and the reference for what the schema actually looks like.
# The numbers in it are whichever machine ran it, so regenerate deliberately --
# no build target depends on this.
testdata: bench
	@./$(BENCH) --full --time 0.05 --reps 1 --warmup 0.02 --json \
	    > testdata/full-run.json
	@echo "wrote testdata/full-run.json"

serve:
	@$(PYTHON) web/server.py

# Build, measure, upload -- the whole path to a row on the board in one command.
# The benchmark does the upload itself; HUB defaults to the local hub above,
# and TOKEN (or $CPCPUB_TOKEN) puts the run on your account:
#
#   make submit LABEL="workstation, quiet"
#   make submit HUB=http://my.server:8782 TOKEN=... LABEL="sbc"
HUB ?= http://127.0.0.1:8080
submit: bench
	@./$(BENCH) --full --submit '$(HUB)' \
	    $(if $(TOKEN),--token '$(TOKEN)') \
	    $(if $(LABEL),--label '$(LABEL)') $(if $(NOTES),--notes '$(NOTES)')

clean:
	@$(MAKE) -C bench clean
	@rm -rf web/__pycache__ tests/__pycache__
