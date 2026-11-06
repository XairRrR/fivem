/* SetThreadPriority(THREAD_MODE_BACKGROUND_BEGIN/END) et SetThreadInformation(ThreadMemoryPriority).
 * Pendant le mode arrière-plan, le thread reste 3 s en pause : `ionice -p <tid>` doit afficher « idle ». */
#include <windows.h>
#include <stdio.h>

static DWORD WINAPI other( void *arg ) { Sleep( 5000 ); return 0; }

/* a thread created by a background thread does not start in background mode */
static DWORD WINAPI child( void *arg )
{
    BOOL r = SetThreadPriority( GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN );
    *(BOOL *)arg = r;
    printf( "BEGIN in a thread created in background mode: %d err %lu (attendu 1)\n", r, r ? 0 : GetLastError() );
    return 0;
}

int main(void)
{
    MEMORY_PRIORITY_INFORMATION mp = { MEMORY_PRIORITY_LOW };
    HANDLE h = CreateThread( NULL, 0, other, NULL, 0, NULL );
    int ok = 1;
    BOOL r;

    r = SetThreadPriority( GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN );
    printf( "BEGIN: %d err %lu (attendu 1)\n", r, r ? 0 : GetLastError() ); ok &= r;
    fflush( stdout );
    {
        BOOL child_ok = FALSE;
        HANDLE c = CreateThread( NULL, 0, child, &child_ok, 0, NULL );
        WaitForSingleObject( c, INFINITE );
        ok &= child_ok;
    }
    Sleep( 3000 );
    r = SetThreadPriority( GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN );
    printf( "BEGIN again: %d err %lu (attendu 0 / 400)\n", r, GetLastError() ); ok &= !r && GetLastError() == 400;
    r = SetThreadPriority( GetCurrentThread(), THREAD_MODE_BACKGROUND_END );
    printf( "END: %d (attendu 1)\n", r ); ok &= r;
    r = SetThreadPriority( GetCurrentThread(), THREAD_MODE_BACKGROUND_END );
    printf( "END again: %d err %lu (attendu 0 / 401)\n", r, GetLastError() ); ok &= !r && GetLastError() == 401;
    r = SetThreadPriority( h, THREAD_MODE_BACKGROUND_BEGIN );
    printf( "BEGIN other thread: %d err %lu (attendu 0 / 87)\n", r, GetLastError() ); ok &= !r && GetLastError() == 87;
    r = SetThreadInformation( GetCurrentThread(), ThreadMemoryPriority, &mp, sizeof(mp) );
    printf( "ThreadMemoryPriority: %d (attendu 1)\n", r ); ok &= r;
    printf( "RESULT: %s\n", ok ? "PASS" : "FAIL" );
    return 0;
}
