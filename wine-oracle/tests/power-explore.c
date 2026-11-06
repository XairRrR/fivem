/* Exploration de PowerCreateRequest / PowerSetRequest / PowerClearRequest : ce que Windows renvoie
 * et ce que voit le système (powercfg /requests, administrateur). Pas de PASS/FAIL. */
#include <windows.h>
#include <stdio.h>

#ifdef _MSC_VER
#define GUARD(what, call) __try { call; } __except (EXCEPTION_EXECUTE_HANDLER) { \
    printf( "%-40s EXCEPTION %08lx\n", what, GetExceptionCode() ); }
#else
#define GUARD(what, call) do { call; } while (0)
#endif

static void requests( const char *when )
{
    char line[512];
    FILE *f = _popen( "powercfg /requests", "r" );
    int section_printed = 0;
    char section[64] = "";

    printf( "powercfg /requests (%s):\n", when );
    if (!f) { printf( "  (powercfg not available)\n" ); return; }
    while (fgets( line, sizeof(line), f ))
    {
        size_t n = strlen( line );
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (line[n - 1] == ':' ) { snprintf( section, sizeof(section), "%s", line ); section_printed = 0; continue; }
        if (!strcmp( line, "None." )) continue;
        if (!section_printed) { printf( "  %s\n", section ); section_printed = 1; }
        printf( "    %s\n", line );
    }
    _pclose( f );
}

static void result( const char *what, BOOL r )
{
    printf( "%-40s %d err %lu\n", what, r, r ? 0 : GetLastError() );
}

int main(void)
{
    REASON_CONTEXT ctx = { POWER_REQUEST_CONTEXT_VERSION, POWER_REQUEST_CONTEXT_SIMPLE_STRING };
    HANDLE h, h2, ev;
    int t;

    setvbuf( stdout, NULL, _IONBF, 0 );
    ctx.Reason.SimpleReasonString = (WCHAR *)L"wine oracle power request";

    SetLastError( 0xdeadbeef );
    GUARD( "create NULL", h = PowerCreateRequest( NULL );
           printf( "%-40s handle %s err %lu\n", "create NULL", h && h != INVALID_HANDLE_VALUE ? "ok" : "bad", GetLastError() ) );
    {
        REASON_CONTEXT bad = ctx;
        bad.Version = 1;
        SetLastError( 0xdeadbeef );
        h = PowerCreateRequest( &bad );
        printf( "%-40s handle %p err %lu\n", "create version 1", h, GetLastError() );
        bad.Version = POWER_REQUEST_CONTEXT_VERSION;
        bad.Flags = 0;
        SetLastError( 0xdeadbeef );
        h = PowerCreateRequest( &bad );
        printf( "%-40s handle %p err %lu\n", "create flags 0", h, GetLastError() );
    }

    SetLastError( 0xdeadbeef );
    h = PowerCreateRequest( &ctx );
    printf( "%-40s handle %s err %lu\n", "create simple string", h && h != INVALID_HANDLE_VALUE ? "ok" : "bad",
            GetLastError() );
    {
        DWORD flags = 0;
        printf( "  GetHandleInformation %d flags %lu\n", GetHandleInformation( h, &flags ), flags );
        printf( "  WaitForSingleObject(0) %lu err %lu\n", WaitForSingleObject( h, 0 ), GetLastError() );
    }

    requests( "created, nothing set" );
    for (t = PowerRequestDisplayRequired; t <= 3; t++)
    {
        char what[64];
        SetLastError( 0xdeadbeef );
        snprintf( what, sizeof(what), "clear type %d before set", t );
        result( what, PowerClearRequest( h, t ) );
    }
    for (t = PowerRequestDisplayRequired; t <= 5; t++)
    {
        char what[64];
        SetLastError( 0xdeadbeef );
        snprintf( what, sizeof(what), "set type %d", t );
        result( what, PowerSetRequest( h, t ) );
    }
    SetLastError( 0xdeadbeef );
    result( "set type -1", PowerSetRequest( h, (POWER_REQUEST_TYPE)-1 ) );
    requests( "display, system, away, execution set" );

    SetLastError( 0xdeadbeef );
    result( "set display again", PowerSetRequest( h, PowerRequestDisplayRequired ) );
    SetLastError( 0xdeadbeef );
    result( "clear display (1)", PowerClearRequest( h, PowerRequestDisplayRequired ) );
    requests( "display set twice, cleared once" );
    SetLastError( 0xdeadbeef );
    result( "clear display (2)", PowerClearRequest( h, PowerRequestDisplayRequired ) );
    SetLastError( 0xdeadbeef );
    result( "clear display (3)", PowerClearRequest( h, PowerRequestDisplayRequired ) );
    requests( "display cleared twice" );

    /* a second request object for the same type, then closing without clearing */
    h2 = PowerCreateRequest( &ctx );
    SetLastError( 0xdeadbeef );
    result( "second request: set system", PowerSetRequest( h2, PowerRequestSystemRequired ) );
    SetLastError( 0xdeadbeef );
    result( "first request: clear system", PowerClearRequest( h, PowerRequestSystemRequired ) );
    requests( "system: first cleared, second set" );
    CloseHandle( h2 );
    requests( "second closed without clearing" );
    CloseHandle( h );
    requests( "first closed (away, execution still set)" );

    ev = CreateEventW( NULL, TRUE, FALSE, NULL );
    SetLastError( 0xdeadbeef );
    GUARD( "set on an event", result( "set on an event", PowerSetRequest( ev, PowerRequestSystemRequired ) ) );
    SetLastError( 0xdeadbeef );
    GUARD( "clear on an event", result( "clear on an event", PowerClearRequest( ev, PowerRequestSystemRequired ) ) );
    CloseHandle( ev );
    SetLastError( 0xdeadbeef );
    GUARD( "set NULL", result( "set NULL", PowerSetRequest( NULL, PowerRequestSystemRequired ) ) );
    SetLastError( 0xdeadbeef );
    GUARD( "set closed handle", result( "set closed handle", PowerSetRequest( h, PowerRequestSystemRequired ) ) );

    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
