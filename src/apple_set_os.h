#ifndef APPLE_SET_OS_H
#define APPLE_SET_OS_H

extern "C" {
#include <efibind.h>
#include <efidef.h>
}

typedef struct _EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;

EFI_STATUS apple_set_os_run(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable);

#endif
