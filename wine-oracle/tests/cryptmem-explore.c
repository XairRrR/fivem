/* Exploration de CryptProtectMemory / CryptUnprotectMemory et RtlEncryptMemory (SystemFunction040/041) :
 * erreurs, déterminisme, chaînage des blocs, déchiffrement depuis un autre processus. Pas de PASS/FAIL. */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#define GUARD(what, call) __try { call; } __except (EXCEPTION_EXECUTE_HANDLER) { \
    printf( "%-34s EXCEPTION %08lx\n", what, GetExceptionCode() ); }
#else
#define GUARD(what, call) do { call; } while (0)
#endif

typedef LONG (WINAPI *rtl_crypt_fn)( void *, ULONG, ULONG );
static rtl_crypt_fn pRtlEncryptMemory, pRtlDecryptMemory;

static void hex( const char *what, const BYTE *p, int n )
{
    printf( "%-34s", what );
    for (int i = 0; i < n; i++) printf( "%02x", p[i] );
    printf( "\n" );
}

static void fill( BYTE *p, int n ) { for (int i = 0; i < n; i++) p[i] = (BYTE)(i % 16 + 0x40); }

/* child: decrypt the hex buffer given on the command line with the given flags */
static int child( DWORD flags, const char *in )
{
    BYTE buf[64];
    int n = strlen( in ) / 2;
    for (int i = 0; i < n; i++) sscanf( in + 2 * i, "%2hhx", &buf[i] );
    SetLastError( 0xdeadbeef );
    BOOL r = CryptUnprotectMemory( buf, n, flags );
    printf( "  child unprotect flags %lu: %d err %lu, plaintext %s\n", flags, r, r ? 0 : GetLastError(),
            buf[0] == 0x40 && buf[1] == 0x41 && buf[15] == 0x4f ? "RECOVERED" : "not recovered" );
    return 0;
}

int main( int argc, char **argv )
{
    BYTE a[32], b[32], c[48];
    char exe[MAX_PATH], cmd[512];
    HMODULE advapi = LoadLibraryA( "advapi32.dll" );

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (argc == 4 && !strcmp( argv[1], "child" )) return child( atoi( argv[2] ), argv[3] );
    pRtlEncryptMemory = (rtl_crypt_fn)GetProcAddress( advapi, "SystemFunction040" );
    pRtlDecryptMemory = (rtl_crypt_fn)GetProcAddress( advapi, "SystemFunction041" );
    printf( "CRYPTPROTECTMEMORY_BLOCK_SIZE %d\n", CRYPTPROTECTMEMORY_BLOCK_SIZE );

    for (int len = 0; len <= 33; len++)
    {
        if (len != 0 && len != 1 && len != 8 && len != 15 && len != 16 && len != 17 && len != 24 && len != 32 && len != 33)
            continue;
        fill( a, 32 );
        SetLastError( 0xdeadbeef );
        BOOL r = CryptProtectMemory( a, len, CRYPTPROTECTMEMORY_SAME_PROCESS );
        printf( "protect len %-2d: %d err %lu\n", len, r, r ? 0 : GetLastError() );
        if (r && len) CryptUnprotectMemory( a, len, CRYPTPROTECTMEMORY_SAME_PROCESS );
    }
    for (DWORD flags = 0; flags <= 8; flags++)
    {
        fill( a, 16 );
        SetLastError( 0xdeadbeef );
        BOOL r = CryptProtectMemory( a, 16, flags );
        printf( "protect flags %lu: %d err %lu\n", flags, r, r ? 0 : GetLastError() );
        if (r)
        {
            SetLastError( 0xdeadbeef );
            r = CryptUnprotectMemory( a, 16, flags );
            printf( "  unprotect: %d err %lu, %s\n", r, r ? 0 : GetLastError(), a[0] == 0x40 && a[15] == 0x4f ? "round trip" : "DIFFERENT" );
        }
    }
    /* (the arguments of printf are evaluated in no given order: read the error first) */
    {
        BOOL r; DWORD err;
        SetLastError( 0xdeadbeef );
        r = CryptProtectMemory( NULL, 0, 0 ); err = GetLastError();
        printf( "protect NULL len 0: %d err %lu\n", r, err );
        SetLastError( 0xdeadbeef );
        GUARD( "protect NULL len 16", r = CryptProtectMemory( NULL, 16, 0 ); err = GetLastError();
               printf( "protect NULL len 16: %d err %lu\n", r, err ) );
        SetLastError( 0xdeadbeef );
        GUARD( "unprotect NULL len 16", r = CryptUnprotectMemory( NULL, 16, 0 ); err = GetLastError();
               printf( "unprotect NULL len 16: %d err %lu\n", r, err ) );
    }
    if (pRtlEncryptMemory)
    {
        fill( a, 16 );
        printf( "RtlEncryptMemory len 15: %#lx\n", pRtlEncryptMemory( a, 15, 0 ) );
        printf( "RtlEncryptMemory flags 7: %#lx\n", pRtlEncryptMemory( a, 16, 7 ) );
        printf( "RtlEncryptMemory len 16: %#lx\n", pRtlEncryptMemory( a, 16, 0 ) );
        printf( "RtlDecryptMemory len 16: %#lx\n", pRtlDecryptMemory( a, 16, 0 ) );
    }

    for (DWORD flags = 0; flags <= 2; flags++)
    {
        printf( "flags %lu:\n", flags );
        fill( a, 32 ); fill( b, 32 );
        CryptProtectMemory( a, 32, flags );
        CryptProtectMemory( b, 32, flags );
        hex( "  two identical blocks ->", a, 32 );
        printf( "  same plaintext twice gives %s ciphertext\n", memcmp( a, b, 32 ) ? "a different" : "the same" );
        printf( "  block 1 %s block 0 (identical plaintext blocks)\n", memcmp( a, a + 16, 16 ) ? "differs from" : "equals" );
        fill( c, 48 ); c[0] ^= 1;
        memcpy( b, c, 32 ); fill( c, 48 );
        CryptProtectMemory( b, 32, flags );
        printf( "  changing byte 0 changes block 1: %s\n", memcmp( a + 16, b + 16, 16 ) ? "yes (chained)" : "no" );
        fill( c, 48 );
        CryptProtectMemory( c, 48, flags );
        printf( "  48-byte buffer: first 32 bytes %s the 32-byte ciphertext\n", memcmp( a, c, 32 ) ? "differ from" : "equal" );
        /* decrypt with other flags */
        for (DWORD other = 0; other <= 2; other++)
        {
            if (other == flags) continue;
            memcpy( b, a, 32 );
            SetLastError( 0xdeadbeef );
            BOOL r = CryptUnprotectMemory( b, 32, other );
            printf( "  unprotect with flags %lu: %d err %lu, %s\n", other, r, r ? 0 : GetLastError(),
                    b[0] == 0x40 && b[15] == 0x4f ? "RECOVERED" : "not recovered" );
        }
        /* another process */
        {
            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            int n = snprintf( cmd, sizeof(cmd), "\"%s\" child %lu ", (GetModuleFileNameA( NULL, exe, sizeof(exe) ), exe), flags );
            for (int i = 0; i < 32; i++) n += snprintf( cmd + n, sizeof(cmd) - n, "%02x", a[i] );
            if (CreateProcessA( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
            {
                WaitForSingleObject( pi.hProcess, 10000 );
                CloseHandle( pi.hProcess );
                CloseHandle( pi.hThread );
            }
        }
    }
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
