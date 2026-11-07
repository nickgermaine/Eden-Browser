from wpe.patch_support import replace_once
from wpe.adopted.jsc_switch import switched


def regexp_patch(text):
    text, switch = switched(text, "WEBKIT_EDEN_JSC_REGEXP_ALTERNATIVES", "mergeThreeAlternatives")
    text = replace_once(text, '#include <cstdlib>', '#include <cstdlib>\n#include <bit>')
    start = text.index('    static std::optional<MaskedAlternativeInfo> create(')
    end = text.index('\n};', start)
    body = text[start:end]
    body = replace_once(body, '        if (alternatives.size() != maxAlternatives)', f'''        {switch}
        if (alternatives.size() != maxAlternatives && !(mergeThreeAlternatives && alternatives.size() == 3))''')
    body = replace_once(body, '        for (unsigned i = 0; i < maxAlternatives; ++i) {', '        std::array<Alternative, 3> prefixes { };\n        for (unsigned i = 0; i < alternatives.size(); ++i) {')
    body = replace_once(body, '            info.alternatives[i].chars = chars;\n            info.alternatives[i].mask = mask;', '            prefixes[i].chars = chars & mask;\n            prefixes[i].mask = mask;')
    body = replace_once(body, '        info.numAlternatives = 2;', '''        if (alternatives.size() == 3) {
            unsigned bestBits = 0;
            unsigned first = 0;
            unsigned second = 1;
            uint32_t bestMask = 0;
            for (unsigned a = 0; a < 3; ++a) {
                for (unsigned b = a + 1; b < 3; ++b) {
                    uint32_t mask = prefixes[a].mask & prefixes[b].mask & ~(prefixes[a].chars ^ prefixes[b].chars);
                    unsigned bits = std::popcount(mask);
                    if (bits > bestBits) {
                        bestBits = bits;
                        first = a;
                        second = b;
                        bestMask = mask;
                    }
                }
            }
            if (bestBits < 20) {
                return std::nullopt;
            }
            info.alternatives[0] = { prefixes[first].chars & bestMask, bestMask };
            info.alternatives[1] = prefixes[3 - first - second];
        } else {
            info.alternatives[0] = prefixes[0];
            info.alternatives[1] = prefixes[1];
        }
        info.numAlternatives = 2;''')
    return text[:start] + body + text[end:]


PATCHES = {"Source/JavaScriptCore/yarr/YarrJIT.cpp": regexp_patch}
SOURCES = ["jsc_switch.py"]
