/* Exploration de SetCurrentProcessExplicitAppUserModelID / GetCurrentProcessExplicitAppUserModelID. */
#include <windows.h>
#include <shobjidl.h>
#include <stdio.h>

#ifdef _MSC_VER
#define GUARD(what, call) __try { call; } __except (EXCEPTION_EXECUTE_HANDLER) { \
    printf( "%-36s EXCEPTION %08lx\n", what, GetExceptionCode() ); }
#else
#define GUARD(what, call) do { call; } while (0)
#endif

static void get( const char *what )
{
    WCHAR *id = (WCHAR *)0xdeadbeef;
    HRESULT hr = GetCurrentProcessExplicitAppUserModelID( &id );
    printf( "%-36s hr %08lx id %s", what, hr, id == (WCHAR *)0xdeadbeef ? "untouched" : id ? "set" : "NULL" );
    if (hr == S_OK && id && id != (WCHAR *)0xdeadbeef)
    {
        printf( " \"%ls\" (len %u)", id, (unsigned)wcslen( id ) );
        CoTaskMemFree( id );
    }
    printf( "\n" );
}

int main(void)
{
    WCHAR buf[300];
    HRESULT hr;

    setvbuf( stdout, NULL, _IONBF, 0 );
    get( "get before set" );
    GUARD( "get NULL", hr = GetCurrentProcessExplicitAppUserModelID( NULL ); printf( "%-36s hr %08lx\n", "get NULL", hr ) );
    GUARD( "set NULL", hr = SetCurrentProcessExplicitAppUserModelID( NULL ); printf( "%-36s hr %08lx\n", "set NULL", hr ) );
    get( "get after set NULL" );
    hr = SetCurrentProcessExplicitAppUserModelID( L"" );
    printf( "%-36s hr %08lx\n", "set empty", hr );
    get( "get after set empty" );
    for (int len = 127; len <= 130; len++)
    {
        char what[64];
        for (int i = 0; i < len; i++) buf[i] = L'a' + i % 26;
        buf[len] = 0;
        hr = SetCurrentProcessExplicitAppUserModelID( buf );
        snprintf( what, sizeof(what), "set length %d", len );
        printf( "%-36s hr %08lx\n", what, hr );
        get( "  get" );
    }
    hr = SetCurrentProcessExplicitAppUserModelID( L"CitizenFX.FiveM.Client" );
    printf( "%-36s hr %08lx\n", "set CitizenFX.FiveM.Client", hr );
    get( "get" );
    {
        WCHAR *a = NULL, *b = NULL;
        GetCurrentProcessExplicitAppUserModelID( &a );
        GetCurrentProcessExplicitAppUserModelID( &b );
        printf( "two gets return %s pointers\n", a == b ? "the same" : "different" );
        CoTaskMemFree( a );
        CoTaskMemFree( b );
    }
    hr = SetCurrentProcessExplicitAppUserModelID( L"Second.Id" );
    printf( "%-36s hr %08lx\n", "set again", hr );
    get( "get" );
    hr = SetCurrentProcessExplicitAppUserModelID( L"with space" );
    printf( "%-36s hr %08lx\n", "set with a space", hr );
    get( "get" );
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
