#pragma once

extern "C" {
#include <efibind.h>
#include <efidef.h>
}
#include <stdarg.h>

struct _EFI_FILE_HANDLE;
struct _EFI_SYSTEM_TABLE;
using EFI_SYSTEM_TABLE = _EFI_SYSTEM_TABLE;

namespace NLogging::NPrivate {

class TFileLog {
public:
	TFileLog() = default;
	TFileLog(const TFileLog&) = delete;
	TFileLog& operator=(const TFileLog&) = delete;
	~TFileLog() {
		Close();
	}

	void Open(EFI_HANDLE image, EFI_SYSTEM_TABLE* systemTable);
	void Close();
	// formattedLength is the full character count returned by GNU-EFI's VPrint.
	void Write(
		EFI_SYSTEM_TABLE* systemTable,
		const wchar_t* format,
		va_list args,
		UINTN formattedLength);

private:
	EFI_SYSTEM_TABLE* SystemTable_ = nullptr;
	_EFI_FILE_HANDLE* File_ = nullptr;

	bool WriteAll(void* data, UINTN size);
	void Disable();
};

} // namespace NLogging::NPrivate
