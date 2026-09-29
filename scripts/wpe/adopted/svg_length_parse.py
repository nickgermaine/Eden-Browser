from wpe.patch_support import replace_once


SOURCE = "Source/WebCore/"

ELEMENT_LENGTHS = {
    "SVGRectElement": (
        ("xAttr", "m_x"),
        ("yAttr", "m_y"),
        ("rxAttr", "m_rx"),
        ("ryAttr", "m_ry"),
        ("widthAttr", "m_width"),
        ("heightAttr", "m_height"),
    ),
    "SVGCircleElement": (("cxAttr", "m_cx"), ("cyAttr", "m_cy"), ("rAttr", "m_r")),
    "SVGLineElement": (("x1Attr", "m_x1"), ("y1Attr", "m_y1"), ("x2Attr", "m_x2"), ("y2Attr", "m_y2")),
    "SVGEllipseElement": (("cxAttr", "m_cx"), ("cyAttr", "m_cy"), ("rxAttr", "m_rx"), ("ryAttr", "m_ry")),
    "SVGUseElement": (("xAttr", "m_x"), ("yAttr", "m_y"), ("widthAttr", "m_width"), ("heightAttr", "m_height")),
    "SVGImageElement": (("xAttr", "m_x"), ("yAttr", "m_y"), ("widthAttr", "m_width"), ("heightAttr", "m_height")),
    "SVGForeignObjectElement": (("xAttr", "m_x"), ("yAttr", "m_y"), ("widthAttr", "m_width"), ("heightAttr", "m_height")),
}


def svg_property_patch(text):
    if "CleanWithSuccessfulLengthAttributeParse" in text:
        raise RuntimeError("The SVG length parse state is already present")
    text = replace_once(
        text,
        "enum class SVGPropertyState : uint8_t { Clean, Dirty };",
        "enum class SVGPropertyState : uint8_t { Clean, CleanWithSuccessfulLengthAttributeParse, Dirty };",
    )
    text = replace_once(
        text,
        "        ASSERT(m_state == SVGPropertyState::Clean);\n"
        "        m_owner = owner;\n"
        "        m_access = access;",
        "        ASSERT(m_state != SVGPropertyState::Dirty);\n"
        "        m_state = SVGPropertyState::Clean;\n"
        "        m_owner = owner;\n"
        "        m_access = access;",
    )
    text = replace_once(
        text,
        "    bool isDirty() const { return m_state == SVGPropertyState::Dirty; }\n"
        "    void setDirty() { m_state = SVGPropertyState::Dirty; }\n",
        "    bool isDirty() const { return m_state == SVGPropertyState::Dirty; }\n"
        "    bool hasSuccessfulLengthAttributeParse() const { return m_state == SVGPropertyState::CleanWithSuccessfulLengthAttributeParse; }\n"
        "    void setLengthAttributeParseSucceeded(bool succeeded)\n"
        "    {\n"
        "        if (isDirty())\n"
        "            return;\n"
        "        m_state = succeeded ? SVGPropertyState::CleanWithSuccessfulLengthAttributeParse : SVGPropertyState::Clean;\n"
        "    }\n"
        "    void setDirty() { m_state = SVGPropertyState::Dirty; }\n",
    )
    text = replace_once(
        text,
        "    void commitChange()\n"
        "    {\n"
        "        if (!m_owner)\n"
        "            return;\n",
        "    void commitChange()\n"
        "    {\n"
        "        invalidateLengthAttributeParseSuccess();\n"
        "        if (!m_owner)\n"
        "            return;\n",
    )
    text = replace_once(
        text,
        "        if (m_state == SVGPropertyState::Clean)\n"
        "            return std::nullopt;\n",
        "        if (m_state != SVGPropertyState::Dirty)\n"
        "            return std::nullopt;\n",
    )
    return replace_once(
        text,
        "protected:\n"
        "    SVGProperty(SVGPropertyOwner* owner = nullptr, SVGPropertyAccess access = SVGPropertyAccess::ReadWrite)\n",
        "protected:\n"
        "    void invalidateLengthAttributeParseSuccess()\n"
        "    {\n"
        "        if (m_state == SVGPropertyState::CleanWithSuccessfulLengthAttributeParse)\n"
        "            m_state = SVGPropertyState::Clean;\n"
        "    }\n\n"
        "    SVGProperty(SVGPropertyOwner* owner = nullptr, SVGPropertyAccess access = SVGPropertyAccess::ReadWrite)\n",
    )


def svg_value_property_patch(text):
    return replace_once(
        text,
        "    void setValue(const PropertyType& value) { m_value = value; }",
        "    void setValue(const PropertyType& value)\n"
        "    {\n"
        "        invalidateLengthAttributeParseSuccess();\n"
        "        m_value = value;\n"
        "    }",
    )


def length_element_patch(element, length_attributes):
    def patch(text):
        if "EdenSVGLengthAttributeParse.h" in text:
            raise RuntimeError(f"The SVG length parse skip is already present in {element}")
        text = replace_once(
            text,
            f'#include "{element}.h"\n',
            f'#include "{element}.h"\n#include "EdenSVGLengthAttributeParse.h"\n',
        )
        for attribute, member in length_attributes:
            anchor = f"    case AttributeNames::{attribute}:\n"
            start = text.index(anchor) + len(anchor)
            end = text.index("\n        break;", start)
            statement = text[start:end].strip()
            if statement.count("setBaseValInternal(") != 1:
                raise RuntimeError(f"Unexpected SVG length parse case in {element}: {attribute}")
            replacement = (
                f"        if (!edenShouldSkipSVGLengthAttributeParse(oldValue, newValue, {member}.get())) {{\n"
                f"            {statement}\n"
                f"            {member}->baseVal()->setLengthAttributeParseSucceeded(parseError == SVGParsingError::None);\n"
                "        }"
            )
            text = replace_once(text, anchor + text[start:end], anchor + replacement)
        return text

    return patch


PATCHES = {
    SOURCE + "svg/properties/SVGProperty.h": svg_property_patch,
    SOURCE + "svg/properties/SVGValueProperty.h": svg_value_property_patch,
}

for _element, _length_attributes in ELEMENT_LENGTHS.items():
    PATCHES[SOURCE + f"svg/{_element}.cpp"] = length_element_patch(_element, _length_attributes)

FILES = {
    SOURCE + "svg/EdenSVGLengthAttributeParse.h": "svg_length_parse.h",
}

SOURCES = ["svg_length_parse.h"]
