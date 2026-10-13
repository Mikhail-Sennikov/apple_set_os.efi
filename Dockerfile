FROM debian:trixie-slim AS toolchain

WORKDIR /build

RUN apt-get update && apt-get install -y \
  build-essential \
  git

FROM toolchain AS gnu-efi-build

WORKDIR /tmp/gnu-efi-source

# Keep GNU-EFI's ABI flags; only change optimization and enable GCC LTO.
RUN chown _apt:root /tmp/gnu-efi-source && \
  sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources && \
  apt-get update && \
  apt-get source --only-source --download-only gnu-efi && \
  dpkg-source --extract gnu-efi_*.dsc /tmp/gnu-efi && \
  GNU_EFI_CFLAGS="-Wno-error=pragmas -mno-red-zone -mno-avx -fPIE -g -Wall -Wextra -Wno-pointer-sign -Werror \
    -funsigned-char -fshort-wchar -fno-strict-aliasing -ffreestanding -fno-stack-protector \
    -fno-stack-check -fno-merge-all-constants -Os -flto -ffat-lto-objects" && \
  make -C /tmp/gnu-efi ARCH=x86_64 PREFIX=/opt/gnu-efi AR=gcc-ar RANLIB=gcc-ranlib \
    CFLAGS="$GNU_EFI_CFLAGS" lib gnuefi && \
  make -C /tmp/gnu-efi ARCH=x86_64 PREFIX=/opt/gnu-efi AR=gcc-ar RANLIB=gcc-ranlib \
    CFLAGS="$GNU_EFI_CFLAGS" SUBDIRS="lib gnuefi inc" install

FROM toolchain

COPY --from=gnu-efi-build /opt/gnu-efi /opt/gnu-efi
ENV GNU_EFI=/opt/gnu-efi

CMD make clean && make
