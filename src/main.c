// Copyright (c) 2015 Bruno Bierbaumer

#include "apple_set_os.h"

#ifdef APPLE_SET_OS_DEBUG
#include "debug/debug.h"
#else
static VOID
console_write(EFI_SYSTEM_TABLE *system_table, CHAR16 *text)
{
	if (system_table->ConOut != NULL)
		system_table->ConOut->OutputString(system_table->ConOut, text);
}
#endif

/* GNU-EFI crt0 invokes efi_main using the native C calling convention. */
EFI_STATUS
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table)
{
	APPLE_SET_OS_INTERFACE *protocol = NULL;
	APPLE_SET_OS_RESULT result = { 0 };
	APPLE_SET_OS_OBSERVER observer = NULL;
	EFI_STATUS locate_status;
	EFI_STATUS status;

#ifdef APPLE_SET_OS_DEBUG
	status = apple_set_os_debug_begin(image, system_table);
	if (status != EFI_SUCCESS)
		return status;
	observer = apple_set_os_debug_observer;
#else
	(void)image;
	console_write(system_table, L"apple_set_os started\r\n");
#endif

	locate_status = apple_set_os_locate(system_table->BootServices, &protocol);
#ifdef APPLE_SET_OS_DEBUG
	status = apple_set_os_debug_before_apply(locate_status, protocol);
	if (status != EFI_SUCCESS)
		return apple_set_os_debug_finish(status, &result);
#else
	if (locate_status != EFI_SUCCESS) {
		console_write(system_table,
			L"Could not locate the apple set os protocol.\r\n");
		return locate_status;
	}
#endif

	status = EFI_SUCCESS;
	if (locate_status == EFI_SUCCESS)
		status = apple_set_os_apply(protocol, &result, observer, NULL);

#ifdef APPLE_SET_OS_DEBUG
	return apple_set_os_debug_finish(status, &result);
#else
	if (status != EFI_SUCCESS)
		return status;

	if (result.revision >= 2) {
		console_write(system_table, result.vendor_attempted ?
			L"Called OS vendor callback with " APPLE_SET_OS_VENDOR_TEXT L".\r\n" :
			L"Vendor callback is unavailable.\r\n");
	}
	if (result.revision > 0) {
		console_write(system_table, result.version_attempted ?
			L"Called OS version callback with " APPLE_SET_OS_VERSION_TEXT L".\r\n" :
			L"Version callback is unavailable.\r\n");
	}
	return EFI_SUCCESS;
#endif
}
