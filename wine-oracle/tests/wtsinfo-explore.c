/* Exploration : WTSQuerySessionInformationW(WTSSessionInfo, WTSSessionInfoEx, WTSConnectState,
 * WTSClientProtocolType, WTSWinStationName) pour la session courante. Pas de PASS/FAIL. */
#include <windows.h>
#include <wtsapi32.h>
#include <stdio.h>

static void show_time( const char *name, LARGE_INTEGER t )
{
    FILETIME ft, now;
    ULONGLONG n;
    GetSystemTimeAsFileTime( &now );
    n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    ft.dwLowDateTime = t.LowPart; ft.dwHighDateTime = t.HighPart;
    if (!t.QuadPart) printf( "  %-16s 0\n", name );
    else printf( "  %-16s %lld s before now\n", name, (long long)((LONGLONG)n - t.QuadPart) / 10000000 );
}

int main(void)
{
    WCHAR *buf;
    DWORD count, sid = 0;
    BOOL r;

    setvbuf( stdout, NULL, _IONBF, 0 );
    ProcessIdToSessionId( GetCurrentProcessId(), &sid );
    printf( "session %lu\n", sid );

    r = WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, &buf, &count );
    printf( "WTSSessionInfoEx: %d err %lu count %lu (sizeof WTSINFOEXW %u)\n", r, r ? 0 : GetLastError(), count,
            (unsigned)sizeof(WTSINFOEXW) );
    if (r)
    {
        WTSINFOEXW *info = (WTSINFOEXW *)buf;
        WTSINFOEX_LEVEL1_W *l1 = &info->Data.WTSInfoExLevel1;
        printf( "  Level %lu SessionId %lu SessionState %d SessionFlags %ld\n", info->Level, l1->SessionId,
                l1->SessionState, l1->SessionFlags );
        printf( "  WinStationName \"%ls\" UserName \"%ls\" DomainName \"%ls\"\n", l1->WinStationName, l1->UserName,
                l1->DomainName );
        show_time( "LogonTime", l1->LogonTime );
        show_time( "ConnectTime", l1->ConnectTime );
        show_time( "DisconnectTime", l1->DisconnectTime );
        show_time( "LastInputTime", l1->LastInputTime );
        show_time( "CurrentTime", l1->CurrentTime );
        printf( "  bytes in %lu out %lu frames in %lu out %lu\n", l1->IncomingBytes, l1->OutgoingBytes,
                l1->IncomingFrames, l1->OutgoingFrames );
        WTSFreeMemory( buf );
    }
    r = WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfo, &buf, &count );
    printf( "WTSSessionInfo: %d count %lu\n", r, count );
    if (r)
    {
        WTSINFOW *info = (WTSINFOW *)buf;
        printf( "  State %d SessionId %lu WinStationName \"%ls\" Domain \"%ls\" UserName \"%ls\"\n", info->State,
                info->SessionId, info->WinStationName, info->Domain, info->UserName );
        show_time( "LogonTime", info->LogonTime );
        show_time( "ConnectTime", info->ConnectTime );
        show_time( "LastInputTime", info->LastInputTime );
        show_time( "CurrentTime", info->CurrentTime );
        WTSFreeMemory( buf );
    }
    r = WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSWinStationName, &buf, &count );
    printf( "WTSWinStationName: %d count %lu \"%ls\"\n", r, count, r ? buf : L"" );
    if (r) WTSFreeMemory( buf );
    r = WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSClientProtocolType, &buf, &count );
    printf( "WTSClientProtocolType: %d count %lu value %u\n", r, count, r ? *(USHORT *)buf : 0 );
    if (r) WTSFreeMemory( buf );
    r = WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, 12345, WTSSessionInfoEx, &buf, &count );
    printf( "WTSSessionInfoEx of session 12345: %d err %lu\n", r, r ? 0 : GetLastError() );
    if (r) WTSFreeMemory( buf );
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
