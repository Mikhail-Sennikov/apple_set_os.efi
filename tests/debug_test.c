#include <stdio.h>
#include <string.h>

/* Exercise the actual module against mocked EFI file and event services. */
#include "../src/debug/debug.c"

static EFI_BOOT_SERVICES services;
static EFI_FILE main_file, main_root, snapshot_file, snapshot_root;
static EFI_FILE_IO_INTERFACE filesystem;
static EFI_LOADED_IMAGE windows_image;
static CHAR16 contents[8192];
static UINTN content_bytes, main_position, snapshot_position;
static int main_closed, root_closed, open_count, locate_count, event_closed;
static int callback_count, callback_before_close, vendor_calls, version_calls;
static EFI_STATUS write_error, flush_error;

static struct {
	EFI_DEVICE_PATH file;
	CHAR16 path[sizeof(WINDOWS_BOOT_PATH) / sizeof(CHAR16)];
	EFI_DEVICE_PATH end;
} windows_path;

static EFI_STATUS EFIAPI
mock_write(EFI_FILE *file, UINTN *size, VOID *buffer)
{
	UINTN *position = file == &main_file ? &main_position : &snapshot_position;
	if (write_error != EFI_SUCCESS)
		return write_error;
	if (*position + *size > sizeof(contents))
		return EFI_VOLUME_FULL;
	memcpy((UINT8 *)contents + *position, buffer, *size);
	*position += *size;
	if (*position > content_bytes)
		content_bytes = *position;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mock_flush(EFI_FILE *file)
{
	(void)file;
	return flush_error;
}

static EFI_STATUS EFIAPI
mock_close(EFI_FILE *file)
{
	if (file == &main_file) main_closed = 1;
	if (file == &main_root) root_closed = 1;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mock_set_position(EFI_FILE *file, UINT64 position)
{
	(void)file;
	snapshot_position = position == ~(UINT64)0 ? content_bytes : position;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mock_get_position(EFI_FILE *file, UINT64 *position)
{
	(void)file;
	*position = snapshot_position;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mock_open(EFI_FILE *root, EFI_FILE **file, CHAR16 *path, UINT64 mode, UINT64 attributes)
{
	(void)root; (void)path; (void)mode; (void)attributes;
	++open_count;
	if (!main_closed || !root_closed)
		callback_before_close = 1;
	*file = &snapshot_file;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mock_open_volume(EFI_FILE_IO_INTERFACE *self, EFI_FILE **root)
{
	(void)self;
	*root = &snapshot_root;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mock_locate_handle(EFI_LOCATE_SEARCH_TYPE type, EFI_GUID *guid, VOID *key,
	UINTN *size, EFI_HANDLE *handle)
{
	(void)type; (void)guid; (void)key;
	++locate_count;
	if (!main_closed || !root_closed)
		callback_before_close = 1;
	if (locate_count > 1)
		return EFI_NOT_FOUND;
	*size = sizeof(*handle);
	*handle = (EFI_HANDLE)&windows_image;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mock_handle_protocol(EFI_HANDLE handle, EFI_GUID *guid, VOID **interface)
{
	(void)guid;
	if (handle == (EFI_HANDLE)&windows_image) {
		*interface = &windows_image;
		++callback_count;
	} else {
		*interface = &filesystem;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mock_locate_protocol(EFI_GUID *guid, VOID *registration, VOID **interface)
{
	(void)guid; (void)registration;
	*interface = NULL;
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI
mock_locate_buffer(EFI_LOCATE_SEARCH_TYPE type, EFI_GUID *guid, VOID *key,
	UINTN *count, EFI_HANDLE **handles)
{
	(void)type; (void)guid; (void)key;
	*count = 0;
	*handles = NULL;
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI
mock_close_event(EFI_EVENT event)
{
	(void)event;
	++event_closed;
	return EFI_SUCCESS;
}

static VOID EFIAPI
mock_vendor(CHAR8 *value)
{
	(void)value;
	++vendor_calls;
	write_error = EFI_DEVICE_ERROR;
}

static VOID EFIAPI
mock_version(CHAR8 *value)
{
	(void)value;
	++version_calls;
}

static void
reset_case(void)
{
	memset(&services, 0, sizeof(services));
	memset(contents, 0, sizeof(contents));
	content_bytes = main_position = snapshot_position = 0;
	main_closed = root_closed = open_count = locate_count = event_closed = 0;
	callback_count = callback_before_close = vendor_calls = version_calls = 0;
	write_error = flush_error = EFI_SUCCESS;
	services.LocateHandle = mock_locate_handle;
	services.LocateHandleBuffer = mock_locate_buffer;
	services.HandleProtocol = mock_handle_protocol;
	services.LocateProtocol = mock_locate_protocol;
	services.CloseEvent = mock_close_event;
	BS = &services;
	main_file = (EFI_FILE){ .Write = mock_write, .Flush = mock_flush, .Close = mock_close };
	main_root = (EFI_FILE){ .Close = mock_close };
	snapshot_file = (EFI_FILE){ .Write = mock_write, .Flush = mock_flush, .Close = mock_close,
		.SetPosition = mock_set_position, .GetPosition = mock_get_position };
	snapshot_root = (EFI_FILE){ .Open = mock_open, .Close = mock_close };
	filesystem = (EFI_FILE_IO_INTERFACE){ .OpenVolume = mock_open_volume };
	main_log = (DIAG_LOG){ .root = &main_root, .file = &main_file };
	set_os_log_context = (SET_OS_LOG_CONTEXT){ .log = &main_log };
	loaded_image_registration = &services;
	loaded_image_event = &services;
	loaded_image_notify_ready = loaded_image_notify_busy = bootmgfw_seen = 0;
	setters_attempted = 0;
	set_os_protocol_present = 0;
	set_os_result = (APPLE_SET_OS_RESULT){ 0 };
	diag_device = &filesystem;
	windows_path.file.Type = MEDIA_DEVICE_PATH;
	windows_path.file.SubType = MEDIA_FILEPATH_DP;
	SetDevicePathNodeLength(&windows_path.file, sizeof(windows_path.file) + sizeof(windows_path.path));
	memcpy(windows_path.path, WINDOWS_BOOT_PATH, sizeof(WINDOWS_BOOT_PATH));
	SetDevicePathEndNode(&windows_path.end);
	windows_image.FilePath = &windows_path.file;
	windows_image.DeviceHandle = diag_device;
}

static int
contains(CHAR16 *needle)
{
	UINTN index, count = content_bytes / sizeof(CHAR16), length = StrLen(needle);
	for (index = 0; index + length <= count; ++index) {
		if (memcmp(contents + index, needle, length * sizeof(CHAR16)) == 0)
			return 1;
	}
	return 0;
}

int
main(void)
{
	APPLE_SET_OS_RESULT result = { 0 };
	APPLE_SET_OS_INTERFACE protocol = { 2, mock_version, mock_vendor };
	EFI_STATUS status;

	reset_case();
	on_loaded_image(NULL, NULL);
	if (locate_count != 0 || open_count != 0) return 10;
	if (apple_set_os_debug_finish(EFI_SUCCESS, &result) != EFI_SUCCESS) return 11;
	if (callback_before_close || callback_count != 1 || open_count != 1 || !event_closed) return 12;
	if (!contains(L"run stage=driver_end") ||
		!contains(L"windows_image phase=loaded") ||
		!contains(L"windows_image phase=snapshot_complete")) return 13;

	reset_case();
	write_error = EFI_DEVICE_ERROR;
	status = apple_set_os_apply(&protocol, &result, apple_set_os_debug_observer, NULL);
	if (status != EFI_DEVICE_ERROR || vendor_calls || version_calls) return 20;
	if (apple_set_os_debug_finish(status, &result) != EFI_DEVICE_ERROR) return 21;
	if (!main_closed || !event_closed || locate_count) return 22;

	reset_case();
	status = apple_set_os_apply(&protocol, &result, apple_set_os_debug_observer, NULL);
	if (status != EFI_SUCCESS || vendor_calls != 1 || version_calls != 1) return 30;
	if (set_os_log_context.first_log_error != EFI_DEVICE_ERROR) return 31;
	if (apple_set_os_debug_finish(status, &result) != EFI_SUCCESS) return 32;

	reset_case();
	result = (APPLE_SET_OS_RESULT){ 2, 1, 1 };
	flush_error = EFI_DEVICE_ERROR;
	if (apple_set_os_debug_finish(EFI_SUCCESS, &result) != EFI_SUCCESS) return 40;
	if (locate_count || open_count || !event_closed || loaded_image_notify_ready) return 41;

	puts("debug tests passed");
	return 0;
}
