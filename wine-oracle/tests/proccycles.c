/* QueryProcessCycleTime / ProcessCycleTime d'un autre processus, ouvert avec seulement
 * PROCESS_QUERY_LIMITED_INFORMATION (suffisant sous Windows) : non nul, croissant, cohérent avec
 * GetProcessTimes, qui doit donner les temps de ce processus et non ceux de l'appelant. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

static ULONG64 cpu_100ns( HANDLE process )
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes( process, &c, &e, &k, &u )) return 0;
    return ((ULONG64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONG64)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

int main( int argc, char **argv )
{
    char exe[MAX_PATH], cmd[MAX_PATH + 16];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE limited;
    ULONG64 c1 = 0, c2 = 0, t1, t2, self = 0;
    DWORD start;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (argc == 2 && !strcmp( argv[1], "spin" ))
    {
        volatile ULONG64 x = 0;
        start = GetTickCount();
        while (GetTickCount() - start < 4000) x++;
        return 0;
    }

    CHECK( QueryProcessCycleTime( GetCurrentProcess(), &self ) && self > 0, "own process: %llu cycles", self );

    GetModuleFileNameA( NULL, exe, sizeof(exe) );
    snprintf( cmd, sizeof(cmd), "\"%s\" spin", exe );
    if (!CreateProcessA( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        printf( "FAIL: CreateProcess %lu\nRESULT: FAIL\n", GetLastError() );
        return 0;
    }
    limited = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.dwProcessId );
    CHECK( limited != NULL, "open the child with PROCESS_QUERY_LIMITED_INFORMATION" );

    Sleep( 1000 );
    CHECK( QueryProcessCycleTime( limited, &c1 ) && c1 > 0, "child: %llu cycles after 1 s", c1 );
    t1 = cpu_100ns( pi.hProcess );
    Sleep( 1000 );
    CHECK( QueryProcessCycleTime( limited, &c2 ) && c2 > c1, "child: cycles increase (%llu -> %llu)", c1, c2 );
    t2 = cpu_100ns( pi.hProcess );
    /* GetProcessTimes of another process: the child's times, not ours (Wine used to return the
     * caller's); the child burned ~1 s of CPU between the two reads, we almost none */
    CHECK( t2 > t1 + 5000000, "GetProcessTimes of the child advances with its CPU use (%llu -> %llu)", t1, t2 );
    CHECK( cpu_100ns( GetCurrentProcess() ) < t2, "and differs from ours" );
    /* the cycles grow at the TSC rate while the child burns CPU: between 0.3 and 10 GHz */
    if (t2 > t1)
    {
        double ghz = (double)(c2 - c1) / ((t2 - t1) * 100.0);
        CHECK( ghz > 0.3 && ghz < 10, "cycles per CPU nanosecond of the child: %.2f", ghz );
    }

    {
        HANDLE none = OpenProcess( SYNCHRONIZE, FALSE, pi.dwProcessId );
        ULONG64 c = 0;
        SetLastError( 0xdeadbeef );
        CHECK( !QueryProcessCycleTime( none, &c ) && GetLastError() == ERROR_ACCESS_DENIED,
               "a handle without query access fails with ERROR_ACCESS_DENIED (%lu)", GetLastError() );
        CloseHandle( none );
    }

    TerminateProcess( pi.hProcess, 0 );
    WaitForSingleObject( pi.hProcess, 5000 );
    CloseHandle( limited );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
