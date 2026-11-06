/* WTSRegisterSessionNotification : sémantique relevée sur Windows Server 2022 (tests/wts-explore.c,
 * oracle run 42313418361) ; sous Wine, avec tests/wts.host (faux logind sur un bus privé), les
 * changements de la session logind doivent arriver en WM_WTSSESSION_CHANGE. */
#include <windows.h>
#include <wtsapi32.h>
#include <stdio.h>

static int failures;
static char bus[512];
static DWORD session;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

/* ask the fake logind for a change (Unix command, see fake-logind.py) */
static void fake_logind( const char *args )
{
    char cmd[1024];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    snprintf( cmd, sizeof(cmd), "Z:\\usr\\bin\\sh -c \"busctl --address='%s' call org.freedesktop.login1 "
              "/org/wine/test org.wine.Test %s\"", bus, args );
    if (CreateProcessA( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
    }
}

/* the next WM_WTSSESSION_CHANGE within timeout ms: its wParam, or 0 */
static WPARAM wait_change( HWND hwnd, DWORD timeout, LPARAM *lparam )
{
    DWORD start = GetTickCount();
    MSG msg;

    while (GetTickCount() - start < timeout)
    {
        while (PeekMessageA( &msg, hwnd, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_WTSSESSION_CHANGE)
            {
                if (lparam) *lparam = msg.lParam;
                return msg.wParam;
            }
            DispatchMessageA( &msg );
        }
        MsgWaitForMultipleObjects( 0, NULL, FALSE, 50, QS_ALLINPUT );
    }
    return 0;
}

static void expect( HWND hwnd, const char *args, WPARAM expected, const char *what )
{
    LPARAM lparam = -1;
    WPARAM got;

    fake_logind( args );
    got = wait_change( hwnd, 5000, &lparam );
    CHECK( got == expected && (!got || lparam == session), "%s: event %Iu (expected %Iu), session %Id",
           what, got, expected, lparam );
}

int main(void)
{
    HWND hwnd = CreateWindowA( "static", "wts", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL );
    HWND other = CreateWindowA( "static", "other", 0, 0, 0, 10, 10, NULL, NULL, NULL, NULL );
    DWORD err;
    BOOL r;

    setvbuf( stdout, NULL, _IONBF, 0 );
    ProcessIdToSessionId( GetCurrentProcessId(), &session );

#define TRY(call) (SetLastError( 0xdeadbeef ), r = (call), err = GetLastError(), r)
    CHECK( !TRY( WTSRegisterSessionNotification( NULL, NOTIFY_FOR_THIS_SESSION ) ) && err == ERROR_INVALID_PARAMETER,
           "register NULL fails with ERROR_INVALID_PARAMETER (%lu)", err );
    CHECK( !TRY( WTSRegisterSessionNotification( (HWND)0x12345, NOTIFY_FOR_THIS_SESSION ) ) && err == ERROR_INVALID_PARAMETER,
           "register a bogus window fails with ERROR_INVALID_PARAMETER (%lu)", err );
    CHECK( !TRY( WTSRegisterSessionNotification( hwnd, 2 ) ) && err == ERROR_INVALID_PARAMETER,
           "flags 2 fail with ERROR_INVALID_PARAMETER (%lu)", err );
    CHECK( TRY( WTSRegisterSessionNotification( hwnd, NOTIFY_FOR_THIS_SESSION ) ), "register" );
    CHECK( TRY( WTSRegisterSessionNotification( hwnd, NOTIFY_FOR_ALL_SESSIONS ) ), "register again (all sessions)" );
    CHECK( TRY( WTSUnRegisterSessionNotification( hwnd ) ), "unregister (1)" );
    CHECK( TRY( WTSUnRegisterSessionNotification( hwnd ) ), "unregister (2): registrations are counted" );
    CHECK( !TRY( WTSUnRegisterSessionNotification( hwnd ) ) && err == ERROR_FILE_NOT_FOUND,
           "a third unregister fails with ERROR_FILE_NOT_FOUND (%lu)", err );
    CHECK( !TRY( WTSUnRegisterSessionNotification( NULL ) ) && err == ERROR_INVALID_PARAMETER,
           "unregister NULL fails with ERROR_INVALID_PARAMETER (%lu)", err );
    CHECK( !TRY( WTSUnRegisterSessionNotification( other ) ) && err == ERROR_FILE_NOT_FOUND,
           "unregister a window never registered fails with ERROR_FILE_NOT_FOUND (%lu)", err );
    CHECK( TRY( WTSRegisterSessionNotificationEx( WTS_CURRENT_SERVER_HANDLE, other, NOTIFY_FOR_THIS_SESSION ) ) &&
           TRY( WTSUnRegisterSessionNotificationEx( WTS_CURRENT_SERVER_HANDLE, other ) ), "Ex variants" );

    if (!GetEnvironmentVariableA( "WTS_TEST_BUS", bus, sizeof(bus) ))
        printf( "skip: no fake logind (run through tests/wts.host under Wine)\n" );
    else
    {
        WTSRegisterSessionNotification( hwnd, NOTIFY_FOR_THIS_SESSION );
        Sleep( 1500 );   /* the watcher thread connects and subscribes */
        expect( hwnd, "Lock", WTS_SESSION_LOCK, "logind Lock" );
        expect( hwnd, "Unlock", WTS_SESSION_UNLOCK, "logind Unlock" );
        expect( hwnd, "SetLockedHint b true", WTS_SESSION_LOCK, "LockedHint true" );
        expect( hwnd, "Lock", 0, "Lock while locked: no second event" );
        expect( hwnd, "SetLockedHint b false", WTS_SESSION_UNLOCK, "LockedHint false" );
        expect( hwnd, "SetActive b false", WTS_CONSOLE_DISCONNECT, "session inactive" );
        expect( hwnd, "SetActive b true", WTS_CONSOLE_CONNECT, "session active again" );
        WTSUnRegisterSessionNotification( hwnd );
        expect( hwnd, "Lock", 0, "no event once unregistered" );
    }
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
