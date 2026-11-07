/* CertFindChainInStore(CERT_CHAIN_FIND_BY_ISSUER) (libcef, certificats clients) : recherche par émetteur
 * parmi les certificats ayant une clé privée. Sans résultat : NULL et CRYPT_E_NOT_FOUND (Windows
 * Server 2022, tests/misc2-explore.c). Wine n'avait qu'un stub. */
#define CERT_CHAIN_FIND_BY_ISSUER_PARA_HAS_EXTRA_FIELDS   /* as the Windows SDK requires for the indexes */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>

static int failures;
static int callback_calls;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

static BOOL WINAPI refuse( PCCERT_CONTEXT cert, void *arg ) { callback_calls++; return FALSE; }

static PCCERT_CHAIN_CONTEXT find( HCERTSTORE store, DWORD flags, CERT_CHAIN_FIND_BY_ISSUER_PARA *para,
                                  PCCERT_CHAIN_CONTEXT prev, DWORD *err )
{
    PCCERT_CHAIN_CONTEXT chain;
    SetLastError( 0xdeadbeef );
    chain = CertFindChainInStore( store, X509_ASN_ENCODING, flags, CERT_CHAIN_FIND_BY_ISSUER, para, prev );
    *err = GetLastError();
    return chain;
}

int main(void)
{
    WCHAR container[64];
    BYTE name_buf[256];
    CERT_NAME_BLOB name = { sizeof(name_buf), name_buf }, other = { 0, NULL };
    CRYPT_KEY_PROV_INFO prov = { 0 };
    CERT_CHAIN_FIND_BY_ISSUER_PARA para = { sizeof(para) };
    HCERTSTORE store, nokey;
    PCCERT_CONTEXT cert, copy;
    PCCERT_CHAIN_CONTEXT chain;
    HCRYPTPROV hprov;
    DWORD err, chain_index = 99, element_index = 99;
    BYTE other_buf[256];

    setvbuf( stdout, NULL, _IONBF, 0 );
    swprintf( container, ARRAYSIZE(container), L"wine-certchain-%lu", GetCurrentProcessId() );
    if (!CertStrToNameW( X509_ASN_ENCODING, L"CN=wine certchain test", CERT_X500_NAME_STR, NULL, name_buf, &name.cbData, NULL ) ||
        !CryptAcquireContextW( &hprov, container, NULL, PROV_RSA_FULL, CRYPT_NEWKEYSET ))
    {
        printf( "FAIL: setup %lu\nRESULT: FAIL\n", GetLastError() );
        return 0;
    }
    {
        HCRYPTKEY key;
        CryptGenKey( hprov, AT_KEYEXCHANGE, (1024 << 16) | CRYPT_EXPORTABLE, &key );
        CryptDestroyKey( key );
    }
    prov.pwszContainerName = container;
    prov.dwProvType = PROV_RSA_FULL;
    prov.dwKeySpec = AT_KEYEXCHANGE;
    cert = CertCreateSelfSignCertificate( hprov, &name, 0, &prov, NULL, NULL, NULL, NULL );
    CHECK( cert != NULL, "self-signed certificate with a key (%lu)", cert ? 0 : GetLastError() );
    if (!cert) goto done;

    store = CertOpenStore( CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL );
    CertAddCertificateContextToStore( store, cert, CERT_STORE_ADD_ALWAYS, NULL );

    chain = find( store, 0, &para, NULL, &err );
    CHECK( chain && chain->cChain >= 1 && CertCompareCertificate( X509_ASN_ENCODING,
           chain->rgpChain[0]->rgpElement[0]->pCertContext->pCertInfo, cert->pCertInfo ),
           "any issuer: the chain of the certificate (%#lx)", chain ? 0 : err );
    if (chain)
    {
        chain = find( store, 0, &para, chain, &err );
        CHECK( !chain && err == CRYPT_E_NOT_FOUND, "no second chain: CRYPT_E_NOT_FOUND (%#lx)", err );
    }

    para.cIssuer = 1;
    para.rgIssuer = &name;
    para.pdwIssuerChainIndex = &chain_index;
    para.pdwIssuerElementIndex = &element_index;
    chain = find( store, 0, &para, NULL, &err );
    CHECK( chain && !chain_index && !element_index, "its own issuer: found, chain %lu element %lu", chain_index, element_index );
    if (chain) CertFreeCertificateChain( chain );

    other.cbData = sizeof(other_buf);
    other.pbData = other_buf;
    CertStrToNameW( X509_ASN_ENCODING, L"CN=somebody else", CERT_X500_NAME_STR, NULL, other_buf, &other.cbData, NULL );
    para.rgIssuer = &other;
    chain = find( store, 0, &para, NULL, &err );
    CHECK( !chain && err == CRYPT_E_NOT_FOUND, "another issuer: CRYPT_E_NOT_FOUND (%#lx)", err );

    para.cIssuer = 0;
    para.pfnFindCallback = refuse;
    chain = find( store, 0, &para, NULL, &err );
    CHECK( !chain && callback_calls == 1, "the callback refuses it (%d calls)", callback_calls );
    para.pfnFindCallback = NULL;

    /* the same certificate without its key */
    nokey = CertOpenStore( CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL );
    CertAddEncodedCertificateToStore( nokey, X509_ASN_ENCODING, cert->pbCertEncoded, cert->cbCertEncoded,
                                      CERT_STORE_ADD_ALWAYS, &copy );
    chain = find( nokey, 0, &para, NULL, &err );
    CHECK( !chain && err == CRYPT_E_NOT_FOUND, "without a private key: not found (%#lx)", err );
    chain = find( nokey, CERT_CHAIN_FIND_BY_ISSUER_NO_KEY_FLAG, &para, NULL, &err );
    CHECK( chain != NULL, "unless CERT_CHAIN_FIND_BY_ISSUER_NO_KEY_FLAG (%#lx)", chain ? 0 : err );
    if (chain) CertFreeCertificateChain( chain );
    CertFreeCertificateContext( copy );
    CertCloseStore( nokey, 0 );

    chain = find( store, 0, NULL, NULL, &err );
    CHECK( !chain && err == CRYPT_E_NOT_FOUND, "NULL parameter: CRYPT_E_NOT_FOUND (%#lx)", err );

    CertCloseStore( store, 0 );
    CertFreeCertificateContext( cert );
done:
    CryptReleaseContext( hprov, 0 );
    CryptAcquireContextW( &hprov, container, NULL, PROV_RSA_FULL, CRYPT_DELETEKEYSET );
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
