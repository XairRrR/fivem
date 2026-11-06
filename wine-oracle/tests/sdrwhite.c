/* DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL) pour chaque écran actif. */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    DISPLAYCONFIG_PATH_INFO paths[16];
    DISPLAYCONFIG_MODE_INFO modes[32];
    UINT32 np = 16, nm = 32, i, ok = 0;
    LONG ret = QueryDisplayConfig( QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL );

    printf( "QueryDisplayConfig: %ld, %u paths\n", ret, np );
    for (i = 0; i < np; i++)
    {
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl = {{DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(wl),
                                             paths[i].targetInfo.adapterId, paths[i].targetInfo.id}};
        ret = DisplayConfigGetDeviceInfo( &wl.header );
        printf( "path %u: DisplayConfigGetDeviceInfo = %ld, SDRWhiteLevel = %lu (%.0f nits)\n",
                i, ret, wl.SDRWhiteLevel, wl.SDRWhiteLevel * 80.0 / 1000 );
        if (!ret && wl.SDRWhiteLevel) ok++;
    }
    printf( "RESULT: %s\n", np && ok == np ? "PASS" : "FAIL" );
    return 0;
}
