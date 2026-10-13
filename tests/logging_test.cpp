#include <logging/logging.h>

extern "C" {
#include <efi.h>
#include <efilib.h>
}
#include <stddef.h>
#include <stdio.h>

// A long fixture verifies that neither the format nor a string argument loses its tail.
static constexpr wchar_t LONG_TEXT[] =
	L"[long] begin:"
	L"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
	L"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
	L"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
	L"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
	L"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
	L"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
	L"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
	L"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
	L":tail\n";
static constexpr size_t LONG_TEXT_LENGTH = sizeof(LONG_TEXT) / sizeof(LONG_TEXT[0]) - 1;
// Capture the longest fixture, the CR inserted before LF, and a terminator.
static constexpr size_t CAPTURE_CAPACITY = LONG_TEXT_LENGTH + 2;

// InitializeLib retains these addresses after its first call.
static EFI_BOOT_SERVICES SERVICES;
static EFI_SYSTEM_TABLE SYSTEM_TABLE;
static EFI_SYSTEM_TABLE OTHER_SYSTEM_TABLE;
static SIMPLE_TEXT_OUTPUT_INTERFACE CONSOLE;
static SIMPLE_TEXT_OUTPUT_MODE CONSOLE_MODE;
static CHAR16 CONTENTS[CAPTURE_CAPACITY];
static size_t CONTENT_LENGTH;
static size_t OUTPUT_CALLS;
static size_t POOL_OPERATIONS;
static bool OUTPUT_VALID;
static EFI_STATUS OUTPUT_STATUS;

static EFI_STATUS EFIAPI MockOutputString(SIMPLE_TEXT_OUTPUT_INTERFACE* conOut, CHAR16* text) {
	++OUTPUT_CALLS;
	if (conOut != &CONSOLE || text == nullptr) {
		OUTPUT_VALID = false;
		return EFI_INVALID_PARAMETER;
	}
	if (OUTPUT_STATUS != EFI_SUCCESS) {
		return OUTPUT_STATUS;
	}
	for (size_t index = 0; text[index] != 0; ++index) {
		if (CONTENT_LENGTH + 1 >= CAPTURE_CAPACITY) {
			OUTPUT_VALID = false;
			return EFI_VOLUME_FULL;
		}
		CONTENTS[CONTENT_LENGTH++] = text[index];
	}
	CONTENTS[CONTENT_LENGTH] = 0;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockAllocatePool(EFI_MEMORY_TYPE, UINTN, void** buffer) {
	++POOL_OPERATIONS;
	if (buffer != nullptr) {
		*buffer = nullptr;
	}
	return EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI MockFreePool(void*) {
	++POOL_OPERATIONS;
	return EFI_ACCESS_DENIED;
}

static void ClearCapture() {
	CONTENT_LENGTH = 0;
	CONTENTS[0] = 0;
	OUTPUT_CALLS = 0;
	POOL_OPERATIONS = 0;
	OUTPUT_VALID = true;
	OUTPUT_STATUS = EFI_SUCCESS;
}

static void ResetFixture() {
	SERVICES = {};
	SYSTEM_TABLE = {};
	OTHER_SYSTEM_TABLE = {};
	CONSOLE = {};
	CONSOLE_MODE = {};
	SERVICES.AllocatePool = MockAllocatePool;
	SERVICES.FreePool = MockFreePool;
	CONSOLE.OutputString = MockOutputString;
	CONSOLE.Mode = &CONSOLE_MODE;
	CONSOLE_MODE.Attribute = EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
	SYSTEM_TABLE.BootServices = &SERVICES;
	SYSTEM_TABLE.ConOut = &CONSOLE;
	ClearCapture();
}

static bool Check(bool condition, const char* message) {
	if (!condition) {
		fprintf(stderr, "%s\n", message);
	}
	return condition;
}

static bool CheckText(const wchar_t* expected, const char* message) {
	if (!OUTPUT_VALID || POOL_OPERATIONS != 0) {
		fprintf(stderr, "%s: invalid console call or unexpected pool operation\n", message);
		return false;
	}
	for (size_t index = 0; index < CONTENT_LENGTH; ++index) {
		if (expected[index] == 0 || CONTENTS[index] != static_cast<CHAR16>(expected[index])) {
			fprintf(stderr, "%s: mismatch at character %zu\n", message, index);
			return false;
		}
	}
	return Check(expected[CONTENT_LENGTH] == 0, message);
}

static bool CheckNoOutput(const char* message) {
	return Check(OUTPUT_CALLS == 0 && CONTENT_LENGTH == 0 && POOL_OPERATIONS == 0, message);
}

static bool CheckSilentLog(EFI_SYSTEM_TABLE* systemTable, const char* message) {
	if (!Check(!NLogging::IsAvailable(systemTable), "invalid console must not be available")) {
		return false;
	}
	NLogging::Log(systemTable, L"must not appear\n");
	return CheckNoOutput(message);
}

static bool CheckInvalidInputs() {
	if (!CheckSilentLog(nullptr, "null system table: unexpected console call")) {
		return false;
	}
	SYSTEM_TABLE.BootServices = nullptr;
	if (!CheckSilentLog(&SYSTEM_TABLE, "missing boot services: unexpected console call")) {
		return false;
	}
	SYSTEM_TABLE.BootServices = &SERVICES;
	NLogging::Log(&SYSTEM_TABLE, /* format */ nullptr);
	if (!CheckNoOutput("null format: unexpected console call")) {
		return false;
	}
	SYSTEM_TABLE.ConOut = nullptr;
	if (!CheckSilentLog(&SYSTEM_TABLE, "missing console: unexpected console call")) {
		return false;
	}
	SYSTEM_TABLE.ConOut = &CONSOLE;
	CONSOLE.OutputString = nullptr;
	if (!CheckSilentLog(&SYSTEM_TABLE, "missing output callback: unexpected console call")) {
		return false;
	}
	CONSOLE.OutputString = MockOutputString;
	CONSOLE.Mode = nullptr;
	if (!CheckSilentLog(&SYSTEM_TABLE, "missing console mode: unexpected console call")) {
		return false;
	}
	CONSOLE.Mode = &CONSOLE_MODE;
	return true;
}

static bool CheckBeforeFirstOutput() {
	ResetFixture();
	if (!Check(ST == nullptr && BS == nullptr, "GNU-EFI initialized before the first log") ||
		!Check(NLogging::IsAvailable(&SYSTEM_TABLE), "valid console must be available before initialization") ||
		!CheckInvalidInputs())
	{
		return false;
	}
	OTHER_SYSTEM_TABLE = SYSTEM_TABLE;
	if (!Check(
		NLogging::IsAvailable(&OTHER_SYSTEM_TABLE),
		"a capability query must not bind the logging session to a system table"))
	{
		return false;
	}
	return Check(ST == nullptr && BS == nullptr, "capability queries and invalid calls must not initialize GNU-EFI") &&
		CheckNoOutput("capability queries must not write to the console or allocate memory");
}

static bool CheckFormatting() {
	ResetFixture();
	NLogging::Log(
		/* systemTable */ &SYSTEM_TABLE,
		/* format */ L"status=%r text=%a count=%d id=%04x raw=%016lx\n",
		/* status */ EFI_DEVICE_ERROR,
		/* text */ "Apple Inc.",
		/* count */ UINT32{42},
		/* id */ UINT32{0x8086},
		/* raw */ UINT64{0x123456789abcdef0});
	return Check(ST == &SYSTEM_TABLE && BS == &SERVICES, "first valid log must initialize GNU-EFI") &&
		Check(NLogging::IsAvailable(&SYSTEM_TABLE), "bound console must remain available") &&
		CheckText(
			L"status=Device Error text=Apple Inc. count=42 id=8086 raw=123456789ABCDEF0\r\n",
			"formatted output: EFI status, ASCII text and numeric arguments");
}

static bool CheckBasicOutput() {
	ResetFixture();
	NLogging::Log(&SYSTEM_TABLE, L"");
	if (!CheckText(L"", "empty message")) {
		return false;
	}

	constexpr wchar_t unicodeText[] = L"\u041f\u0440\u0438\u0432\u0435\u0442 \U0001f680";
	ClearCapture();
	NLogging::Log(&SYSTEM_TABLE, unicodeText);
	if (!CheckText(unicodeText, "UTF-16 text and surrogate pair")) {
		return false;
	}

	for (size_t index = 0; index < 3; ++index) {
		ClearCapture();
		NLogging::Log(&SYSTEM_TABLE, L"same text\n");
		if (!CheckText(L"same text\r\n", "repeated message")) {
			return false;
		}
	}
	return true;
}

static bool CheckGuards() {
	ResetFixture();
	if (!CheckInvalidInputs()) {
		return false;
	}
	if (!Check(
		ST == &SYSTEM_TABLE && BS == &SERVICES,
		"invalid later calls must preserve GNU-EFI global pointers"))
	{
		return false;
	}
	OTHER_SYSTEM_TABLE = SYSTEM_TABLE;
	if (!CheckSilentLog(&OTHER_SYSTEM_TABLE, "different cached GNU-EFI table: unexpected console call")) {
		return false;
	}
	if (!Check(
		ST == &SYSTEM_TABLE && BS == &SERVICES,
		"different table must not change GNU-EFI global pointers"))
	{
		return false;
	}
	ST = &OTHER_SYSTEM_TABLE;
	if (!CheckSilentLog(&SYSTEM_TABLE, "changed GNU-EFI table: unexpected console call")) {
		return false;
	}
	if (!Check(ST == &OTHER_SYSTEM_TABLE, "capability query must not repair a changed GNU-EFI table")) {
		return false;
	}
	ST = &SYSTEM_TABLE;
	if (!Check(NLogging::IsAvailable(&SYSTEM_TABLE), "restored GNU-EFI table must be available")) {
		return false;
	}
	NLogging::Log(&SYSTEM_TABLE, L"original table still works\n");
	return CheckText(
		L"original table still works\r\n",
		"original table must remain usable after rejected calls");
}

static bool CheckConsoleError() {
	ResetFixture();
	OUTPUT_STATUS = EFI_DEVICE_ERROR;
	NLogging::Log(&SYSTEM_TABLE, L"firmware rejected this output\n");
	const bool consoleCallValid =
		OUTPUT_CALLS != 0 && CONTENT_LENGTH == 0 && OUTPUT_VALID && POOL_OPERATIONS == 0;
	if (!Check(consoleCallValid, "console error: invalid callback or unexpected pool operation")) {
		return false;
	}
	ClearCapture();
	NLogging::Log(&SYSTEM_TABLE, L"console recovered\n");
	return CheckText(L"console recovered\r\n", "console error: later output must remain usable");
}

static bool CheckLongOutput() {
	ResetFixture();
	CHAR16 longArgument[LONG_TEXT_LENGTH + 1];
	for (size_t index = 0; index <= LONG_TEXT_LENGTH; ++index) {
		longArgument[index] = static_cast<CHAR16>(LONG_TEXT[index]);
	}
	for (unsigned testKind = 0; testKind < 2; ++testKind) {
		ClearCapture();
		if (testKind == 0) {
			NLogging::Log(&SYSTEM_TABLE, LONG_TEXT);
		} else {
			NLogging::Log(&SYSTEM_TABLE, L"%s", longArgument);
		}
		const bool outputComplete = OUTPUT_VALID && CONTENT_LENGTH == LONG_TEXT_LENGTH + 1;
		if (!Check(outputComplete, "long literal and long %s argument: output must not be truncated")) {
			return false;
		}
		const bool noPoolOperations = POOL_OPERATIONS == 0;
		if (!Check(noPoolOperations, "long output: no pool allocations or frees")) {
			return false;
		}
		const bool lineEndingCorrect =
			CONTENTS[LONG_TEXT_LENGTH - 1] == L'\r' && CONTENTS[LONG_TEXT_LENGTH] == L'\n';
		if (!Check(lineEndingCorrect, "long output: exactly one final CRLF")) {
			return false;
		}
		for (size_t index = 0; index < LONG_TEXT_LENGTH - 1; ++index) {
			if (!Check(
				CONTENTS[index] == static_cast<CHAR16>(LONG_TEXT[index]),
				"long output: prefix, body and tail must be preserved"))
			{
				return false;
			}
		}
	}
	return true;
}

int main() {
	if (!CheckBeforeFirstOutput() || !CheckFormatting() || !CheckBasicOutput() || !CheckGuards() ||
		!CheckConsoleError() || !CheckLongOutput())
	{
		return 1;
	}
	puts("Logging tests passed");
	return 0;
}
