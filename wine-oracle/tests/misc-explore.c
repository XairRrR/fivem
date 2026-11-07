/* Exploration groupée : CallNtPowerInformation(SystemPowerInformation), fonctions ETW sans session de
 * traçage, DecryptFileW sur un fichier ordinaire, DwmGetWindowAttribute(DWMWA_CLOAKED),
 * DeregisterEventSource, EnableTrace. Pas de PASS/FAIL. */
#include <windows.h>
#include <powrprof.h>
#include <evntprov.h>
#include <wmistr.h>
#include <evntrace.h>
#include <dwmapi.h>
#include <stdio.h>

typedef struct
{
    ULONG MaxIdlenessAllowed;
    ULONG Idleness;
    ULONG TimeRemaining;
    UCHAR CoolingMode;
} SYS_POWER_INFO;

static const GUID provider = { 0x4f50731a, 0x89cf, 0x4782, { 0xb3, 0xe0, 0xdc, 0xe8, 0xc9, 0x04, 0x76, 0xba } };

int main(void)
{
    SYS_POWER_INFO spi;
    LONG st;
    ULONG r;
    REGHANDLE reg = 0;
    EVENT_DESCRIPTOR desc = { 1, 0, 0, 4, 0, 0, 0 };
    WCHAR tmp[MAX_PATH], file[MAX_PATH];
    HANDLE f, src;
    HWND hwnd;
    DWORD cloaked, err;
    HRESULT hr;
    BOOL b;

    setvbuf( stdout, NULL, _IONBF, 0 );
    for (int i = 0; i < 3; i++)
    {
        memset( &spi, 0xcc, sizeof(spi) );
        st = CallNtPowerInformation( SystemPowerInformation, NULL, 0, &spi, sizeof(spi) );
        printf( "SystemPowerInformation %#lx: MaxIdlenessAllowed %lu Idleness %lu TimeRemaining %lu CoolingMode %u\n",
                st, spi.MaxIdlenessAllowed, spi.Idleness, spi.TimeRemaining, spi.CoolingMode );
        Sleep( 500 );
    }
    st = CallNtPowerInformation( SystemPowerInformation, NULL, 0, &spi, 8 );
    printf( "SystemPowerInformation size 8: %#lx\n", st );

    r = EventRegister( &provider, NULL, NULL, &reg );
    printf( "EventRegister %lu handle %s\n", r, reg ? "set" : "0" );
    printf( "EventEnabled %d EventProviderEnabled %d\n", EventEnabled( reg, &desc ), EventProviderEnabled( reg, 4, 0 ) );
    r = EventWriteTransfer( reg, &desc, NULL, NULL, 0, NULL );
    printf( "EventWriteTransfer %lu\n", r );
    {
        static const char traits[] = { 6, 0, 'w', 'i', 'n', 0 };
        r = EventSetInformation( reg, EventProviderSetTraits, (void *)traits, sizeof(traits) );
        printf( "EventSetInformation(traits) %lu\n", r );
        r = EventSetInformation( reg, 99, NULL, 0 );
        printf( "EventSetInformation(class 99) %lu\n", r );
    }
    printf( "EventUnregister %lu\n", EventUnregister( reg ) );
    SetLastError( 0xdeadbeef );
    r = GetTraceEnableFlags( 0 );
    err = GetLastError();
    printf( "GetTraceEnableFlags(0) %#lx err %lu\n", r, err );
    SetLastError( 0xdeadbeef );
    r = GetTraceEnableFlags( 0x1234 );
    err = GetLastError();
    printf( "GetTraceEnableFlags(bogus) %#lx err %lu\n", r, err );
    r = EnableTrace( TRUE, 0, TRACE_LEVEL_INFORMATION, &provider, 0 );
    printf( "EnableTrace(session 0) %lu\n", r );
    r = EnableTrace( TRUE, 0, TRACE_LEVEL_INFORMATION, &provider, 0x1234 );
    printf( "EnableTrace(bogus session) %lu\n", r );

    src = RegisterEventSourceW( NULL, L"WineOracleTest" );
    printf( "RegisterEventSource %s\n", src ? "ok" : "NULL" );
    SetLastError( 0xdeadbeef );
    b = DeregisterEventSource( src ); err = GetLastError();
    printf( "DeregisterEventSource %d err %lu\n", b, b ? 0 : err );
    SetLastError( 0xdeadbeef );
    b = DeregisterEventSource( NULL ); err = GetLastError();
    printf( "DeregisterEventSource(NULL) %d err %lu\n", b, b ? 0 : err );
    SetLastError( 0xdeadbeef );
    b = DeregisterEventSource( (HANDLE)0x1234 ); err = GetLastError();
    printf( "DeregisterEventSource(bogus) %d err %lu\n", b, b ? 0 : err );

    GetTempPathW( MAX_PATH, tmp );
    swprintf( file, MAX_PATH, L"%swine-decrypt-%lu.txt", tmp, GetCurrentProcessId() );
    f = CreateFileW( file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    WriteFile( f, "hello", 5, &err, NULL );
    CloseHandle( f );
    SetLastError( 0xdeadbeef );
    b = DecryptFileW( file, 0 ); err = GetLastError();
    printf( "DecryptFileW(plain file) %d err %lu, attributes %#lx\n", b, b ? 0 : err, GetFileAttributesW( file ) );
    SetLastError( 0xdeadbeef );
    b = DecryptFileW( L"C:\\does\\not\\exist.txt", 0 ); err = GetLastError();
    printf( "DecryptFileW(missing) %d err %lu\n", b, b ? 0 : err );
    DeleteFileW( file );

    hwnd = CreateWindowA( "static", "cloak", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL );
    cloaked = 0xdeadbeef;
    hr = DwmGetWindowAttribute( hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked) );
    printf( "DWMWA_CLOAKED hidden window %#lx value %#lx\n", hr, cloaked );
    ShowWindow( hwnd, SW_SHOW );
    cloaked = 0xdeadbeef;
    hr = DwmGetWindowAttribute( hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked) );
    printf( "DWMWA_CLOAKED shown window %#lx value %#lx\n", hr, cloaked );
    ShowWindow( hwnd, SW_MINIMIZE );
    cloaked = 0xdeadbeef;
    hr = DwmGetWindowAttribute( hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked) );
    printf( "DWMWA_CLOAKED minimized window %#lx value %#lx\n", hr, cloaked );
    {
        BOOL dark = TRUE;
        hr = DwmSetWindowAttribute( hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark) );
        printf( "DwmSetWindowAttribute(dark mode) %#lx\n", hr );
        MARGINS m = { -1, -1, -1, -1 };
        hr = DwmExtendFrameIntoClientArea( hwnd, &m );
        printf( "DwmExtendFrameIntoClientArea %#lx\n", hr );
    }
    DestroyWindow( hwnd );
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
