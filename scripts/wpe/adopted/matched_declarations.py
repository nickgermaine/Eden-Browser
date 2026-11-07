from wpe.patch_support import replace_once


STYLE = "Source/WebCore/style/"


HELPERS = """
static bool edenMatchedDeclarationsCountersEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

enum class EdenMatchedDeclarationsRejection : uint8_t {
    DocumentElement,
    PseudoElementNameArgument,
    AttrContent,
    Zoom,
    WritingMode,
    Direction,
    ContainerDependent,
    TreeCountingFunctions,
    AnchorFunctions,
    StyleFontNotCurrent,
    ParentFontNotCurrent,
    RandomCachingKeyMap,
    Count
};

static constexpr size_t edenMatchedDeclarationsRejectionCount = static_cast<size_t>(EdenMatchedDeclarationsRejection::Count);

struct EdenMatchedDeclarationsCounters {
    unsigned long long lookups { 0 };
    unsigned long long nonCacheableMatchResult { 0 };
    unsigned long long nothingCacheable { 0 };
    unsigned long long noBucket { 0 };
    unsigned long long declarationsUnequal { 0 };
    unsigned long long inheritedCustomPropertiesUnequal { 0 };
    unsigned long long noMatchingEntry { 0 };
    unsigned long long fullHits { 0 };
    unsigned long long partialHits { 0 };
    unsigned long long unusableAfterHighPriority { 0 };
    unsigned long long additions { 0 };
    unsigned long long bucketFull { 0 };
    unsigned long long evictions { 0 };
    std::array<unsigned long long, edenMatchedDeclarationsRejectionCount> rejectedOnFind { };
    std::array<unsigned long long, edenMatchedDeclarationsRejectionCount> rejectedOnAdd { };
};

static EdenMatchedDeclarationsCounters& edenMatchedDeclarationsCounters()
{
    static thread_local EdenMatchedDeclarationsCounters counters;
    return counters;
}

static thread_local EdenMatchedDeclarationsRejection edenMatchedDeclarationsLastRejection = EdenMatchedDeclarationsRejection::Count;

static bool edenMatchedDeclarationsReject(EdenMatchedDeclarationsRejection rejection)
{
    if (edenMatchedDeclarationsCountersEnabled())
        edenMatchedDeclarationsLastRejection = rejection;
    return false;
}

static void edenMatchedDeclarationsLogRejections(const char* phase, const std::array<unsigned long long, edenMatchedDeclarationsRejectionCount>& rejections)
{
    WTFLogAlways("Eden matched declarations: rejected on %s: document-element %llu pseudo-name-argument %llu attr-content %llu zoom %llu writing-mode %llu direction %llu container-dependent %llu tree-counting %llu anchor-functions %llu style-font-not-current %llu parent-font-not-current %llu random-caching-key %llu",
        phase, rejections[0], rejections[1], rejections[2], rejections[3], rejections[4], rejections[5], rejections[6], rejections[7], rejections[8], rejections[9], rejections[10], rejections[11]);
}

static void edenMatchedDeclarationsCountLookup()
{
    auto& counters = edenMatchedDeclarationsCounters();
    if (++counters.lookups % 4096)
        return;
    WTFLogAlways("Eden matched declarations: lookups %llu full-hits %llu partial-hits %llu non-cacheable-match-result %llu nothing-cacheable %llu no-bucket %llu declarations-unequal %llu inherited-custom-properties-unequal %llu no-matching-entry %llu unusable-after-high-priority %llu additions %llu bucket-full %llu evictions %llu",
        counters.lookups, counters.fullHits, counters.partialHits, counters.nonCacheableMatchResult, counters.nothingCacheable, counters.noBucket, counters.declarationsUnequal, counters.inheritedCustomPropertiesUnequal, counters.noMatchingEntry, counters.unusableAfterHighPriority, counters.additions, counters.bucketFull, counters.evictions);
    edenMatchedDeclarationsLogRejections("find", counters.rejectedOnFind);
    edenMatchedDeclarationsLogRejections("add", counters.rejectedOnAdd);
}

void MatchedDeclarationsCache::edenCountRejection(bool whileAdding)
{
    if (!edenMatchedDeclarationsCountersEnabled())
        return;
    auto rejection = std::exchange(edenMatchedDeclarationsLastRejection, EdenMatchedDeclarationsRejection::Count);
    if (rejection == EdenMatchedDeclarationsRejection::Count)
        return;
    auto& counters = edenMatchedDeclarationsCounters();
    ++(whileAdding ? counters.rejectedOnAdd : counters.rejectedOnFind)[static_cast<size_t>(rejection)];
}

void MatchedDeclarationsCache::edenRemoveUnusableEntry(unsigned hash, const Entry& unusableEntry)
{
    if (edenMatchedDeclarationsCountersEnabled())
        ++edenMatchedDeclarationsCounters().unusableAfterHighPriority;

    auto it = m_entries.find(hash);
    if (it == m_entries.end())
        return;
    it->value.removeFirstMatching([&](auto& entry) {
        return &entry == &unusableEntry;
    });
    if (it->value.isEmpty())
        m_entries.remove(it);
}

"""


REJECTIONS = [
    ("    if (&element == element.document().documentElement())\n        return false;\n", "DocumentElement"),
    ("    if (!style.pseudoElementNameArgument().isNull())\n        return false;\n", "PseudoElementNameArgument"),
    ("    if (style.hasAttrContent() || (style.pseudoElementType() && parentStyle.hasAttrContent()))\n        return false;\n", "AttrContent"),
    ("    if (style.zoom() != Style::ComputedStyle::initialZoom())\n        return false;\n", "Zoom"),
    ("    if (style.isContainerDependent())\n        return false;\n", "ContainerDependent"),
    ("    if (style.useTreeCountingFunctions())\n        return false;\n", "TreeCountingFunctions"),
    ("    if (style.usesAnchorFunctions())\n        return false;\n", "AnchorFunctions"),
    ("    if (!style.fontCascade().isCurrent(fontSelector.get()))\n        return false;\n", "StyleFontNotCurrent"),
    ("    if (!parentStyle.fontCascade().isCurrent(fontSelector.get()))\n        return false;\n", "ParentFontNotCurrent"),
    ("    if (element.hasRandomCachingKeyMap())\n        return false;\n", "RandomCachingKeyMap"),
]


FIND_BODY = """    if (!hash)
        return std::nullopt;

    auto it = m_entries.find(hash);
    if (it == m_entries.end())
        return std::nullopt;

    const Entry* partiallyMatchingEntry = nullptr;
    for (auto& entry : it->value) {
        SUPPRESS_UNCOUNTED_ARG if (!matchResult.cacheablePropertiesEqual(*entry.matchResult))
            continue;

        if (&entry.parentRenderStyle->inheritedCustomProperties() != &inheritedCustomProperties)
            continue;

        if (parentStyle.inheritedEqual(*entry.parentRenderStyle))
            return std::make_optional(Result { .entry = entry, .inheritedEqual = true });
        partiallyMatchingEntry = &entry;
    }
    if (partiallyMatchingEntry)
        return std::make_optional(Result { .entry = *partiallyMatchingEntry, .inheritedEqual = false });
    return std::nullopt;
"""


FIND_BODY_COUNTED = """    bool edenCounting = edenMatchedDeclarationsCountersEnabled();
    if (edenCounting)
        edenMatchedDeclarationsCountLookup();

    if (!hash) {
        if (edenCounting) {
            auto& edenCounters = edenMatchedDeclarationsCounters();
            ++(matchResult.isCompletelyNonCacheable ? edenCounters.nonCacheableMatchResult : edenCounters.nothingCacheable);
        }
        return std::nullopt;
    }

    auto it = m_entries.find(hash);
    if (it == m_entries.end()) {
        if (edenCounting)
            ++edenMatchedDeclarationsCounters().noBucket;
        return std::nullopt;
    }

    const Entry* partiallyMatchingEntry = nullptr;
    for (auto& entry : it->value) {
        SUPPRESS_UNCOUNTED_ARG if (!matchResult.cacheablePropertiesEqual(*entry.matchResult)) {
            if (edenCounting)
                ++edenMatchedDeclarationsCounters().declarationsUnequal;
            continue;
        }

        if (&entry.parentRenderStyle->inheritedCustomProperties() != &inheritedCustomProperties) {
            if (edenCounting)
                ++edenMatchedDeclarationsCounters().inheritedCustomPropertiesUnequal;
            continue;
        }

        if (parentStyle.inheritedEqual(*entry.parentRenderStyle)) {
            if (edenCounting)
                ++edenMatchedDeclarationsCounters().fullHits;
            return std::make_optional(Result { .entry = entry, .inheritedEqual = true });
        }
        partiallyMatchingEntry = &entry;
    }
    if (partiallyMatchingEntry) {
        if (edenCounting)
            ++edenMatchedDeclarationsCounters().partialHits;
        return std::make_optional(Result { .entry = *partiallyMatchingEntry, .inheritedEqual = false });
    }
    if (edenCounting)
        ++edenMatchedDeclarationsCounters().noMatchingEntry;
    return std::nullopt;
"""


def cache_header_patch(text):
    return replace_once(
        text,
        "    void remove(unsigned hash);\n",
        "    void remove(unsigned hash);\n"
        "    void edenRemoveUnusableEntry(unsigned hash, const Entry&);\n"
        "    static void edenCountRejection(bool whileAdding);\n",
    )


def cache_patch(text):
    text = replace_once(
        text,
        '#include "config.h"\n#include "MatchedDeclarationsCache.h"\n',
        '#include "config.h"\n#include <cstdlib>\n#include <cstring>\n#include "MatchedDeclarationsCache.h"\n',
    )
    text = replace_once(
        text,
        "#include <wtf/TZoneMallocInlines.h>\n#include <wtf/text/StringHash.h>\n",
        "#include <array>\n#include <utility>\n#include <wtf/TZoneMallocInlines.h>\n#include <wtf/text/StringHash.h>\n",
    )
    text = replace_once(
        text,
        "WTF_MAKE_TZONE_ALLOCATED_IMPL(MatchedDeclarationsCache);\n",
        "WTF_MAKE_TZONE_ALLOCATED_IMPL(MatchedDeclarationsCache);\n" + HELPERS,
    )

    for anchor, name in REJECTIONS:
        text = replace_once(
            text,
            anchor,
            anchor.replace("return false;", f"return edenMatchedDeclarationsReject(EdenMatchedDeclarationsRejection::{name});"),
        )
    text = replace_once(
        text,
        "    if (style.writingMode().computedWritingMode() != Style::ComputedStyle::initialWritingMode()\n"
        "        || style.writingMode().computedTextDirection() != Style::ComputedStyle::initialDirection())\n"
        "        return false;\n",
        "    if (style.writingMode().computedWritingMode() != Style::ComputedStyle::initialWritingMode())\n"
        "        return edenMatchedDeclarationsReject(EdenMatchedDeclarationsRejection::WritingMode);\n"
        "    if (style.writingMode().computedTextDirection() != Style::ComputedStyle::initialDirection())\n"
        "        return edenMatchedDeclarationsReject(EdenMatchedDeclarationsRejection::Direction);\n",
    )

    text = replace_once(text, FIND_BODY, FIND_BODY_COUNTED)

    text = replace_once(
        text,
        "    if (addResult.iterator->value.size() < maxEntriesPerHash)\n"
        "        addResult.iterator->value.append(Entry { &matchResult, Style::ComputedStyle::clonePtr(style), Style::ComputedStyle::clonePtr(parentStyle) });\n",
        "    bool edenCounting = edenMatchedDeclarationsCountersEnabled();\n"
        "    if (addResult.iterator->value.size() < maxEntriesPerHash) {\n"
        "        addResult.iterator->value.append(Entry { &matchResult, Style::ComputedStyle::clonePtr(style), Style::ComputedStyle::clonePtr(parentStyle) });\n"
        "        if (edenCounting)\n"
        "            ++edenMatchedDeclarationsCounters().additions;\n"
        "    } else if (edenCounting)\n"
        "        ++edenMatchedDeclarationsCounters().bucketFull;\n",
    )
    return replace_once(
        text,
        "    if (m_entries.size() > maximumSize)\n        m_entries.remove(m_entries.random());\n",
        "    if (m_entries.size() > maximumSize) {\n"
        "        m_entries.remove(m_entries.random());\n"
        "        if (edenCounting)\n"
        "            ++edenMatchedDeclarationsCounters().evictions;\n"
        "    }\n",
    )


def resolver_patch(text):
    text = replace_once(
        text,
        "    auto hasUsableEntry = cacheResult && MatchedDeclarationsCache::isCacheable(element.get(), style, parentStyle);\n",
        "    auto hasUsableEntry = cacheResult && MatchedDeclarationsCache::isCacheable(element.get(), style, parentStyle);\n"
        "    if (cacheResult && !hasUsableEntry)\n"
        "        MatchedDeclarationsCache::edenCountRejection(false);\n",
    )
    text = replace_once(
        text,
        "        m_matchedDeclarationsCache.remove(cacheHash);\n"
        "        applyMatchedProperties(state, matchResult, PropertyCascade::normalProperties());\n",
        "        m_matchedDeclarationsCache.edenRemoveUnusableEntry(cacheHash, cacheResult->entry);\n"
        "        applyMatchedProperties(state, matchResult, PropertyCascade::normalProperties());\n",
    )
    return replace_once(
        text,
        "    if (((cacheHash && !cacheResult) || (cacheResult && !cacheResult->inheritedEqual)) && MatchedDeclarationsCache::isCacheable(element, style, parentStyle))\n"
        "        m_matchedDeclarationsCache.add(style, parentStyle, cacheHash, matchResult);\n",
        "    if ((cacheHash && !cacheResult) || (cacheResult && !cacheResult->inheritedEqual)) {\n"
        "        if (MatchedDeclarationsCache::isCacheable(element, style, parentStyle))\n"
        "            m_matchedDeclarationsCache.add(style, parentStyle, cacheHash, matchResult);\n"
        "        else\n"
        "            MatchedDeclarationsCache::edenCountRejection(true);\n"
        "    }\n",
    )


PATCHES = {
    STYLE + "MatchedDeclarationsCache.h": cache_header_patch,
    STYLE + "MatchedDeclarationsCache.cpp": cache_patch,
    STYLE + "StyleResolver.cpp": resolver_patch,
}
