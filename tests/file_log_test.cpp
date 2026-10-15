#include <logging/file_log.h>
#include <logging/logger.h>
#include <logging/logging.h>

extern "C" {
#include <efi.h>
#include <efilib.h>

// GNU-EFI's stock _entry sets this before calling the application entry point.
extern EFI_HANDLE LibImageHandle;
}
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <type_traits>

// Keep every test's log lifecycle balanced, including failed assertions.
class TLogScope {
public:
	TLogScope() {
		NLogging::NPrivate::GetLogger().Close();
	}
	~TLogScope() {
		NLogging::NPrivate::GetLogger().Close();
	}
	TLogScope(const TLogScope&) = delete;
	TLogScope& operator=(const TLogScope&) = delete;
};

static_assert(!std::is_copy_constructible_v<TLogScope>);
static_assert(!std::is_move_constructible_v<TLogScope>);
static_assert(!std::is_copy_constructible_v<NLogging::NPrivate::TFileLog>);
static_assert(!std::is_copy_assignable_v<NLogging::NPrivate::TFileLog>);
static_assert(!std::is_move_constructible_v<NLogging::NPrivate::TFileLog>);
static_assert(!std::is_move_assignable_v<NLogging::NPrivate::TFileLog>);

static constexpr EFI_GUID EXPECTED_VARIABLE_GUID = {
	.Data1 = 0x80f05b32,
	.Data2 = 0x6ba8,
	.Data3 = 0x4c7e,
	.Data4 = { 0xba, 0x1b, 0x40, 0x23, 0x57, 0xff, 0x91, 0xd8 }
};
static constexpr EFI_GUID LOADED_IMAGE_GUID = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static constexpr EFI_GUID FILE_SYSTEM_GUID = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static constexpr wchar_t LOG_PATH[] = L"\\EFI\\apple-set-os.log";
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

// Enough for the long formatted fixture, its CRLF and several appended messages.
static constexpr size_t TEXT_CAPACITY = 2048;
static constexpr size_t FILE_CAPACITY = TEXT_CAPACITY * sizeof(CHAR16);
static constexpr size_t VARIABLE_CAPACITY = 128;
static constexpr UINT64 END_OF_FILE_POSITION = ~UINT64{0};

enum class EFailure {
	None,
	QueryVariable,
	ReadVariable,
	LoadedImage,
	FileSystem,
	OpenVolume,
	OpenFile,
	ReadFile,
	StartSeek,
	EndSeek,
	Position
};

static EFI_BOOT_SERVICES SERVICES;
static EFI_RUNTIME_SERVICES RUNTIME;
static EFI_SYSTEM_TABLE SYSTEM_TABLE;
static SIMPLE_TEXT_OUTPUT_INTERFACE CONSOLE;
static SIMPLE_TEXT_OUTPUT_MODE CONSOLE_MODE;
static EFI_LOADED_IMAGE LOADED_IMAGE;
static EFI_FILE_IO_INTERFACE FILE_SYSTEM;
static EFI_FILE ROOT;
static EFI_FILE LOG_FILE;
static char IMAGE_TOKEN;
static char DEVICE_TOKEN;
static EFailure FAILURE;
static bool MOCK_VALID;
static bool VARIABLE_PRESENT;
static bool VARIABLE_CHANGED;
static CHAR16 VARIABLE_DATA[VARIABLE_CAPACITY];
static UINTN VARIABLE_SIZE;
static size_t VARIABLE_CALLS;
static size_t VARIABLE_CALL_LIMIT;
static size_t SET_VARIABLE_CALLS;
static size_t PROTOCOL_CALLS;
static size_t OPEN_CALLS;
static size_t FILE_OPEN_COUNT;
static size_t ROOT_CLOSE_CALLS;
static size_t FILE_CLOSE_CALLS;
static size_t POOL_OPERATIONS;
static size_t LIVE_POOL_BLOCKS;
static void* POOL_BLOCKS[8];
static bool ALLOCATION_FAILS;
static unsigned char FILE_DATA[FILE_CAPACITY];
static size_t FILE_LENGTH;
static UINT64 FILE_POSITION;
static size_t WRITE_CALLS;
static size_t WRITE_LIMIT;
static EFI_STATUS WRITE_STATUS;
static bool WRITE_RETURNS_ZERO;
static bool WRITE_RETURNS_TOO_MUCH;
static EFI_STATUS FLUSH_STATUS;
static size_t FLUSH_CALLS;
static CHAR16 CONSOLE_TEXT[TEXT_CAPACITY];
static size_t CONSOLE_LENGTH;
static EFI_STATUS CONSOLE_STATUS;

static bool Check(bool condition, const char* message) {
	if (!condition) {
		fprintf(stderr, "%s\n", message);
	}
	return condition;
}

static bool SameText(const CHAR16* actual, const wchar_t* expected) {
	for (size_t index = 0;; ++index) {
		if (actual[index] != static_cast<CHAR16>(expected[index])) {
			return false;
		}
		if (expected[index] == 0) {
			return true;
		}
	}
}

static void SetVariableText(const wchar_t* text) {
	size_t length = 0;
	do {
		VARIABLE_DATA[length] = static_cast<CHAR16>(text[length]);
	} while (text[length++] != 0);
	VARIABLE_SIZE = length * sizeof(CHAR16);
}

static EFI_STATUS EFIAPI MockGetVariable(
	CHAR16* name,
	EFI_GUID* guid,
	UINT32* attributes,
	UINTN* size,
	void* data)
{
	++VARIABLE_CALLS;
	if (VARIABLE_CALLS > VARIABLE_CALL_LIMIT) {
		// Stop a broken recursive retry before it exhausts the native test stack.
		MOCK_VALID = false;
		return EFI_NOT_FOUND;
	}
	const bool argumentsValid = name != nullptr && SameText(name, L"AppleSetOsLogPath") &&
		guid != nullptr && memcmp(guid, &EXPECTED_VARIABLE_GUID, sizeof(*guid)) == 0 && size != nullptr;
	MOCK_VALID = MOCK_VALID && argumentsValid;
	if (!argumentsValid) {
		return EFI_INVALID_PARAMETER;
	}
	if (!VARIABLE_PRESENT) {
		return EFI_NOT_FOUND;
	}
	if (data == nullptr) {
		if (FAILURE == EFailure::QueryVariable) {
			return EFI_DEVICE_ERROR;
		}
		*size = VARIABLE_SIZE;
		return EFI_BUFFER_TOO_SMALL;
	}
	if (FAILURE == EFailure::ReadVariable) {
		return EFI_DEVICE_ERROR;
	}
	if (VARIABLE_CHANGED) {
		*size = VARIABLE_SIZE + sizeof(CHAR16);
		return EFI_BUFFER_TOO_SMALL;
	}
	if (*size < VARIABLE_SIZE) {
		*size = VARIABLE_SIZE;
		return EFI_BUFFER_TOO_SMALL;
	}
	memcpy(data, VARIABLE_DATA, VARIABLE_SIZE);
	*size = VARIABLE_SIZE;
	if (attributes != nullptr) {
		*attributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
			EFI_VARIABLE_RUNTIME_ACCESS;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockSetVariable(CHAR16*, EFI_GUID*, UINT32, UINTN, void*) {
	++SET_VARIABLE_CALLS;
	return EFI_ACCESS_DENIED;
}

static EFI_STATUS EFIAPI MockAllocatePool(EFI_MEMORY_TYPE, UINTN size, void** buffer) {
	++POOL_OPERATIONS;
	if (buffer == nullptr || size == 0) {
		MOCK_VALID = false;
		return EFI_INVALID_PARAMETER;
	}
	*buffer = nullptr;
	if (ALLOCATION_FAILS) {
		return EFI_OUT_OF_RESOURCES;
	}
	for (void*& block : POOL_BLOCKS) {
		if (block == nullptr) {
			block = malloc(size);
			*buffer = block;
			LIVE_POOL_BLOCKS += block != nullptr;
			return block != nullptr ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
		}
	}
	MOCK_VALID = false;
	return EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI MockFreePool(void* buffer) {
	++POOL_OPERATIONS;
	for (void*& block : POOL_BLOCKS) {
		if (block != nullptr && block == buffer) {
			free(block);
			block = nullptr;
			--LIVE_POOL_BLOCKS;
			return EFI_SUCCESS;
		}
	}
	MOCK_VALID = false;
	return EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI MockHandleProtocol(EFI_HANDLE handle, EFI_GUID* guid, void** protocol) {
	++PROTOCOL_CALLS;
	if (guid == nullptr || protocol == nullptr) {
		MOCK_VALID = false;
		return EFI_INVALID_PARAMETER;
	}
	*protocol = nullptr;
	if (memcmp(guid, &LOADED_IMAGE_GUID, sizeof(*guid)) == 0) {
		MOCK_VALID = MOCK_VALID && handle == &IMAGE_TOKEN;
		if (FAILURE == EFailure::LoadedImage) {
			return EFI_NOT_FOUND;
		}
		*protocol = &LOADED_IMAGE;
		return EFI_SUCCESS;
	}
	if (memcmp(guid, &FILE_SYSTEM_GUID, sizeof(*guid)) == 0) {
		MOCK_VALID = MOCK_VALID && handle == &DEVICE_TOKEN;
		if (FAILURE == EFailure::FileSystem) {
			return EFI_UNSUPPORTED;
		}
		*protocol = &FILE_SYSTEM;
		return EFI_SUCCESS;
	}
	MOCK_VALID = false;
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockOpenVolume(EFI_FILE_IO_INTERFACE* fileSystem, EFI_FILE** root) {
	MOCK_VALID = MOCK_VALID && fileSystem == &FILE_SYSTEM && root != nullptr;
	if (FAILURE == EFailure::OpenVolume) {
		return EFI_DEVICE_ERROR;
	}
	*root = &ROOT;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockOpen(
	EFI_FILE* root,
	EFI_FILE** file,
	CHAR16* path,
	UINT64 mode,
	UINT64 attributes)
{
	++OPEN_CALLS;
	MOCK_VALID = MOCK_VALID && root == &ROOT && file != nullptr && path != nullptr &&
		SameText(path, LOG_PATH) && mode == (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
			EFI_FILE_MODE_CREATE) && attributes == 0;
	if (FAILURE == EFailure::OpenFile) {
		return EFI_ACCESS_DENIED;
	}
	++FILE_OPEN_COUNT;
	*file = &LOG_FILE;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockClose(EFI_FILE* file) {
	if (file == &ROOT) {
		++ROOT_CLOSE_CALLS;
	} else if (file == &LOG_FILE) {
		++FILE_CLOSE_CALLS;
	} else {
		MOCK_VALID = false;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockRead(EFI_FILE* file, UINTN* size, void* data) {
	MOCK_VALID = MOCK_VALID && file == &LOG_FILE && size != nullptr && data != nullptr;
	if (FAILURE == EFailure::ReadFile) {
		return EFI_DEVICE_ERROR;
	}
	if (FILE_POSITION > FILE_LENGTH) {
		MOCK_VALID = false;
		return EFI_DEVICE_ERROR;
	}
	const size_t remaining = FILE_LENGTH - static_cast<size_t>(FILE_POSITION);
	if (*size > remaining) {
		*size = remaining;
	}
	memcpy(data, FILE_DATA + FILE_POSITION, *size);
	FILE_POSITION += *size;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockWrite(EFI_FILE* file, UINTN* size, void* data) {
	++WRITE_CALLS;
	MOCK_VALID = MOCK_VALID && file == &LOG_FILE && size != nullptr && data != nullptr;
	if (WRITE_STATUS != EFI_SUCCESS) {
		return WRITE_STATUS;
	}
	if (WRITE_RETURNS_ZERO) {
		*size = 0;
		return EFI_SUCCESS;
	}
	if (WRITE_RETURNS_TOO_MUCH) {
		++*size;
		return EFI_SUCCESS;
	}
	if (*size > WRITE_LIMIT) {
		*size = WRITE_LIMIT;
	}
	if (FILE_POSITION > FILE_CAPACITY || *size > FILE_CAPACITY - FILE_POSITION) {
		MOCK_VALID = false;
		return EFI_VOLUME_FULL;
	}
	memcpy(FILE_DATA + FILE_POSITION, data, *size);
	FILE_POSITION += *size;
	if (FILE_POSITION > FILE_LENGTH) {
		FILE_LENGTH = static_cast<size_t>(FILE_POSITION);
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockSetPosition(EFI_FILE* file, UINT64 position) {
	MOCK_VALID = MOCK_VALID && file == &LOG_FILE;
	if ((position == 0 && FAILURE == EFailure::StartSeek) ||
		(position == END_OF_FILE_POSITION && FAILURE == EFailure::EndSeek))
	{
		return EFI_DEVICE_ERROR;
	}
	FILE_POSITION = position == END_OF_FILE_POSITION ? FILE_LENGTH : position;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockGetPosition(EFI_FILE* file, UINT64* position) {
	MOCK_VALID = MOCK_VALID && file == &LOG_FILE && position != nullptr;
	if (FAILURE == EFailure::Position) {
		return EFI_DEVICE_ERROR;
	}
	*position = FILE_POSITION;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockFlush(EFI_FILE* file) {
	MOCK_VALID = MOCK_VALID && file == &LOG_FILE;
	++FLUSH_CALLS;
	return FLUSH_STATUS;
}

static EFI_STATUS EFIAPI MockOutputString(SIMPLE_TEXT_OUTPUT_INTERFACE* console, CHAR16* text) {
	MOCK_VALID = MOCK_VALID && console == &CONSOLE && text != nullptr;
	if (CONSOLE_STATUS != EFI_SUCCESS) {
		return CONSOLE_STATUS;
	}
	for (size_t index = 0; text[index] != 0; ++index) {
		if (CONSOLE_LENGTH + 1 >= TEXT_CAPACITY) {
			MOCK_VALID = false;
			return EFI_VOLUME_FULL;
		}
		CONSOLE_TEXT[CONSOLE_LENGTH++] = text[index];
	}
	CONSOLE_TEXT[CONSOLE_LENGTH] = 0;
	return EFI_SUCCESS;
}

static void ResetFixture() {
	SERVICES = {};
	RUNTIME = {};
	SYSTEM_TABLE = {};
	CONSOLE = {};
	CONSOLE_MODE = {};
	LOADED_IMAGE = {};
	FILE_SYSTEM = {};
	ROOT = {};
	LOG_FILE = {};
	SERVICES.AllocatePool = MockAllocatePool;
	SERVICES.FreePool = MockFreePool;
	SERVICES.HandleProtocol = MockHandleProtocol;
	RUNTIME.GetVariable = MockGetVariable;
	RUNTIME.SetVariable = MockSetVariable;
	SYSTEM_TABLE.BootServices = &SERVICES;
	SYSTEM_TABLE.RuntimeServices = &RUNTIME;
	SYSTEM_TABLE.ConOut = &CONSOLE;
	CONSOLE.OutputString = MockOutputString;
	CONSOLE.Mode = &CONSOLE_MODE;
	CONSOLE_MODE.Attribute = EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
	LOADED_IMAGE.DeviceHandle = &DEVICE_TOKEN;
	FILE_SYSTEM.OpenVolume = MockOpenVolume;
	ROOT.Open = MockOpen;
	ROOT.Close = MockClose;
	LOG_FILE.Close = MockClose;
	LOG_FILE.Read = MockRead;
	LOG_FILE.Write = MockWrite;
	LOG_FILE.SetPosition = MockSetPosition;
	LOG_FILE.GetPosition = MockGetPosition;
	LOG_FILE.Flush = MockFlush;
	FAILURE = EFailure::None;
	MOCK_VALID = true;
	VARIABLE_PRESENT = true;
	VARIABLE_CHANGED = false;
	SetVariableText(LOG_PATH);
	VARIABLE_CALLS = 0;
	VARIABLE_CALL_LIMIT = 2;
	SET_VARIABLE_CALLS = 0;
	PROTOCOL_CALLS = 0;
	OPEN_CALLS = 0;
	FILE_OPEN_COUNT = 0;
	ROOT_CLOSE_CALLS = 0;
	FILE_CLOSE_CALLS = 0;
	POOL_OPERATIONS = 0;
	ALLOCATION_FAILS = false;
	FILE_LENGTH = 0;
	FILE_POSITION = 0;
	WRITE_CALLS = 0;
	WRITE_LIMIT = FILE_CAPACITY;
	WRITE_STATUS = EFI_SUCCESS;
	WRITE_RETURNS_ZERO = false;
	WRITE_RETURNS_TOO_MUCH = false;
	FLUSH_STATUS = EFI_SUCCESS;
	FLUSH_CALLS = 0;
	CONSOLE_LENGTH = 0;
	CONSOLE_TEXT[0] = 0;
	CONSOLE_STATUS = EFI_SUCCESS;
	// Native tests call Log directly, so reproduce stock GNU-EFI entry setup.
	InitializeLib(/* image */ nullptr, /* systemTable */ &SYSTEM_TABLE);
	LibImageHandle = &IMAGE_TOKEN;
}

static bool CheckCleanup() {
	return Check(MOCK_VALID, "invalid mock arguments, file overflow or pool ownership") &&
		Check(LIVE_POOL_BLOCKS == 0, "pool allocation leaked") &&
		Check(ROOT_CLOSE_CALLS == OPEN_CALLS, "root handle leaked or closed twice") &&
		Check(FILE_CLOSE_CALLS == FILE_OPEN_COUNT, "file handle leaked or closed twice") &&
		Check(SET_VARIABLE_CALLS == 0, "logging must never write firmware variables");
}

static bool CheckConsole(const wchar_t* expected) {
	return Check(SameText(CONSOLE_TEXT, expected), "console output changed or was truncated");
}

static bool CheckConsoleTail(const wchar_t* expected) {
	size_t length = 0;
	while (expected[length] != 0) {
		++length;
	}
	if (!Check(CONSOLE_LENGTH >= length, "normal console message was lost after a file error")) {
		return false;
	}
	return Check(SameText(CONSOLE_TEXT + CONSOLE_LENGTH - length, expected),
		"normal console message changed after a file error");
}

static bool CheckFile(const wchar_t* expected) {
	size_t length = 0;
	while (expected[length] != 0) {
		++length;
	}
	if (!Check(FILE_LENGTH == (length + 1) * sizeof(CHAR16), "file size: missing text or extra NUL/BOM")) {
		return false;
	}
	if (!Check(FILE_DATA[0] == 0xff && FILE_DATA[1] == 0xfe, "file must begin with a UTF-16LE BOM")) {
		return false;
	}
	for (size_t index = 0; index < length; ++index) {
		const unsigned value = static_cast<unsigned>(expected[index]);
		const size_t offset = (index + 1) * sizeof(CHAR16);
		if (!Check(
			FILE_DATA[offset] == (value & 0xff) && FILE_DATA[offset + 1] == (value >> 8),
			"file text mismatch, duplicated CR or byte order error"))
		{
			return false;
		}
	}
	return true;
}

static bool CheckFileLogDestructor() {
	ResetFixture();
	size_t expectedFlushCount = 0;
	{
		NLogging::NPrivate::TFileLog fileLog;
		fileLog.Open(&IMAGE_TOKEN, &SYSTEM_TABLE);
		if (!Check(VARIABLE_CALLS == 2 && FILE_OPEN_COUNT == 1 && FILE_CLOSE_CALLS == 0,
			"direct file log must read configuration and retain its opened file"))
		{
			return false;
		}
		expectedFlushCount = FLUSH_CALLS + 1;
	}
	return Check(FILE_CLOSE_CALLS == 1 && FLUSH_CALLS == expectedFlushCount,
		"file log destructor must flush and close its opened file exactly once") &&
		CheckFile(L"") && CheckConsole(L"") && CheckCleanup();
}

static bool CheckFileLogRepeatedClose() {
	ResetFixture();
	size_t expectedFlushCount = 0;
	{
		NLogging::NPrivate::TFileLog fileLog;
		fileLog.Open(&IMAGE_TOKEN, &SYSTEM_TABLE);
		expectedFlushCount = FLUSH_CALLS + 1;
		fileLog.Close();
		fileLog.Close();
		if (!Check(FILE_CLOSE_CALLS == 1 && FLUSH_CALLS == expectedFlushCount,
			"repeated Close must not flush or close an already closed file"))
		{
			return false;
		}
	}
	return Check(FILE_CLOSE_CALLS == 1 && FLUSH_CALLS == expectedFlushCount,
		"destructor after explicit Close must not flush or close the file again") && CheckCleanup();
}

static bool CheckFileLogFailedOpenDestruction() {
	for (unsigned testKind = 0; testKind < 3; ++testKind) {
		ResetFixture();
		switch (testKind) {
			case 0: VARIABLE_PRESENT = false; break;
			case 1: WRITE_STATUS = EFI_VOLUME_FULL; break;
			case 2: FLUSH_STATUS = EFI_DEVICE_ERROR; break;
		}
		size_t previousFlushCount = 0;
		size_t previousCloseCount = 0;
		{
			NLogging::NPrivate::TFileLog fileLog;
			fileLog.Open(&IMAGE_TOKEN, &SYSTEM_TABLE);
			previousFlushCount = FLUSH_CALLS;
			previousCloseCount = FILE_CLOSE_CALLS;
			if (!CheckCleanup()) {
				return false;
			}
		}
		if (!Check(FLUSH_CALLS == previousFlushCount && FILE_CLOSE_CALLS == previousCloseCount,
			"destructor after failed Open must not flush or close a missing or released file") ||
			!CheckConsole(L"") || !CheckCleanup())
		{
			return false;
		}
	}
	return true;
}

static bool CheckBeforeFirstMessage() {
	ResetFixture();
	{
		TLogScope logScope;
		if (!Check(NLogging::IsAvailable(&SYSTEM_TABLE), "valid console must be available without opening a file")) {
			return false;
		}
		NLogging::Log(&SYSTEM_TABLE, /* format */ nullptr);
		NLogging::Log(&SYSTEM_TABLE, L"");
		NLogging::Log(/* systemTable */ nullptr, L"invalid table\n");
		SYSTEM_TABLE.BootServices = nullptr;
		NLogging::Log(&SYSTEM_TABLE, L"missing boot services\n");
		SYSTEM_TABLE.BootServices = &SERVICES;
		SYSTEM_TABLE.ConOut = nullptr;
		NLogging::Log(&SYSTEM_TABLE, L"missing console\n");
		SYSTEM_TABLE.ConOut = &CONSOLE;
		CONSOLE.OutputString = nullptr;
		NLogging::Log(&SYSTEM_TABLE, L"missing output callback\n");
		CONSOLE.OutputString = MockOutputString;
		CONSOLE.Mode = nullptr;
		NLogging::Log(&SYSTEM_TABLE, L"missing console mode\n");
		CONSOLE.Mode = &CONSOLE_MODE;
		if (!Check(VARIABLE_CALLS == 0 && PROTOCOL_CALLS == 0 && OPEN_CALLS == 0 &&
			POOL_OPERATIONS == 0 && WRITE_CALLS == 0 && CONSOLE_LENGTH == 0,
			"queries, null/empty formats and invalid console must not initialize a file session"))
		{
			return false;
		}
		NLogging::Log(&SYSTEM_TABLE, L"first real message\n");
	}
	return CheckFile(L"first real message\r\n") && CheckConsole(L"first real message\r\n") &&
		CheckCleanup() && Check(VARIABLE_CALLS == 2 && FILE_OPEN_COUNT == 1,
			"the first real message must open the configured log exactly once");
}

static bool CheckDisabledConfiguration() {
	for (unsigned testKind = 0; testKind < 4; ++testKind) {
		ResetFixture();
		switch (testKind) {
			case 0: VARIABLE_PRESENT = false; break;
			case 1: SYSTEM_TABLE.RuntimeServices = nullptr; break;
			case 2: RUNTIME.GetVariable = nullptr; break;
			case 3: FAILURE = EFailure::QueryVariable; break;
		}
		{
			TLogScope logScope;
			NLogging::Log(&SYSTEM_TABLE, L"console only\n");
			const size_t previousVariableCount = VARIABLE_CALLS;
			// A newly available variable must not trigger another attempt in this session.
			VARIABLE_PRESENT = true;
			SYSTEM_TABLE.RuntimeServices = &RUNTIME;
			RUNTIME.GetVariable = MockGetVariable;
			FAILURE = EFailure::None;
			NLogging::Log(&SYSTEM_TABLE, L"still console only\n");
			if (!Check(VARIABLE_CALLS == previousVariableCount,
				"missing or failed configuration must not be retried before Close"))
			{
				return false;
			}
		}
		if (!CheckConsoleTail(L"still console only\r\n") || !CheckCleanup() ||
			!Check(PROTOCOL_CALLS == 0 && POOL_OPERATIONS == 0 && WRITE_CALLS == 0,
				"disabled configuration must not inspect filesystems or allocate formatting buffers"))
		{
			return false;
		}
	}
	return true;
}

static bool CheckNewFile() {
	ResetFixture();
	{
		TLogScope logScope;
		const size_t previousFlushCount = FLUSH_CALLS;
		NLogging::Log(
			&SYSTEM_TABLE,
			L"status=%r text=%a value=%04x count=%d raw=%016lx tail=%a\n",
			EFI_DEVICE_ERROR,
			"Apple Inc.",
			UINT32{0x8086},
			UINT32{42},
			UINT64{0x123456789abcdef0},
			"complete");
		if (!Check(FLUSH_CALLS > previousFlushCount, "every logged message must be flushed")) {
			return false;
		}
		NLogging::Log(&SYSTEM_TABLE, L"\u041f\u0440\u0438\u0432\u0435\u0442 \U0001f680\n");
	}
	constexpr wchar_t expected[] =
		L"status=Device Error text=Apple Inc. value=8086 count=42 raw=123456789ABCDEF0 tail=complete\r\n"
		L"\u041f\u0440\u0438\u0432\u0435\u0442 \U0001f680\r\n";
	return CheckFile(expected) && CheckConsole(expected) && CheckCleanup() &&
		Check(FILE_CLOSE_CALLS == 1, "successful file must close at the end of the logging session");
}

static bool CheckAppendingAndRepeatedSessions() {
	ResetFixture();
	VARIABLE_CALL_LIMIT = 6; // Three sessions each query and read the path once.
	FILE_DATA[0] = 0xff;
	FILE_DATA[1] = 0xfe;
	FILE_DATA[2] = 'A';
	FILE_DATA[3] = 0;
	FILE_LENGTH = 4;
	{
		TLogScope logScope;
		NLogging::Log(&SYSTEM_TABLE, L"B\n");
	}
	if (!CheckFile(L"AB\r\n") || !CheckCleanup() || !Check(FILE_CLOSE_CALLS == 1, "append file leaked")) {
		return false;
	}
	{
		TLogScope logScope;
		if (!Check(OPEN_CALLS == 1, "Close must not eagerly open the next file session")) {
			return false;
		}
		NLogging::Log(&SYSTEM_TABLE, L"C\n");
	}
	{
		TLogScope logScope;
		if (!Check(OPEN_CALLS == 2, "next session must stay lazy until its first log")) {
			return false;
		}
		NLogging::Log(&SYSTEM_TABLE, L"D\n");
	}
	return CheckFile(L"AB\r\nC\r\nD\r\n") && CheckConsole(L"B\r\nC\r\nD\r\n") && CheckCleanup() &&
		Check(FILE_CLOSE_CALLS == 3 && VARIABLE_CALLS == 6,
			"each lazy session must reopen once and close its own file handle");
}

static bool CheckLongFormatting() {
	for (unsigned testKind = 0; testKind < 2; ++testKind) {
		ResetFixture();
		CHAR16 longArgument[sizeof(LONG_TEXT) / sizeof(LONG_TEXT[0])];
		wchar_t expected[sizeof(longArgument) / sizeof(longArgument[0]) + 1];
		size_t length = 0;
		while (LONG_TEXT[length] != 0) {
			longArgument[length] = static_cast<CHAR16>(LONG_TEXT[length]);
			expected[length] = LONG_TEXT[length];
			++length;
		}
		longArgument[length] = 0;
		expected[length - 1] = L'\r';
		expected[length] = L'\n';
		expected[length + 1] = 0;
		{
			TLogScope logScope;
			if (testKind == 0) {
				NLogging::Log(&SYSTEM_TABLE, LONG_TEXT);
			} else {
				NLogging::Log(&SYSTEM_TABLE, L"%s", longArgument);
			}
		}
		if (!CheckFile(expected) || !CheckConsole(expected) || !CheckCleanup()) {
			return false;
		}
	}
	return true;
}

static bool CheckMalformedConfiguration() {
	for (unsigned testKind = 0; testKind < 6; ++testKind) {
		ResetFixture();
		switch (testKind) {
			case 0: SetVariableText(L"relative.log"); break;
			case 1: SetVariableText(L""); break;
			case 2: --VARIABLE_SIZE; break;
			case 3: VARIABLE_SIZE -= sizeof(CHAR16); break;
			case 4: VARIABLE_DATA[2] = 0; break;
			case 5: VARIABLE_CHANGED = true; break;
		}
		{
			TLogScope logScope;
			NLogging::Log(&SYSTEM_TABLE, L"invalid path: console survives\n");
		}
		if (!CheckConsoleTail(L"invalid path: console survives\r\n") || !CheckCleanup() ||
			!Check(OPEN_CALLS == 0 && WRITE_CALLS == 0, "malformed configuration opened a file"))
		{
			return false;
		}
	}
	return true;
}

static bool CheckOpenFailures() {
	constexpr EFailure failures[] = {
		EFailure::ReadVariable, EFailure::LoadedImage, EFailure::FileSystem,
		EFailure::OpenVolume, EFailure::OpenFile, EFailure::ReadFile,
		EFailure::StartSeek, EFailure::EndSeek, EFailure::Position
	};
	for (EFailure failure : failures) {
		ResetFixture();
		FAILURE = failure;
		{
			TLogScope logScope;
			NLogging::Log(&SYSTEM_TABLE, L"open failed: console survives\n");
			const size_t previousVariableCount = VARIABLE_CALLS;
			FAILURE = EFailure::None;
			NLogging::Log(&SYSTEM_TABLE, L"still no file\n");
			if (!Check(WRITE_CALLS == 0 && VARIABLE_CALLS == previousVariableCount,
				"failed open must disable the file sink without retrying or recursive attempts"))
			{
				return false;
			}
		}
		if (!CheckConsoleTail(L"open failed: console survives\r\nstill no file\r\n") || !CheckCleanup()) {
			return false;
		}
	}
	return true;
}

static bool CheckInvalidExistingFiles() {
	for (unsigned testKind = 0; testKind < 3; ++testKind) {
		ResetFixture();
		FILE_DATA[0] = 0xff;
		FILE_DATA[1] = 0xfe;
		FILE_LENGTH = 2;
		if (testKind == 0) {
			FILE_DATA[0] = 'A';
			FILE_DATA[1] = 0;
		} else if (testKind == 1) {
			FILE_LENGTH = 1;
		} else {
			FILE_DATA[2] = 'A';
			FILE_LENGTH = 3;
		}
		const size_t previousFileLength = FILE_LENGTH;
		{
			TLogScope logScope;
			NLogging::Log(&SYSTEM_TABLE, L"invalid file: console survives\n");
		}
		if (!CheckConsoleTail(L"invalid file: console survives\r\n") || !CheckCleanup() ||
			!Check(WRITE_CALLS == 0 && FILE_LENGTH == previousFileLength,
				"invalid existing file must not be overwritten or appended"))
		{
			return false;
		}
	}
	return true;
}

static bool CheckPartialWrites() {
	ResetFixture();
	WRITE_LIMIT = 1; // Even an odd-byte short write must resume at the exact next byte.
	{
		TLogScope logScope;
		NLogging::Log(&SYSTEM_TABLE, L"partial write\n");
	}
	return CheckFile(L"partial write\r\n") && CheckConsole(L"partial write\r\n") && CheckCleanup() &&
		Check(WRITE_CALLS == FILE_LENGTH, "short writes must be retried until all bytes are written");
}

static bool CheckInitialWriteFailures() {
	for (unsigned testKind = 0; testKind < 3; ++testKind) {
		ResetFixture();
		switch (testKind) {
			case 0: WRITE_STATUS = EFI_VOLUME_FULL; break;
			case 1: WRITE_RETURNS_ZERO = true; break;
			case 2: FLUSH_STATUS = EFI_DEVICE_ERROR; break;
		}
		{
			TLogScope logScope;
			NLogging::Log(&SYSTEM_TABLE, L"BOM failed: console survives\n");
			const size_t previousWriteCount = WRITE_CALLS;
			const size_t previousVariableCount = VARIABLE_CALLS;
			NLogging::Log(&SYSTEM_TABLE, L"console continues\n");
			if (!Check(WRITE_CALLS == previousWriteCount && VARIABLE_CALLS == previousVariableCount,
				"failed BOM initialization enabled or retried file logging"))
			{
				return false;
			}
		}
		if (!CheckConsole(L"BOM failed: console survives\r\nconsole continues\r\n") || !CheckCleanup()) {
			return false;
		}
	}
	return true;
}

static bool CheckEmptyMessages() {
	ResetFixture();
	{
		TLogScope logScope;
		const size_t previousWriteCount = WRITE_CALLS;
		const size_t previousPoolCount = POOL_OPERATIONS;
		NLogging::Log(&SYSTEM_TABLE, L"");
		NLogging::Log(&SYSTEM_TABLE, /* format */ nullptr);
		if (!Check(WRITE_CALLS == previousWriteCount && POOL_OPERATIONS == previousPoolCount,
			"empty or null messages must not write or allocate a formatting buffer"))
		{
			return false;
		}
	}
	return CheckConsole(L"") && CheckCleanup() &&
		Check(FILE_LENGTH == 0 && VARIABLE_CALLS == 0 && PROTOCOL_CALLS == 0 && OPEN_CALLS == 0,
			"empty messages must not open a log or write its BOM");
}

static bool CheckWriteFailures() {
	for (unsigned testKind = 0; testKind < 5; ++testKind) {
		ResetFixture();
		{
			TLogScope logScope;
			NLogging::Log(&SYSTEM_TABLE, L"file ready\n");
			switch (testKind) {
				case 0: WRITE_STATUS = EFI_VOLUME_FULL; break;
				case 1: WRITE_RETURNS_ZERO = true; break;
				case 2: WRITE_RETURNS_TOO_MUCH = true; break;
				case 3: FLUSH_STATUS = EFI_DEVICE_ERROR; break;
				case 4: ALLOCATION_FAILS = true; break;
			}
			NLogging::Log(&SYSTEM_TABLE, L"sink failed\n");
			const size_t previousWriteCount = WRITE_CALLS;
			const size_t previousPoolCount = POOL_OPERATIONS;
			const size_t previousVariableCount = VARIABLE_CALLS;
			NLogging::Log(&SYSTEM_TABLE, L"console continues\n");
			if (!Check(
				WRITE_CALLS == previousWriteCount && POOL_OPERATIONS == previousPoolCount &&
					VARIABLE_CALLS == previousVariableCount,
				"a failed file sink must stop writing and allocating formatting buffers"))
			{
				return false;
			}
		}
		if (!CheckConsole(L"file ready\r\nsink failed\r\nconsole continues\r\n") || !CheckCleanup() ||
			!Check(FILE_CLOSE_CALLS == 1, "failed file sink did not close its handle"))
		{
			return false;
		}
	}
	return true;
}

static bool CheckAllocationAndCallbackFailures() {
	for (unsigned testKind = 0; testKind < 7; ++testKind) {
		ResetFixture();
		switch (testKind) {
			case 0: ALLOCATION_FAILS = true; break;
			case 1: LOG_FILE.Read = nullptr; break;
			case 2: LOG_FILE.Write = nullptr; break;
			case 3: LOG_FILE.SetPosition = nullptr; break;
			case 4: LOG_FILE.GetPosition = nullptr; break;
			case 5: LOG_FILE.Flush = nullptr; break;
			case 6: SERVICES.AllocatePool = nullptr; break;
		}
		{
			TLogScope logScope;
			NLogging::Log(&SYSTEM_TABLE, L"incomplete services: console survives\n");
		}
		if (!CheckConsoleTail(L"incomplete services: console survives\r\n") || !CheckCleanup() ||
			!Check(WRITE_CALLS == 0, "allocation or missing callback failure enabled file writes"))
		{
			return false;
		}
	}
	return true;
}

int main() {
	if (!CheckFileLogDestructor() || !CheckFileLogRepeatedClose() || !CheckFileLogFailedOpenDestruction() ||
		!CheckBeforeFirstMessage() || !CheckDisabledConfiguration() || !CheckNewFile() ||
		!CheckAppendingAndRepeatedSessions() ||
		!CheckLongFormatting() || !CheckMalformedConfiguration() || !CheckOpenFailures() ||
		!CheckInvalidExistingFiles() || !CheckPartialWrites() || !CheckInitialWriteFailures() ||
		!CheckEmptyMessages() || !CheckWriteFailures() || !CheckAllocationAndCallbackFailures())
	{
		return 1;
	}
	puts("File logging tests passed");
	return 0;
}
