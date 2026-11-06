/* Exploration : SetThreadInformation/GetThreadInformation(ThreadPowerThrottling) et
 * SetProcessInformation/GetProcessInformation(ProcessPowerThrottling). Pas de PASS/FAIL. */
#include <windows.h>
#include <stdio.h>


static void tset( const char *what, ULONG version, ULONG control, ULONG state, DWORD size )
{
    THREAD_POWER_THROTTLING_STATE s = { version, control, state };
    BOOL r;
    DWORD err;
    SetLastError( 0xdeadbeef );
    r = SetThreadInformation( GetCurrentThread(), ThreadPowerThrottling, &s, size );
    err = GetLastError();
    printf( "%-48s %d err %lu\n", what, r, r ? 0 : err );
}

typedef BOOL (WINAPI *get_thread_info_fn)( HANDLE, THREAD_INFORMATION_CLASS, void *, DWORD );

static void tget( const char *what )
{
    static get_thread_info_fn pGetThreadInformation;
    THREAD_POWER_THROTTLING_STATE s = { 1, 0xcccc, 0xcccc };
    BOOL r;
    DWORD err;

    if (!pGetThreadInformation)
        pGetThreadInformation = (get_thread_info_fn)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "GetThreadInformation" );
    if (!pGetThreadInformation)
    {
        printf( "%-48s GetThreadInformation not exported\n", what );
        return;
    }
    SetLastError( 0xdeadbeef );
    r = pGetThreadInformation( GetCurrentThread(), ThreadPowerThrottling, &s, sizeof(s) );
    err = GetLastError();
    printf( "%-48s %d err %lu version %lu control %#lx state %#lx\n", what, r, r ? 0 : err, s.Version, s.ControlMask,
            s.StateMask );
}

static void pset( const char *what, ULONG version, ULONG control, ULONG state, DWORD size )
{
    PROCESS_POWER_THROTTLING_STATE s = { version, control, state };
    BOOL r;
    DWORD err;
    SetLastError( 0xdeadbeef );
    r = SetProcessInformation( GetCurrentProcess(), ProcessPowerThrottling, &s, size );
    err = GetLastError();
    printf( "%-48s %d err %lu\n", what, r, r ? 0 : err );
}

static void pget( const char *what )
{
    PROCESS_POWER_THROTTLING_STATE s = { 1, 0xcccc, 0xcccc };
    BOOL r;
    DWORD err;
    SetLastError( 0xdeadbeef );
    r = GetProcessInformation( GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s) );
    err = GetLastError();
    printf( "%-48s %d err %lu version %lu control %#lx state %#lx\n", what, r, r ? 0 : err, s.Version, s.ControlMask,
            s.StateMask );
}

int main(void)
{
    setvbuf( stdout, NULL, _IONBF, 0 );
    tget( "thread get initially" );
    tset( "thread EcoQoS on", 1, 1, 1, sizeof(THREAD_POWER_THROTTLING_STATE) );
    tget( "thread get after EcoQoS on" );
    tset( "thread HighQoS (control 1, state 0)", 1, 1, 0, sizeof(THREAD_POWER_THROTTLING_STATE) );
    tget( "thread get after HighQoS" );
    tset( "thread default (control 0, state 0)", 1, 0, 0, sizeof(THREAD_POWER_THROTTLING_STATE) );
    tget( "thread get after default" );
    tset( "thread state without control (0, 1)", 1, 0, 1, sizeof(THREAD_POWER_THROTTLING_STATE) );
    tset( "thread version 0", 0, 1, 1, sizeof(THREAD_POWER_THROTTLING_STATE) );
    tset( "thread version 2", 2, 1, 1, sizeof(THREAD_POWER_THROTTLING_STATE) );
    tset( "thread unknown mask bit 2", 1, 2, 2, sizeof(THREAD_POWER_THROTTLING_STATE) );
    tset( "thread size 8", 1, 1, 1, 8 );
    tset( "thread size 16", 1, 1, 1, 16 );

    pget( "process get initially" );
    pset( "process EcoQoS on", 1, 1, 1, sizeof(PROCESS_POWER_THROTTLING_STATE) );
    pget( "process get after EcoQoS" );
    pset( "process ignore timer resolution", 1, 4, 4, sizeof(PROCESS_POWER_THROTTLING_STATE) );
    pget( "process get after timer resolution" );
    pset( "process HighQoS", 1, 1, 0, sizeof(PROCESS_POWER_THROTTLING_STATE) );
    pget( "process get after HighQoS" );
    pset( "process unknown bit 2", 1, 2, 2, sizeof(PROCESS_POWER_THROTTLING_STATE) );
    pset( "process version 2", 2, 1, 1, sizeof(PROCESS_POWER_THROTTLING_STATE) );
    pset( "process default", 1, 0, 0, sizeof(PROCESS_POWER_THROTTLING_STATE) );
    pget( "process get after default" );
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
