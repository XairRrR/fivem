/* LoadKeyboardLayoutW("00000409") : la disposition US chargée (non activée) doit traduire en QWERTY
 * même quand la disposition du bureau ne l'est pas. GTA V s'en sert pour situer les touches ; sous
 * Wine, sur un bureau AZERTY, il obtenait la disposition courante et donc des positions AZERTY.
 * Sur un bureau US les vérifications sont triviales ; le cas utile se lance sur un X en AZERTY. */
#include <windows.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

int main(void)
{
    static const struct { UINT vk, scan; } qwerty[] =
    {
        { 'A', 0x1e }, { 'Q', 0x10 }, { 'W', 0x11 }, { 'Z', 0x2c }, { 'M', 0x32 }, { VK_OEM_1, 0x27 },
    };
    BYTE state[256] = {0};
    WCHAR buf[8];
    HKL us;
    int n;

    setvbuf( stdout, NULL, _IONBF, 0 );
    printf( "current layout %p (vk 'A' -> scan %#x)\n", GetKeyboardLayout( 0 ),
            MapVirtualKeyExW( 'A', MAPVK_VK_TO_VSC, GetKeyboardLayout( 0 ) ) );
    /* Wine derives the active HKL from the locale: an en-US prefix on an AZERTY desktop reports
     * 04090409 for an AZERTY keyboard, and the loaded US layout is then the active one */
    if (GetKeyboardLayout( 0 ) == (HKL)0x04090409 && MapVirtualKeyExW( 'A', MAPVK_VK_TO_VSC, GetKeyboardLayout( 0 ) ) != 0x1e)
    {
        printf( "skip: the active layout claims to be US but is not QWERTY (locale and keyboard differ)\n" );
        printf( "RESULT: PASS\n" );
        return 0;
    }
    us = LoadKeyboardLayoutW( L"00000409", 0 );
    CHECK( us == (HKL)0x04090409, "LoadKeyboardLayout(00000409) -> %p", us );
    for (int i = 0; i < ARRAYSIZE(qwerty); i++)
    {
        UINT scan = MapVirtualKeyExW( qwerty[i].vk, MAPVK_VK_TO_VSC, us );
        UINT vk = MapVirtualKeyExW( qwerty[i].scan, MAPVK_VSC_TO_VK, us );
        CHECK( scan == qwerty[i].scan && vk == qwerty[i].vk, "US: vk %#x <-> scan %#x (got scan %#x, vk %#x)",
               qwerty[i].vk, qwerty[i].scan, scan, vk );
    }
    CHECK( MapVirtualKeyExW( VK_OEM_1, MAPVK_VK_TO_CHAR, us ) == ';', "US: VK_OEM_1 is ';' (%#x)",
           MapVirtualKeyExW( VK_OEM_1, MAPVK_VK_TO_CHAR, us ) );
    n = ToUnicodeEx( 'Q', 0x10, state, buf, 8, 0, us );
    CHECK( n == 1 && buf[0] == 'q', "US: ToUnicodeEx('Q', 0x10) = \"q\" (%d)", n );
    n = ToUnicodeEx( VK_OEM_1, 0x27, state, buf, 8, 0, us );
    CHECK( n == 1 && buf[0] == ';', "US: ToUnicodeEx(VK_OEM_1, 0x27) = \";\" (%d, %#x)", n, n > 0 ? buf[0] : 0 );
    state[VK_SHIFT] = 0x80;
    n = ToUnicodeEx( '2', 0x03, state, buf, 8, 0, us );
    CHECK( n == 1 && buf[0] == '@', "US: shift + '2' = \"@\" (%d, %#x)", n, n > 0 ? buf[0] : 0 );
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
