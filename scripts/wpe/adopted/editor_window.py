from wpe.patch_support import replace_once


def editor_patch(text):
    text = replace_once(text, '#include <wtf/text/MakeString.h>', '#include <wtf/text/MakeString.h>\n#include <wtf/SystemTracing.h>\n#include <cstdlib>')
    helper = '''static SimpleRange edenSurroundingWindow(const SimpleRange& fullRange, const SimpleRange& selectionRange) {
    static unsigned limit = [] {
        const char* setting = std::getenv("WEBKIT_EDEN_EDITOR_WINDOW");
        if (!setting) {
            return 1024U;
        }
        char* end = nullptr;
        unsigned long value = std::strtoul(setting, &end, 10);
        return end != setting && !*end && ((value >= 128 && value <= 16384) || !value) ? static_cast<unsigned>(value) : 0U;
    }();
    if (!limit || !contains<ComposedTree>(fullRange, selectionRange)) {
        return fullRange;
    }
    CharacterIterator selected(selectionRange);
    selected.advance(limit);
    if (!selected.atEnd()) {
        return fullRange;
    }
    BackwardsCharacterIterator before({ fullRange.start, selectionRange.start });
    before.advance(limit - 1);
    if (!before.atEnd() && !before.text().isEmpty() && U16_IS_TRAIL(before.text()[before.text().length() - 1])) {
        before.advance(1);
    }
    CharacterIterator after({ selectionRange.end, fullRange.end });
    after.advance(limit);
    if (!after.atEnd() && !after.text().isEmpty() && U16_IS_TRAIL(after.text()[0])) {
        after.advance(1);
    }
    return { before.atEnd() ? fullRange.start : before.range().start, after.atEnd() ? fullRange.end : after.range().start };
}

'''
    text = replace_once(text, 'void WebPage::getPlatformEditorState(', helper + 'void WebPage::getPlatformEditorState(')
    text = replace_once(text, '        auto compositionRange = frame.editor().compositionRange();', '''        auto compositionRange = frame.editor().compositionRange();
        WTFBeginSignpost(this, EdenEditorSurroundingText);
        auto selectedRange = makeSimpleRange(selection.visibleStart(), selection.visibleEnd());
        if (surroundingRange && selectedRange) {
            auto window = edenSurroundingWindow(*surroundingRange, *selectedRange);
            if (!compositionRange || contains<ComposedTree>(window, *compositionRange)) {
                surroundingRange = WTF::move(window);
            }
        }''')
    text = replace_once(text, 'auto cursorPositionRange = makeSimpleRange(surroundingStart, selectionStart);', 'auto cursorPositionRange = surroundingRange ? makeSimpleRange(surroundingRange->start, selectionStart) : std::nullopt;')
    text = replace_once(text, 'auto selectionPositionRange = makeSimpleRange(surroundingStart, selection.visibleEnd());', 'auto selectionPositionRange = surroundingRange ? makeSimpleRange(surroundingRange->start, selection.visibleEnd()) : std::nullopt;')
    anchor = '            postLayoutData.surroundingContextSelectionPosition = selectionPositionRange ? characterCount(*selectionPositionRange): 0;\n        }'
    text = replace_once(text, anchor, anchor + '''
        WTFEndSignpost(this, EdenEditorSurroundingText);
        if (std::getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
            WTFLogAlways("Eden editor window: length=%llu cursor=%llu selection=%llu", static_cast<unsigned long long>(postLayoutData.surroundingContext.length()), static_cast<unsigned long long>(postLayoutData.surroundingContextCursorPosition), static_cast<unsigned long long>(postLayoutData.surroundingContextSelectionPosition));
        }''')
    return text


PATCHES = {
    'Source/WebKit/WebProcess/WebPage/glib/WebPageGLib.cpp': editor_patch,
}
