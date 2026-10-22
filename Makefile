# QemuHDAAudio.kext — build with any recent Xcode / Apple clang on macOS.
#
#   make deps      fetch MacKernelSDK + 10.6 IOAudioFamily headers into deps/
#   make           build build/QemuHDAAudio.kext (x86_64, Mac OS X 10.6+)
#   make zip       build/QemuHDAAudio-<version>.zip: kext + guest scripts (CI artifact)
#   make check     static checks on the linked binary (load commands, symbols)
#   make objects   compile only (also works on Linux with plain clang, as a
#                  quick syntax/header check — no Mach-O linker needed)
#   make clean
#
# No Xcode project: a plain clang invocation is easier to keep compatible
# with old kernels and easier to run in CI.

KEXT_NAME  := QemuHDAAudio
BUNDLE_ID  := org.qemuhda.driver.QemuHDAAudio
VERSION    ?= $(shell cat VERSION 2>/dev/null || echo 0.0.0)

DEPS       := deps
MKSDK      := $(DEPS)/MacKernelSDK
OVERLAY    := $(DEPS)/ioaudio-overlay
BUILD      := build
OBJDIR     := $(BUILD)/obj
KEXT       := $(BUILD)/$(KEXT_NAME).kext
BINARY     := $(KEXT)/Contents/MacOS/$(KEXT_NAME)

UNAME_S    := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CC         := $(shell xcrun -f clang)
CXX        := $(shell xcrun -f clang++)
else
CC         := clang
CXX        := clang++
endif

TARGET     := -target x86_64-apple-macos10.6

# Recent LLVM lowers static destructors on Mach-O to __cxa_atexit calls, which
# no kernel exports. Force them into __mod_term_func (run by libkmod's _stop)
# when the compiler knows the option; Apple clang may already do this for
# -fapple-kext, in which case the option may be absent and is skipped.
DTOR_FLAGS := $(shell $(CXX) -target x86_64-apple-macos10.6 -mllvm -disable-atexit-based-global-dtor-lowering \
                -x c++ -c /dev/null -o /dev/null >/dev/null 2>&1 && echo "-mllvm -disable-atexit-based-global-dtor-lowering")

DEFINES    := -DKERNEL -DAPPLE -DNeXT -DPRAGMA_MARK=0 \
              -DQHDA_BUNDLE_ID=$(BUNDLE_ID) \
              -DQHDA_VERSION_STRING=\"$(VERSION)\"

# Overlay first: the 10.6 IOAudioFamily headers must win over MacKernelSDK's.
INCLUDES   := -nostdinc -isystem $(OVERLAY) -isystem $(MKSDK)/Headers -Isrc

KFLAGS     := -mkernel -fno-builtin -fno-common \
              -fno-stack-protector -fno-exceptions -fno-use-cxa-atexit \
              -fno-asynchronous-unwind-tables -fno-unwind-tables -O2 \
              $(DTOR_FLAGS)

WARNINGS   := -Wall -Wno-unused-parameter -Wno-deprecated-declarations \
              -Wno-inconsistent-missing-override -Wno-overloaded-virtual \
              -Wno-unused-private-field -Wno-unknown-warning-option

CFLAGS     := $(TARGET) $(KFLAGS) $(DEFINES) $(INCLUDES) $(WARNINGS) -std=gnu11
CXXFLAGS   := $(TARGET) $(KFLAGS) $(DEFINES) $(INCLUDES) $(WARNINGS) \
              -std=gnu++11 -nostdinc++ -fno-rtti -fapple-kext

# cc_kext provides compiler builtins (e.g. 128-bit division) for kexts.
CC_KEXT    := $(shell $(CC) -print-file-name=lib/darwin/libclang_rt.cc_kext.a 2>/dev/null)
ifeq ($(wildcard $(CC_KEXT)),)
CC_KEXT    :=
endif

LDFLAGS    := $(TARGET) -nostdlib -Xlinker -kext \
              $(MKSDK)/Library/x86_64/libkmod.a $(CC_KEXT)

SRCS_CXX   := src/QemuHDAAudioDevice.cpp src/QemuHDAAudioEngine.cpp
SRCS_C     := src/kmod_info.c
OBJS       := $(SRCS_CXX:src/%.cpp=$(OBJDIR)/%.o) $(SRCS_C:src/%.c=$(OBJDIR)/%.o)
HEADERS    := $(wildcard src/*.h)

.PHONY: all deps objects kext zip check clean distclean

all: kext

deps:
	./scripts/fetch-deps.sh

$(MKSDK)/Headers $(OVERLAY):
	./scripts/fetch-deps.sh

$(OBJDIR)/%.o: src/%.cpp $(HEADERS) | $(MKSDK)/Headers $(OVERLAY)
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: src/%.c | $(MKSDK)/Headers $(OVERLAY)
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

objects: $(OBJS)

kext: $(BINARY) $(KEXT)/Contents/Info.plist

$(BINARY): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@
	python3 scripts/macho-compat.py $@

$(KEXT)/Contents/Info.plist: Resources/Info.plist.in VERSION
	@mkdir -p $(dir $@)
	sed -e 's/@EXECUTABLE@/$(KEXT_NAME)/g' \
	    -e 's/@BUNDLE_ID@/$(BUNDLE_ID)/g' \
	    -e 's/@VERSION@/$(VERSION)/g' $< > $@
	plutil -lint $@ 2>/dev/null || true

# Static checks on the linked binary (run in CI).
check: kext
	@echo "== file"; file $(BINARY)
	@echo "== load commands"; otool -l $(BINARY) | grep -E '^ *cmd ' | sort | uniq -c
	@echo "== segments/sections"; otool -l $(BINARY) | grep -E 'segname|sectname' | sort | uniq -c
	@echo "== undefined symbols (resolved by the kernel at load time)"; nm -um $(BINARY) | c++filt
	python3 scripts/macho-compat.py $(BINARY) --check
	@if nm -um $(BINARY) | grep -qE '___cxa_atexit|___dso_handle|___stack_chk|___chkstk'; then \
	  echo "error: binary references symbols no kernel exports"; exit 1; fi

# Distribution zip: kext + guest helper scripts in one folder.
DIST       := $(BUILD)/dist/$(KEXT_NAME)-$(VERSION)

zip: kext
	rm -rf $(BUILD)/dist && mkdir -p $(DIST)
	cp -R $(KEXT) $(DIST)/
	cp guest/install.sh guest/uninstall.sh guest/collect-logs.sh $(DIST)/
	mkdir -p $(DIST)/opencore
	cp guest/opencore/apply-manual-fsb.sh guest/opencore/oc-manual-fsb.py $(DIST)/opencore/
	chmod +x $(DIST)/*.sh $(DIST)/opencore/*
	cd $(BUILD)/dist && rm -f ../$(KEXT_NAME)-$(VERSION).zip && \
	  ditto -c -k --sequesterRsrc --keepParent $(KEXT_NAME)-$(VERSION) ../$(KEXT_NAME)-$(VERSION).zip
	@echo "-> $(BUILD)/$(KEXT_NAME)-$(VERSION).zip"

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(DEPS)
