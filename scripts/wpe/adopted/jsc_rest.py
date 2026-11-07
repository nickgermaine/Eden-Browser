import re

from wpe.patch_support import replace_once
from wpe.adopted.jsc_switch import switched


def rest_patch(text):
    text, switch = switched(text, "WEBKIT_EDEN_JSC_REST_CLONE", "cloneRest")
    anchor = "    auto sourceStructure = source->structure();\n    if (canPerformFastPropertyEnumerationForCopyDataProperties(sourceStructure)) [[likely]] {"
    return replace_once(text, anchor, anchor + f'''
        {switch}
        if (cloneRest && (!excludedSet || excludedSet->isEmpty()) && objectCloneFast(vm, target, source)) {{
            return JSValue::encode(target);
        }}
''')


PATCHES = {"Source/JavaScriptCore/runtime/JSGlobalObjectFunctions.cpp": rest_patch}
SOURCES = ["jsc_switch.py"]
