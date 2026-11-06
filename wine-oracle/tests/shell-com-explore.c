/* Exploration : SHSetLocalizedName (desktop.ini, attributs, SHGetLocalizedName) et CoInitializeSecurity
 * (premier appel, appels suivants, avant CoInitialize). Pas de PASS/FAIL. */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdio.h>

typedef HRESULT (WINAPI *set_fn)( const WCHAR *, const WCHAR *, int );
typedef HRESULT (WINAPI *get_fn)( const WCHAR *, WCHAR *, UINT, int * );

static void dump_ini( const WCHAR *dir )
{
    WCHAR path[MAX_PATH];
    char buf[1024];
    DWORD got = 0, attrs;
    HANDLE f;

    swprintf( path, MAX_PATH, L"%s\\desktop.ini", dir );
    attrs = GetFileAttributesW( path );
    printf( "  desktop.ini attributes %#lx\n", attrs );
    if ((f = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL )) == INVALID_HANDLE_VALUE)
        return;
    ReadFile( f, buf, sizeof(buf) - 1, &got, NULL );
    CloseHandle( f );
    printf( "  desktop.ini %lu bytes:", got );
    for (DWORD i = 0; i < got; i++)
    {
        unsigned char c = buf[i];
        if (c >= 32 && c < 127) printf( "%c", c );
        else printf( "\\x%02x", c );
    }
    printf( "\n" );
}

int main(void)
{
    HMODULE shell32 = LoadLibraryA( "shell32.dll" );
    set_fn pSHSetLocalizedName = (set_fn)GetProcAddress( shell32, "SHSetLocalizedName" );
    get_fn pSHGetLocalizedName = (get_fn)GetProcAddress( shell32, "SHGetLocalizedName" );
    WCHAR tmp[MAX_PATH], dir[MAX_PATH], module[MAX_PATH];
    HRESULT hr;
    int id;

    setvbuf( stdout, NULL, _IONBF, 0 );
    GetTempPathW( MAX_PATH, tmp );
    swprintf( dir, MAX_PATH, L"%swine-localized-%lu", tmp, GetCurrentProcessId() );
    CreateDirectoryW( dir, NULL );
    printf( "folder attributes before %#lx\n", GetFileAttributesW( dir ) );

    hr = pSHSetLocalizedName( dir, L"%SystemRoot%\\system32\\shell32.dll", 21769 );
    printf( "SHSetLocalizedName(env path) hr %08lx, folder attributes %#lx\n", hr, GetFileAttributesW( dir ) );
    dump_ini( dir );
    module[0] = 0; id = 0;
    hr = pSHGetLocalizedName ? pSHGetLocalizedName( dir, module, MAX_PATH, &id ) : E_NOTIMPL;
    printf( "SHGetLocalizedName hr %08lx module \"%ls\" id %d\n", hr, module, id );

    hr = pSHSetLocalizedName( dir, L"C:\\Some\\Module.exe", 103 );
    printf( "SHSetLocalizedName(again, plain path) hr %08lx, folder attributes %#lx\n", hr, GetFileAttributesW( dir ) );
    dump_ini( dir );

    hr = pSHSetLocalizedName( (WCHAR *)L"C:\\does\\not\\exist", L"x.dll", 1 );
    printf( "SHSetLocalizedName(missing folder) hr %08lx\n", hr );
    {
        WCHAR file[MAX_PATH];
        HANDLE f;
        swprintf( file, MAX_PATH, L"%s\\plain.txt", dir );
        f = CreateFileW( file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
        CloseHandle( f );
        hr = pSHSetLocalizedName( file, L"x.dll", 1 );
        printf( "SHSetLocalizedName(a file) hr %08lx\n", hr );
        DeleteFileW( file );
    }

    /* CoInitializeSecurity */
    hr = CoInitializeSecurity( NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL );
    printf( "CoInitializeSecurity before CoInitialize hr %08lx\n", hr );
    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    hr = CoInitializeSecurity( NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL );
    printf( "CoInitializeSecurity first hr %08lx\n", hr );
    hr = CoInitializeSecurity( NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL );
    printf( "CoInitializeSecurity second hr %08lx\n", hr );
    hr = CoInitializeSecurity( NULL, 5, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL );
    printf( "CoInitializeSecurity cAuthSvc 5 NULL list hr %08lx\n", hr );
    CoUninitialize();

    {
        WCHAR ini[MAX_PATH];
        swprintf( ini, MAX_PATH, L"%s\\desktop.ini", dir );
        SetFileAttributesW( ini, FILE_ATTRIBUTE_NORMAL );
        DeleteFileW( ini );
        SetFileAttributesW( dir, FILE_ATTRIBUTE_NORMAL );
        RemoveDirectoryW( dir );
    }
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
