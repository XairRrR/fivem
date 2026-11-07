/* QueryPerformanceCounter read in user mode from the TSC (as Windows does with an invariant TSC):
 * the counter must stay monotonic between threads and processors, agree with
 * NtQueryPerformanceCounter, advance at QueryPerformanceFrequency, and timeouts must still be
 * right. GTA V / FiveM call it ~400 000 times per second; under Wine each call was a system call.
 * Also prints the cost of one call. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

static NTSTATUS (WINAPI *pNtQueryPerformanceCounter)( LARGE_INTEGER *, LARGE_INTEGER * );
static volatile LONGLONG published;
static volatile LONG backwards, stop;

static LONGLONG qpc(void)
{
    LARGE_INTEGER v;
    QueryPerformanceCounter( &v );
    return v.QuadPart;
}

/* each thread checks its own readings, and that it never reads less than a value another thread
 * published before */
static DWORD WINAPI worker( void *arg )
{
    LONGLONG last = 0, seen, now;
    int i;

    for (i = 0; i < 2000000 && !stop; i++)
    {
        seen = published;
        now = qpc();
        if (now < last || now < seen) InterlockedIncrement( &backwards );
        last = now;
        if (!(i & 63))
        {
            LONGLONG old;
            do { if ((old = published) >= now) break; }
            while (InterlockedCompareExchange64( &published, now, old ) != old);
        }
    }
    return 0;
}

int main(void)
{
    LARGE_INTEGER freq, a, b, c;
    LONGLONG t0, t1, d;
    ULONGLONG i0, i1;
    HANDLE threads[8], event;
    double ratio, ns;
    DWORD start, waited;
    int i, n = 0;

    setvbuf( stdout, NULL, _IONBF, 0 );
    pNtQueryPerformanceCounter = (void *)GetProcAddress( GetModuleHandleA( "ntdll" ), "NtQueryPerformanceCounter" );

    QueryPerformanceFrequency( &freq );
    printf( "frequency %lld Hz\n", freq.QuadPart );
    CHECK( freq.QuadPart > 0, "QueryPerformanceFrequency" );

    /* the two paths give the same timeline */
    for (i = 0; i < 100000; i++)
    {
        QueryPerformanceCounter( &a );
        pNtQueryPerformanceCounter( &b, NULL );
        QueryPerformanceCounter( &c );
        if (b.QuadPart < a.QuadPart || c.QuadPart < b.QuadPart) n++;
    }
    CHECK( !n, "QueryPerformanceCounter and NtQueryPerformanceCounter interleave in order (%d out of order)", n );

    /* rate: against the precise system time (100 ns) over half a second */
    GetSystemTimePreciseAsFileTime( (FILETIME *)&i0 );
    t0 = qpc();
    Sleep( 500 );
    GetSystemTimePreciseAsFileTime( (FILETIME *)&i1 );
    t1 = qpc();
    ratio = (double)(t1 - t0) / freq.QuadPart / ((double)(i1 - i0) / 10000000);
    CHECK( ratio > 0.999 && ratio < 1.001, "rate matches the precise system time (ratio %.6f)", ratio );

    /* monotonic between threads on all processors */
    for (i = 0; i < ARRAYSIZE(threads); i++) threads[i] = CreateThread( NULL, 0, worker, NULL, 0, NULL );
    WaitForMultipleObjects( ARRAYSIZE(threads), threads, TRUE, 30000 );
    stop = 1;
    CHECK( !backwards, "monotonic across %d threads (%ld backwards)", (int)ARRAYSIZE(threads), backwards );

    /* timeouts follow the kernel clock, whatever the counter source */
    event = CreateEventW( NULL, TRUE, FALSE, NULL );
    start = GetTickCount();
    WaitForSingleObject( event, 200 );
    waited = GetTickCount() - start;
    CHECK( waited >= 180 && waited < 400, "200 ms wait lasted %lu ms", waited );
    t0 = qpc();
    Sleep( 20 );
    d = (qpc() - t0) * 1000 / freq.QuadPart;
    CHECK( d >= 15 && d < 200, "Sleep(20) measured %lld ms", d );

    /* cost of one call (informative) */
    t0 = qpc();
    for (i = 0; i < 1000000; i++) qpc();
    ns = (double)(qpc() - t0) * 1e9 / freq.QuadPart / 1000000;
    printf( "cost: %.1f ns per QueryPerformanceCounter\n", ns );

    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
