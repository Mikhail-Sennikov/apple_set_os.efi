ARCH = x86_64
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

CC ?= gcc
LD ?= ld
OBJCOPY ?= objcopy
DEBUG ?= 0
GNU_EFI ?= /usr
GNU_EFI_INC ?= $(GNU_EFI)/include/efi
GNU_EFI_LIB ?= $(GNU_EFI)/lib

ifeq ($(DEBUG),0)
MODE := release
TARGET := apple_set_os.efi
FORMAT := efi-app-$(ARCH)
SOURCES := src/main.c src/apple_set_os.c
else ifeq ($(DEBUG),1)
MODE := debug
TARGET := apple_set_os_driver_diag.efi
FORMAT := efi-bsdrv-$(ARCH)
SOURCES := src/main.c src/apple_set_os.c src/debug/debug.c
else
$(error DEBUG must be 0 or 1)
endif

BUILD_DIR := build/$(MODE)
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
SHARED := $(BUILD_DIR)/apple_set_os.so
CONFIG := $(BUILD_DIR)/.config

# Keep the EFI ABI and platform requirements even when user flags are supplied.
override EFI_CPPFLAGS := -Isrc -I$(GNU_EFI_INC) -I$(GNU_EFI_INC)/$(ARCH) \
	-UGNU_EFI_USE_MS_ABI -DGNU_EFI_USE_MS_ABI -D$(ARCH) -UAPPLE_SET_OS_DEBUG
ifeq ($(DEBUG),1)
override EFI_CPPFLAGS += -DAPPLE_SET_OS_DEBUG
endif
override ABI_CFLAGS := -m64 -mabi=sysv -mno-red-zone -maccumulate-outgoing-args -fshort-wchar
override EFI_CFLAGS := $(ABI_CFLAGS) -fPIC -ffreestanding -fno-stack-protector \
	-Wall -Werror
EFI_CRT := $(GNU_EFI_LIB)/crt0-efi-$(ARCH).o
EFI_LDS := $(GNU_EFI_LIB)/elf_$(ARCH)_efi.lds
override EFI_LDFLAGS := -T $(EFI_LDS) -Bsymbolic -shared -nostdlib -znocombreloc
EFI_LIBS := $(GNU_EFI_LIB)/libefi.a $(GNU_EFI_LIB)/libgnuefi.a
LIBGCC := $(shell $(CC) $(CPPFLAGS) $(CFLAGS) $(EFI_CPPFLAGS) $(EFI_CFLAGS) \
	-print-libgcc-file-name)
EFI_SECTIONS := -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
	-j .rel -j .rela -j .reloc

TEST_DIR := build/tests
TEST_CONFIG := $(TEST_DIR)/.config
TEST_CFLAGS ?= -std=c11 -Wall -Wextra -Werror
TEST_LDFLAGS ?=
TEST_BINS := $(TEST_DIR)/protocol_test $(TEST_DIR)/debug_test
TEST_OBJECTS := $(TEST_DIR)/protocol_test.o $(TEST_DIR)/debug_test.o \
	$(TEST_DIR)/apple_set_os.o

# Update the stamp only when its contents change, so normal builds stay incremental.
shell_quote = '$(subst ','"'"',$(1))'
CONFIG_VARS := ARCH DEBUG GNU_EFI GNU_EFI_INC GNU_EFI_LIB CC LD OBJCOPY \
	CPPFLAGS CFLAGS LDFLAGS LDLIBS EFI_CPPFLAGS EFI_CFLAGS EFI_LDFLAGS \
	EFI_CRT EFI_LDS EFI_LIBS LIBGCC EFI_SECTIONS FORMAT
TEST_CONFIG_VARS := ARCH GNU_EFI GNU_EFI_INC GNU_EFI_LIB CC CPPFLAGS CFLAGS \
	TEST_CFLAGS TEST_LDFLAGS LDLIBS EFI_CPPFLAGS ABI_CFLAGS EFI_LIBS

$(CONFIG): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' $(foreach name,$(CONFIG_VARS),$(call shell_quote,$(name)=$($(name)))) > $@.tmp
	@cmp -s $@.tmp $@ || mv -f $@.tmp $@
	@rm -f $@.tmp

$(TEST_CONFIG): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' $(foreach name,$(TEST_CONFIG_VARS),$(call shell_quote,$(name)=$($(name)))) > $@.tmp
	@cmp -s $@.tmp $@ || mv -f $@.tmp $@
	@rm -f $@.tmp

$(BUILD_DIR)/%.o: src/%.c $(CONFIG)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EFI_CPPFLAGS) $(EFI_CFLAGS) \
		-MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(SHARED): $(OBJECTS) $(CONFIG) $(EFI_CRT) $(EFI_LDS) $(EFI_LIBS) $(LIBGCC)
	$(LD) $(LDFLAGS) $(EFI_LDFLAGS) -o $@ $(EFI_CRT) $(OBJECTS) \
		$(LDLIBS) $(EFI_LIBS) $(LIBGCC)

$(TARGET): $(SHARED) $(CONFIG)
	$(OBJCOPY) $(EFI_SECTIONS) -S --target=$(FORMAT) $< $@

$(TEST_DIR)/%.o: tests/%.c $(TEST_CONFIG)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(EFI_CPPFLAGS) $(ABI_CFLAGS) \
		-MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(TEST_DIR)/apple_set_os.o: src/apple_set_os.c $(TEST_CONFIG)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(EFI_CPPFLAGS) $(ABI_CFLAGS) \
		-MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(TEST_BINS): %: %.o $(TEST_DIR)/apple_set_os.o $(TEST_CONFIG) $(EFI_LIBS)
	$(CC) $(CFLAGS) $(TEST_CFLAGS) $(TEST_LDFLAGS) $(ABI_CFLAGS) -o $@ \
		$@.o $(TEST_DIR)/apple_set_os.o $(LDLIBS) $(EFI_LIBS)

all: $(TARGET)

diag:
	$(MAKE) DEBUG=1 all

test: $(TEST_BINS)
	$(TEST_DIR)/protocol_test
	$(TEST_DIR)/debug_test

clean:
	rm -rf build
	rm -f apple_set_os.efi apple_set_os_driver_diag.efi *.so *.o \
		tests/*.obj tests/protocol_test tests/protocol_test.exe

FORCE:

-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

.PHONY: all diag test clean FORCE
