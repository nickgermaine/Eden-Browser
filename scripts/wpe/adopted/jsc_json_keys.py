from wpe.adopted.jsc_switch import switched
from wpe.patch_support import replace_once


def json_keys_patch(text):
    text, switch = switched(text, "WEBKIT_EDEN_JSC_JSON_KEYS", "useJSONKeyHint")
    before = """            if (type == TokComma) {
                type = m_lexer.next();
                bool isPropertyKey = type == TokString;
                if constexpr (parserMode != StrictJSON)
"""
    after = f"""            if (type == TokComma) {{
                if constexpr (sizeof(CharType) == 2 && parserMode == StrictJSON) {{
                    {switch}
                    type = useJSONKeyHint ? m_lexer.nextMaybeIdentifier() : m_lexer.next();
                }} else {{
                    type = m_lexer.next();
                }}
                bool isPropertyKey = type == TokString;
                if constexpr (parserMode != StrictJSON)
"""
    return replace_once(text, before, after)


PATCHES = {"Source/JavaScriptCore/runtime/LiteralParser.cpp": json_keys_patch}
SOURCES = ["jsc_switch.py"]
