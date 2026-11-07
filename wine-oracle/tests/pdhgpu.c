/* PDH : l'utilisation GPU lue par l'overlay de FiveM (conhost-v2, cl_drawGpuUsage), même code :
 * \GPU Engine(*)\Utilization Percentage, instances filtrées par le LUID de l'adaptateur DXGI et
 * « _engtype_3D », puis PdhFormatFromRawValue entre deux tableaux bruts. PdhFormatFromRawValue était
 * un stub (exception) et PdhGetRawCounterArrayW n'était pas implémenté. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

static LUID adapter_luid(void)
{
    LUID luid = { (DWORD)-1, -1 };
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapter;
    DXGI_ADAPTER_DESC1 desc;

    if (FAILED( CreateDXGIFactory1( &IID_IDXGIFactory1, (void **)&factory ) )) return luid;
    if (SUCCEEDED( IDXGIFactory1_EnumAdapters1( factory, 0, &adapter ) ))
    {
        if (SUCCEEDED( IDXGIAdapter1_GetDesc1( adapter, &desc ) ))
        {
            luid = desc.AdapterLuid;
            printf( "adapter %ls, luid %08lx:%08lx\n", desc.Description, luid.HighPart, luid.LowPart );
        }
        IDXGIAdapter1_Release( adapter );
    }
    IDXGIFactory1_Release( factory );
    return luid;
}

static PDH_RAW_COUNTER_ITEM_W *raw_array( PDH_HCOUNTER counter, DWORD *items, PDH_STATUS *status )
{
    DWORD size = 0;
    PDH_RAW_COUNTER_ITEM_W *buffer;

    *items = 0;
    *status = PdhGetRawCounterArrayW( counter, &size, items, NULL );
    if (*status != PDH_MORE_DATA && *status) return NULL;
    if (!(buffer = malloc( size ? size : 1 ))) return NULL;
    *status = PdhGetRawCounterArrayW( counter, &size, items, buffer );
    return buffer;
}

int main(void)
{
    PDH_HQUERY query;
    PDH_HCOUNTER counter;
    PDH_RAW_COUNTER_ITEM_W *first, *second;
    PDH_FMT_COUNTERVALUE value;
    PDH_COUNTER_INFO_W *info;
    DWORD items1, items2, size = 0, type = 0, matched = 0;
    PDH_STATUS status;
    WCHAR ref[64];
    LUID luid;
    double total = 0;

    setvbuf( stdout, NULL, _IONBF, 0 );
    luid = adapter_luid();
    if (luid.HighPart == -1)
    {
        printf( "skip: no DXGI adapter\nRESULT: PASS\n" );
        return 0;
    }
    swprintf( ref, ARRAYSIZE(ref), L"luid_0x%08X_0x%08X", luid.HighPart, luid.LowPart );

    PdhOpenQueryW( NULL, 0, &query );
    status = PdhAddEnglishCounterW( query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter );
    CHECK( !status, "add \\GPU Engine(*)\\Utilization Percentage (%#lx)", status );
    if (status) goto done;
    PdhCollectQueryData( query );
    first = raw_array( counter, &items1, &status );
    CHECK( first && !status && items1 > 0, "first raw array: %lu items (%#lx)", items1, status );

    PdhGetCounterInfoW( counter, FALSE, &size, NULL );
    if (size && (info = malloc( size )) && !PdhGetCounterInfoW( counter, FALSE, &size, info ))
    {
        type = info->dwType;
        free( info );
    }
    CHECK( type != 0, "counter type %#lx", type );

    Sleep( 700 );
    PdhCollectQueryData( query );
    second = raw_array( counter, &items2, &status );
    CHECK( second && !status && items2 == items1, "second raw array: %lu items", items2 );
    if (!first || !second) goto done;

    for (DWORD i = 0; i < items2; i++)
    {
        printf( "  instance %ls\n", second[i].szName );
        if (!wcsstr( second[i].szName, ref ) || !wcsstr( second[i].szName, L"_engtype_3D" )) continue;
        for (DWORD j = 0; j < items1; j++)
        {
            if (wcscmp( first[j].szName, second[i].szName )) continue;
            status = PdhFormatFromRawValue( type, PDH_FMT_DOUBLE, NULL, &second[i].RawValue, &first[j].RawValue, &value );
            CHECK( !status && value.doubleValue >= 0 && value.doubleValue <= 100, "%ls: %.1f%%", second[i].szName,
                   value.doubleValue );
            total += value.doubleValue;
            matched++;
        }
    }
    CHECK( matched > 0, "an instance of the DXGI adapter (%ls) with the 3D engine", ref );
    printf( "GPU usage %.1f%%\n", total );
    free( first );
    free( second );
done:
    PdhCloseQuery( query );
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
