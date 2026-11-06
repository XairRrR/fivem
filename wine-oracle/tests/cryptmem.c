/* CryptProtectMemory / CryptUnprotectMemory / RtlEncryptMemory. Valeurs attendues relevées sur Windows
 * Server 2022 (tests/cryptmem-explore.c, oracle run 42310982647). */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

typedef LONG (WINAPI *rtl_crypt_fn)( void *, ULONG, ULONG );

static void fill( BYTE *p, int n ) { for (int i = 0; i < n; i++) p[i] = (BYTE)(i % 16 + 0x40); }
static int is_plain( const BYTE *p, int n ) { for (int i = 0; i < n; i++) if (p[i] != (BYTE)(i % 16 + 0x40)) return 0; return 1; }

/* child: exit code 1 if it recovers the plaintext of the hex buffer */
static int child( DWORD flags, const char *in )
{
    BYTE buf[64];
    int n = strlen( in ) / 2;
    for (int i = 0; i < n; i++) sscanf( in + 2 * i, "%2hhx", &buf[i] );
    if (!CryptUnprotectMemory( buf, n, flags )) return 2;
    return is_plain( buf, n );
}

static int child_recovers( DWORD flags, const BYTE *data, int n )
{
    char exe[MAX_PATH], cmd[512];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 3;
    int len;

    GetModuleFileNameA( NULL, exe, sizeof(exe) );
    len = snprintf( cmd, sizeof(cmd), "\"%s\" child %lu ", exe, flags );
    for (int i = 0; i < n; i++) len += snprintf( cmd + len, sizeof(cmd) - len, "%02x", data[i] );
    if (!CreateProcessA( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi )) return -1;
    WaitForSingleObject( pi.hProcess, 20000 );
    GetExitCodeProcess( pi.hProcess, &code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    return code;
}

int main( int argc, char **argv )
{
    static const int lengths[] = { 0, 1, 8, 15, 16, 17, 24, 32, 33 };
    BYTE a[48], b[48];
    rtl_crypt_fn pRtlEncryptMemory, pRtlDecryptMemory;
    HMODULE advapi = LoadLibraryA( "advapi32.dll" );
    BOOL r;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (argc == 4 && !strcmp( argv[1], "child" )) return child( atoi( argv[2] ), argv[3] );
    pRtlEncryptMemory = (rtl_crypt_fn)GetProcAddress( advapi, "SystemFunction040" );
    pRtlDecryptMemory = (rtl_crypt_fn)GetProcAddress( advapi, "SystemFunction041" );

    for (int i = 0; i < ARRAYSIZE(lengths); i++)
    {
        int len = lengths[i], valid = len && !(len % 8);
        fill( a, 48 );
        SetLastError( 0xdeadbeef );
        r = CryptProtectMemory( a, len, CRYPTPROTECTMEMORY_SAME_PROCESS );
        if (valid)
        {
            CHECK( r && !is_plain( a, len ), "length %d is encrypted", len );
            CHECK( CryptUnprotectMemory( a, len, CRYPTPROTECTMEMORY_SAME_PROCESS ) && is_plain( a, len ),
                   "length %d round trip", len );
        }
        else CHECK( !r && GetLastError() == ERROR_INVALID_PARAMETER && is_plain( a, 48 ),
                    "length %d fails with ERROR_INVALID_PARAMETER (%lu)", len, GetLastError() );
    }
    for (DWORD flags = 3; flags <= 8; flags++)
    {
        if (flags == 4) continue;
        fill( a, 16 );
        SetLastError( 0xdeadbeef );
        r = CryptProtectMemory( a, 16, flags );
        CHECK( !r && GetLastError() == ERROR_INVALID_PARAMETER, "flags %lu fail with ERROR_INVALID_PARAMETER (%lu)",
               flags, GetLastError() );
    }
    fill( a, 16 );
    CHECK( CryptProtectMemory( a, 16, 4 ) && !is_plain( a, 16 ), "flags 4 (for the system) encrypt" );
    SetLastError( 0xdeadbeef );
    CHECK( !CryptUnprotectMemory( a, 16, 4 ) && GetLastError() == ERROR_ACCESS_DENIED,
           "flags 4 cannot be decrypted by a user process (%lu)", GetLastError() );
    {
        DWORD err;
        SetLastError( 0xdeadbeef );
        r = CryptProtectMemory( NULL, 0, 0 );
        err = GetLastError();
        CHECK( !r, "NULL, length 0 fails (error %lu)", err );
    }
    CHECK( pRtlEncryptMemory( a, 15, 0 ) == (LONG)0xc000000d /* STATUS_INVALID_PARAMETER */, "RtlEncryptMemory length 15" );
    CHECK( pRtlEncryptMemory( a, 16, 7 ) == (LONG)0xc000000d /* STATUS_INVALID_PARAMETER */, "RtlEncryptMemory options 7" );
    fill( a, 16 );
    CHECK( !pRtlEncryptMemory( a, 16, 0 ) && !pRtlDecryptMemory( a, 16, 0 ) && is_plain( a, 16 ), "RtlEncryptMemory round trip" );

    for (DWORD flags = 0; flags <= 2; flags++)
    {
        fill( a, 48 ); fill( b, 48 );
        CryptProtectMemory( a, 32, flags );
        CryptProtectMemory( b, 32, flags );
        CHECK( !memcmp( a, b, 32 ), "flags %lu: deterministic", flags );
        CHECK( memcmp( a, a + 16, 16 ), "flags %lu: identical plaintext blocks give different blocks", flags );
        fill( b, 48 ); b[0] ^= 1;
        CryptProtectMemory( b, 32, flags );
        CHECK( memcmp( a + 16, b + 16, 16 ), "flags %lu: chained", flags );
        fill( b, 48 );
        CryptProtectMemory( b, 48, flags );
        CHECK( !memcmp( a, b, 32 ), "flags %lu: the ciphertext of a prefix is a prefix", flags );
        fill( b, 48 );
        CryptProtectMemory( b, 24, flags );
        /* Windows: a length that is not a multiple of 16 uses another cipher */
        CHECK( memcmp( a, b, 16 ), "flags %lu: 24 bytes do not start like 32", flags );
        for (DWORD other = 0; other <= 2; other++)
        {
            if (other == flags) continue;
            memcpy( b, a, 32 );
            CHECK( CryptUnprotectMemory( b, 32, other ) && !is_plain( b, 32 ),
                   "flags %lu: decrypting with flags %lu succeeds with garbage", flags, other );
        }
        {
            int got = child_recovers( flags, a, 32 );
            CHECK( got == (flags ? 1 : 0), "flags %lu: another process %s the data (%d)", flags,
                   flags ? "decrypts" : "cannot decrypt", got );
        }
        CHECK( CryptUnprotectMemory( a, 32, flags ) && is_plain( a, 32 ), "flags %lu: round trip", flags );
    }
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
