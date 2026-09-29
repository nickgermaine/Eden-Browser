import re


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError('The pinned source does not match the engine experiment: ' + before[:100])
    return text.replace(before, after, 1)


def without_comments(text):
    tokens = re.compile(r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.DOTALL)
    return tokens.sub(lambda match: '\n' * match[0].count('\n') if match[0].startswith('/') else match[0], text)


def without_cmake_comments(text):
    tokens = re.compile(r'#\[(?P<comment>=*)\[.*?\](?P=comment)\]|\[(?P<argument>=*)\[.*?\](?P=argument)\]|"(?:\\.|[^"\\])*"|#[^\n]*', re.DOTALL)
    marker = "\0"
    marked = tokens.sub(lambda match: marker if match[0].startswith("#") else match[0], text)
    lines = []
    for line in marked.split("\n"):
        if marker in line:
            line = line.replace(marker, "").rstrip()
            if not line.strip():
                continue
        lines.append(line)
    return "\n".join(lines)


def without_source_list_comments(text):
    lines = []
    for line in text.split("\n"):
        if "//" in line:
            line = line[:line.index("//")].rstrip()
            if not line.strip():
                continue
        lines.append(line)
    return "\n".join(lines)
