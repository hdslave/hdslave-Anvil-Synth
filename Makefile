# Anvil: a CLAP percussion synthesizer for the Akai MPC Sample (64-bit ARM Linux, Cortex-A55).
# Build inside a Linux arm64 image with ./build.sh, which runs `make all test`.
#   make            build/Anvil.clap
#   make test       the tests against the built plug-in (tests/host.cpp) and the sound's fingerprint
#                   (tests/fingerprint.cpp: the output to the bit, against tests/fingerprint.txt)
#   make bench      build/bench: renders a busy pattern; tools/bench.sh counts its instructions
#   make sanitize   the tests and the fingerprint's scenarios under AddressSanitizer and UBSan (build/asan)
CXX ?= g++
OUT := build
WARN := -Wall -Wextra -Werror
# The MPC's cores. -ffp-contract=fast lets the compiler fuse multiply-adds; nothing here relies on IEEE errno,
# traps or exceptions.
OPT := -O3 -mcpu=cortex-a55 -ffp-contract=fast -fno-math-errno -fno-trapping-math
CXXFLAGS := -std=c++17 $(OPT) $(WARN) -fno-exceptions -fno-rtti -fvisibility=hidden -fPIC -Ithird_party/clap/include -Isrc
# Only clap_entry is exported; libstdc++ and libgcc are linked in so the MPC needs nothing beyond libc and libm. -s
# leaves out the symbol table (the code is the same; the exported clap_entry stays).
LDFLAGS := -shared -s -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-z,now -Wl,-O1 -Wl,--version-script=src/exports.map

SOURCES := src/anvil.cpp
HEADERS := $(wildcard src/*.h src/ui/*.h)

.PHONY: all test bench clean fingerprint-update sanitize sp16 sp16-test
all: $(OUT)/Anvil.clap

$(OUT)/Anvil.clap: $(SOURCES) $(HEADERS) src/exports.map Makefile
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(OUT)/host: tests/host.cpp tests/wav.h tests/png.h src/melodee_screen.h Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include tests/host.cpp -ldl -o $@

$(OUT)/fingerprint: tests/fingerprint.cpp tests/fingerprint_scenarios.h Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include tests/fingerprint.cpp -ldl -o $@

test: $(OUT)/Anvil.clap $(OUT)/host $(OUT)/fingerprint
	$(OUT)/host $(OUT)/Anvil.clap $(OUT)
	$(OUT)/fingerprint $(OUT)/Anvil.clap tests/fingerprint.txt > /dev/null

# The sound's hashes after a change meant to change the sound (see tests/fingerprint.cpp).
fingerprint-update: $(OUT)/Anvil.clap $(OUT)/fingerprint
	$(OUT)/fingerprint $(OUT)/Anvil.clap > tests/fingerprint.txt

$(OUT)/bench: tests/bench.cpp $(SOURCES) $(HEADERS) Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 $(OPT) $(WARN) -g -Ithird_party/clap/include -Isrc tests/bench.cpp -o $@

bench: $(OUT)/bench

# The tests under AddressSanitizer and UBSan (build/asan): a read past a buffer, a leak or undefined behaviour stops
# them (abort_on_error, so a report is never taken for a plain exit 1). The fingerprint's hashes hold only for the
# release flags, so their mismatch (exit 1) is expected here; every scenario must still run clean — exit 1 is also
# what a load failure returns, so the scenario lines in its log are counted against tests/fingerprint.txt.
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g1
SAN_ENV := ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:abort_on_error=1
sanitize:
	@mkdir -p $(OUT)/asan
	$(CXX) $(subst -O3,-O1,$(CXXFLAGS)) $(SAN) $(SOURCES) -shared -Wl,--version-script=src/exports.map -o $(OUT)/asan/Anvil.clap
	$(CXX) -std=c++17 -O1 $(WARN) $(SAN) -Ithird_party/clap/include tests/host.cpp -ldl -o $(OUT)/asan/host
	$(CXX) -std=c++17 -O1 $(WARN) $(SAN) -Ithird_party/clap/include tests/fingerprint.cpp -ldl -o $(OUT)/asan/fingerprint
	$(SAN_ENV) $(OUT)/asan/host $(OUT)/asan/Anvil.clap $(OUT)/asan
	@$(SAN_ENV) $(OUT)/asan/fingerprint $(OUT)/asan/Anvil.clap tests/fingerprint.txt > $(OUT)/asan/fingerprint.log 2>&1; \
	    rc=$$?; if [ $$rc -gt 1 ]; then cat $(OUT)/asan/fingerprint.log; exit $$rc; fi; \
	    ran=$$(grep -Ec '^[a-z0-9-]+ [0-9a-f]{16}$$' $(OUT)/asan/fingerprint.log); want=$$(grep -c '' tests/fingerprint.txt); \
	    if [ "$$ran" -ne "$$want" ]; then echo "sanitize: $$ran of $$want fingerprint scenarios ran (a load failure also exits 1)"; cat $(OUT)/asan/fingerprint.log; exit 1; fi; \
	    echo "sanitize: every fingerprint scenario ran clean"

clean:
	rm -rf $(OUT)

# Device shared object: ARMv7-A Cortex-A9, softfp, glibc 2.13. Built in sp16-toolchain:gcc11-egl
# (arm-linux-gnueabi-g++ is not on the Mac). Output build/Anvil-sp16.clap.
SP16_CXX ?= arm-linux-gnueabi-g++
SP16_OPT := -O3 -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -ffp-contract=fast -fno-math-errno -fno-trapping-math
SP16_CXXFLAGS := -std=c++17 $(SP16_OPT) $(WARN) -fno-exceptions -fno-rtti -fvisibility=hidden -fPIC -Ithird_party/clap/include -Isrc

sp16: $(OUT)/Anvil-sp16.clap

$(OUT)/Anvil-sp16.clap: $(SOURCES) $(HEADERS) src/exports.map Makefile
	@mkdir -p $(OUT)
	$(SP16_CXX) $(SP16_CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $@

# The host tests on the device object, under qemu-arm as a Cortex-A9 with the toolchain's glibc 2.13 sysroot, also in
# sp16-toolchain:gcc11-egl. The test program links libstdc++ statically: the sysroot's is older than GCC 11's.
# Pictures go to build/sp16/.
$(OUT)/sp16/host: tests/host.cpp tests/wav.h tests/png.h src/melodee_screen.h src/sixteen_screen.h Makefile
	@mkdir -p $(OUT)/sp16
	$(SP16_CXX) -std=c++17 -O2 $(WARN) -Wno-psabi -Ithird_party/clap/include tests/host.cpp -static-libstdc++ -static-libgcc -ldl -o $@

sp16-test: $(OUT)/Anvil-sp16.clap $(OUT)/sp16/host
	sp16-check-glibc $(OUT)/Anvil-sp16.clap
	qemu-arm -cpu cortex-a9 -L /opt/sp16/sysroot $(OUT)/sp16/host $(OUT)/Anvil-sp16.clap $(OUT)/sp16
