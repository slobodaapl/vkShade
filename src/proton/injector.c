#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "bridge_identity.h"

#define WAIT_LIMIT_MS 10000
#define PATH_CAPACITY 32768

typedef WCHAR*(WINAPI* wine_get_dos_file_name_fn)(const char*);
typedef DWORD(WINAPI* get_module_file_name_ex_fn)(HANDLE, HMODULE, LPWSTR, DWORD);

static int parse_number(const WCHAR* s, unsigned base, uint64_t* out)
{
    uint64_t value = 0;
    unsigned digit;
    if (base == 16 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X'))
        s += 2;
    if (!*s)
        return 0;
    while (*s)
    {
        if (*s >= L'0' && *s <= L'9')
            digit = *s - L'0';
        else if (*s >= L'a' && *s <= L'f')
            digit = *s - L'a' + 10;
        else if (*s >= L'A' && *s <= L'F')
            digit = *s - L'A' + 10;
        else
            return 0;
        if (digit >= base || value > (UINT64_MAX - digit) / base)
            return 0;
        value = value * base + digit;
        ++s;
    }
    *out = value;
    return value != 0;
}

static int module_image(HMODULE module, uintptr_t* base, DWORD* size)
{
    const IMAGE_DOS_HEADER*   dos = (const IMAGE_DOS_HEADER*) module;
    const IMAGE_NT_HEADERS64* nt;
    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return 0;
    nt = (const IMAGE_NT_HEADERS64*) ((const BYTE*) module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !nt->OptionalHeader.SizeOfImage)
        return 0;
    *base = (uintptr_t) module;
    *size = nt->OptionalHeader.SizeOfImage;
    return 1;
}

static int remote_read(HANDLE process, uintptr_t base, DWORD size, uint64_t offset, void* buffer, size_t length)
{
    SIZE_T read;
    if (offset > size || length > (uint64_t) size - offset || base > UINTPTR_MAX - offset || length > UINTPTR_MAX - (base + offset))
        return 0;
    return ReadProcessMemory(process, (const void*) (base + (uintptr_t) offset), buffer, length, &read) && read == length;
}

static int remote_export(HANDLE process, uintptr_t base, DWORD module_size, uintptr_t* address)
{
    static const char      wanted[] = "vkshade_companion_initialize_v1";
    IMAGE_DOS_HEADER       dos;
    IMAGE_NT_HEADERS64     nt;
    IMAGE_EXPORT_DIRECTORY exports;
    IMAGE_SECTION_HEADER   section;
    IMAGE_DATA_DIRECTORY   dir;
    uint64_t               sections_offset;
    DWORD                  i, function = 0, image_size;
    int                    matches = 0, executable = 0;
    if (!remote_read(process, base, module_size, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0
        || !remote_read(process, base, module_size, (DWORD) dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.FileHeader.SizeOfOptionalHeader < sizeof(nt.OptionalHeader)
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT
        || !nt.FileHeader.NumberOfSections || !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > module_size)
        return 0;
    image_size = nt.OptionalHeader.SizeOfImage;
    if ((uint64_t) (DWORD) dos.e_lfanew + sizeof(nt) > image_size)
        return 0;
    sections_offset = (uint64_t) (DWORD) dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    if (sections_offset > image_size || nt.FileHeader.NumberOfSections > (image_size - sections_offset) / sizeof(section))
        return 0;
    dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(exports) || dir.VirtualAddress > image_size || dir.Size > image_size - dir.VirtualAddress
        || !remote_read(process, base, image_size, dir.VirtualAddress, &exports, sizeof(exports)) || !exports.NumberOfNames
        || !exports.NumberOfFunctions || !exports.AddressOfNames || !exports.AddressOfNameOrdinals || !exports.AddressOfFunctions
        || exports.AddressOfNames > image_size || exports.NumberOfNames > (image_size - exports.AddressOfNames) / sizeof(DWORD)
        || exports.AddressOfNameOrdinals > image_size || exports.NumberOfNames > (image_size - exports.AddressOfNameOrdinals) / sizeof(WORD)
        || exports.AddressOfFunctions > image_size || exports.NumberOfFunctions > (image_size - exports.AddressOfFunctions) / sizeof(DWORD))
        return 0;
    for (i = 0; i < exports.NumberOfNames; ++i)
    {
        DWORD    name_rva;
        WORD     ordinal;
        char     chunk[1];
        uint64_t pos  = 0;
        int      same = 1, terminated = 0;
        if (!remote_read(process, base, image_size, (uint64_t) exports.AddressOfNames + (uint64_t) i * sizeof(name_rva), &name_rva, sizeof(name_rva))
            || !remote_read(
                process, base, image_size, (uint64_t) exports.AddressOfNameOrdinals + (uint64_t) i * sizeof(ordinal), &ordinal, sizeof(ordinal))
            || ordinal >= exports.NumberOfFunctions || name_rva >= image_size)
            return 0;
        while ((uint64_t) name_rva + pos < image_size)
        {
            size_t j, count = sizeof(chunk);
            if (count > image_size - ((uint64_t) name_rva + pos))
                count = (size_t) (image_size - ((uint64_t) name_rva + pos));
            if (!remote_read(process, base, image_size, (uint64_t) name_rva + pos, chunk, count))
                return 0;
            for (j = 0; j < count; ++j, ++pos)
            {
                if (pos >= sizeof(wanted) || chunk[j] != wanted[pos])
                    same = 0;
                if (!chunk[j])
                {
                    terminated = 1;
                    break;
                }
            }
            if (terminated)
                break;
        }
        if (!terminated)
            return 0;
        if (same && pos == sizeof(wanted) - 1)
        {
            if (++matches != 1
                || !remote_read(process,
                                base,
                                image_size,
                                (uint64_t) exports.AddressOfFunctions + (uint64_t) ordinal * sizeof(function),
                                &function,
                                sizeof(function)))
                return 0;
        }
    }
    if (matches != 1 || !function || function >= image_size || (function >= dir.VirtualAddress && function - dir.VirtualAddress < dir.Size))
        return 0;
    for (i = 0; i < nt.FileHeader.NumberOfSections; ++i)
    {
        DWORD length;
        if (!remote_read(process, base, image_size, sections_offset + (uint64_t) i * sizeof(section), &section, sizeof(section)))
            return 0;
        length = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
        if (section.VirtualAddress > image_size || length > image_size - section.VirtualAddress)
            return 0;
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && function >= section.VirtualAddress && function - section.VirtualAddress < length)
            ++executable;
    }
    if (executable != 1 || base > UINTPTR_MAX - function)
        return 0;
    *address = base + function;
    return 1;
}

static int
find_remote_module(DWORD pid, HANDLE process, const WCHAR* key, int match_path, get_module_file_name_ex_fn get_name, uintptr_t* base, DWORD* size)
{
    HANDLE         snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W entry;
    WCHAR*         path  = NULL;
    int            found = 0, failed = 0;
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    if (match_path)
    {
        path = (WCHAR*) malloc(PATH_CAPACITY * sizeof(*path));
        if (!path)
        {
            CloseHandle(snapshot);
            return 0;
        }
    }
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot, &entry))
        failed = 1;
    else
        do
        {
            const WCHAR* name = entry.szModule;
            if (match_path)
            {
                DWORD length = get_name(process, entry.hModule, path, PATH_CAPACITY);
                if (!length || length >= PATH_CAPACITY - 1)
                {
                    failed = 1;
                    break;
                }
                name = path;
            }
            if (CompareStringOrdinal(name, -1, key, -1, TRUE) == CSTR_EQUAL)
            {
                ++found;
                *base = (uintptr_t) entry.modBaseAddr;
                *size = entry.modBaseSize;
            }
        } while (Module32NextW(snapshot, &entry));
    if (!failed && GetLastError() != ERROR_NO_MORE_FILES)
        failed = 1;
    free(path);
    CloseHandle(snapshot);
    return !failed && found == 1 && *size != 0;
}

static int wait_thread(HANDLE thread, DWORD* result)
{
    DWORD wait = WaitForSingleObject(thread, WAIT_LIMIT_MS);
    if (wait == WAIT_TIMEOUT)
        return -1;
    if (wait != WAIT_OBJECT_0 || !GetExitCodeThread(thread, result))
        return 0;
    return 1;
}

static int injector_main(int argc, WCHAR** argv)
{
    uint64_t                   pid_value, address, cookie;
    vkshade_proton_identity    identity;
    HANDLE                     process = NULL, thread = NULL;
    HMODULE                    kernel, psapi          = NULL;
    wine_get_dos_file_name_fn  wine_path;
    get_module_file_name_ex_fn get_name;
    WCHAR *                    dos_path = NULL, *full_path = NULL;
    char*                      unix_path   = NULL;
    LPVOID                     remote_path = NULL;
    SIZE_T                     transferred, path_bytes;
    uintptr_t                  local_base, remote_base, rva;
    DWORD                      local_size, remote_size, result;
    MEMORY_BASIC_INFORMATION   info;
    FARPROC                    load_library;
    uintptr_t                  initialize;
    WCHAR                      owner_path[PATH_CAPACITY], *owner_name;
    int                        utf8_size, status = 1, waited, path_in_use = 0;

    if (argc != 5 || !parse_number(argv[1], 10, &pid_value) || pid_value > UINT32_MAX || !parse_number(argv[2], 16, &address)
        || !parse_number(argv[3], 16, &cookie) || argv[4][0] != L'/')
    {
        fwprintf(stderr, L"usage: injector PID identity-address-hex cookie-hex /absolute/unix/path\n");
        return 1;
    }
    process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD, FALSE, (DWORD) pid_value);
    if (!process)
    {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        goto done;
    }
    if (!ReadProcessMemory(process, (const void*) (uintptr_t) address, &identity, sizeof(identity), &transferred) || transferred != sizeof(identity)
        || identity.cookie != cookie || !identity.callback_address)
    {
        fwprintf(stderr, L"target identity validation failed\n");
        goto done;
    }
    kernel    = GetModuleHandleW(L"kernel32.dll");
    wine_path = kernel ? (wine_get_dos_file_name_fn) GetProcAddress(kernel, "wine_get_dos_file_name") : NULL;
    if (!wine_path)
    {
        fwprintf(stderr, L"Wine path conversion API unavailable\n");
        goto done;
    }
    utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[4], -1, NULL, 0, NULL, NULL);
    if (!utf8_size || !(unix_path = (char*) malloc((size_t) utf8_size))
        || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[4], -1, unix_path, utf8_size, NULL, NULL) || !(dos_path = wine_path(unix_path)))
    {
        fwprintf(stderr, L"cannot convert companion path through Wine: %lu\n", GetLastError());
        goto done;
    }
    full_path = (WCHAR*) malloc(PATH_CAPACITY * sizeof(*full_path));
    if (!full_path)
    {
        fwprintf(stderr, L"out of memory\n");
        goto done;
    }
    result = GetFullPathNameW(dos_path, PATH_CAPACITY, full_path, NULL);
    if (!result || result >= PATH_CAPACITY)
    {
        fwprintf(stderr, L"invalid or oversized companion path\n");
        goto done;
    }
    psapi    = LoadLibraryW(L"psapi.dll");
    get_name = psapi ? (get_module_file_name_ex_fn) GetProcAddress(psapi, "GetModuleFileNameExW") : NULL;
    if (!get_name)
    {
        fwprintf(stderr, L"remote module path API unavailable\n");
        goto done;
    }
    load_library = GetProcAddress(kernel, "LoadLibraryW");
    if (!load_library || !VirtualQuery((const void*) load_library, &info, sizeof(info))
        || !module_image((HMODULE) info.AllocationBase, &local_base, &local_size) || (uintptr_t) load_library < local_base
        || (uintptr_t) load_library - local_base >= local_size)
    {
        fwprintf(stderr, L"cannot identify LoadLibraryW owner\n");
        goto done;
    }
    result = GetModuleFileNameW((HMODULE) info.AllocationBase, owner_path, PATH_CAPACITY);
    if (!result || result >= PATH_CAPACITY - 1)
    {
        fwprintf(stderr, L"cannot identify LoadLibraryW owner path\n");
        goto done;
    }
    owner_name = wcsrchr(owner_path, L'\\');
    owner_name = owner_name ? owner_name + 1 : owner_path;
    rva        = (uintptr_t) load_library - local_base;
    if (!find_remote_module((DWORD) pid_value, process, owner_name, 0, get_name, &remote_base, &remote_size) || rva >= remote_size
        || remote_base > UINTPTR_MAX - rva)
    {
        fwprintf(stderr, L"target LoadLibraryW owner missing, ambiguous, or invalid\n");
        goto done;
    }
    path_bytes  = (wcslen(full_path) + 1) * sizeof(*full_path);
    remote_path = VirtualAllocEx(process, NULL, path_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path || !WriteProcessMemory(process, remote_path, full_path, path_bytes, &transferred) || transferred != path_bytes)
    {
        fwprintf(stderr, L"cannot write target DLL path: %lu\n", GetLastError());
        goto done;
    }
    thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE) (remote_base + rva), remote_path, 0, NULL);
    if (!thread)
    {
        fwprintf(stderr, L"cannot start target loader: %lu\n", GetLastError());
        goto done;
    }
    path_in_use = 1;
    waited      = wait_thread(thread, &result);
    if (waited == 1)
        path_in_use = 0;
    if (waited != 1)
    {
        fwprintf(stderr, L"target DLL load wait failed or timed out\n");
        goto done;
    }
    CloseHandle(thread);
    thread = NULL;
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    remote_path = NULL;
    if (!find_remote_module((DWORD) pid_value, process, full_path, 1, get_name, &remote_base, &remote_size)
        || !remote_export(process, remote_base, remote_size, &initialize))
    {
        fwprintf(stderr, L"exact target companion path missing, ambiguous, or invalid\n");
        goto done;
    }
    thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE) initialize, (void*) (uintptr_t) address, 0, NULL);
    if (!thread)
    {
        fwprintf(stderr, L"cannot start companion initialization: %lu\n", GetLastError());
        goto done;
    }
    waited = wait_thread(thread, &result);
    if (waited != 1 || result != 0)
    {
        fwprintf(stderr, L"companion initialization failed or timed out\n");
        goto done;
    }
    status = 0;
done:
    if (thread)
        CloseHandle(thread);
    if (remote_path && !path_in_use)
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    if (psapi)
        FreeLibrary(psapi);
    if (dos_path)
        HeapFree(GetProcessHeap(), 0, dos_path);
    free(full_path);
    free(unix_path);
    if (process)
        CloseHandle(process);
    return status;
}

void mainCRTStartup(void)
{
    int     argc, status;
    WCHAR** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        fwprintf(stderr, L"cannot parse command line: %lu\n", GetLastError());
        ExitProcess(1);
    }
    status = injector_main(argc, argv);
    LocalFree(argv);
    ExitProcess((UINT) status);
}
