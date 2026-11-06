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

/* WTSSessionInfoEx of the current session, NULL on failure */
static WTSINFOEX_LEVEL1_W *session_info_ex( WTSINFOEXW **info )
{
    DWORD count = 0;

    if (!WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx,
                                      (WCHAR **)info, &count ))
        return NULL;
    if (count != sizeof(WTSINFOEXW) || (*info)->Level != 1) return NULL;
    return &(*info)->Data.WTSInfoExLevel1;
}

static void check_session_info(void)
{
    WTSINFOEXW *info;
    WTSINFOEX_LEVEL1_W *l1 = session_info_ex( &info );
    FILETIME now;
    WCHAR *name;
    DWORD count, err;
    LONGLONG n;

    CHECK( l1 != NULL, "WTSSessionInfoEx: level 1, %u bytes", (unsigned)sizeof(WTSINFOEXW) );
    if (!l1) return;
    GetSystemTimeAsFileTime( &now );
    n = ((LONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    CHECK( l1->SessionId == session && l1->SessionState == WTSActive, "session %lu, active (%d)", l1->SessionId,
           l1->SessionState );
    /* Windows: WTS_SESSIONSTATE_UNLOCK; Wine without logind: WTS_SESSIONSTATE_UNKNOWN */
    CHECK( l1->SessionFlags == WTS_SESSIONSTATE_UNLOCK || (!bus[0] && l1->SessionFlags == (LONG)WTS_SESSIONSTATE_UNKNOWN),
           "not locked (SessionFlags %ld)", l1->SessionFlags );
    CHECK( !wcscmp( l1->WinStationName, L"Console" ), "WinStationName \"%ls\"", l1->WinStationName );
    if (bus[0] || !GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "wine_get_version" ))
        CHECK( l1->LogonTime.QuadPart > 0 && l1->LogonTime.QuadPart < n && l1->ConnectTime.QuadPart > 0,
               "logon %lld s ago", (n - l1->LogonTime.QuadPart) / 10000000 );
    CHECK( !l1->LastInputTime.QuadPart && !l1->DisconnectTime.QuadPart, "no last input or disconnect time" );
    WTSFreeMemory( info );

    SetLastError( 0xdeadbeef );
    CHECK( !WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, 12345, WTSSessionInfoEx, (WCHAR **)&info, &count ) &&
           (err = GetLastError()) == RPC_S_INVALID_TAG, "another session fails with RPC_S_INVALID_TAG (%lu)", GetLastError() );
    CHECK( WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSWinStationName, &name, &count ) &&
           count == 16 && !wcscmp( name, L"Console" ), "WTSWinStationName \"Console\"" );
    WTSFreeMemory( name );
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

    if (!GetEnvironmentVariableA( "WTS_TEST_BUS", bus, sizeof(bus) )) bus[0] = 0;
    check_session_info();
    if (!bus[0])
        printf( "skip: no fake logind (run through tests/wts.host under Wine)\n" );
    else
    {
        WTSINFOEXW *info;
        WTSINFOEX_LEVEL1_W *l1;

        WTSRegisterSessionNotification( hwnd, NOTIFY_FOR_THIS_SESSION );
        Sleep( 1500 );   /* the watcher thread connects and subscribes */
        expect( hwnd, "Lock", WTS_SESSION_LOCK, "logind Lock" );
        expect( hwnd, "Unlock", WTS_SESSION_UNLOCK, "logind Unlock" );
        expect( hwnd, "SetLockedHint b true", WTS_SESSION_LOCK, "LockedHint true" );
        l1 = session_info_ex( &info );
        CHECK( l1 && l1->SessionFlags == WTS_SESSIONSTATE_LOCK, "WTSSessionInfoEx says locked (%ld)", l1 ? l1->SessionFlags : -2 );
        if (l1) WTSFreeMemory( info );
        expect( hwnd, "Lock", 0, "Lock while locked: no second event" );
        expect( hwnd, "SetLockedHint b false", WTS_SESSION_UNLOCK, "LockedHint false" );
        expect( hwnd, "SetActive b false", WTS_CONSOLE_DISCONNECT, "session inactive" );
        l1 = session_info_ex( &info );
        CHECK( l1 && l1->SessionState == WTSDisconnected, "WTSSessionInfoEx says disconnected (%d)", l1 ? l1->SessionState : -2 );
        if (l1) WTSFreeMemory( info );
        expect( hwnd, "SetActive b true", WTS_CONSOLE_CONNECT, "session active again" );
        WTSUnRegisterSessionNotification( hwnd );
        expect( hwnd, "Lock", 0, "no event once unregistered" );
    }
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
