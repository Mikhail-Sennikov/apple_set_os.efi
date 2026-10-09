#pragma once

struct _EFI_SYSTEM_TABLE;
using EFI_SYSTEM_TABLE = _EFI_SYSTEM_TABLE;
struct _SIMPLE_TEXT_OUTPUT_INTERFACE;

class TConsoleOut {
public:
	explicit TConsoleOut(EFI_SYSTEM_TABLE* systemTable);
	void Write(const wchar_t* text) const;

private:
	_SIMPLE_TEXT_OUTPUT_INTERFACE* conOut;
};
