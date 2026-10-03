#include "apple_set_os.h"

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
	return apple_set_os_run(image, systemTable);
}
