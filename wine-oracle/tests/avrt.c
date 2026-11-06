/* MMCSS (avrt.dll). Valeurs attendues relevées sur Windows Server 2022 (tests/avrt-explore.c,
 * oracle run 42307099955). L'effet sur la priorité est vérifié avec GetThreadPriority() sous
 * Windows, et avec la valeur nice du thread Linux (Z:\proc\thread-self\stat) sous Wine. */
#include <windows.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

static int read_stat_field( const char *path, int field, char *comm, int comm_size )
{
    char buf[1024], *p, *open_paren;
    FILE *f = fopen( path, "r" );
    int i, value = 1000;

    if (!f) return 1000;
    if (fgets( buf, sizeof(buf), f ) && (open_paren = strchr( buf, '(' )) && (p = strrchr( buf, ')' )))
    {
        if (comm) snprintf( comm, comm_size, "%.*s", (int)(p - open_paren - 1), open_paren + 1 );
        for (i = 3, p += 2; i < field && p; i++) if ((p = strchr( p, ' ' ))) p++;
        if (p) value = atoi( p );
    }
    fclose( f );
    return value;
}

/* Lowest nice level among the Linux threads of this process, or 1000 when not running on Wine.
 * (Wine opens files from the wineserver, so /proc/self would be the wineserver: find our process
 * in /proc by its name.) */
static int linux_nice(void)
{
    char exe[MAX_PATH], *name, comm[64], path[MAX_PATH];
    WIN32_FIND_DATAA fd, td;
    HANDLE find, tfind;
    int best = 1000;

    GetModuleFileNameA( NULL, exe, sizeof(exe) );
    name = strrchr( exe, '\\' ) ? strrchr( exe, '\\' ) + 1 : exe;
    if ((find = FindFirstFileA( "Z:\\proc\\*", &fd )) == INVALID_HANDLE_VALUE) return 1000;
    do
    {
        if (fd.cFileName[0] < '1' || fd.cFileName[0] > '9') continue;
        snprintf( path, sizeof(path), "Z:\\proc\\%s\\stat", fd.cFileName );
        if (read_stat_field( path, 3, comm, sizeof(comm) ) == 1000 && !comm[0]) continue;
        if (strncmp( comm, name, 15 )) continue;
        snprintf( path, sizeof(path), "Z:\\proc\\%s\\task\\*", fd.cFileName );
        if ((tfind = FindFirstFileA( path, &td )) == INVALID_HANDLE_VALUE) continue;
        do
        {
            int nice;
            if (td.cFileName[0] < '1' || td.cFileName[0] > '9') continue;
            snprintf( path, sizeof(path), "Z:\\proc\\%s\\task\\%s\\stat", fd.cFileName, td.cFileName );
            if ((nice = read_stat_field( path, 19, NULL, 0 )) < best) best = nice;
        } while (FindNextFileA( tfind, &td ));
        FindClose( tfind );
    } while (FindNextFileA( find, &fd ));
    FindClose( find );
    return best;
}

static int raised(void)
{
    int nice = linux_nice();
    return nice == 1000 ? GetThreadPriority( GetCurrentThread() ) > 0 : nice < 0;
}

static DWORD WINAPI worker( void *arg )
{
    DWORD index = 0, index2 = 0;
    ULONG resp = 0;
    HANDLE h, h2;
    int p, nice0 = linux_nice();

    printf( "running on %s\n", nice0 == 1000 ? "Windows" : "Wine (Linux nice checks)" );

    SetLastError( 0xdeadbeef );
    h = AvSetMmThreadCharacteristicsA( "Audio", &index );
    CHECK( h != NULL && index > 0, "set Audio: handle %p index %lu", h, index );
    CHECK( raised(), "Audio raises the thread priority (GetThreadPriority %d, nice %d)",
           GetThreadPriority( GetCurrentThread() ), linux_nice() );
    CHECK( AvQuerySystemResponsiveness( h, &resp ) && resp == 20, "AvQuerySystemResponsiveness = %lu", resp );
    for (p = AVRT_PRIORITY_VERYLOW; p <= AVRT_PRIORITY_CRITICAL; p++)
        CHECK( AvSetMmThreadPriority( h, p ), "AvSetMmThreadPriority(%d)", p );
    SetLastError( 0xdeadbeef );
    CHECK( !AvSetMmThreadPriority( h, 5 ) && GetLastError() == ERROR_INVALID_PARAMETER,
           "AvSetMmThreadPriority(5) fails with ERROR_INVALID_PARAMETER (%lu)", GetLastError() );

    SetLastError( 0xdeadbeef );
    h2 = AvSetMmThreadCharacteristicsA( "Audio", &index2 );
    CHECK( !h2 && GetLastError() == ERROR_THREAD_ALREADY_IN_TASK,
           "second task on the same thread fails with ERROR_THREAD_ALREADY_IN_TASK (%lu)", GetLastError() );

    CHECK( AvRevertMmThreadCharacteristics( h ), "revert" );
    CHECK( nice0 == 1000 ? GetThreadPriority( GetCurrentThread() ) == 0 : linux_nice() == nice0,
           "revert restores the priority (GetThreadPriority %d, nice %d)", GetThreadPriority( GetCurrentThread() ),
           linux_nice() );

    h = AvSetMmThreadCharacteristicsA( "pro audio", &index2 );
    CHECK( h != NULL && index2 > index, "task names are case insensitive, indexes increase (%lu > %lu)", index2, index );
    if (h) AvRevertMmThreadCharacteristics( h );

    SetLastError( 0xdeadbeef );
    h = AvSetMmThreadCharacteristicsA( "Bogus Task", &index );
    CHECK( !h && GetLastError() == ERROR_INVALID_TASK_NAME, "unknown task fails with ERROR_INVALID_TASK_NAME (%lu)",
           GetLastError() );

    SetLastError( 0xdeadbeef );
    CHECK( !AvRevertMmThreadCharacteristics( NULL ) && GetLastError() == ERROR_INVALID_HANDLE, "revert NULL" );
    SetLastError( 0xdeadbeef );
    CHECK( !AvSetMmThreadPriority( NULL, AVRT_PRIORITY_HIGH ) && GetLastError() == ERROR_INVALID_HANDLE,
           "set priority NULL" );
    SetLastError( 0xdeadbeef );
    CHECK( !AvQuerySystemResponsiveness( NULL, &resp ) && GetLastError() == ERROR_INVALID_HANDLE, "responsiveness NULL" );

    h = AvSetMmMaxThreadCharacteristicsA( "Audio", "Pro Audio", &index );
    CHECK( h != NULL, "AvSetMmMaxThreadCharacteristics(Audio, Pro Audio)" );
    if (h) AvRevertMmThreadCharacteristics( h );
    SetLastError( 0xdeadbeef );
    h = AvSetMmMaxThreadCharacteristicsA( "Audio", NULL, &index );
    CHECK( !h && GetLastError() == ERROR_INVALID_TASK_INDEX,
           "AvSetMmMaxThreadCharacteristics(Audio, NULL) fails with ERROR_INVALID_TASK_INDEX (%lu)", GetLastError() );
    return 0;
}

int main(void)
{
    HANDLE t;

    setvbuf( stdout, NULL, _IONBF, 0 );
    t = CreateThread( NULL, 0, worker, NULL, 0, NULL );
    WaitForSingleObject( t, INFINITE );
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
