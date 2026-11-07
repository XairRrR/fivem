/* Valeurs relevées sur Windows Server 2022 (tests/misc2-explore.c, oracle run 42359129943) : événements
 * UIA avec un fournisseur NULL, WSALookupServiceEnd(NULL), DhcpCApiInitialize. */
#include <winsock2.h>
#include <windows.h>
#include <dhcpcsdk.h>
#include <uiautomation.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

int main(void)
{
    WSADATA wsa;
    VARIANT a, b;
    DWORD version = 0;
    HRESULT hr;
    int n;

    setvbuf( stdout, NULL, _IONBF, 0 );
    hr = UiaRaiseStructureChangedEvent( NULL, StructureChangeType_ChildAdded, NULL, 0 );
    CHECK( hr == E_INVALIDARG, "UiaRaiseStructureChangedEvent(NULL): E_INVALIDARG (%#lx)", hr );
    hr = UiaRaiseAsyncContentLoadedEvent( NULL, AsyncContentLoadedState_Completed, 100.0 );
    CHECK( hr == E_INVALIDARG, "UiaRaiseAsyncContentLoadedEvent(NULL): E_INVALIDARG (%#lx)", hr );
    VariantInit( &a );
    VariantInit( &b );
    hr = UiaRaiseAutomationPropertyChangedEvent( NULL, UIA_NamePropertyId, a, b );
    CHECK( hr == E_INVALIDARG, "UiaRaiseAutomationPropertyChangedEvent(NULL): E_INVALIDARG (%#lx)", hr );
    hr = UiaRaiseChangesEvent( NULL, 0, NULL );
    CHECK( hr == E_INVALIDARG, "UiaRaiseChangesEvent(NULL): E_INVALIDARG (%#lx)", hr );
    CHECK( UiaClientsAreListening(), "UiaClientsAreListening is TRUE" );
    CHECK( UiaDisconnectAllProviders() == S_OK, "UiaDisconnectAllProviders" );

    WSAStartup( MAKEWORD(2, 2), &wsa );
    n = WSALookupServiceEnd( NULL );
    CHECK( n == SOCKET_ERROR && WSAGetLastError() == WSA_INVALID_HANDLE, "WSALookupServiceEnd(NULL): WSA_INVALID_HANDLE (%d)",
           WSAGetLastError() );
    WSACleanup();

    CHECK( !DhcpCApiInitialize( &version ) && version == 2, "DhcpCApiInitialize: version %lu", version );
    DhcpCApiCleanup();

    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
