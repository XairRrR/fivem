/* PowerCreateRequest / PowerSetRequest / PowerClearRequest. Valeurs attendues relevées sur Windows
 * Server 2022 (tests/power-explore.c, oracle run 42309645625). L'effet est vérifié sur le système :
 * `powercfg /requests` sous Windows (administrateur), `systemd-inhibit --list` (inhibiteur logind
 * « idle ») sous Wine ; sans logind (conteneur de CI), seule la sémantique est vérifiée. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures, on_wine;
static char exe_name[MAX_PATH];

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

/* run a command and return its output in buf */
static BOOL run_capture( const char *cmd, char *buf, DWORD size )
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE rd, wr;
    char cmdline[512];
    DWORD got, total = 0;

    if (on_wine)
    {
        /* Wine closes the standard handles of a Unix binary started by a process without a
         * console: let the shell write to a file */
        char path[64];
        FILE *f;

        snprintf( path, sizeof(path), "/tmp/wine-power-test-%lu.txt", GetCurrentProcessId() );
        snprintf( cmdline, sizeof(cmdline), "Z:\\usr\\bin\\sh -c \"%s > %s 2>&1\"", cmd, path );
        if (!CreateProcessA( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi )) return FALSE;
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
        snprintf( cmdline, sizeof(cmdline), "Z:%s", path );
        for (char *c = cmdline; *c; c++) if (*c == '/') *c = '\\';
        /* the Unix process is not a Windows one: wait for its file */
        for (int i = 0; i < 100; i++)
        {
            Sleep( 50 );
            if ((f = fopen( cmdline, "r" )))
            {
                Sleep( 200 );
                total = fread( buf, 1, size - 1, f );
                fclose( f );
                break;
            }
        }
        buf[total] = 0;
        DeleteFileA( cmdline );
        return total > 0;
    }

    if (!CreatePipe( &rd, &wr, &sa, 0 )) return FALSE;
    SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 );
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
    si.hStdOutput = si.hStdError = wr;
    snprintf( cmdline, sizeof(cmdline), "%s", cmd );
    if (!CreateProcessA( NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
    {
        CloseHandle( rd );
        CloseHandle( wr );
        return FALSE;
    }
    CloseHandle( wr );
    while (total < size - 1 && ReadFile( rd, buf + total, size - 1 - total, &got, NULL ) && got) total += got;
    buf[total] = 0;
    WaitForSingleObject( pi.hProcess, 10000 );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    CloseHandle( rd );
    return total > 0;
}

/* 1 if the host lists a request of this process keeping the system awake, 0 if not, -1 if unknown */
static int system_kept_awake(void)
{
    static char out[65536];
    char *line, *next, section[64] = "";
    int found = 0;

    if (!run_capture( on_wine ? "systemd-inhibit --list --no-pager" : "powercfg /requests",
                      out, sizeof(out) ))
        return -1;
    if (on_wine && !strstr( out, "WHO" )) return -1;   /* no logind */
    for (line = out; line && *line; line = next)
    {
        size_t n;
        if ((next = strchr( line, '\n' ))) *next++ = 0;
        n = strlen( line );
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (on_wine)
        {
            if (strstr( line, "Wine: " ) && strstr( line, exe_name ) && strstr( line, "idle" )) found = 1;
        }
        else if (n && line[n - 1] == ':') snprintf( section, sizeof(section), "%s", line );
        else if (!strcmp( section, "SYSTEM:" ) && strstr( line, exe_name )) found = 1;
    }
    return found;
}

static void check_host( int expected, const char *when )
{
    int state = system_kept_awake();
    if (state < 0) printf( "skip: host power requests not visible (%s)\n", when );
    else CHECK( state == expected, "host %s the system awake %s", expected ? "keeps" : "no longer keeps", when );
}

int main(void)
{
    REASON_CONTEXT ctx = { POWER_REQUEST_CONTEXT_VERSION, POWER_REQUEST_CONTEXT_SIMPLE_STRING };
    char path[MAX_PATH], *p;
    HANDLE h, h2, ev;
    int t;

    setvbuf( stdout, NULL, _IONBF, 0 );
    on_wine = !!GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "wine_get_version" );
    GetModuleFileNameA( NULL, path, sizeof(path) );
    snprintf( exe_name, sizeof(exe_name), "%s", (p = strrchr( path, '\\' )) ? p + 1 : path );
    ctx.Reason.SimpleReasonString = (WCHAR *)L"wine-fivem power test";

    h = PowerCreateRequest( NULL );
    CHECK( h && h != INVALID_HANDLE_VALUE, "create with a NULL context" );
    CloseHandle( h );
    {
        REASON_CONTEXT bad = ctx;
        bad.Version = 1;
        SetLastError( 0xdeadbeef );
        CHECK( PowerCreateRequest( &bad ) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER,
               "version 1 fails with ERROR_INVALID_PARAMETER (%lu)", GetLastError() );
        bad.Version = POWER_REQUEST_CONTEXT_VERSION;
        bad.Flags = 0;
        SetLastError( 0xdeadbeef );
        CHECK( PowerCreateRequest( &bad ) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER,
               "flags 0 fails with ERROR_INVALID_PARAMETER (%lu)", GetLastError() );
    }

    h = PowerCreateRequest( &ctx );
    CHECK( h && h != INVALID_HANDLE_VALUE, "create with a simple reason" );
    check_host( 0, "before any request" );

    for (t = 0; t <= 3; t++)
    {
        SetLastError( 0xdeadbeef );
        CHECK( !PowerClearRequest( h, t ) && GetLastError() == ERROR_NOT_SUPPORTED,
               "clear type %d before set fails with ERROR_NOT_SUPPORTED (%lu)", t, GetLastError() );
    }
    SetLastError( 0xdeadbeef );
    CHECK( !PowerSetRequest( h, 4 ) && GetLastError() == ERROR_NOT_SUPPORTED, "type 4 fails with ERROR_NOT_SUPPORTED" );
    SetLastError( 0xdeadbeef );
    CHECK( !PowerSetRequest( h, (POWER_REQUEST_TYPE)-1 ) && GetLastError() == ERROR_NOT_SUPPORTED,
           "type -1 fails with ERROR_NOT_SUPPORTED" );

    CHECK( PowerSetRequest( h, PowerRequestSystemRequired ), "set system" );
    check_host( 1, "after a system request" );
    CHECK( PowerSetRequest( h, PowerRequestSystemRequired ), "set system again" );
    CHECK( PowerClearRequest( h, PowerRequestSystemRequired ), "clear system (1)" );
    check_host( 1, "after setting twice and clearing once" );
    CHECK( PowerClearRequest( h, PowerRequestSystemRequired ), "clear system (2)" );
    SetLastError( 0xdeadbeef );
    CHECK( !PowerClearRequest( h, PowerRequestSystemRequired ) && GetLastError() == ERROR_NOT_SUPPORTED,
           "a third clear fails with ERROR_NOT_SUPPORTED (%lu)", GetLastError() );
    check_host( 0, "after clearing" );

    CHECK( PowerSetRequest( h, PowerRequestDisplayRequired ), "set display" );
    check_host( 1, "after a display request" );
    CHECK( PowerClearRequest( h, PowerRequestDisplayRequired ), "clear display" );

    /* a request closed without being cleared is dropped */
    h2 = PowerCreateRequest( &ctx );
    CHECK( PowerSetRequest( h2, PowerRequestSystemRequired ), "second request: set system" );
    CloseHandle( h2 );
    Sleep( 3000 );
    check_host( 0, "once the request is closed" );
    SetLastError( 0xdeadbeef );
    CHECK( !PowerSetRequest( h2, PowerRequestSystemRequired ) && GetLastError() == ERROR_INVALID_HANDLE,
           "a closed request fails with ERROR_INVALID_HANDLE (%lu)", GetLastError() );
    CloseHandle( h );

    ev = CreateEventW( NULL, TRUE, FALSE, NULL );
    SetLastError( 0xdeadbeef );
    CHECK( !PowerSetRequest( ev, PowerRequestSystemRequired ) && GetLastError() == ERROR_INVALID_HANDLE,
           "an event is not a power request (%lu)", GetLastError() );
    CloseHandle( ev );
    SetLastError( 0xdeadbeef );
    CHECK( !PowerSetRequest( NULL, PowerRequestSystemRequired ) && GetLastError() == ERROR_INVALID_HANDLE,
           "NULL fails with ERROR_INVALID_HANDLE (%lu)", GetLastError() );

    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
