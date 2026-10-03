ARCH	= x86_64

TARGET	= apple_set_os.efi
FORMAT 	= efi-app-$(ARCH)
OBJECTS	= src/main.o src/apple_set_os.o

INC		= /usr/include/efi
CFLAGS	= -I$(INC) -I$(INC)/$(ARCH) \
		 -DGNU_EFI_USE_MS_ABI -fPIC -fshort-wchar -ffreestanding \
		 -fno-stack-protector -maccumulate-outgoing-args \
		 -Wall -D$(ARCH) -Werror -m64 -mno-red-zone

LDFLAGS	= -T /usr/lib/elf_$(ARCH)_efi.lds -Bsymbolic -shared -nostdlib -znocombreloc \
		  /usr/lib/crt0-efi-$(ARCH).o

%.efi: %.so
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel \
		-j .rela -j .reloc -S --target=$(FORMAT) $^ $@

%.so: $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^ $(shell $(CC) $(CFLAGS) -print-libgcc-file-name) \
	/usr/lib/libgnuefi.a

all: $(TARGET)

$(OBJECTS): src/apple_set_os.h

clean:
	rm -f $(TARGET) *.so $(OBJECTS)
