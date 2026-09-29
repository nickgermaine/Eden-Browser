#pragma once

#include "QualifiedName.h"
#include "RuleData.h"
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/StringHash.h>

namespace WebCore {
    namespace Style {

        struct EdenAuthorRuleMemoResult {
            const RuleData *ruleData{nullptr};
            unsigned specificity{0};
            bool eligible{false};
            bool matches{false};
        };

        struct EdenAuthorRuleMemoEntry {
            WTF_MAKE_TZONE_ALLOCATED_INLINE(EdenAuthorRuleMemoEntry);

          public:
            AtomString localName;
            AtomString namespaceURI;
            AtomString id;
            Vector<AtomString, 4> classNames;
            Vector<QualifiedName, 4> attributeNames;
            Vector<AtomString, 4> attributeValues;
            uint8_t flags{0};
            Vector<EdenAuthorRuleMemoResult, 32> rules;
        };

        struct EdenAuthorRuleMemo {
            using EntryBucket = Vector<std::unique_ptr<EdenAuthorRuleMemoEntry>, 1>;

            HashMap<unsigned, EntryBucket, AlreadyHashed> entries;
            HashMap<unsigned, EntryBucket, AlreadyHashed> rejectedEntries;
            unsigned entryCount{0};
            unsigned rejectedEntryCount{0};

            void clear() {
                entries.clear();
                rejectedEntries.clear();
                entryCount = 0;
                rejectedEntryCount = 0;
            }
        };

    }
}
