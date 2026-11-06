/* Exploration de MMCSS (avrt.dll) : ce que Windows renvoie, pour écrire le test et
 * l'implémentation de Wine à partir de la référence. Pas de PASS/FAIL : tout est affiché. */
#include <windows.h>
#include <avrt.h>
#include <stdio.h>

/* calls that may fault on Windows (e.g. a NULL output pointer) report it instead of killing the test */
#ifdef _MSC_VER
#define GUARD(what, call) __try { call; } __except (EXCEPTION_EXECUTE_HANDLER) { \
    printf( "%-36s EXCEPTION %08lx\n", what, GetExceptionCode() ); }
#else
#define GUARD(what, call) do { call; } while (0)
#endif

static void show( const char *what, HANDLE h, DWORD index )
{
    DWORD err = GetLastError();
    printf( "%-36s handle %s err %lu index %lu thread priority %d\n", what, h ? "ok" : "NULL",
            h ? 0 : err, index, GetThreadPriority( GetCurrentThread() ) );
}

static DWORD join_index;

static DWORD WINAPI joiner( void *arg )
{
    DWORD idx = join_index;
    HANDLE h;

    SetLastError( 0xdeadbeef );
    h = AvSetMmThreadCharacteristicsA( "Playback", &idx );
    show( "index: other thread joins (Playback)", h, idx );
    if (h) AvRevertMmThreadCharacteristics( h );
    return 0;
}

/* join: a second thread uses the index of a task still active on the first thread */
static const char *join_task1, *join_task2;

static DWORD WINAPI join_thread( void *arg )
{
    DWORD idx = join_index;
    char what[80];
    HANDLE h;

    SetLastError( 0xdeadbeef );
    if (join_task2) h = AvSetMmMaxThreadCharacteristicsA( join_task1, join_task2, &idx );
    else h = AvSetMmThreadCharacteristicsA( join_task1, &idx );
    snprintf( what, sizeof(what), "join: %s%s%s", join_task1, join_task2 ? "+" : "", join_task2 ? join_task2 : "" );
    show( what, h, idx );
    if (h) AvRevertMmThreadCharacteristics( h );
    return 0;
}

static void join_case( const char *owner1, const char *owner2, const char *task1, const char *task2 )
{
    DWORD idx = 0;
    HANDLE a, t;

    a = owner2 ? AvSetMmMaxThreadCharacteristicsA( owner1, owner2, &idx ) : AvSetMmThreadCharacteristicsA( owner1, &idx );
    printf( "join: owner %s%s%s index %lu handle %s\n", owner1, owner2 ? "+" : "", owner2 ? owner2 : "", idx,
            a ? "ok" : "NULL" );
    if (!a) return;
    join_index = idx;
    join_task1 = task1;
    join_task2 = task2;
    t = CreateThread( NULL, 0, join_thread, NULL, 0, NULL );
    WaitForSingleObject( t, INFINITE );
    CloseHandle( t );
    AvRevertMmThreadCharacteristics( a );
}

static DWORD WINAPI worker( void *arg )
{
    DWORD index = 0;
    HANDLE h;
    ULONG resp = 0;
    BOOL r;
    static const char *const tasks[] = { "Audio", "Pro Audio", "Games", "Playback", "Capture",
                                         "Distribution", "Window Manager", "audio", "Bogus Task" };
    int i;

    printf( "start thread priority %d\n", GetThreadPriority( GetCurrentThread() ) );
    for (i = 0; i < ARRAYSIZE(tasks); i++)
    {
        char what[64];
        index = 0;
        SetLastError( 0xdeadbeef );
        h = AvSetMmThreadCharacteristicsA( tasks[i], &index );
        snprintf( what, sizeof(what), "set \"%s\"", tasks[i] );
        show( what, h, index );
        if (h)
        {
            SetLastError( 0xdeadbeef );
            r = AvQuerySystemResponsiveness( h, &resp );
            printf( "  AvQuerySystemResponsiveness %d err %lu value %lu\n", r, r ? 0 : GetLastError(), resp );
            for (int p = AVRT_PRIORITY_LOW; p <= AVRT_PRIORITY_CRITICAL; p++)
            {
                SetLastError( 0xdeadbeef );
                r = AvSetMmThreadPriority( h, p );
                printf( "  AvSetMmThreadPriority(%d) %d err %lu -> thread priority %d\n", p, r,
                        r ? 0 : GetLastError(), GetThreadPriority( GetCurrentThread() ) );
            }
            SetLastError( 0xdeadbeef );
            r = AvSetMmThreadPriority( h, 5 );
            printf( "  AvSetMmThreadPriority(5) %d err %lu\n", r, r ? 0 : GetLastError() );
            SetLastError( 0xdeadbeef );
            r = AvRevertMmThreadCharacteristics( h );
            printf( "  revert %d err %lu -> thread priority %d\n", r, r ? 0 : GetLastError(),
                    GetThreadPriority( GetCurrentThread() ) );
        }
    }

    /* the same task twice on one thread, then nested different tasks */
    index = 0;
    h = AvSetMmThreadCharacteristicsA( "Audio", &index );
    show( "set Audio (1st)", h, index );
    {
        DWORD index2 = 0;
        HANDLE h2 = AvSetMmThreadCharacteristicsA( "Audio", &index2 );
        show( "set Audio (2nd, same thread)", h2, index2 );
        printf( "  same handle: %s\n", h2 == h ? "yes" : "no" );
        if (h2 && h2 != h) AvRevertMmThreadCharacteristics( h2 );
    }
    if (h) AvRevertMmThreadCharacteristics( h );

    SetLastError( 0xdeadbeef );
    GUARD( "set NULL name", h = AvSetMmThreadCharacteristicsA( NULL, &index ); show( "set NULL name", h, index ) );
    SetLastError( 0xdeadbeef );
    GUARD( "set NULL index", h = AvSetMmThreadCharacteristicsA( "Audio", NULL ); show( "set NULL index", h, 0 );
           if (h) AvRevertMmThreadCharacteristics( h ) );
    SetLastError( 0xdeadbeef );
    GUARD( "revert NULL", r = AvRevertMmThreadCharacteristics( NULL );
           printf( "revert NULL %d err %lu\n", r, r ? 0 : GetLastError() ) );
    SetLastError( 0xdeadbeef );
    GUARD( "set priority NULL", r = AvSetMmThreadPriority( NULL, AVRT_PRIORITY_HIGH );
           printf( "set priority NULL %d err %lu\n", r, r ? 0 : GetLastError() ) );
    SetLastError( 0xdeadbeef );
    GUARD( "responsiveness NULL", r = AvQuerySystemResponsiveness( NULL, &resp );
           printf( "responsiveness NULL %d err %lu value %lu\n", r, r ? 0 : GetLastError(), resp ) );

    index = 0;
    SetLastError( 0xdeadbeef );
    h = AvSetMmMaxThreadCharacteristicsA( "Audio", "Pro Audio", &index );
    show( "max(Audio, Pro Audio)", h, index );
    if (h) AvRevertMmThreadCharacteristics( h );
    SetLastError( 0xdeadbeef );
    h = AvSetMmMaxThreadCharacteristicsA( "Audio", NULL, &index );
    show( "max(Audio, NULL)", h, index );
    if (h) AvRevertMmThreadCharacteristics( h );
    /* TaskIndex is in/out: 0 on input for a new task, a previous index to join it */
    {
        DWORD idx = 0, stale;
        HANDLE a = AvSetMmThreadCharacteristicsA( "Audio", &idx );
        show( "index: set Audio with 0", a, idx );
        stale = idx;
        AvRevertMmThreadCharacteristics( a );
        SetLastError( 0xdeadbeef );
        a = AvSetMmThreadCharacteristicsA( "Audio", &idx );
        show( "index: set Audio with the reverted index", a, idx );
        if (a) AvRevertMmThreadCharacteristics( a );
        idx = 12345;
        SetLastError( 0xdeadbeef );
        a = AvSetMmThreadCharacteristicsA( "Audio", &idx );
        show( "index: set Audio with 12345", a, idx );
        if (a) AvRevertMmThreadCharacteristics( a );
        idx = stale;
        SetLastError( 0xdeadbeef );
        a = AvSetMmMaxThreadCharacteristicsA( "Audio", "Pro Audio", &idx );
        show( "index: max with the reverted index", a, idx );
        if (a) AvRevertMmThreadCharacteristics( a );
        idx = 0;
        SetLastError( 0xdeadbeef );
        a = AvSetMmMaxThreadCharacteristicsA( "Audio", "Pro Audio", &idx );
        show( "index: max with 0", a, idx );
        if (a)
        {
            HANDLE t;
            join_index = idx;
            t = CreateThread( NULL, 0, joiner, NULL, 0, NULL );
            WaitForSingleObject( t, INFINITE );
            CloseHandle( t );
            AvRevertMmThreadCharacteristics( a );
        }
    }
    join_case( "Audio", NULL, "Audio", NULL );
    join_case( "Audio", NULL, "audio", NULL );
    join_case( "Audio", NULL, "Pro Audio", NULL );
    join_case( "Audio", "Pro Audio", "Audio", "Pro Audio" );
    join_case( "Audio", "Pro Audio", "Pro Audio", NULL );
    join_case( "Audio", "Pro Audio", "Audio", NULL );
    {
        DWORD idx = 0;
        SetLastError( 0xdeadbeef );
        h = AvSetMmMaxThreadCharacteristicsA( "Audio", NULL, &idx );
        show( "null: max(Audio, NULL) with 0", h, idx );
        if (h) AvRevertMmThreadCharacteristics( h );
        /* max(NULL, Pro Audio) kills the process on Windows (run 42308701768) */
        idx = 0;
        SetLastError( 0xdeadbeef );
        GUARD( "null: max(NULL, Pro Audio)", h = AvSetMmMaxThreadCharacteristicsA( NULL, "Pro Audio", &idx );
               show( "null: max(NULL, Pro Audio) with 0", h, idx ); if (h) AvRevertMmThreadCharacteristics( h ) );
        idx = 0;
        SetLastError( 0xdeadbeef );
        GUARD( "null: max(NULL, NULL)", h = AvSetMmMaxThreadCharacteristicsA( NULL, NULL, &idx );
               show( "null: max(NULL, NULL) with 0", h, idx ); if (h) AvRevertMmThreadCharacteristics( h ) );
        idx = 0;
        SetLastError( 0xdeadbeef );
        GUARD( "null: max(Bogus Task, Audio)", h = AvSetMmMaxThreadCharacteristicsA( "Bogus Task", "Audio", &idx );
               show( "null: max(Bogus Task, Audio) with 0", h, idx ); if (h) AvRevertMmThreadCharacteristics( h ) );
        idx = 0;
        SetLastError( 0xdeadbeef );
        GUARD( "null: max(Audio, Bogus Task)", h = AvSetMmMaxThreadCharacteristicsA( "Audio", "Bogus Task", &idx );
               show( "null: max(Audio, Bogus Task) with 0", h, idx ); if (h) AvRevertMmThreadCharacteristics( h ) );
        idx = 0;
        SetLastError( 0xdeadbeef );
        h = AvSetMmMaxThreadCharacteristicsA( "Capture", "Window Manager", &idx );
        show( "null: max(Capture, Window Manager) tie", h, idx );
        if (h)
        {
            join_index = idx;
            join_task1 = "Window Manager";
            join_task2 = NULL;
            WaitForSingleObject( CreateThread( NULL, 0, join_thread, NULL, 0, NULL ), INFINITE );
            join_task1 = "Capture";
            WaitForSingleObject( CreateThread( NULL, 0, join_thread, NULL, 0, NULL ), INFINITE );
            AvRevertMmThreadCharacteristics( h );
        }
    }
    printf( "end thread priority %d\n", GetThreadPriority( GetCurrentThread() ) );
    return 0;
}

int main(void)
{
    setvbuf( stdout, NULL, _IONBF, 0 );
    HANDLE t = CreateThread( NULL, 0, worker, NULL, 0, NULL );
    WaitForSingleObject( t, INFINITE );
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
