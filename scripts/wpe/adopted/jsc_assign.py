import re

from wpe.patch_support import replace_once
from wpe.adopted.jsc_switch import switched


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


PATCHES = {"Source/JavaScriptCore/runtime/ObjectConstructor.cpp": assign_patch}
SOURCES = ["jsc_switch.py"]
