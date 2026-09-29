from pathlib import Path

from wpe.patch_support import replace_once


INLINE = "Source/WebCore/layout/formattingContexts/inline/"


def intrinsic_header_patch(text):
    return replace_once(
        text,
        "    InlineLayoutUnit simplifiedMinimumWidth(const ElementBox& root) const;",
        """    InlineLayoutUnit simplifiedMinimumWidth(const ElementBox& root) const;
    InlineLayoutUnit minimumContentSizeUsingLineBuilder();
    std::optional<InlineLayoutUnit> minimumContentSizeFromCachedText();""",
    )


def intrinsic_source_patch(text):
    text = replace_once(
        text,
        '#include "TextOnlySimpleLineBuilder.h"',
        '#include "TextOnlySimpleLineBuilder.h"\n#include <cmath>\n#include <cstdlib>\n#include <cstring>',
    )
    implementation = Path(__file__).with_name("intrinsic_width.inc").read_text()
    return replace_once(
        text,
        "InlineLayoutUnit IntrinsicWidthHandler::minimumContentSize()\n",
        implementation + "\nInlineLayoutUnit IntrinsicWidthHandler::minimumContentSizeUsingLineBuilder()\n",
    )


PATCHES = {
    INLINE + "IntrinsicWidthHandler.cpp": intrinsic_source_patch,
    INLINE + "IntrinsicWidthHandler.h": intrinsic_header_patch,
}
SOURCES = ["intrinsic_width.inc"]
