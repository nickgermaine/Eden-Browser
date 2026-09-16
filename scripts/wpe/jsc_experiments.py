import re
import subprocess
import tarfile

from .engine_experiments import replace_once, without_comments


def switched(text, name, variable):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    return text, f'''static const bool {variable} = [] {{
                const char* value = std::getenv("{name}");
                return value && !std::strcmp(value, "1");
            }}();'''


def assign_patch(text):
    table = re.search(r'/\* Source for ObjectConstructor\.lut\.h\n(@begin objectConstructorTable.*?@end)\n\*/', text, re.DOTALL)
    if not table:
        raise RuntimeError("The pinned Object constructor generator table is missing")
    text = replace_once(text, table[0], '[[maybe_unused]] static constexpr auto objectConstructorTableSource = R"EdenTable(\n' + table[1] + '\n)EdenTable";')
    text, switch = switched(text, "WEBKIT_EDEN_JSC_ASSIGN_CLONE", "cloneFirstSource")
    return replace_once(text, "            MarkedArgumentBuffer structures;\n            for (unsigned i = 1; i < argsCount; ++i) {", f'''            MarkedArgumentBuffer structures;
            {switch}
            unsigned startIndex = 1;
            if (cloneFirstSource && objectCloneFast(vm, targetObject, asObject(callFrame->uncheckedArgument(1)))) {{
                startIndex = 2;
            }}
            for (unsigned i = startIndex; i < argsCount; ++i) {{''')


def rest_patch(text):
    text, switch = switched(text, "WEBKIT_EDEN_JSC_REST_CLONE", "cloneRest")
    anchor = "    auto sourceStructure = source->structure();\n    if (canPerformFastPropertyEnumerationForCopyDataProperties(sourceStructure)) [[likely]] {"
    return replace_once(text, anchor, anchor + f'''
        {switch}
        if (cloneRest && (!excludedSet || excludedSet->isEmpty()) && objectCloneFast(vm, target, source)) {{
            return JSValue::encode(target);
        }}
''')


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


PATCHES = {
    "Source/JavaScriptCore/runtime/ObjectConstructor.cpp": assign_patch,
    "Source/JavaScriptCore/runtime/JSGlobalObjectFunctions.cpp": rest_patch,
    "Source/JavaScriptCore/yarr/YarrJIT.cpp": regexp_patch,
}


def prepare_jsc_experiments(root, source, version, enabled=True):
    archive = root / "third_party/wpe/packages" / f"wpewebkit-{version}.tar.xz"
    with tarfile.open(archive) as package:
        originals = {path: package.extractfile(f"wpewebkit-{version}/{path}").read().decode() for path in PATCHES}
    for path, original in originals.items():
        text = original
        if enabled:
            text = PATCHES[path](text)
            text = subprocess.run(
                ["clang-format", "--style=file:" + str(root / ".clang-format")],
                input=without_comments(text).replace('#include "config.h"\n', '#include "config.h"\n\n'),
                text=True, capture_output=True, check=True,
            ).stdout
        destination = source / path
        if destination.read_text() != text:
            destination.write_text(text)
