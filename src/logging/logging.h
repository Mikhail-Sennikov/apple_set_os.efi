#pragma once

struct _EFI_SYSTEM_TABLE;
using EFI_SYSTEM_TABLE = _EFI_SYSTEM_TABLE;

namespace NLogging {

void Log(EFI_SYSTEM_TABLE* systemTable, const wchar_t* format, ...);

} // namespace NLogging
