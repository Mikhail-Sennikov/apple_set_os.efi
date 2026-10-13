ARCH	= x86_64
GNU_EFI ?= /usr
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
CORE_SOURCES = src/apple_set_os.cpp src/efi/apple_os_info.cpp src/logging/logging.cpp
CORE_HEADERS = src/apple_set_os.h src/efi/apple_os_info.h src/logging/logging.h
DEBUG_SOURCES = src/debug/apple_os_info.cpp src/debug/boot_environment.cpp src/debug/gpu.cpp src/debug/loaded_image.cpp
DEBUG_HEADERS = $(DEBUG_SOURCES:.cpp=.h) src/debug/protocol_lookup.h
BUILD_INPUTS = Dockerfile Makefile src/main.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(DEBUG_SOURCES) $(DEBUG_HEADERS)
BUILD_COMMIT ?= $(shell git -c safe.directory="$(CURDIR)" rev-parse --short=12 HEAD 2>/dev/null || printf unknown)
BUILD_ID = $(BUILD_COMMIT)+$(shell sha256sum $(sort $(BUILD_INPUTS)) | sha256sum | cut -c1-16)
BUILD_CPPFLAGS = -DAPPLE_SET_OS_BUILD_ID='"$(BUILD_ID)"'
SOURCES = src/main.cpp $(CORE_SOURCES) $(DEBUG_SOURCES)
TARGET = apple_set_os.efi
FORMAT = efi-app-$(ARCH)
BUILD_DIR = build
OBJECTS = $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
SHARED = $(BUILD_DIR)/apple_set_os.so
TEST_TARGETS = build/tests/protocol_test build/tests/logging_test build/tests/debug_test

INC		= $(GNU_EFI)/include/efi
EFI_LIBS = $(GNU_EFI)/lib/libefi.a $(GNU_EFI)/lib/libgnuefi.a
CXXFLAGS	= -Isrc -I$(INC) -I$(INC)/$(ARCH) \
		 -DGNU_EFI_USE_MS_ABI -fPIC -fshort-wchar -ffreestanding \
		 -fno-stack-protector -maccumulate-outgoing-args \
		 -Wall -D$(ARCH) -Werror -m64 -mno-red-zone \
		 -std=c++23 -Os -flto -fvisibility=hidden \
		 -fno-exceptions -fno-rtti -fno-threadsafe-statics

LDFLAGS	= -nostdlib -shared \
		 -Wl,--no-undefined,-T,$(GNU_EFI)/lib/elf_$(ARCH)_efi.lds \
		 -Wl,-Bsymbolic,-znocombreloc,--exclude-libs,ALL

$(TARGET): $(SHARED) Makefile
	objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel \
		-j .rela -j .reloc -S --target=$(FORMAT) $< $@

$(SHARED): $(OBJECTS) Makefile
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(GNU_EFI)/lib/crt0-efi-$(ARCH).o $(OBJECTS) $(EFI_LIBS) \
		$(shell $(CXX) $(CXXFLAGS) -print-libgcc-file-name)

$(BUILD_DIR)/%.o: src/%.cpp Makefile
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BUILD_CPPFLAGS) -MMD -MP -c $< -o $@

# Keep the timestamp unchanged when the build ID is unchanged.
$(BUILD_DIR)/build-id: FORCE
	mkdir -p $(dir $@)
	@printf '%s\n' '$(BUILD_ID)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(BUILD_DIR)/debug/boot_environment.o: $(BUILD_INPUTS) $(BUILD_DIR)/build-id

all: $(TARGET)

build/tests/protocol_test: tests/protocol_test.cpp $(BUILD_INPUTS) $(BUILD_DIR)/build-id
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BUILD_CPPFLAGS) -o $@ \
		tests/protocol_test.cpp $(CORE_SOURCES) $(DEBUG_SOURCES) $(EFI_LIBS)

build/tests/logging_test: tests/logging_test.cpp src/logging/logging.cpp src/logging/logging.h Makefile
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/logging_test.cpp src/logging/logging.cpp $(EFI_LIBS)

build/tests/debug_test: tests/debug_test.cpp $(BUILD_INPUTS) $(BUILD_DIR)/build-id
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BUILD_CPPFLAGS) -o $@ \
		tests/debug_test.cpp $(CORE_SOURCES) $(DEBUG_SOURCES) $(EFI_LIBS)

test: $(TEST_TARGETS)
	build/tests/protocol_test
	build/tests/logging_test
	build/tests/debug_test

clean:
	rm -f $(TARGET) $(SHARED) $(OBJECTS) \
		$(OBJECTS:.o=.d) $(TEST_TARGETS)

-include $(OBJECTS:.o=.d)

FORCE:

.PHONY: all test clean FORCE
