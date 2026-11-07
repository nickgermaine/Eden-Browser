from pathlib import Path

from wpe.patch_support import replace_once


SOURCE = "Source/WebCore/"


def document_header_patch(text):
    if "listItemsWithCachedValues() const" in text:
        raise RuntimeError("The list renumbering document counter is already present")
    text = replace_once(
        text,
        "    bool hasDataListElements() const { return m_dataListElementCount; }\n",
        "    unsigned listItemsWithCachedValues() const { return m_listItemsWithCachedValues; }\n"
        "    void incrementListItemsWithCachedValues() { ++m_listItemsWithCachedValues; }\n"
        "    void decrementListItemsWithCachedValues() { ASSERT(m_listItemsWithCachedValues); --m_listItemsWithCachedValues; }\n"
        "    bool hasDataListElements() const { return m_dataListElementCount; }\n",
    )
    return replace_once(
        text,
        "    unsigned m_dataListElementCount { 0 };\n",
        "    unsigned m_dataListElementCount { 0 };\n"
        "    unsigned m_listItemsWithCachedValues { 0 };\n",
    )


def render_list_item_header_patch(text):
    if "void setCachedValue(int) const;" in text:
        raise RuntimeError("The list renumbering cache declarations are already present")
    return replace_once(
        text,
        "    void updateValueNow() const;\n",
        "    void updateValueNow() const;\n"
        "    void setCachedValue(int) const;\n"
        "    void clearCachedValue() const;\n"
        "    void willBeDestroyed() final;\n",
    )


def render_list_item_patch(text):
    text = replace_once(
        text,
        '#include "ContainerNodeInlines.h"',
        '#include "ContainerNodeInlines.h"\n#include "Document.h"',
    )
    text = replace_once(
        text,
        "#include <wtf/TZoneMallocInlines.h>\n",
        "#include <wtf/TZoneMallocInlines.h>\n#include <atomic>\n#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n",
    )
    text = replace_once(
        text,
        "using namespace HTMLNames;\n",
        """using namespace HTMLNames;

static bool edenListRenumberingEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_LIST_RENUMBERING");
        return !value || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool edenListRenumberingDiagnosticsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_LIST_RENUMBERING_DIAGNOSTICS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

static bool edenListRenumberingVerificationEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_VERIFY_LIST_RENUMBERING");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

static unsigned edenListRenumberingDiagnosticEventIndex()
{
    static std::atomic<unsigned> eventCount { 0 };
    constexpr unsigned eventLimit = 8192;
    unsigned current = eventCount.load(std::memory_order_relaxed);
    while (current <= eventLimit) {
        if (eventCount.compare_exchange_weak(current, current + 1, std::memory_order_relaxed))
            return current;
    }
    return eventLimit + 1;
}

static void reportEdenListRenumberingEvent(const char* outcome, unsigned examined)
{
    unsigned eventIndex = edenListRenumberingDiagnosticEventIndex();
    if (eventIndex < 8192)
        std::fprintf(stderr, "EDEN_LIST_RENUMBERING|outcome=%s|examined=%u\\n", outcome, examined);
    else if (eventIndex == 8192)
        std::fprintf(stderr, "EDEN_LIST_RENUMBERING|truncated=1\\n");
}
""",
    )
    text = replace_once(
        text,
        "void RenderListItem::updateValueNow() const\n{",
        """void RenderListItem::setCachedValue(int value) const
{
    if (!m_value)
        document().incrementListItemsWithCachedValues();
    m_value = value;
}

void RenderListItem::clearCachedValue() const
{
    if (!m_value)
        return;
    document().decrementListItemsWithCachedValues();
    m_value = std::nullopt;
}

void RenderListItem::willBeDestroyed()
{
    clearCachedValue();
    RenderBlockFlow::willBeDestroyed();
}

void RenderListItem::updateValueNow() const
{""",
    )
    text = replace_once(
        text,
        "    auto& startValue = startItem->m_value;\n    if (!startValue) {",
        "    if (!startItem->m_value) {",
    )
    text = replace_once(
        text,
        "                startValue = *listDirectives.resetValue;\n",
        "                startItem->setCachedValue(*listDirectives.resetValue);\n",
    )
    text = replace_once(
        text,
        "                startValue = startForReversedOrderedList(*orderedList) - defaultIncrement;\n",
        "                startItem->setCachedValue(startForReversedOrderedList(*orderedList) - defaultIncrement);\n",
    )
    text = replace_once(
        text,
        "                startValue = orderedList ? orderedList->start() - defaultIncrement : 0;\n",
        "                startItem->setCachedValue(orderedList ? orderedList->start() - defaultIncrement : 0);\n",
    )
    text = replace_once(
        text,
        "        startValue = valueForItem(startValue.value_or(0), directives);\n",
        "        startItem->setCachedValue(valueForItem(startItem->m_value.value_or(0), directives));\n",
    )
    text = replace_once(
        text,
        "    int value = *startValue;\n",
        "    int value = *startItem->m_value;\n",
    )
    text = replace_once(
        text,
        "        item->m_value = valueForItem(value, directives);\n",
        "        item->setCachedValue(valueForItem(value, directives));\n",
    )
    text = replace_once(
        text,
        "void RenderListItem::updateValue()\n{\n    m_value = std::nullopt;\n",
        "void RenderListItem::updateValue()\n{\n    clearCachedValue();\n",
    )
    return update_list_marker_numbers_patch(text)


def update_list_marker_numbers_patch(text):
    text = replace_once(
        text,
        "        isInReversedOrderedList = orderedList->isReversed();\n    }\n\n",
        "        isInReversedOrderedList = orderedList->isReversed();\n    }\n\n"
        "    bool diagnosticsEnabled = edenListRenumberingDiagnosticsEnabled();\n"
        "    if (edenListRenumberingEnabled() && !document().listItemsWithCachedValues()) {\n"
        "        if (edenListRenumberingVerificationEnabled()) {\n"
        "            RELEASE_ASSERT(!m_value);\n"
        "            auto* verifiedItem = this;\n"
        "            unsigned verifiedItems = 1;\n"
        "            auto subsequentListItem = isInReversedOrderedList ? previousListItem : nextListItem;\n"
        "            while ((verifiedItem = subsequentListItem(*list, *verifiedItem))) {\n"
        "                RELEASE_ASSERT(!verifiedItem->m_value);\n"
        "                ++verifiedItems;\n"
        "            }\n"
        "            if (diagnosticsEnabled)\n"
        "                reportEdenListRenumberingEvent(\"verified-skip\", verifiedItems);\n"
        "        }\n"
        "        if (diagnosticsEnabled)\n"
        "            reportEdenListRenumberingEvent(\"skip\", 0);\n"
        "        return;\n    }\n\n",
    )
    anchor = (
        "    auto* item = this;\n"
        "    auto subsequentListItem = isInReversedOrderedList ? previousListItem : nextListItem;\n"
        "    while ((item = subsequentListItem(*list, *item)) && item->m_value)\n"
        "        item->updateValue();\n"
    )
    replacement = (
        "    auto* item = this;\n"
        "    auto subsequentListItem = isInReversedOrderedList ? previousListItem : nextListItem;\n"
        "    if (diagnosticsEnabled) {\n"
        "        unsigned examined = 0;\n"
        "        while ((item = subsequentListItem(*list, *item))) {\n"
        "            ++examined;\n"
        "            if (!item->m_value)\n"
        "                break;\n"
        "            item->updateValue();\n"
        "        }\n"
        "        reportEdenListRenumberingEvent(\"scan\", examined);\n"
        "        return;\n"
        "    }\n"
        "    while ((item = subsequentListItem(*list, *item)) && item->m_value)\n"
        "        item->updateValue();\n"
    )
    return replace_once(text, anchor, replacement)

def list_marker_test_patch(text):
    if "ListRenumbering" in text:
        raise RuntimeError("The list renumbering regression is already present")
    text = replace_once(
        text,
        '#include "TestPageHarness.h"',
        '#include "TestPageHarness.h"\n#include "EdenListRenumberingFixture.h"\n'
        '#include <WebCore/ContainerNodeInlines.h>\n#include <WebCore/HTMLNames.h>\n'
        '#include <WebCore/HTMLOListElement.h>\n'
        '#include <WebCore/RenderListItem.h>',
    )
    tests = Path(__file__).with_name("list_renumbering_tests.inc").read_text()
    return replace_once(text, "TEST(CachedMatchFinder, CountMatchesFindsMatches)", tests + "\nTEST(CachedMatchFinder, CountMatchesFindsMatches)")


PATCHES = {
    SOURCE + "dom/Document.h": document_header_patch,
    SOURCE + "rendering/RenderListItem.h": render_list_item_header_patch,
    SOURCE + "rendering/RenderListItem.cpp": render_list_item_patch,
    "Tools/TestWebKitAPI/Tests/WebCore/CachedMatchFinder.cpp": list_marker_test_patch,
}

FILES = {
    "Tools/TestWebKitAPI/Tests/WebCore/EdenListRenumberingFixture.h": "list_renumbering_fixture.h",
}

SOURCES = ["list_renumbering_tests.inc", "list_renumbering_fixture.h"]
