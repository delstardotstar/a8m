CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
CXX := $(CROSS)g++
OBJCOPY := $(CROSS)objcopy
BUILD := build
# make DEBUG=1: undefined-behaviour traps, stack protector, sample-read
# bounds checks and unmapped guard regions (A8_DEBUG), in build-debug/.
ifdef DEBUG
BUILD := build-debug
DEBUG_CFLAGS := -DA8_DEBUG -fsanitize=undefined -fsanitize-trap=all -fstack-protector-strong
endif

ARCH := -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -marm
CFLAGS := $(ARCH) -O2 -g -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -std=c11 \
	-I$(BUILD) -Iinclude -Imdkit/include -Ifatfs -Isrc -Itallent $(if $(PROFILE_VOICE),-DPROFILE_VOICE=$(PROFILE_VOICE)) \
	$(if $(PROFILE_CHECK),-DPROFILE_CHECK) $(DEBUG_CFLAGS) $(EXTRA)
# Vendored mdkit: upstream builds clean with clang; GCC adds two style warnings.
MDKIT_CFLAGS := $(ARCH) -O2 -g -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -std=c99 \
	-Wno-misleading-indentation -Wno-type-limits -Wno-unused-function -Iinclude -Imdkit/include
# Vendored MNMD voices (C++ behind mdkit_voice wrappers, fw/mnmd/UPSTREAM).
# -fwrapv: upstream builds its host checks so.
MNMD_CXXFLAGS := $(ARCH) -O2 -g -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -std=c++11 \
	-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fwrapv \
	-Imnmd/include -Imnmd/voices -Imdkit/include -Isrc -Iinclude $(DEBUG_CFLAGS)
LDFLAGS := -T src/link.ld -nostdlib -Wl,--no-warn-rwx-segments -Wl,--gc-sections
SRCS := $(sort $(wildcard src/*.c) src/font.c) $(wildcard src/*.S) $(wildcard tallent/*.c tallent/*.S)
MDKIT_SRCS := $(wildcard mdkit/src/*/*/*.c)
MDKIT_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(MDKIT_SRCS))
MNMD_SRCS := $(wildcard mnmd/src/*.cpp)
MNMD_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(MNMD_SRCS))
# Vendored FatFs, built with its own (relaxed) warnings.
FATFS_OBJS := $(BUILD)/fatfs/ff.o $(BUILD)/fatfs/ffunicode.o

# fw/VERSION holds the release version. A clean tree at tag v<VERSION> builds
# as that version; anything else is <VERSION>-dev+<hash>[.dirty].
SEMVER := $(shell cat VERSION)
HASH := $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)$(shell git diff --quiet HEAD 2>/dev/null; [ $$? = 1 ] && echo .dirty)
TAGGED := $(shell git tag --points-at HEAD 2>/dev/null | grep -qx 'v$(SEMVER)' && echo yes)
VERSION := $(if $(and $(TAGGED),$(if $(findstring .dirty,$(HASH)),,yes)),$(SEMVER),$(SEMVER)-dev+$(HASH))
RELEASE := $(if $(filter $(SEMVER),$(VERSION)),1,0)

.PHONY: all clean FORCE
all: $(BUILD)/app

src/font.c: tools/mkfont.py
	python3 -I tools/mkfont.py

# Rewritten only when the version changes, so it triggers a relink then.
$(BUILD)/version.h: VERSION FORCE
	@mkdir -p $(BUILD)
	@printf '#define FW_VERSION "%s"\n#define FW_SEMVER "%s"\n#define FW_HASH "%s"\n#define FW_RELEASE %s\n' '$(VERSION)' '$(SEMVER)' '$(HASH)' '$(RELEASE)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD)/mdkit/%.o: mdkit/%.c $(wildcard mdkit/include/*.h)
	@mkdir -p $(@D)
	@$(CC) $(MDKIT_CFLAGS) -I$(<D) -c $< -o $@

$(BUILD)/mnmd/%.o: mnmd/%.cpp $(wildcard mnmd/*/*.hpp mnmd/include/opxy/*.hpp mdkit/include/*.h) src/voices.h src/preset.h
	@mkdir -p $(@D)
	@$(CXX) $(MNMD_CXXFLAGS) -c $< -o $@

$(BUILD)/fatfs/%.o: fatfs/%.c fatfs/ffconf.h
	@mkdir -p $(@D)
	@$(CC) $(ARCH) -O2 -g -ffreestanding -fno-builtin -nostdlib -std=c99 -Iinclude -Ifatfs -c $< -o $@

# Always relinked: make 3.81 (macOS) has 1 s timestamps and can miss a
# version change made in the same second as the last link.
$(BUILD)/fw.elf: $(SRCS) $(MDKIT_OBJS) $(MNMD_OBJS) $(FATFS_OBJS) src/link.ld $(wildcard src/*.h tallent/*.h) $(BUILD)/version.h FORCE
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(MDKIT_OBJS) $(MNMD_OBJS) $(FATFS_OBJS) -lgcc

$(BUILD)/app: $(BUILD)/fw.elf tools/mkapp.py
	$(OBJCOPY) -O binary $< $(BUILD)/fw.bin
	python3 -I tools/mkapp.py $(BUILD)/fw.bin $@
	@echo "version $(VERSION)"

clean:
	rm -rf $(BUILD) src/font.c
