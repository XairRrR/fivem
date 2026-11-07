/* PDH : l'utilisation CPU lue par l'overlay de FiveM (conhost-v2, cl_drawCpuUsage) :
 * \Processor Information(_Total)\% Processor Utility, et la durée de fonctionnement en double.
 * Wine renvoyait une constante (500000) et un double aberrant. */
#include <windows.h>
#include <pdh.h>
#include <stdio.h>

static int failures;
static volatile LONG stop;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

static DWORD WINAPI spin( void *arg )
{
    volatile ULONG64 x = 0;
    while (!stop) x++;
    return 0;
}

static double sample( PDH_HQUERY query, PDH_HCOUNTER counter, DWORD wait, PDH_STATUS *status )
{
    PDH_FMT_COUNTERVALUE value = {0};
    Sleep( wait );
    PdhCollectQueryData( query );
    *status = PdhGetFormattedCounterValue( counter, PDH_FMT_DOUBLE, NULL, &value );
    return value.doubleValue;
}

static void test_cpu( const WCHAR *path )
{
    PDH_HQUERY query;
    PDH_HCOUNTER counter;
    PDH_STATUS status;
    SYSTEM_INFO si;
    HANDLE threads[64];
    DWORD i, n;
    double idle, busy;

    PdhOpenQueryW( NULL, 0, &query );
    status = PdhAddEnglishCounterW( query, path, 0, &counter );
    CHECK( !status, "add %ls (%#lx)", path, status );
    if (status) return;
    PdhCollectQueryData( query );
    idle = sample( query, counter, 600, &status );
    CHECK( !status && idle >= 0 && idle <= 100, "%ls: %.1f%% (status %#lx)", path, idle, status );

    GetSystemInfo( &si );
    n = min( si.dwNumberOfProcessors, 64 );
    stop = 0;
    for (i = 0; i < n; i++) threads[i] = CreateThread( NULL, 0, spin, NULL, 0, NULL );
    Sleep( 300 );
    PdhCollectQueryData( query );
    busy = sample( query, counter, 800, &status );
    stop = 1;
    WaitForMultipleObjects( n, threads, TRUE, 5000 );
    for (i = 0; i < n; i++) CloseHandle( threads[i] );
    /* % Processor Utility accounts for the frequency and goes over 100 on Windows (132% measured) */
    CHECK( !status && busy > 50 && busy <= 400, "%ls with %lu busy threads: %.1f%%", path, n, busy );
    PdhCloseQuery( query );
}

int main(void)
{
    PDH_HQUERY query;
    PDH_HCOUNTER counter;
    PDH_FMT_COUNTERVALUE value;
    PDH_STATUS status;
    double expected;

    setvbuf( stdout, NULL, _IONBF, 0 );
    test_cpu( L"\\Processor Information(_Total)\\% Processor Utility" );
    test_cpu( L"\\Processor(_Total)\\% Processor Time" );

    PdhOpenQueryW( NULL, 0, &query );
    PdhAddEnglishCounterW( query, L"\\System\\System Up Time", 0, &counter );
    PdhCollectQueryData( query );
    status = PdhGetFormattedCounterValue( counter, PDH_FMT_DOUBLE, NULL, &value );
    expected = GetTickCount64() / 1000.0;
    CHECK( !status && value.doubleValue > expected - 10 && value.doubleValue < expected + 10,
           "System Up Time as a double: %.0f s (tick count %.0f s)", value.doubleValue, expected );
    PdhCloseQuery( query );

    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
