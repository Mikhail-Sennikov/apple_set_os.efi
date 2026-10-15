[CmdletBinding()]
param(
    [string]$Path = '\EFI\apple-set-os.log',
    [switch]$Disable,
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'
if (-not $Disable -and
    ($Path.Length -lt 2 -or -not $Path.StartsWith('\') -or $Path.EndsWith('\') -or
        $Path.Contains('"') -or $Path.Contains([string][char]0)))
{
    throw 'Use an absolute EFI file path, such as \EFI\apple-set-os.log, without a drive letter.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try {
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $isAdministrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} finally {
    $identity.Dispose()
}
if (-not $isAdministrator) {
    if ($Elevated) { throw 'Administrator token was not granted.' }
    $arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -Elevated' -f $PSCommandPath
    if ($Disable) { $arguments += ' -Disable' }
    else { $arguments += ' -Path "{0}"' -f $Path }
    $process = Start-Process -FilePath 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
        -Verb RunAs -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    try {
        $exitCode = $process.ExitCode
    } finally {
        $process.Dispose()
    }
    exit $exitCode
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class TAppleSetOsLogVariable {
    public const string Name = "AppleSetOsLogPath";
    public const string Guid = "{80f05b32-6ba8-4c7e-ba1b-402357ff91d8}";
    // NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS
    public const uint Attributes = 7;

    [StructLayout(LayoutKind.Sequential)]
    private struct TLuid {
        public uint LowPart;
        public int HighPart;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TPrivilege {
        public uint Count;
        public TLuid Luid;
        public uint Flags;
    }

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool LookupPrivilegeValue(string system, string name, out TLuid luid);
    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool AdjustTokenPrivileges(IntPtr token, bool disable, ref TPrivilege state,
        uint size, IntPtr previous, IntPtr needed);
    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFirmwareEnvironmentVariableEx(string name, string guid,
        byte[] buffer, uint size, out uint attributes);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetFirmwareEnvironmentVariableEx(string name, string guid,
        byte[] buffer, uint size, uint attributes);

    public static void EnableAccess() {
        const uint TOKEN_QUERY = 0x0008;
        const uint TOKEN_ADJUST_PRIVILEGES = 0x0020;
        const uint SE_PRIVILEGE_ENABLED = 0x0002;
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, out token)) {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        try {
            var state = new TPrivilege { Count = 1, Flags = SE_PRIVILEGE_ENABLED };
            if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out state.Luid) ||
                !AdjustTokenPrivileges(token, false, ref state, 0, IntPtr.Zero, IntPtr.Zero)) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            int error = Marshal.GetLastWin32Error();
            if (error != 0) { throw new Win32Exception(error); }
        } finally { CloseHandle(token); }
    }

    public static byte[] Read(out uint attributes) {
        const int ERROR_ENVVAR_NOT_FOUND = 203;
        const int ERROR_INSUFFICIENT_BUFFER = 122;
        // Grow from one UTF-16 code unit; do not assume a firmware variable size limit.
        int size = sizeof(ushort);
        while (true) {
            var buffer = new byte[size];
            uint length = GetFirmwareEnvironmentVariableEx(Name, Guid, buffer, (uint)size, out attributes);
            if (length != 0) {
                Array.Resize(ref buffer, checked((int)length));
                return buffer;
            }
            int error = Marshal.GetLastWin32Error();
            if (error == ERROR_ENVVAR_NOT_FOUND) { return null; }
            if (error != ERROR_INSUFFICIENT_BUFFER) { throw new Win32Exception(error); }
            size = checked(size * 2);
        }
    }

    public static void Write(byte[] value, uint attributes) {
        uint length = value == null ? 0 : checked((uint)value.Length);
        if (!SetFirmwareEnvironmentVariableEx(Name, Guid, value, length, attributes)) {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
    }
}
'@

[TAppleSetOsLogVariable]::EnableAccess()
[uint32]$previousAttributes = 0
$previous = [TAppleSetOsLogVariable]::Read([ref]$previousAttributes)
if ($null -ne $previous -and $previousAttributes -ne [TAppleSetOsLogVariable]::Attributes) {
    throw 'Existing variable has unexpected attributes; refusing to replace it.'
}
$value = $null
if (-not $Disable) { $value = [Text.Encoding]::Unicode.GetBytes($Path + [char]0) }
$updated = $false
try {
    if (-not $Disable -or $null -ne $previous) {
        [TAppleSetOsLogVariable]::Write($value, [TAppleSetOsLogVariable]::Attributes)
        $updated = $true
    }
    [uint32]$actualAttributes = 0
    $actual = [TAppleSetOsLogVariable]::Read([ref]$actualAttributes)
    $matches = $null -eq $actual -and $null -eq $value
    if ($null -ne $actual -and $null -ne $value) {
        $matches = [Convert]::ToBase64String($actual) -eq [Convert]::ToBase64String($value)
    }
    if (-not $matches -or
        ($null -ne $actual -and $actualAttributes -ne [TAppleSetOsLogVariable]::Attributes))
    {
        throw 'Firmware variable readback does not match the requested value.'
    }
} catch {
    if ($updated) { [TAppleSetOsLogVariable]::Write($previous, [TAppleSetOsLogVariable]::Attributes) }
    throw
}
[pscustomobject]@{
    Name = [TAppleSetOsLogVariable]::Name
    Guid = [TAppleSetOsLogVariable]::Guid
    Enabled = -not $Disable
    Path = $(if ($Disable) { $null } else { $Path })
    PreviousValueBase64 = $(if ($null -eq $previous) { $null } else { [Convert]::ToBase64String($previous) })
    Verified = $true
}
