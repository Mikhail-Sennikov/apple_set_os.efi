# apple_set_os.efi

Tiny EFI utility for making the Intel integrated GPU available when booting
Linux or Windows on affected Intel-based Macs.

Apple firmware can hide the Intel GPU when booting a non-macOS system.
The utility identifies the OS as macOS through the Apple OS Info protocol.

This fork has been tested on `MacBookPro16,4` with Intel UHD Graphics 630,
using Clover to run the utility automatically before Windows.

## Credits

Thanks to Bruno Bierbaumer for the original
[apple_set_os.efi](https://github.com/0xbb/apple_set_os.efi) project on which this fork is based.
The original project targeted `MacBookPro11,3`.

Thanks to [Andreas Heider](https://github.com/ah-) for discovering
[the underlying hack](https://lists.gnu.org/archive/html/grub-devel/2013-12/msg00442.html).

## Usage

### Clover

Build the [EFI application](#build), then copy `apple_set_os.efi` to
`\EFI\CLOVER\drivers\UEFI\apple_set_os.efi` on the EFI System Partition that
contains Clover (not necessarily the Windows ESP).

Boot through Clover and select Windows normally. Clover runs the utility from
the drivers directory during startup, before launching Windows; do not add it
as a chainloader menu entry. Load only one copy of Apple Set OS.

### rEFInd and GRUB (EFI application)

Copy the apple_set_os.efi binary (download it from [releases](https://github.com/Mikhail-Sennikov/apple_set_os.efi/releases)) to EFI System Partition (ESP) :
```
mkdir /boot/efi/EFI/custom
cp apple_set_os.efi /boot/efi/EFI/custom
```
[rEFInd](http://www.rodsbooks.com/refind/) should automatically show a new icon for apple_set_os.efi.

Grub can be configured to start apple_set_os.efi automatically by adding the following lines to  ``/etc/grub.d/40_custom``:
```
search --no-floppy --set=root --label EFI
chainloader (${root})/EFI/custom/apple_set_os.efi
boot
```

### Optional file log

Console logging is always enabled. To also append the log to a file, set the
UEFI variable `AppleSetOsLogPath` under GUID
`80f05b32-6ba8-4c7e-ba1b-402357ff91d8`. Its value is an absolute EFI path encoded
as NUL-terminated UTF-16LE, with attributes
`NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS` (`7`).

The path refers to the filesystem from which `apple_set_os.efi` was loaded, not
a Windows drive letter. For example, `\EFI\apple-set-os.log` uses the same ESP
as the application, regardless of the bootloader.

On Windows, from the repository root (approve the administrator prompt):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Set-LogPath.ps1 -Path '\EFI\apple-set-os.log'
```

To disable file logging, delete only this variable:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Set-LogPath.ps1 -Disable
```

The variable is read and the file is opened lazily on the first nonempty log
message. The file is UTF-16LE with a BOM and is appended across boots. Parent
directories must already exist. Existing nonempty files without a UTF-16LE BOM
are left unchanged. Writes are flushed before execution continues, and the file is closed
before the application returns. If the variable is absent, there are no file
operations or additional formatting allocations. Configuration or file errors
leave console logging and the Apple OS Info operation intact.

## Build

The build targets x86_64 UEFI and requires GCC with C++23/LTO support and GNU-EFI
3.0.18 or newer. The logger uses the image handle saved by GNU-EFI's standard entry.
`make` produces one `apple_set_os.efi` (EFI application) in the repository root.

### Debian (x86_64)

```sh
sudo apt-get update
sudo apt-get install -y build-essential git gnu-efi
git clone https://github.com/Mikhail-Sennikov/apple_set_os.efi.git
cd apple_set_os.efi
make clean
make test
make
```

### Docker

The Dockerfile uses Debian Trixie and builds the latest available GNU-EFI source
package from its configured repositories with `-Os` and LTO. APT verifies the
downloads, and Debian patches are applied normally. The normal GNU-EFI formatter,
startup and linker script are retained. A package-only Debian build also works,
but its prebuilt GNU-EFI library limits the size reduction.

From the repository root, with Docker running Linux containers (the commands
work in Bash and PowerShell):

```sh
docker build -t apple_set_os .
docker run --rm --mount type=bind,source=.,target=/build apple_set_os sh -c "make clean && make test && make"
```

To refresh the GNU-EFI source package instead of reusing Docker's cached build:

```sh
docker build --no-cache-filter gnu-efi-build -t apple_set_os .
```

The repository is [bind-mounted](https://docs.docker.com/engine/storage/bind-mounts/)
at `/build`, so the output remains on the host.
