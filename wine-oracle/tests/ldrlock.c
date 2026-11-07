/* LdrLockLoaderLock / LdrUnlockLoaderLock et LDR_LOCK_LOADER_LOCK_FLAG_RAISE_ON_ERRORS (1) : une erreur
 * est levée en exception au lieu d'être renvoyée. Appelé avec le flag 1 dans le jeu (128 fois). */
#include <windows.h>
#include <winternl.h>
#include <setjmp.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

typedef LONG (WINAPI *lock_fn)( ULONG, ULONG *, ULONG_PTR * );
typedef LONG (WINAPI *unlock_fn)( ULONG, ULONG_PTR );

static jmp_buf jmp;
static DWORD raised;

static LONG CALLBACK handler( EXCEPTION_POINTERS *ptrs )
{
    raised = ptrs->ExceptionRecord->ExceptionCode;
    longjmp( jmp, 1 );
}

int main(void)
{
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    lock_fn pLdrLockLoaderLock = (lock_fn)GetProcAddress( ntdll, "LdrLockLoaderLock" );
    unlock_fn pLdrUnlockLoaderLock = (unlock_fn)GetProcAddress( ntdll, "LdrUnlockLoaderLock" );
    ULONG result = 0xdead;
    ULONG_PTR magic = 0;
    LONG status;
    void *veh;

    setvbuf( stdout, NULL, _IONBF, 0 );
    status = pLdrLockLoaderLock( 1, &result, &magic );
    CHECK( !status && result == 1 && magic, "lock with RAISE_ON_ERRORS (%#lx, result %lu)", status, result );
    status = pLdrUnlockLoaderLock( 1, magic );
    CHECK( !status, "unlock with RAISE_ON_ERRORS (%#lx)", status );
    status = pLdrLockLoaderLock( 3, &result, &magic );
    CHECK( !status && result == 1, "try-lock with RAISE_ON_ERRORS (%#lx, result %lu)", status, result );
    pLdrUnlockLoaderLock( 0, magic );

    status = pLdrLockLoaderLock( 4, &result, &magic );
    CHECK( status == (LONG)0xc00000ef /* STATUS_INVALID_PARAMETER_1 */, "unknown flag without raise: returned (%#lx)", status );
    status = pLdrLockLoaderLock( 0, &result, NULL );
    CHECK( status == (LONG)0xc00000f1 /* STATUS_INVALID_PARAMETER_3 */, "no magic without raise: returned (%#lx)", status );

    veh = AddVectoredExceptionHandler( 1, handler );
    raised = 0;
    if (!setjmp( jmp )) status = pLdrLockLoaderLock( 1, &result, NULL );
    CHECK( raised == 0xc00000f1, "no magic with RAISE_ON_ERRORS: raised (%#lx)", raised );
    raised = 0;
    if (!setjmp( jmp )) status = pLdrUnlockLoaderLock( 1, 0x1234 );
    CHECK( raised == 0xc00000f0 /* STATUS_INVALID_PARAMETER_2 */, "bad magic with RAISE_ON_ERRORS: raised (%#lx)", raised );
    RemoveVectoredExceptionHandler( veh );

    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
