/* Exploration : LoadKeyboardLayoutW / UnloadKeyboardLayout / GetKeyboardLayoutList et traductions
 * MapVirtualKeyEx / VkKeyScanEx / ToUnicodeEx avec une disposition chargée mais non active. */
#include <windows.h>
#include <stdio.h>

static void list( const char *when )
{
    HKL layouts[32];
    int n = GetKeyboardLayoutList( 32, layouts );
    printf( "%-28s current %p, %d in list:", when, GetKeyboardLayout( 0 ), n );
    for (int i = 0; i < n; i++) printf( " %p", layouts[i] );
    printf( "\n" );
}

static void translate( const char *name, HKL hkl )
{
    BYTE state[256] = {0};
    WCHAR buf[8];
    int n;
    printf( "%s (%p):\n", name, hkl );
    printf( "  VK_TO_VSC  'A' %#x  'Q' %#x  'W' %#x  'Z' %#x  'M' %#x  VK_OEM_1 %#x\n",
            MapVirtualKeyExW( 'A', MAPVK_VK_TO_VSC, hkl ), MapVirtualKeyExW( 'Q', MAPVK_VK_TO_VSC, hkl ),
            MapVirtualKeyExW( 'W', MAPVK_VK_TO_VSC, hkl ), MapVirtualKeyExW( 'Z', MAPVK_VK_TO_VSC, hkl ),
            MapVirtualKeyExW( 'M', MAPVK_VK_TO_VSC, hkl ), MapVirtualKeyExW( VK_OEM_1, MAPVK_VK_TO_VSC, hkl ) );
    printf( "  VSC_TO_VK  0x10 %#x  0x11 %#x  0x1e %#x  0x27 %#x  0x2c %#x\n",
            MapVirtualKeyExW( 0x10, MAPVK_VSC_TO_VK, hkl ), MapVirtualKeyExW( 0x11, MAPVK_VSC_TO_VK, hkl ),
            MapVirtualKeyExW( 0x1e, MAPVK_VSC_TO_VK, hkl ), MapVirtualKeyExW( 0x27, MAPVK_VSC_TO_VK, hkl ),
            MapVirtualKeyExW( 0x2c, MAPVK_VSC_TO_VK, hkl ) );
    printf( "  VK_TO_CHAR 'A' %#x  VK_OEM_1 %#x  '2' %#x\n", MapVirtualKeyExW( 'A', MAPVK_VK_TO_CHAR, hkl ),
            MapVirtualKeyExW( VK_OEM_1, MAPVK_VK_TO_CHAR, hkl ), MapVirtualKeyExW( '2', MAPVK_VK_TO_CHAR, hkl ) );
    printf( "  VkKeyScanEx 'a' %#x  'q' %#x  'm' %#x  ';' %#x\n", VkKeyScanExW( L'a', hkl ), VkKeyScanExW( L'q', hkl ),
            VkKeyScanExW( L'm', hkl ), VkKeyScanExW( L';', hkl ) );
    n = ToUnicodeEx( 'Q', 0x10, state, buf, 8, 0, hkl );
    printf( "  ToUnicodeEx('Q', 0x10) %d \"%.*ls\"\n", n, n > 0 ? n : 0, buf );
    n = ToUnicodeEx( 'A', 0x1e, state, buf, 8, 0, hkl );
    printf( "  ToUnicodeEx('A', 0x1e) %d \"%.*ls\"\n", n, n > 0 ? n : 0, buf );
}

int main(void)
{
    HKL us, fr, de;

    setvbuf( stdout, NULL, _IONBF, 0 );
    printf( "user LCID %#lx, UI language %#x\n", GetUserDefaultLCID(), GetUserDefaultUILanguage() );
    list( "start" );
    us = LoadKeyboardLayoutW( L"00000409", 0 );
    printf( "LoadKeyboardLayout(00000409, 0) -> %p\n", us );
    list( "after loading US" );
    fr = LoadKeyboardLayoutW( L"0000040C", 0 );
    printf( "LoadKeyboardLayout(0000040C, 0) -> %p err %lu\n", fr, fr ? 0 : GetLastError() );
    list( "after loading FR" );
    de = LoadKeyboardLayoutW( L"00000407", KLF_NOTELLSHELL );
    printf( "LoadKeyboardLayout(00000407, NOTELLSHELL) -> %p\n", de );
    printf( "LoadKeyboardLayout(bogus) -> %p err %lu\n", LoadKeyboardLayoutW( L"0000ABCD", 0 ), GetLastError() );
    translate( "current", GetKeyboardLayout( 0 ) );
    translate( "US", us );
    translate( "FR", fr );
    translate( "DE", de );
    translate( "NULL", NULL );

    SetLastError( 0xdeadbeef );
    printf( "UnloadKeyboardLayout(FR) %d err %lu\n", UnloadKeyboardLayout( fr ), GetLastError() );
    list( "after unloading FR" );
    SetLastError( 0xdeadbeef );
    printf( "UnloadKeyboardLayout(FR again) %d err %lu\n", UnloadKeyboardLayout( fr ), GetLastError() );
    SetLastError( 0xdeadbeef );
    printf( "UnloadKeyboardLayout(bogus) %d err %lu\n", UnloadKeyboardLayout( (HKL)0x12345678 ), GetLastError() );
    translate( "FR after unload", fr );
    printf( "ActivateKeyboardLayout(DE) -> %p\n", ActivateKeyboardLayout( de, 0 ) );
    list( "after activating DE" );
    translate( "current (DE)", GetKeyboardLayout( 0 ) );
    ActivateKeyboardLayout( us, 0 );
    UnloadKeyboardLayout( de );
    list( "end" );
    printf( "RESULT: PASS\n" );   /* exploration only */
    return 0;
}
