/* Emoji through DirectWrite, as Chromium (FiveM's menu, server descriptions) and FiveM's font
 * renderer (the in-game "FiveM (bNNNN) <emoji>" mark) use it: "Segoe UI Emoji" exists, the system
 * font fallback maps emoji code points to a color font, and its color glyph runs are produced.
 * Under Wine the prefix aliases Segoe UI Emoji to a color emoji font of the system (Twemoji
 * Mozilla, COLR) and Segoe UI to a sans font. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dwrite_3.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

struct source
{
    IDWriteTextAnalysisSource iface;
    const WCHAR *text;
    UINT32 length;
};

static HRESULT WINAPI src_QueryInterface( IDWriteTextAnalysisSource *iface, REFIID riid, void **obj )
{
    static const GUID iid_unknown = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
    if (IsEqualIID( riid, &IID_IDWriteTextAnalysisSource ) || IsEqualIID( riid, &iid_unknown ))
    {
        *obj = iface;
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI src_AddRef( IDWriteTextAnalysisSource *iface ) { return 2; }
static ULONG WINAPI src_Release( IDWriteTextAnalysisSource *iface ) { return 1; }
static HRESULT WINAPI src_GetTextAtPosition( IDWriteTextAnalysisSource *iface, UINT32 pos, const WCHAR **text, UINT32 *len )
{
    struct source *s = CONTAINING_RECORD( iface, struct source, iface );
    *text = pos < s->length ? s->text + pos : NULL;
    *len = pos < s->length ? s->length - pos : 0;
    return S_OK;
}
static HRESULT WINAPI src_GetTextBeforePosition( IDWriteTextAnalysisSource *iface, UINT32 pos, const WCHAR **text, UINT32 *len )
{
    *text = NULL;
    *len = 0;
    return S_OK;
}
static DWRITE_READING_DIRECTION WINAPI src_GetParagraphReadingDirection( IDWriteTextAnalysisSource *iface )
{
    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}
static HRESULT WINAPI src_GetLocaleName( IDWriteTextAnalysisSource *iface, UINT32 pos, UINT32 *len, const WCHAR **locale )
{
    *locale = L"en-us";
    *len = 1;
    return S_OK;
}
static HRESULT WINAPI src_GetNumberSubstitution( IDWriteTextAnalysisSource *iface, UINT32 pos, UINT32 *len,
                                                 IDWriteNumberSubstitution **sub )
{
    *sub = NULL;
    return S_OK;
}
static IDWriteTextAnalysisSourceVtbl src_vtbl =
{
    src_QueryInterface, src_AddRef, src_Release, src_GetTextAtPosition, src_GetTextBeforePosition,
    src_GetParagraphReadingDirection, src_GetLocaleName, src_GetNumberSubstitution,
};

static void family_name( IDWriteFont *font, WCHAR *out, UINT32 size )
{
    IDWriteFontFamily *family;
    IDWriteLocalizedStrings *names;

    out[0] = 0;
    if (FAILED(IDWriteFont_GetFontFamily( font, &family ))) return;
    if (SUCCEEDED(IDWriteFontFamily_GetFamilyNames( family, &names )))
    {
        IDWriteLocalizedStrings_GetString( names, 0, out, size );
        IDWriteLocalizedStrings_Release( names );
    }
    IDWriteFontFamily_Release( family );
}

/* the fallback font of one emoji, and whether DirectWrite gives color layers for it */
static void check_emoji( IDWriteFactory2 *factory, IDWriteFontFallback *fallback, IDWriteFontCollection *collection,
                         const WCHAR *emoji, const char *label )
{
    struct source src = { { &src_vtbl }, emoji, (UINT32)wcslen( emoji ) };
    IDWriteColorGlyphRunEnumerator *layers = NULL;
    IDWriteFontFace *face = NULL;
    IDWriteFont *font = NULL;
    DWRITE_GLYPH_RUN run = {0};
    UINT32 mapped = 0, cp, count = 0;
    UINT16 glyph = 0;
    float scale = 0;
    WCHAR name[64];
    HRESULT hr;

    hr = IDWriteFontFallback_MapCharacters( fallback, &src.iface, 0, src.length, collection, L"Segoe UI",
                                            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                            &mapped, &font, &scale );
    if (FAILED(hr) || !font)
    {
        CHECK( 0, "%s: no fallback font (%#lx)", label, hr );
        return;
    }
    family_name( font, name, ARRAYSIZE(name) );
    IDWriteFont_CreateFontFace( font, &face );
    cp = emoji[0] >= 0xd800 && emoji[0] < 0xdc00 ? 0x10000 + ((emoji[0] - 0xd800) << 10) + (emoji[1] - 0xdc00) : emoji[0];
    IDWriteFontFace_GetGlyphIndices( face, &cp, 1, &glyph );
    run.fontFace = face;
    run.fontEmSize = 32;
    run.glyphCount = 1;
    run.glyphIndices = &glyph;
    hr = IDWriteFactory2_TranslateColorGlyphRun( factory, 0, 0, &run, NULL, DWRITE_MEASURING_MODE_NATURAL, NULL, 0, &layers );
    if (SUCCEEDED(hr))
    {
        BOOL more = FALSE;
        while (SUCCEEDED(IDWriteColorGlyphRunEnumerator_MoveNext( layers, &more )) && more) count++;
        IDWriteColorGlyphRunEnumerator_Release( layers );
    }
    CHECK( glyph && count > 1, "%s: fallback %ls, glyph %u, %u color layers (%#lx)", label, name, glyph, count, hr );
    IDWriteFontFace_Release( face );
    IDWriteFont_Release( font );
}

int main(void)
{
    IDWriteFontCollection *collection;
    IDWriteFontFallback *fallback;
    IDWriteFactory2 *factory;
    UINT32 index;
    BOOL exists;
    HRESULT hr;

    setvbuf( stdout, NULL, _IONBF, 0 );
    hr = DWriteCreateFactory( DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory2, (IUnknown **)&factory );
    if (FAILED(hr)) { printf( "no IDWriteFactory2 (%#lx)\nRESULT: FAIL\n", hr ); return 1; }
    IDWriteFactory2_GetSystemFontCollection( factory, (IDWriteFontCollection **)&collection, FALSE );

    exists = FALSE;
    IDWriteFontCollection_FindFamilyName( collection, L"Segoe UI Emoji", &index, &exists );
    CHECK( exists, "Segoe UI Emoji family exists" );
    exists = FALSE;
    IDWriteFontCollection_FindFamilyName( collection, L"Segoe UI", &index, &exists );
    CHECK( exists, "Segoe UI family exists (FiveM's in-game mark)" );

    IDWriteFactory2_GetSystemFontFallback( factory, &fallback );
    check_emoji( factory, fallback, collection, L"\xD83D\xDD25", "U+1F525 fire" );
    check_emoji( factory, fallback, collection, L"\xD83E\xDD59", "U+1F959 stuffed flatbread (FiveM's evening mark)" );
    check_emoji( factory, fallback, collection, L"\x2705", "U+2705 check mark" );

    IDWriteFontFallback_Release( fallback );
    IDWriteFontCollection_Release( collection );
    IDWriteFactory2_Release( factory );
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
