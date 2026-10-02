#ifndef APPLE_SET_OS_H
#define APPLE_SET_OS_H

#include <efi.h>

#if !defined(__x86_64__) || !defined(HAVE_USE_MS_ABI)
#error Apple Set OS requires GNU-EFI Microsoft x64 ABI support
#endif

#define APPLE_SET_OS_VENDOR_TEXT  "Apple Inc."
#define APPLE_SET_OS_VERSION_TEXT "Mac OS X 10.9"

extern EFI_GUID APPLE_SET_OS_GUID;

typedef VOID (EFIAPI *APPLE_SET_OS_CALLBACK)(IN CHAR8 *value);

typedef struct {
	UINTN revision;
	APPLE_SET_OS_CALLBACK set_os_version;
	APPLE_SET_OS_CALLBACK set_os_vendor;
} APPLE_SET_OS_INTERFACE;

typedef enum {
	APPLE_SET_OS_BEFORE_VENDOR,
	APPLE_SET_OS_AFTER_VENDOR,
	APPLE_SET_OS_BEFORE_VERSION,
	APPLE_SET_OS_AFTER_VERSION
} APPLE_SET_OS_STAGE;

typedef EFI_STATUS (*APPLE_SET_OS_OBSERVER)(
	IN VOID *context,
	IN APPLE_SET_OS_STAGE stage
);

typedef struct {
	UINTN revision;
	BOOLEAN vendor_attempted;
	BOOLEAN version_attempted;
} APPLE_SET_OS_RESULT;

EFI_STATUS apple_set_os_locate(
	IN EFI_BOOT_SERVICES *boot_services,
	OUT APPLE_SET_OS_INTERFACE **protocol
);

/* Callback results are VOID; this status reports validation/observer errors. */
EFI_STATUS apple_set_os_apply(
	IN APPLE_SET_OS_INTERFACE *protocol,
	OUT APPLE_SET_OS_RESULT *result,
	IN APPLE_SET_OS_OBSERVER observer,
	IN VOID *observer_context
);

#endif
