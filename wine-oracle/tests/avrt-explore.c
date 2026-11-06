/* Exploration de MMCSS (avrt.dll) : ce que Windows renvoie, pour écrire le test et
 * l'implémentation de Wine à partir de la référence. Pas de PASS/FAIL : tout est affiché. */
#include <windows.h>
#include <avrt.h>
#include <stdio.h>

static void show( const char *what, HANDLE h, DWORD index )
{
    DWORD err = GetLastError();
    printf( "%-36s handle %s err %lu index %lu thread priority %d\n", what, h ? "ok" : "NULL",
            h ? 0 : err, index, GetThreadPriority( GetCurrentThread() ) );
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
        sprintf( what, "set \"%s\"", tasks[i] );
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
    h = AvSetMmThreadCharacteristicsA( NULL, &index );
    show( "set NULL name", h, index );
    SetLastError( 0xdeadbeef );
    h = AvSetMmThreadCharacteristicsA( "Audio", NULL );
    show( "set NULL index", h, 0 );
    if (h) AvRevertMmThreadCharacteristics( h );
    SetLastError( 0xdeadbeef );
    r = AvRevertMmThreadCharacteristics( NULL );
    printf( "revert NULL %d err %lu\n", r, r ? 0 : GetLastError() );
    SetLastError( 0xdeadbeef );
    r = AvSetMmThreadPriority( NULL, AVRT_PRIORITY_HIGH );
    printf( "set priority NULL %d err %lu\n", r, r ? 0 : GetLastError() );
    SetLastError( 0xdeadbeef );
    r = AvQuerySystemResponsiveness( NULL, &resp );
    printf( "responsiveness NULL %d err %lu value %lu\n", r, r ? 0 : GetLastError(), resp );

    index = 0;
    SetLastError( 0xdeadbeef );
    h = AvSetMmMaxThreadCharacteristicsA( "Audio", "Pro Audio", &index );
    show( "max(Audio, Pro Audio)", h, index );
    if (h) AvRevertMmThreadCharacteristics( h );
    SetLastError( 0xdeadbeef );
    h = AvSetMmMaxThreadCharacteristicsA( "Audio", NULL, &index );
    show( "max(Audio, NULL)", h, index );
    if (h) AvRevertMmThreadCharacteristics( h );
    printf( "end thread priority %d\n", GetThreadPriority( GetCurrentThread() ) );
    return 0;
}

int main(void)
{
    HANDLE t = CreateThread( NULL, 0, worker, NULL, 0, NULL );
    WaitForSingleObject( t, INFINITE );
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
