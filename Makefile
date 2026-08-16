# cpu-bench: the benchmark (bench/) and the results hub (web/).
#
#   make            build the benchmark          -> bench/cpu-bench
#   make check      build it, then run the contract and hub tests
#   make testdata   regenerate testdata/full-run.json on this machine
#   make serve      run the hub on http://127.0.0.1:8080
#   make submit     build, measure, and upload the result to a hub
#
# Build tuning lives in bench/Makefile and passes straight through:
#   make native / make sg2000 / make MARCH=x86-64-v3
#
# The vectorization and FMA toggles are no longer build-time: one binary carries
# all four combinations. Pick one with `cpu-bench --variant NAME`, or run them
# all with `cpu-bench --variants`.

PYTHON ?= python3
BENCH  := bench/cpu-bench

.PHONY: all bench check contract test testdata serve submit clean \
        native riscv-v sg2000 sg2000-xthead

all: bench

bench:
	@$(MAKE) -C bench

# The tuning targets, forwarded so the top level is a complete entry point.
native riscv-v sg2000 sg2000-xthead:
	@$(MAKE) -C bench $@

# The two halves are only correct together: the benchmark's JSON is the hub's
# only input, and nothing else in the tree checks that they still agree. So
# `check` runs the contract test against the binary it just built, not against
# the committed sample alone.
check: contract test

contract: bench
	@CPU_BENCH_BIN=$(abspath $(BENCH)) $(PYTHON) tests/test_contract.py

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
# Where it goes and who it is from come from the hub config written by
# `web/submit.sh --save`; HUB, TOKEN and LABEL override that for one run:
#
#   make submit LABEL="workstation, quiet"
#   make submit HUB=https://hub.example TOKEN=... LABEL="sbc"
submit: bench
	@./$(BENCH) --full --json | web/submit.sh \
	    $(if $(HUB),-u '$(HUB)') $(if $(TOKEN),-t '$(TOKEN)') \
	    $(if $(LABEL),-l '$(LABEL)') $(if $(NOTES),-n '$(NOTES)')

clean:
	@$(MAKE) -C bench clean
	@rm -rf web/__pycache__ tests/__pycache__
