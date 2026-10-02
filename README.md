# apple_set_os.efi

Small EFI utility for keeping the Intel integrated GPU available when booting
non-Apple operating systems on affected MacBook Pro models.

This fork fixes the Apple OS Info protocol ABI and follows the call order used
by Linux: set the vendor for protocol revision 2 or later, then set the OS
version for any non-zero revision.

## Apple OS Info ABI

The protocol revision is a native-width `UINTN`. Its OS version and vendor
callbacks use `VOID EFIAPI`; they do not return `EFI_STATUS`.

Treating those callbacks as status-returning functions reads an undefined
value from the return register on x86-64. The original utility could then
mistake that value for an EFI error and return before making both calls. This
fork uses the correct callback types and never interprets a callback result.

Protocol references:

- [OpenCore OS Info protocol definition](https://github.com/acidanthera/OpenCorePkg/blob/master/Include/Apple/Protocol/OSInfo.h#L26-L60)
- [Original GRUB patch](https://lists.gnu.org/archive/html/grub-devel/2013-12/msg00442.html)

## Binaries

- `apple_set_os.efi` is the normal EFI application.
- `apple_set_os_driver_diag.efi` is an optional one-shot Clover-resident
  diagnostic driver.

Both binaries share the entry point and protocol implementation in `src/`.
The diagnostic build adds the module in `src/debug/` at compile time.

## Build

On Debian 12 or another distribution with GNU-EFI:

```sh
apt-get install build-essential gnu-efi
make test
make
```

Build the optional diagnostic driver with:

```sh
make DEBUG=1
```

`make diag` is an alias for this command. Release and diagnostic objects are
kept separately in `build/release/` and `build/debug/`.

If GNU-EFI is installed under another prefix, set `GNU_EFI`, for example:

```sh
make GNU_EFI=/usr/local
```

The include and library directories can also be set independently, for example
with `GNU_EFI_INC=/usr/include/efi GNU_EFI_LIB=/usr/lib64`.
Headers and build settings are tracked, so changing flags or GNU-EFI paths
rebuilds the affected binary. User `CFLAGS`, `CPPFLAGS`, and `LDFLAGS` are
supported while mandatory EFI ABI flags are retained.

`make test` checks the protocol using real GNU-EFI headers and Microsoft x64
callback ABI, and exercises diagnostic log ownership with mocked EFI services.

Build and test both binaries in Docker:

```sh
docker build -t apple-set-os .
docker run --rm -v "$(pwd):/build" apple-set-os
```

Generated binaries are unsigned.

## Usage

The original application can be chainloaded by GRUB, rEFInd, or another EFI
bootloader. A typical GRUB entry is:

```text
search --no-floppy --set=root --label EFI
chainloader (${root})/EFI/custom/apple_set_os.efi
boot
```

For the tested Clover setup, install exactly one copy as:

```text
\EFI\CLOVER\drivers\UEFI\apple_set_os.efi
```

Do not keep multiple loadable Apple Set OS images in that directory.

## Optional Clover diagnostics

`apple_set_os_driver_diag.efi` is a PE subsystem 11 boot-services driver. It
records the Apple protocol calls, PCI `00:02.0` before and after them, and the
point where Clover loads Windows Boot Manager.

For a diagnostic boot only, back up the normal file and install the diagnostic
driver under the same `apple_set_os.efi` name. Arm it with:

```text
\EFI\CLOVER\misc\apple-set-os-driver-diag.once
```

It creates a verified consumed marker before calling the protocol:

```text
\EFI\CLOVER\misc\apple-set-os-driver-diag.consumed
```

Without the armed marker, or when the consumed marker already exists, the
driver remains inert. Its UTF-16 log is written to:

```text
\EFI\CLOVER\misc\apple-set-os-driver-diag.log
```

Restore the normal binary after the test and remove the diagnostic markers.

## Credits

The original program was written for the MacBook Pro 11,3 and was based on the
work of Andreas Heider. See the upstream project at
[0xbb/apple_set_os.efi](https://github.com/0xbb/apple_set_os.efi).
