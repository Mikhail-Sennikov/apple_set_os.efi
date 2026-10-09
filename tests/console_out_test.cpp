#include <util/console_out.h>

extern "C" {
#include <efi.h>
}
#include <stddef.h>
#include <stdio.h>

static_assert(sizeof(TConsoleOut) == sizeof(void*));

static SIMPLE_TEXT_OUTPUT_INTERFACE* EXPECTED_CON_OUT;
static const void* EXPECTED_TEXT;
static EFI_STATUS OUTPUT_STATUS;
static size_t OUTPUT_COUNT;
static bool OUTPUT_ARGUMENTS_VALID;

static EFI_STATUS EFIAPI MockOutputString(SIMPLE_TEXT_OUTPUT_INTERFACE* conOut,
	CHAR16* text) {
	OUTPUT_ARGUMENTS_VALID = OUTPUT_ARGUMENTS_VALID &&
		conOut == EXPECTED_CON_OUT && static_cast<const void*>(text) == EXPECTED_TEXT;
	++OUTPUT_COUNT;
	return OUTPUT_STATUS;
}

static bool CheckWrite(const char* name, const TConsoleOut& console,
	const wchar_t* text, size_t repeats = 1) {
	EXPECTED_TEXT = static_cast<const void*>(text);
	OUTPUT_COUNT = 0;
	OUTPUT_ARGUMENTS_VALID = true;
	for (size_t index = 0; index < repeats; ++index) {
		console.Write(text);
	}
	if (!OUTPUT_ARGUMENTS_VALID || OUTPUT_COUNT != repeats) {
		fprintf(stderr, "%s: text pointer or console callback mismatch\n", name);
		return false;
	}
	return true;
}

int main() {
	SIMPLE_TEXT_OUTPUT_INTERFACE conOut{};
	conOut.OutputString = MockOutputString;
	EFI_SYSTEM_TABLE systemTable{};
	systemTable.ConOut = &conOut;
	TConsoleOut console(&systemTable);
	EXPECTED_CON_OUT = &conOut;
	OUTPUT_STATUS = EFI_SUCCESS;

	if (!CheckWrite("normal text", console, L"apple_set_os\r\n") ||
		!CheckWrite("empty text", console, L"") ||
		!CheckWrite("UTF-16 text", console, L"\u041f\u0440\u0438\u0432\u0435\u0442 \U0001f680") ||
		!CheckWrite("same text repeatedly", console, L"same text", 3))
	{
		return 1;
	}
	OUTPUT_STATUS = EFI_DEVICE_ERROR;
	if (!CheckWrite("firmware error", console, L"still forwarded")) {
		return 1;
	}

	OUTPUT_COUNT = 0;
	console.Write(nullptr);
	TConsoleOut(nullptr).Write(L"no system table");
	EFI_SYSTEM_TABLE noConsoleTable{};
	TConsoleOut(&noConsoleTable).Write(L"no console");
	conOut.OutputString = nullptr;
	console.Write(L"no callback");
	if (OUTPUT_COUNT != 0) {
		fprintf(stderr, "null guards: unexpected console callback\n");
		return 1;
	}

	puts("Console output tests passed");
	return 0;
}
