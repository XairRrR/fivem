/* SHSetLocalizedName / SHGetLocalizedName et CoInitializeSecurity. Valeurs attendues relevées sur
 * Windows Server 2022 (tests/shell-com-explore.c, oracle run 42312286169). */
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

typedef HRESULT (WINAPI *set_fn)( const WCHAR *, const WCHAR *, int );
typedef HRESULT (WINAPI *get_fn)( const WCHAR *, WCHAR *, UINT, int * );

static DWORD read_file( const WCHAR *path, char *buf, DWORD size )
{
    HANDLE f = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    DWORD got = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ReadFile( f, buf, size - 1, &got, NULL );
    CloseHandle( f );
    buf[got] = 0;
    return got;
}

int main(void)
{
    HMODULE shell32 = LoadLibraryA( "shell32.dll" );
    set_fn pSHSetLocalizedName = (set_fn)GetProcAddress( shell32, "SHSetLocalizedName" );
    get_fn pSHGetLocalizedName = (get_fn)GetProcAddress( shell32, "SHGetLocalizedName" );
    WCHAR tmp[MAX_PATH], dir[MAX_PATH], ini[MAX_PATH], module[MAX_PATH], file[MAX_PATH];
    char buf[1024];
    HRESULT hr;
    HANDLE f;
    int id = 0;

    setvbuf( stdout, NULL, _IONBF, 0 );
    GetTempPathW( MAX_PATH, tmp );
    swprintf( dir, MAX_PATH, L"%swine-localized-%lu", tmp, GetCurrentProcessId() );
    swprintf( ini, MAX_PATH, L"%s\\desktop.ini", dir );
    CreateDirectoryW( dir, NULL );

    hr = pSHSetLocalizedName( dir, L"%SystemRoot%\\system32\\shell32.dll", 21769 );
    CHECK( hr == S_OK, "SHSetLocalizedName (%08lx)", hr );
    read_file( ini, buf, sizeof(buf) );
    CHECK( !strcmp( buf, "[.ShellClassInfo]\r\nLocalizedResourceName=@%SystemRoot%\\system32\\shell32.dll,-21769\r\n" ),
           "desktop.ini content: [%s]", buf );
    /* (Windows: exactly 0x6; Wine always adds FILE_ATTRIBUTE_ARCHIVE to files) */
    CHECK( (GetFileAttributesW( ini ) & ~FILE_ATTRIBUTE_ARCHIVE) == (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM),
           "desktop.ini is hidden and system (%#lx)", GetFileAttributesW( ini ) );
    CHECK( GetFileAttributesW( dir ) == (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY),
           "the folder is read-only (%#lx)", GetFileAttributesW( dir ) );
    module[0] = 0;
    hr = pSHGetLocalizedName( dir, module, MAX_PATH, &id );
    CHECK( hr == S_OK && !wcscmp( module, L"%SystemRoot%\\system32\\shell32.dll" ) && id == 21769,
           "SHGetLocalizedName reads it back (%08lx, %d)", hr, id );
    hr = pSHSetLocalizedName( dir, L"C:\\Some\\Module.exe", 103 );
    read_file( ini, buf, sizeof(buf) );
    CHECK( hr == S_OK && !strcmp( buf, "[.ShellClassInfo]\r\nLocalizedResourceName=@C:\\Some\\Module.exe,-103\r\n" ),
           "a second call replaces the value: [%s]", buf );
    hr = pSHSetLocalizedName( L"C:\\does\\not\\exist", L"x.dll", 1 );
    CHECK( hr == HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ), "missing folder (%08lx)", hr );
    swprintf( file, MAX_PATH, L"%s\\plain.txt", dir );
    f = CreateFileW( file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    CloseHandle( f );
    CHECK( pSHSetLocalizedName( file, L"x.dll", 1 ) == S_OK, "a file: S_OK" );
    DeleteFileW( file );

    SetFileAttributesW( ini, FILE_ATTRIBUTE_NORMAL );
    DeleteFileW( ini );
    SetFileAttributesW( dir, FILE_ATTRIBUTE_NORMAL );
    RemoveDirectoryW( dir );

    hr = CoInitializeSecurity( NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL );
    CHECK( hr == CO_E_NOTINITIALIZED, "CoInitializeSecurity before CoInitialize (%08lx)", hr );
    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    hr = CoInitializeSecurity( NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL );
    CHECK( hr == S_OK, "first CoInitializeSecurity (%08lx)", hr );
    hr = CoInitializeSecurity( NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL );
    CHECK( hr == RPC_E_TOO_LATE, "second CoInitializeSecurity: RPC_E_TOO_LATE (%08lx)", hr );
    hr = CoInitializeSecurity( NULL, 5, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL );
    CHECK( hr == E_INVALIDARG, "5 services without a list: E_INVALIDARG (%08lx)", hr );
    CoUninitialize();

    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
