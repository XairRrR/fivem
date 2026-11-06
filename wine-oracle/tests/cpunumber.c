/* GetCurrentProcessorNumber(Ex) : épingler le thread sur chaque processeur (affinité) et vérifier
 * que le numéro renvoyé est celui-là, dans le groupe 0 (Wine n'annonce qu'un groupe). */
#include <windows.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

int main(void)
{
    WORD groups = GetActiveProcessorGroupCount();
    DWORD count = GetActiveProcessorCount( 0 ), cpu, bad = 0, tested = 0;
    DWORD_PTR process_mask, system_mask;
    PROCESSOR_NUMBER pn;

    setvbuf( stdout, NULL, _IONBF, 0 );
    printf( "%u group(s), %lu processors in group 0\n", groups, count );
    GetProcessAffinityMask( GetCurrentProcess(), &process_mask, &system_mask );

    for (cpu = 0; cpu < count && cpu < 8 * sizeof(DWORD_PTR); cpu++)
    {
        int i;
        if (!(process_mask & ((DWORD_PTR)1 << cpu))) continue;
        if (!SetThreadAffinityMask( GetCurrentThread(), (DWORD_PTR)1 << cpu ))
        {
            printf( "cannot pin to %lu (%lu)\n", cpu, GetLastError() );
            continue;
        }
        for (i = 0; i < 10; i++) Sleep( 0 );   /* let the scheduler move the thread */
        memset( &pn, 0xcc, sizeof(pn) );
        GetCurrentProcessorNumberEx( &pn );
        tested++;
        if (pn.Group != 0 || pn.Number != cpu || pn.Reserved != 0 || GetCurrentProcessorNumber() != cpu)
        {
            printf( "pinned to %lu: Ex group %u number %u reserved %u, plain %lu\n", cpu, pn.Group, pn.Number,
                    pn.Reserved, GetCurrentProcessorNumber() );
            bad++;
        }
    }
    SetThreadAffinityMask( GetCurrentThread(), process_mask );

    if (groups == 1) CHECK( tested > 0 && !bad, "pinned thread reports its processor (%lu tested, %lu wrong)", tested, bad );
    else printf( "more than one group: only group 0 tested (%lu tested, %lu wrong)\n", tested, bad );
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
