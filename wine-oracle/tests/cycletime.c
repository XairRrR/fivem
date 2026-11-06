/* Vérifie QueryThreadCycleTime / QueryProcessCycleTime : un thread qui calcule pendant
 * ~300 ms doit accumuler à peu près autant de cycles que le TSC n'avance pendant ce temps,
 * et un thread qui dort presque aucun. */
#include <windows.h>
#include <stdio.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
static inline unsigned long long rdtsc_(void) { unsigned int lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((unsigned long long)hi << 32) | lo; }
#define __rdtsc rdtsc_
#endif

static volatile LONG stop;

static DWORD WINAPI spin( void *arg ) { while (!stop) ; return 0; }
static DWORD WINAPI nap( void *arg ) { Sleep( 300 ); return 0; }

int main(void)
{
    ULONG64 c0, c1, p0, p1, t0, t1, idle = 0;
    HANDLE h, hn;
    BOOL ok;

    ok = QueryProcessCycleTime( GetCurrentProcess(), &p0 );
    printf( "QueryProcessCycleTime: %s\n", ok ? "ok" : "FAILED" );

    h = CreateThread( NULL, 0, spin, NULL, 0, NULL );
    hn = CreateThread( NULL, 0, nap, NULL, 0, NULL );
    Sleep( 20 );
    ok = QueryThreadCycleTime( h, &c0 );
    printf( "QueryThreadCycleTime: %s (err %lu)\n", ok ? "ok" : "FAILED", ok ? 0 : GetLastError() );
    t0 = __rdtsc();
    Sleep( 300 );
    QueryThreadCycleTime( h, &c1 );
    t1 = __rdtsc();
    stop = 1;
    WaitForSingleObject( h, INFINITE );
    WaitForSingleObject( hn, INFINITE );
    QueryThreadCycleTime( hn, &idle );
    QueryProcessCycleTime( GetCurrentProcess(), &p1 );

    printf( "busy thread: %llu cycles over %llu TSC ticks (ratio %.2f, expect ~1.00)\n",
            c1 - c0, t1 - t0, (double)(c1 - c0) / (t1 - t0) );
    printf( "sleeping thread: %llu cycles (expect small)\n", idle );
    printf( "process: %llu cycles (expect >= busy thread)\n", p1 - p0 );
    printf( "RESULT: %s\n", ok && (double)(c1 - c0) / (t1 - t0) > 0.8 && (double)(c1 - c0) / (t1 - t0) < 1.2
            && idle < (t1 - t0) / 10 ? "PASS" : "FAIL" );
    return 0;
}
