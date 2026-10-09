#include <util/console_out.h>

extern "C" {
#include <efi.h>
}

static_assert(sizeof(wchar_t) == sizeof(CHAR16), "EFI text requires 16-bit wchar_t");
static_assert(alignof(wchar_t) == alignof(CHAR16));

TConsoleOut::TConsoleOut(EFI_SYSTEM_TABLE* systemTable)
	: conOut(systemTable != nullptr ? systemTable->ConOut : nullptr) {
}

void TConsoleOut::Write(const wchar_t* text) const {
	if (conOut == nullptr || conOut->OutputString == nullptr || text == nullptr) {
		return;
	}

	// EFI accepts a mutable CHAR16 pointer, but OutputString only reads the text.
	conOut->OutputString(conOut, const_cast<CHAR16*>(reinterpret_cast<const CHAR16*>(text)));
}
