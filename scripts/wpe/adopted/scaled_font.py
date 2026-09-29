from wpe.patch_support import replace_once


def font_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    anchor = '    float size = fontPlatformData.size();'
    return replace_once(text, anchor, anchor + '''
    static bool reuseScaledFont = [] {
        const char* value = std::getenv("WEBKIT_EDEN_REUSE_SCALED_FONT");
        return !value || std::strcmp(value, "0");
    }();
    if (reuseScaledFont && m_scaledFont == fontPlatformData.skFont() && m_isColorBitmapFont == fontPlatformData.isColorBitmapFont() && hb_font_get_ptem(m_font.get()) == size) {
        return m_font.get();
    }''')


PATCHES = {
    'Source/WebCore/platform/graphics/skia/SkiaHarfBuzzFont.cpp': font_patch,
}
