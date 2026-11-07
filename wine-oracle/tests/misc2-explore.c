/* Exploration : UIA sans client, DhcpCApiInitialize, fournisseurs d'espaces de noms Winsock
 * (NLA, utilisé par Chromium pour suivre le réseau), CertFindChainInStore. Pas de PASS/FAIL. */
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <nspapi.h>
#include <dhcpcsdk.h>
#include <uiautomation.h>
#include <wincrypt.h>
#include <stdio.h>

#ifndef NLA_SERVICE_CLASS_GUID
#define NLA_SERVICE_CLASS_GUID { 0x0037e515, 0xb5c9, 0x4a43, { 0xba, 0xda, 0x8b, 0x48, 0xa8, 0x7a, 0xd2, 0x39 } }
#endif

#ifdef _MSC_VER
#define GUARD(what, call) __try { call; } __except (EXCEPTION_EXECUTE_HANDLER) { \
    printf( "%-40s EXCEPTION %08lx\n", what, GetExceptionCode() ); }
#else
#define GUARD(what, call) do { call; } while (0)
#endif

static void nsp(void)
{
    WSADATA wsa;
    DWORD size = 0, err;
    int n, i;
    WSANAMESPACE_INFOW *info;
    WSAQUERYSETW qs = { sizeof(qs) };
    HANDLE lookup;
    GUID nla = NLA_SERVICE_CLASS_GUID;

    WSAStartup( MAKEWORD(2, 2), &wsa );
    n = WSAEnumNameSpaceProvidersW( &size, NULL );
    err = WSAGetLastError();
    printf( "WSAEnumNameSpaceProvidersW(NULL) %d err %lu size %lu\n", n, err, size );
    if (size && (info = malloc( size )))
    {
        n = WSAEnumNameSpaceProvidersW( &size, info );
        printf( "WSAEnumNameSpaceProvidersW %d\n", n );
        for (i = 0; i < n; i++)
            printf( "  provider \"%ls\" namespace %lu active %d version %lu\n", info[i].lpszIdentifier,
                    info[i].dwNameSpace, info[i].fActive, info[i].dwVersion );
        free( info );
    }
    qs.dwNameSpace = NS_NLA;
    qs.lpServiceClassId = &nla;
    n = WSALookupServiceBeginW( &qs, LUP_RETURN_ALL, &lookup );
    err = WSAGetLastError();
    printf( "WSALookupServiceBeginW(NS_NLA) %d err %lu\n", n, n ? err : 0 );
    if (!n)
    {
        BYTE buf[4096];
        WSAQUERYSETW *res = (WSAQUERYSETW *)buf;
        DWORD len = sizeof(buf);
        int count = 0;
        while (!WSALookupServiceNextW( lookup, LUP_RETURN_ALL, &len, res ) && count < 8)
        {
            printf( "  network \"%ls\" comment \"%ls\"\n", res->lpszServiceInstanceName ? res->lpszServiceInstanceName : L"",
                    res->lpszComment ? res->lpszComment : L"" );
            len = sizeof(buf);
            count++;
        }
        printf( "  next done, err %d\n", WSAGetLastError() );
        printf( "WSALookupServiceEnd %d\n", WSALookupServiceEnd( lookup ) );
    }
    n = WSALookupServiceEnd( (HANDLE)0x1234 );
    printf( "WSALookupServiceEnd(bogus) %d err %d\n", n, n ? WSAGetLastError() : 0 );
    n = WSALookupServiceEnd( NULL );
    printf( "WSALookupServiceEnd(NULL) %d err %d\n", n, n ? WSAGetLastError() : 0 );
    WSACleanup();
}

int main(void)
{
    DWORD version = 0, r;
    HRESULT hr;

    setvbuf( stdout, NULL, _IONBF, 0 );
    r = DhcpCApiInitialize( &version );
    printf( "DhcpCApiInitialize %lu version %lu\n", r, version );
    DhcpCApiCleanup();

    nsp();

    printf( "UiaClientsAreListening %d\n", UiaClientsAreListening() );
    GUARD( "UiaRaiseStructureChangedEvent(NULL)", hr = UiaRaiseStructureChangedEvent( NULL, StructureChangeType_ChildAdded, NULL, 0 );
           printf( "UiaRaiseStructureChangedEvent(NULL) %#lx\n", hr ) );
    GUARD( "UiaRaiseAsyncContentLoadedEvent(NULL)", hr = UiaRaiseAsyncContentLoadedEvent( NULL, AsyncContentLoadedState_Completed, 100.0 );
           printf( "UiaRaiseAsyncContentLoadedEvent(NULL) %#lx\n", hr ) );
    {
        VARIANT a, b;
        VariantInit( &a );
        VariantInit( &b );
        GUARD( "UiaRaiseAutomationPropertyChangedEvent(NULL)", hr = UiaRaiseAutomationPropertyChangedEvent( NULL, UIA_NamePropertyId, a, b );
               printf( "UiaRaiseAutomationPropertyChangedEvent(NULL) %#lx\n", hr ) );
    }
    GUARD( "UiaRaiseChangesEvent(NULL)", hr = UiaRaiseChangesEvent( NULL, 0, NULL );
           printf( "UiaRaiseChangesEvent(NULL) %#lx\n", hr ) );
    hr = UiaDisconnectAllProviders();
    printf( "UiaDisconnectAllProviders %#lx\n", hr );

    {
        HCERTSTORE store = CertOpenStore( CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"MY" );
        CERT_CHAIN_FIND_ISSUER_PARA para = { sizeof(para) };
        PCCERT_CHAIN_CONTEXT chain;
        int count = 0;

        printf( "MY store %s\n", store ? "opened" : "missing" );
        SetLastError( 0xdeadbeef );
        chain = CertFindChainInStore( store, X509_ASN_ENCODING, 0, CERT_CHAIN_FIND_BY_ISSUER, &para, NULL );
        printf( "CertFindChainInStore(MY, any issuer) %s err %#lx\n", chain ? "chain" : "NULL", chain ? 0 : GetLastError() );
        while (chain && count++ < 8)
            chain = CertFindChainInStore( store, X509_ASN_ENCODING, 0, CERT_CHAIN_FIND_BY_ISSUER, &para, chain );
        printf( "  %d chains\n", count );
        SetLastError( 0xdeadbeef );
        chain = CertFindChainInStore( store, X509_ASN_ENCODING, 0, 99, &para, NULL );
        printf( "CertFindChainInStore(type 99) %s err %#lx\n", chain ? "chain" : "NULL", chain ? 0 : GetLastError() );
        SetLastError( 0xdeadbeef );
        GUARD( "CertFindChainInStore(NULL para)", chain = CertFindChainInStore( store, X509_ASN_ENCODING, 0, CERT_CHAIN_FIND_BY_ISSUER, NULL, NULL );
               printf( "CertFindChainInStore(NULL para) %s err %#lx\n", chain ? "chain" : "NULL", chain ? 0 : GetLastError() ) );
        {
            HCERTSTORE mem = CertOpenStore( CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL );
            SetLastError( 0xdeadbeef );
            chain = CertFindChainInStore( mem, X509_ASN_ENCODING, 0, CERT_CHAIN_FIND_BY_ISSUER, &para, NULL );
            printf( "CertFindChainInStore(empty store) %s err %#lx\n", chain ? "chain" : "NULL", chain ? 0 : GetLastError() );
            CertCloseStore( mem, 0 );
        }
        if (store) CertCloseStore( store, 0 );
    }
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
