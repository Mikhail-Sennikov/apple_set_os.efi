#pragma once

struct _EFI_SYSTEM_TABLE;
using EFI_SYSTEM_TABLE = _EFI_SYSTEM_TABLE;

namespace NDebug {

void LogGpuList(EFI_SYSTEM_TABLE* systemTable, const wchar_t* phase);

} // namespace NDebug
