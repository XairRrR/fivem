/* Exploration de WTSRegisterSessionNotification(Ex) / WTSUnRegisterSessionNotification(Ex) :
 * erreurs, double enregistrement, fenêtre d'un autre thread, fenêtre détruite. Pas de PASS/FAIL. */
#include <windows.h>
#include <wtsapi32.h>
#include <stdio.h>

#ifdef _MSC_VER
#define GUARD(what, call) __try { call; } __except (EXCEPTION_EXECUTE_HANDLER) { \
    printf( "%-44s EXCEPTION %08lx\n", what, GetExceptionCode() ); }
#else
#define GUARD(what, call) do { call; } while (0)
#endif

static void result( const char *what, BOOL r, DWORD err )
{
    printf( "%-44s %d err %lu\n", what, r, r ? 0 : err );
}

#define TRY(what, call) do { BOOL r_; DWORD e_; SetLastError( 0xdeadbeef ); r_ = (call); e_ = GetLastError(); \
                             result( what, r_, e_ ); } while (0)

static HWND other_window;

static DWORD WINAPI other_thread( void *event )
{
    MSG msg;
    other_window = CreateWindowA( "static", "other", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL );
    SetEvent( event );
    while (GetMessageA( &msg, NULL, 0, 0 )) DispatchMessageA( &msg );
    return 0;
}

int main(void)
{
    HWND hwnd = CreateWindowA( "static", "wts", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL );
    HWND msgonly = CreateWindowA( "static", "msgonly", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL );
    HANDLE event = CreateEventA( NULL, FALSE, FALSE, NULL ), thread;
    DWORD sid = 0;

    setvbuf( stdout, NULL, _IONBF, 0 );
    ProcessIdToSessionId( GetCurrentProcessId(), &sid );
    printf( "session id %lu, WTSGetActiveConsoleSessionId %lu\n", sid, WTSGetActiveConsoleSessionId() );

    TRY( "register NULL", WTSRegisterSessionNotification( NULL, NOTIFY_FOR_THIS_SESSION ) );
    TRY( "register bogus hwnd", WTSRegisterSessionNotification( (HWND)0x12345, NOTIFY_FOR_THIS_SESSION ) );
    TRY( "register flags 2", WTSRegisterSessionNotification( hwnd, 2 ) );
    TRY( "register flags 0 (this session)", WTSRegisterSessionNotification( hwnd, NOTIFY_FOR_THIS_SESSION ) );
    TRY( "register again", WTSRegisterSessionNotification( hwnd, NOTIFY_FOR_THIS_SESSION ) );
    TRY( "register again, all sessions", WTSRegisterSessionNotification( hwnd, NOTIFY_FOR_ALL_SESSIONS ) );
    TRY( "unregister", WTSUnRegisterSessionNotification( hwnd ) );
    TRY( "unregister again", WTSUnRegisterSessionNotification( hwnd ) );
    TRY( "unregister third time", WTSUnRegisterSessionNotification( hwnd ) );
    TRY( "unregister NULL", WTSUnRegisterSessionNotification( NULL ) );
    TRY( "unregister bogus", WTSUnRegisterSessionNotification( (HWND)0x12345 ) );
    TRY( "unregister never registered", WTSUnRegisterSessionNotification( msgonly ) );
    TRY( "register all sessions", WTSRegisterSessionNotification( hwnd, NOTIFY_FOR_ALL_SESSIONS ) );
    TRY( "unregister", WTSUnRegisterSessionNotification( hwnd ) );
    TRY( "register message-only window", WTSRegisterSessionNotification( msgonly, NOTIFY_FOR_THIS_SESSION ) );
    TRY( "unregister message-only window", WTSUnRegisterSessionNotification( msgonly ) );

    thread = CreateThread( NULL, 0, other_thread, event, 0, NULL );
    WaitForSingleObject( event, INFINITE );
    TRY( "register another thread's window", WTSRegisterSessionNotification( other_window, NOTIFY_FOR_THIS_SESSION ) );
    TRY( "unregister another thread's window", WTSUnRegisterSessionNotification( other_window ) );
    PostThreadMessageA( GetThreadId( thread ), WM_QUIT, 0, 0 );
    WaitForSingleObject( thread, INFINITE );

    TRY( "registerEx current server", WTSRegisterSessionNotificationEx( WTS_CURRENT_SERVER_HANDLE, hwnd, NOTIFY_FOR_THIS_SESSION ) );
    TRY( "registerEx again", WTSRegisterSessionNotificationEx( WTS_CURRENT_SERVER_HANDLE, hwnd, NOTIFY_FOR_THIS_SESSION ) );
    TRY( "unregisterEx", WTSUnRegisterSessionNotificationEx( WTS_CURRENT_SERVER_HANDLE, hwnd ) );
    TRY( "unregisterEx again", WTSUnRegisterSessionNotificationEx( WTS_CURRENT_SERVER_HANDLE, hwnd ) );
    TRY( "registerEx bogus server", WTSRegisterSessionNotificationEx( (HANDLE)0x5678, hwnd, NOTIFY_FOR_THIS_SESSION ) );

    TRY( "register then destroy", WTSRegisterSessionNotification( hwnd, NOTIFY_FOR_THIS_SESSION ) );
    DestroyWindow( hwnd );
    TRY( "unregister destroyed window", WTSUnRegisterSessionNotification( hwnd ) );
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
