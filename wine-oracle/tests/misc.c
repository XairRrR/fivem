/* Valeurs relevées sur Windows Server 2022 (tests/misc-explore.c, oracle run 42358171681) :
 * SystemPowerInformation (lu chaque seconde par Social Club), ETW sans session de traçage,
 * DeregisterEventSource. */
#include <windows.h>
#include <powrprof.h>
#include <evntprov.h>
#include <wmistr.h>
#include <evntrace.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

typedef struct { ULONG MaxIdlenessAllowed, Idleness, TimeRemaining; UCHAR CoolingMode; } SYS_POWER_INFO;
static const GUID provider = { 0x4f50731a, 0x89cf, 0x4782, { 0xb3, 0xe0, 0xdc, 0xe8, 0xc9, 0x04, 0x76, 0xba } };

int main(void)
{
    SYS_POWER_INFO spi;
    REGHANDLE reg = 0;
    EVENT_DESCRIPTOR desc = { 1, 0, 0, 4, 0, 0, 0 };
    static const char traits[] = { 6, 0, 'w', 'i', 'n', 0 };
    LONG st;
    ULONG r;
    DWORD err;
    BOOL b;

    setvbuf( stdout, NULL, _IONBF, 0 );
    CallNtPowerInformation( SystemPowerInformation, NULL, 0, &spi, sizeof(spi) );
    Sleep( 500 );
    memset( &spi, 0xcc, sizeof(spi) );
    st = CallNtPowerInformation( SystemPowerInformation, NULL, 0, &spi, sizeof(spi) );
    CHECK( !st && spi.Idleness <= 100 && !spi.MaxIdlenessAllowed && !spi.TimeRemaining && !spi.CoolingMode,
           "SystemPowerInformation: idleness %lu%%, other fields 0 (%#lx)", spi.Idleness, st );
    st = CallNtPowerInformation( SystemPowerInformation, NULL, 0, &spi, 8 );
    CHECK( st == (LONG)0xc0000023, "a small buffer gives STATUS_BUFFER_TOO_SMALL (%#lx)", st );

    CHECK( !EventRegister( &provider, NULL, NULL, &reg ) && reg, "EventRegister" );
    CHECK( !EventEnabled( reg, &desc ) && !EventProviderEnabled( reg, 4, 0 ), "not enabled without a session" );
    CHECK( !EventWriteTransfer( reg, &desc, NULL, NULL, 0, NULL ), "EventWriteTransfer succeeds" );
    CHECK( !EventSetInformation( reg, EventProviderSetTraits, (void *)traits, sizeof(traits) ), "EventSetInformation(traits)" );
    r = EventSetInformation( reg, 99, NULL, 0 );
    CHECK( r == ERROR_NOT_SUPPORTED, "EventSetInformation(unknown class): ERROR_NOT_SUPPORTED (%lu)", r );
    EventUnregister( reg );
    SetLastError( 0xdeadbeef );
    r = GetTraceEnableFlags( 0 );
    err = GetLastError();
    CHECK( !r && err == ERROR_INVALID_HANDLE, "GetTraceEnableFlags(0): 0, ERROR_INVALID_HANDLE (%lu)", err );
    r = EnableTrace( TRUE, 0, TRACE_LEVEL_INFORMATION, &provider, 0 );
    CHECK( r == ERROR_INVALID_PARAMETER, "EnableTrace(session 0): ERROR_INVALID_PARAMETER (%lu)", r );
    r = EnableTrace( TRUE, 0, TRACE_LEVEL_INFORMATION, &provider, 0x1234 );
    CHECK( r == ERROR_INVALID_PARAMETER, "EnableTrace(unknown session): ERROR_INVALID_PARAMETER (%lu)", r );

    {
        HANDLE src = RegisterEventSourceW( NULL, L"WineOracleTest" );
        CHECK( src && DeregisterEventSource( src ), "RegisterEventSource / DeregisterEventSource" );
    }
    SetLastError( 0xdeadbeef );
    b = DeregisterEventSource( NULL );
    err = GetLastError();
    CHECK( !b && err == ERROR_INVALID_HANDLE, "DeregisterEventSource(NULL): ERROR_INVALID_HANDLE (%lu)", err );

    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
