/* SetCurrentProcessExplicitAppUserModelID / GetCurrentProcessExplicitAppUserModelID. Valeurs attendues
 * relevées sur Windows Server 2022 (tests/aumid-explore.c, oracle run 42311670965). */
#include <windows.h>
#include <shobjidl.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

/* S_OK and the ID compares equal to expected, or the HRESULT */
static HRESULT get_is( const WCHAR *expected )
{
    WCHAR *id = (WCHAR *)0xdeadbeef;
    HRESULT hr = GetCurrentProcessExplicitAppUserModelID( &id );

    if (hr == S_OK)
    {
        int same = expected && id && !wcscmp( id, expected );
        CoTaskMemFree( id );
        return same ? S_OK : S_FALSE;
    }
    return id ? (HRESULT)0xbad : hr;   /* the pointer must be NULL on failure */
}

int main(void)
{
    WCHAR buf[200];
    WCHAR *a = NULL, *b = NULL;

    setvbuf( stdout, NULL, _IONBF, 0 );
    CHECK( get_is( NULL ) == E_FAIL, "no ID: E_FAIL and NULL" );
    CHECK( SetCurrentProcessExplicitAppUserModelID( L"CitizenFX.FiveM.Client" ) == S_OK, "set" );
    CHECK( get_is( L"CitizenFX.FiveM.Client" ) == S_OK, "get returns it" );
    GetCurrentProcessExplicitAppUserModelID( &a );
    GetCurrentProcessExplicitAppUserModelID( &b );
    CHECK( a && b && a != b, "each get returns a new copy" );
    CoTaskMemFree( a );
    CoTaskMemFree( b );

    for (int i = 0; i < 127; i++) buf[i] = L'a' + i % 26;
    buf[127] = 0;
    CHECK( SetCurrentProcessExplicitAppUserModelID( buf ) == S_OK && get_is( buf ) == S_OK, "127 characters" );
    buf[127] = L'x'; buf[128] = 0;
    CHECK( SetCurrentProcessExplicitAppUserModelID( buf ) == E_INVALIDARG, "128 characters: E_INVALIDARG" );
    buf[127] = 0;
    CHECK( get_is( buf ) == S_OK, "the previous ID is kept" );
    CHECK( SetCurrentProcessExplicitAppUserModelID( L"with space" ) == S_OK && get_is( L"with space" ) == S_OK,
           "spaces are allowed" );
    CHECK( SetCurrentProcessExplicitAppUserModelID( L"" ) == S_OK, "set empty" );
    CHECK( get_is( NULL ) == E_FAIL, "an empty ID clears it" );

    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
