#ifndef APPLE_SET_OS_DEBUG_H
#define APPLE_SET_OS_DEBUG_H

#include "../apple_set_os.h"

EFI_STATUS apple_set_os_debug_begin(
	IN EFI_HANDLE image,
	IN EFI_SYSTEM_TABLE *system_table
);

EFI_STATUS apple_set_os_debug_before_apply(
	IN EFI_STATUS locate_status,
	IN APPLE_SET_OS_INTERFACE *protocol
);

EFI_STATUS apple_set_os_debug_observer(
	IN VOID *context,
	IN APPLE_SET_OS_STAGE stage
);

/* Finish also releases the log and notification after a pre-setter failure. */
EFI_STATUS apple_set_os_debug_finish(
	IN EFI_STATUS apply_status,
	IN CONST APPLE_SET_OS_RESULT *result
);

#endif
