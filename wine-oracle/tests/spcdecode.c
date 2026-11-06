/* Décode un SpcIndirectDataContent (OID 1.3.6.1.4.1.311.2.1.4) avec CryptDecodeObjectEx :
 * crypt32 doit passer par le décodeur enregistré par wintrust. */
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <stdio.h>

int main(void)
{
    /* SpcIndirectDataContent { data { 1.3.6.1.4.1.311.2.1.15, NULL }, digest { sha1, 20 x 0x11 } } */
    static const BYTE der[] = {
        0x30,0x31, 0x30,0x0e, 0x06,0x0a,0x2b,0x06,0x01,0x04,0x01,0x82,0x37,0x02,0x01,0x0f, 0x05,0x00,
        0x30,0x1f, 0x30,0x07, 0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a, 0x04,0x14,
        0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11 };
    SPC_INDIRECT_DATA_CONTENT *content = NULL;
    DWORD size = 0;
    BOOL ret = CryptDecodeObjectEx( X509_ASN_ENCODING, SPC_INDIRECT_DATA_OBJID, der, sizeof(der),
                                    CRYPT_DECODE_ALLOC_FLAG, NULL, &content, &size );
    printf( "CryptDecodeObjectEx(SPC_INDIRECT_DATA): %d err %lu\n", ret, ret ? 0 : GetLastError() );
    if (ret) printf( "data oid %s, digest oid %s, %lu bytes\n", content->Data.pszObjId,
                     content->DigestAlgorithm.pszObjId, content->Digest.cbData );
    printf( "RESULT: %s\n", ret && content->Digest.cbData == 20 ? "PASS" : "FAIL" );
    return 0;
}
