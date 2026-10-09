ARCH	= x86_64

TARGET	= apple_set_os.efi
FORMAT 	= efi-app-$(ARCH)
OBJECTS	= src/main.o src/apple_set_os.o src/efi/apple_os_info.o src/util/console_out.o
TEST_TARGETS = build/tests/protocol_test build/tests/console_out_test

INC		= /usr/include/efi
CXXFLAGS	= -Isrc -I$(INC) -I$(INC)/$(ARCH) \
		 -DGNU_EFI_USE_MS_ABI -fPIC -fshort-wchar -ffreestanding \
		 -fno-stack-protector -maccumulate-outgoing-args \
		 -Wall -D$(ARCH) -Werror -m64 -mno-red-zone \
		 -std=c++23 -fno-exceptions -fno-rtti

LDFLAGS	= --no-undefined -T /usr/lib/elf_$(ARCH)_efi.lds -Bsymbolic -shared -nostdlib -znocombreloc \
		  /usr/lib/crt0-efi-$(ARCH).o

%.efi: %.so
	objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel \
		-j .rela -j .reloc -S --target=$(FORMAT) $^ $@

%.so: $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^ /usr/lib/libefi.a /usr/lib/libgnuefi.a \
		$(shell $(CXX) $(CXXFLAGS) -print-libgcc-file-name)

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

all: $(TARGET)

src/main.o src/apple_set_os.o: src/apple_set_os.h
src/apple_set_os.o src/efi/apple_os_info.o: src/efi/apple_os_info.h
src/apple_set_os.o src/util/console_out.o: src/util/console_out.h

build/tests/protocol_test: tests/protocol_test.cpp src/apple_set_os.cpp src/apple_set_os.h src/efi/apple_os_info.cpp src/efi/apple_os_info.h src/util/console_out.cpp src/util/console_out.h Makefile
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/protocol_test.cpp src/apple_set_os.cpp src/efi/apple_os_info.cpp src/util/console_out.cpp

build/tests/console_out_test: tests/console_out_test.cpp src/util/console_out.cpp src/util/console_out.h Makefile
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/console_out_test.cpp src/util/console_out.cpp

test: $(TEST_TARGETS)
	build/tests/protocol_test
	build/tests/console_out_test

clean:
	rm -f $(TARGET) *.so $(OBJECTS) $(TEST_TARGETS)

.PHONY: all test clean
