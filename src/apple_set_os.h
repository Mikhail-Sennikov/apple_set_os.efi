#pragma once

extern "C" {
#include <efibind.h>
#include <efidef.h>
}

struct _EFI_SYSTEM_TABLE;
using EFI_SYSTEM_TABLE = _EFI_SYSTEM_TABLE;

EFI_STATUS apple_set_os_run(EFI_HANDLE image, EFI_SYSTEM_TABLE* systemTable);
