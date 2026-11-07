from wpe.patch_support import replace_once


def switched(text, name, variable):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    return text, f'''static const bool {variable} = [] {{
                const char* value = std::getenv("{name}");
                return !value || std::strcmp(value, "0");
            }}();'''


