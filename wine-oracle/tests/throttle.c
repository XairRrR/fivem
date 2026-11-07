/* Power throttling (EcoQoS). Valeurs attendues relevées sur Windows Server 2022
 * (tests/throttle-explore.c, oracle run 42315239464). Sous Wine, l'effet est vérifié sur Linux : un
 * thread EcoQoS a uclamp.max = 512 dans /proc/<pid>/task/<tid>/sched, 1024 sinon. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures, on_wine;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

typedef BOOL (WINAPI *get_thread_info_fn)( HANDLE, THREAD_INFORMATION_CLASS, void *, DWORD );

/* Z:\proc\<pid> of this process (Wine opens files from the wineserver: no /proc/self) */
static int own_proc_dir( char *dir, int size )
{
    char exe[MAX_PATH], *name, path[MAX_PATH], buf[512], *p, *q;
    WIN32_FIND_DATAA fd;
    HANDLE find;
    FILE *f;
    int found = 0;

    GetModuleFileNameA( NULL, exe, sizeof(exe) );
    name = strrchr( exe, '\\' ) ? strrchr( exe, '\\' ) + 1 : exe;
    if ((find = FindFirstFileA( "Z:\\proc\\*", &fd )) == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (fd.cFileName[0] < '1' || fd.cFileName[0] > '9') continue;
        snprintf( path, sizeof(path), "Z:\\proc\\%s\\stat", fd.cFileName );
        if (!(f = fopen( path, "r" ))) continue;
        if (fgets( buf, sizeof(buf), f ) && (p = strchr( buf, '(' )) && (q = strrchr( buf, ')' )) &&
            q - p - 1 > 0 && !strncmp( p + 1, name, min( 15, (int)(q - p - 1) ) ))
        {
            snprintf( dir, size, "Z:\\proc\\%s", fd.cFileName );
            found = 1;
        }
        fclose( f );
    } while (!found && FindNextFileA( find, &fd ));
    FindClose( find );
    return found;
}

/* how many threads of this process have uclamp.max == value, -1 if not visible */
static int threads_with_util_max( int value )
{
    char dir[MAX_PATH], path[MAX_PATH], line[256];
    WIN32_FIND_DATAA fd;
    HANDLE find;
    int count = 0, seen = 0;

    if (!own_proc_dir( dir, sizeof(dir) )) return -1;
    snprintf( path, sizeof(path), "%s\\task\\*", dir );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) return -1;
    do
    {
        FILE *f;
        if (fd.cFileName[0] < '1' || fd.cFileName[0] > '9') continue;
        snprintf( path, sizeof(path), "%s\\task\\%s\\sched", dir, fd.cFileName );
        if (!(f = fopen( path, "r" ))) continue;
        while (fgets( line, sizeof(line), f ))
            if (!strncmp( line, "uclamp.max", 10 ) && strchr( line, ':' ))
            {
                seen = 1;
                if (atoi( strchr( line, ':' ) + 1 ) == value) count++;
            }
        fclose( f );
    } while (FindNextFileA( find, &fd ));
    FindClose( find );
    return seen ? count : -1;
}

static BOOL tset( ULONG version, ULONG control, ULONG state, DWORD size, DWORD *err )
{
    THREAD_POWER_THROTTLING_STATE s[2] = {{ version, control, state }};
    BOOL r;
    SetLastError( 0xdeadbeef );
    r = SetThreadInformation( GetCurrentThread(), ThreadPowerThrottling, s, size );
    *err = GetLastError();
    return r;
}

static BOOL pset( ULONG version, ULONG control, ULONG state, DWORD *err )
{
    PROCESS_POWER_THROTTLING_STATE s = { version, control, state };
    BOOL r;
    SetLastError( 0xdeadbeef );
    r = SetProcessInformation( GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s) );
    *err = GetLastError();
    return r;
}

static DWORD WINAPI idle_thread( void *event )
{
    WaitForSingleObject( event, INFINITE );
    return 0;
}

int main(void)
{
    const DWORD size = sizeof(THREAD_POWER_THROTTLING_STATE);
    get_thread_info_fn pGetThreadInformation;
    THREAD_POWER_THROTTLING_STATE st = { 1, 0, 0 };
    DWORD err;
    int before, n;

    setvbuf( stdout, NULL, _IONBF, 0 );
    on_wine = !!GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "wine_get_version" );

    CHECK( tset( 1, 1, 1, size, &err ), "EcoQoS on" );
    if (on_wine)
    {
        n = threads_with_util_max( 512 );
        if (n < 0) printf( "skip: uclamp not visible in /proc\n" );
        else CHECK( n == 1, "one thread has uclamp.max 512 (%d)", n );
    }
    CHECK( tset( 1, 1, 0, size, &err ), "HighQoS" );
    if (on_wine && (n = threads_with_util_max( 512 )) >= 0) CHECK( n == 0, "HighQoS removes the clamp (%d)", n );
    CHECK( tset( 1, 0, 0, size, &err ), "default" );
    CHECK( tset( 0, 1, 1, size, &err ), "version 0 is accepted" );
    tset( 1, 0, 0, size, &err );
    CHECK( !tset( 2, 1, 1, size, &err ) && err == ERROR_INVALID_PARAMETER, "version 2: ERROR_INVALID_PARAMETER (%lu)", err );
    CHECK( !tset( 1, 0, 1, size, &err ) && err == ERROR_INVALID_PARAMETER, "state outside the control mask (%lu)", err );
    CHECK( !tset( 1, 2, 2, size, &err ) && err == ERROR_INVALID_PARAMETER, "unknown bit (%lu)", err );
    CHECK( !tset( 1, 1, 1, 8, &err ) && err == ERROR_INVALID_PARAMETER, "size 8 (%lu)", err );
    CHECK( tset( 1, 1, 0, 16, &err ), "a larger size is accepted" );

    pGetThreadInformation = (get_thread_info_fn)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "GetThreadInformation" );
    CHECK( pGetThreadInformation != NULL, "kernel32 exports GetThreadInformation" );
    if (pGetThreadInformation)
    {
        SetLastError( 0xdeadbeef );
        CHECK( !pGetThreadInformation( GetCurrentThread(), ThreadPowerThrottling, &st, sizeof(st) ) &&
               GetLastError() == ERROR_INVALID_PARAMETER, "the state cannot be read back (%lu)", GetLastError() );
    }

    {
        HANDLE event = CreateEventA( NULL, TRUE, FALSE, NULL ), threads[3];
        for (int i = 0; i < 3; i++) threads[i] = CreateThread( NULL, 0, idle_thread, event, 0, NULL );
        Sleep( 100 );
        before = on_wine ? threads_with_util_max( 512 ) : -1;
        CHECK( pset( 1, 1, 1, &err ), "process EcoQoS" );
        if (on_wine && before >= 0)
        {
            n = threads_with_util_max( 512 );
            CHECK( n >= 4, "process EcoQoS clamps every thread (%d)", n );
        }
        CHECK( pset( 1, 1, 0, &err ), "process HighQoS" );
        if (on_wine && before >= 0) CHECK( !threads_with_util_max( 512 ), "and HighQoS removes it" );
        SetEvent( event );
        WaitForMultipleObjects( 3, threads, TRUE, 5000 );
    }
    CHECK( !pset( 1, 4, 4, &err ) && err == ERROR_INVALID_PARAMETER, "process IGNORE_TIMER_RESOLUTION: ERROR_INVALID_PARAMETER (%lu)", err );
    CHECK( !pset( 2, 1, 1, &err ) && err == ERROR_INVALID_PARAMETER, "process version 2 (%lu)", err );
    CHECK( pset( 1, 0, 0, &err ), "process default" );

    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
