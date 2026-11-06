/* MMCSS (avrt.dll). Valeurs attendues relevées sur Windows Server 2022 (tests/avrt-explore.c,
 * oracle runs 42307099955, 42307853831, 42308701768). L'effet sur la priorité est vérifié avec GetThreadPriority() sous
 * Windows, et avec la plus basse valeur nice des threads Linux du processus (Z:\proc) sous Wine. */
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

/* Lowest nice the process may set itself, from "Max nice priority" in /proc/<pid>/limits */
static int own_proc_dir( char *dir, int size )
{
    char exe[MAX_PATH], *name, comm[64], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE find;
    int found = 0;

    GetModuleFileNameA( NULL, exe, sizeof(exe) );
    name = strrchr( exe, '\\' ) ? strrchr( exe, '\\' ) + 1 : exe;
    if ((find = FindFirstFileA( "Z:\\proc\\*", &fd )) == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (fd.cFileName[0] < '1' || fd.cFileName[0] > '9') continue;
        snprintf( path, sizeof(path), "Z:\\proc\\%s\\stat", fd.cFileName );
        comm[0] = 0;
        read_stat_field( path, 3, comm, sizeof(comm) );
        if (strncmp( comm, name, 15 )) continue;
        snprintf( dir, size, "Z:\\proc\\%s", fd.cFileName );
        found = 1;
    } while (!found && FindNextFileA( find, &fd ));
    FindClose( find );
    return found;
}

/* On Linux the priority can only be raised with rtkit (system bus) or a RLIMIT_NICE above 20:
 * a CI container has neither, and the priority checks are skipped there. */
static int host_can_raise(void)
{
    char dir[MAX_PATH], path[MAX_PATH], line[256];
    FILE *f;
    int ok = 0;

    {
        /* (GetFileAttributes does not see a Unix socket, the directory listing does) */
        WIN32_FIND_DATAA fd;
        HANDLE find = FindFirstFileA( "Z:\\run\\dbus\\system_bus_socket", &fd );
        if (find != INVALID_HANDLE_VALUE)
        {
            FindClose( find );
            return 1;
        }
    }
    if (!own_proc_dir( dir, sizeof(dir) )) return 0;
    snprintf( path, sizeof(path), "%s\\limits", dir );
    if (!(f = fopen( path, "r" ))) return 0;
    while (fgets( line, sizeof(line), f ))
        if (!strncmp( line, "Max nice priority", 17 )) ok = atoi( line + 26 ) > 20;
    fclose( f );
    return ok;
}

static int can_raise = 1;

static int raised(void)
{
    int nice = linux_nice();
    return nice == 1000 ? GetThreadPriority( GetCurrentThread() ) > 0 : nice < 0;
}

static DWORD shared_index;

/* a second thread joining the task of the first one with its index (measured on Windows: the
 * index belongs to the task that won AvSetMmMaxThreadCharacteristics, Pro Audio here) */
static DWORD WINAPI joiner( void *arg )
{
    DWORD index = shared_index;
    HANDLE h;

    SetLastError( 0xdeadbeef );
    h = AvSetMmThreadCharacteristicsA( "pro audio", &index );
    CHECK( h != NULL && index == shared_index, "another thread joins the active Pro Audio task %lu (err %lu, index %lu)",
           shared_index, h ? 0 : GetLastError(), index );
    if (h) AvRevertMmThreadCharacteristics( h );

    SetLastError( 0xdeadbeef );
    h = AvSetMmThreadCharacteristicsA( "Audio", &index );
    CHECK( !h && GetLastError() == ERROR_INVALID_TASK_INDEX,
           "joining it as another task fails with ERROR_INVALID_TASK_INDEX (%lu)", GetLastError() );
    if (h) AvRevertMmThreadCharacteristics( h );
    return 0;
}

static DWORD WINAPI worker( void *arg )
{
    DWORD index = 0, index2 = 0;
    ULONG resp = 0;
    HANDLE h, h2;
    int p, nice0 = linux_nice();

    if (nice0 != 1000) can_raise = host_can_raise();
    printf( "running on %s\n", nice0 == 1000 ? "Windows" : can_raise ? "Wine (Linux nice checks)"
            : "Wine, the host cannot raise priorities (no rtkit, RLIMIT_NICE <= 20): priority checks skipped" );

    SetLastError( 0xdeadbeef );
    h = AvSetMmThreadCharacteristicsA( "Audio", &index );
    CHECK( h != NULL && index > 0, "set Audio: handle %p index %lu", h, index );
    if (can_raise)
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

    /* TaskIndex is in/out: a stale or unknown index fails and is left untouched */
    {
        DWORD stale = index2, idx = stale;
        SetLastError( 0xdeadbeef );
        h = AvSetMmThreadCharacteristicsA( "Audio", &idx );
        CHECK( !h && GetLastError() == ERROR_INVALID_TASK_INDEX && idx == stale,
               "the index of a reverted task fails with ERROR_INVALID_TASK_INDEX (%lu, index %lu)", GetLastError(), idx );
        idx = 12345;
        SetLastError( 0xdeadbeef );
        h = AvSetMmThreadCharacteristicsA( "Audio", &idx );
        CHECK( !h && GetLastError() == ERROR_INVALID_TASK_INDEX && idx == 12345,
               "an unknown index fails with ERROR_INVALID_TASK_INDEX (%lu, index %lu)", GetLastError(), idx );
        idx = stale;
        SetLastError( 0xdeadbeef );
        h = AvSetMmMaxThreadCharacteristicsA( "Audio", "Pro Audio", &idx );
        CHECK( !h && GetLastError() == ERROR_INVALID_TASK_INDEX,
               "max with the index of a reverted task fails with ERROR_INVALID_TASK_INDEX (%lu)", GetLastError() );
    }

    index = 0;
    h = AvSetMmMaxThreadCharacteristicsA( "Audio", "Pro Audio", &index );
    CHECK( h != NULL && index > index2, "AvSetMmMaxThreadCharacteristics(Audio, Pro Audio) (index %lu)", index );
    if (h)
    {
        HANDLE t;
        shared_index = index;
        t = CreateThread( NULL, 0, joiner, NULL, 0, NULL );
        WaitForSingleObject( t, INFINITE );
        CloseHandle( t );
        AvRevertMmThreadCharacteristics( h );
    }
    index = 0;
    SetLastError( 0xdeadbeef );
    h = AvSetMmMaxThreadCharacteristicsA( "Audio", NULL, &index );
    CHECK( h != NULL && (!can_raise || raised()), "AvSetMmMaxThreadCharacteristics(Audio, NULL) is Audio (err %lu)",
           h ? 0 : GetLastError() );
    if (h) AvRevertMmThreadCharacteristics( h );
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
