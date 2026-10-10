#include <util/console_out.h>

extern "C" {
#include <efi.h>
}

namespace NUtil {

static_assert(sizeof(wchar_t) == sizeof(CHAR16), "EFI text requires 16-bit wchar_t");
static_assert(alignof(wchar_t) == alignof(CHAR16));

TConsoleOut::TConsoleOut(EFI_SYSTEM_TABLE* systemTable)
	: ConsoleOut_(systemTable != nullptr ? systemTable->ConOut : nullptr) {
}

void TConsoleOut::Write(const wchar_t* text) const {
	if (ConsoleOut_ == nullptr || ConsoleOut_->OutputString == nullptr || text == nullptr) {
		return;
	}

	// EFI accepts a mutable CHAR16 pointer, but OutputString only reads the text.
	ConsoleOut_->OutputString(ConsoleOut_, const_cast<CHAR16*>(reinterpret_cast<const CHAR16*>(text)));
}

} // namespace NUtil
