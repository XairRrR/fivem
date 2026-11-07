/* Emoji through DirectWrite, as Chromium (FiveM's menu, server descriptions) and FiveM's font
 * renderer (the in-game "FiveM (bNNNN) <emoji>" mark) use it: "Segoe UI Emoji" and "Segoe UI"
 * exist, the system font fallback maps emoji code points to a color font, and its color glyph
 * runs are produced. Under Wine the prefix aliases Segoe UI Emoji to a color emoji font of the
 * system (Twemoji Mozilla, COLR) and Segoe UI to a sans font (tools/setup-fonts.sh).
 * C++: the Windows SDK's DirectWrite headers have no C interfaces. */
#include <windows.h>
#include <dwrite_3.h>
#include <stdio.h>
#include <wchar.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf( "FAIL: " __VA_ARGS__ ); printf( "\n" ); } \
                              else { printf( "ok: " __VA_ARGS__ ); printf( "\n" ); } } while (0)

class Source : public IDWriteTextAnalysisSource
{
public:
    Source( const WCHAR *text ) : m_text( text ), m_length( (UINT32)wcslen( text ) ) {}
    UINT32 length() const { return m_length; }

    HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void **obj ) override
    {
        if (riid == __uuidof(IDWriteTextAnalysisSource) || riid == __uuidof(IUnknown))
        {
            *obj = this;
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition( UINT32 pos, const WCHAR **text, UINT32 *len ) override
    {
        *text = pos < m_length ? m_text + pos : nullptr;
        *len = pos < m_length ? m_length - pos : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition( UINT32, const WCHAR **text, UINT32 *len ) override
    {
        *text = nullptr;
        *len = 0;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
    {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName( UINT32, UINT32 *len, const WCHAR **locale ) override
    {
        *locale = L"en-us";
        *len = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution( UINT32, UINT32 *len, IDWriteNumberSubstitution **sub ) override
    {
        *sub = nullptr;
        *len = 1;
        return S_OK;
    }

private:
    const WCHAR *m_text;
    UINT32 m_length;
};

static void family_name( IDWriteFont *font, WCHAR *out, UINT32 size )
{
    IDWriteFontFamily *family = nullptr;
    IDWriteLocalizedStrings *names = nullptr;

    out[0] = 0;
    if (FAILED(font->GetFontFamily( &family ))) return;
    if (SUCCEEDED(family->GetFamilyNames( &names )))
    {
        names->GetString( 0, out, size );
        names->Release();
    }
    family->Release();
}

/* the fallback font of one emoji, and whether DirectWrite gives color layers for it */
static void check_emoji( IDWriteFactory2 *factory, IDWriteFontFallback *fallback, IDWriteFontCollection *collection,
                         const WCHAR *emoji, const char *label )
{
    Source source( emoji );
    IDWriteColorGlyphRunEnumerator *layers = nullptr;
    IDWriteFontFace *face = nullptr;
    IDWriteFont *font = nullptr;
    DWRITE_GLYPH_RUN run = {};
    UINT32 mapped = 0, cp, count = 0;
    UINT16 glyph = 0;
    float scale = 0;
    WCHAR name[64];
    HRESULT hr;

    hr = fallback->MapCharacters( &source, 0, source.length(), collection, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, &mapped, &font, &scale );
    if (FAILED(hr) || !font)
    {
        CHECK( 0, "%s: no fallback font (%#lx)", label, hr );
        return;
    }
    family_name( font, name, ARRAYSIZE(name) );
    font->CreateFontFace( &face );
    cp = emoji[0] >= 0xd800 && emoji[0] < 0xdc00 ? 0x10000 + ((emoji[0] - 0xd800) << 10) + (emoji[1] - 0xdc00) : emoji[0];
    face->GetGlyphIndices( &cp, 1, &glyph );
    run.fontFace = face;
    run.fontEmSize = 32;
    run.glyphCount = 1;
    run.glyphIndices = &glyph;
    hr = factory->TranslateColorGlyphRun( 0, 0, &run, nullptr, DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, &layers );
    if (SUCCEEDED(hr))
    {
        BOOL more = FALSE;
        while (SUCCEEDED(layers->MoveNext( &more )) && more) count++;
        layers->Release();
    }
    CHECK( glyph && count > 1, "%s: fallback %ls, glyph %u, %u color layers (%#lx)", label, name, glyph, count, hr );
    face->Release();
    font->Release();
}

int main()
{
    IDWriteFontCollection *collection = nullptr;
    IDWriteFontFallback *fallback = nullptr;
    IDWriteFactory2 *factory = nullptr;
    UINT32 index;
    BOOL exists;
    HRESULT hr;

    setvbuf( stdout, nullptr, _IONBF, 0 );
    hr = DWriteCreateFactory( DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), (IUnknown **)&factory );
    if (FAILED(hr)) { printf( "no IDWriteFactory2 (%#lx)\nRESULT: FAIL\n", hr ); return 1; }
    factory->GetSystemFontCollection( &collection, FALSE );

    exists = FALSE;
    collection->FindFamilyName( L"Segoe UI Emoji", &index, &exists );
    CHECK( exists, "Segoe UI Emoji family exists" );
    exists = FALSE;
    collection->FindFamilyName( L"Segoe UI", &index, &exists );
    CHECK( exists, "Segoe UI family exists (FiveM's in-game mark)" );

    factory->GetSystemFontFallback( &fallback );
    check_emoji( factory, fallback, collection, L"\xD83D\xDD25", "U+1F525 fire" );
    check_emoji( factory, fallback, collection, L"\xD83E\xDD59", "U+1F959 stuffed flatbread (FiveM's evening mark)" );
    check_emoji( factory, fallback, collection, L"\x2705", "U+2705 check mark" );

    fallback->Release();
    collection->Release();
    factory->Release();
    printf( "RESULT: %s\n", failures ? "FAIL" : "PASS" );
    return 0;
}
