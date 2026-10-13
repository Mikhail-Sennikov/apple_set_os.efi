#pragma once

extern "C" {
#include <efibind.h>
#include <efidef.h>
}

struct _EFI_SYSTEM_TABLE;
using EFI_SYSTEM_TABLE = _EFI_SYSTEM_TABLE;

namespace NDebug {

void LogLoadedImage(EFI_HANDLE image, EFI_SYSTEM_TABLE* systemTable);

} // namespace NDebug
