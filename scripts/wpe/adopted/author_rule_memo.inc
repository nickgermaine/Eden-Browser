static constexpr uint8_t edenAuthorShapeHTMLDocument = 1u << 0;
static constexpr uint8_t edenAuthorShapeQuirksMode = 1u << 1;
static constexpr uint8_t edenAuthorShapeHTMLElement = 1u << 2;
static constexpr uint8_t edenAuthorShapeNullID = 1u << 3;
static constexpr uint8_t edenAuthorShapeLink = 1u << 4;
static constexpr uint8_t edenAuthorShapeFocus = 1u << 5;
static constexpr uint8_t edenAuthorShapeFocusVisible = 1u << 6;
static constexpr uint8_t edenAuthorShapeFullscreen = 1u << 7;
static constexpr size_t edenAuthorMemoMaximumAttributes = 4;
static constexpr size_t edenAuthorMemoMaximumClasses = 8;
static constexpr size_t edenAuthorMemoMaximumCandidates = 256;
static constexpr size_t edenAuthorMemoMaximumStaticRules = 64;
static constexpr unsigned edenAuthorMemoMaximumEntries = 32;
static constexpr unsigned edenAuthorMemoMaximumRejectedEntries = 32;
static constexpr size_t edenAuthorMemoMinimumExpensiveRules = 3;

enum class EdenAuthorRuleMemoEvent : uint8_t { Hit, Miss, Bypass };

struct EdenAuthorRuleMemoCounters {
    unsigned long long lookups{0};
    unsigned long long hits{0};
    unsigned long long misses{0};
    unsigned long long bypasses{0};
    unsigned long long reusedRuleCount{0};
    unsigned reports{0};
};

static bool edenAuthorRuleMemoEnabled() {
    static bool enabled = [] {
        const char *value = std::getenv("WEBKIT_EDEN_AUTHOR_RULE_MEMO");
        return !value || std::strcmp(value, "0");
    }();
    return enabled;
}

static bool edenAuthorRuleMemoCountersEnabled() {
    static bool enabled = [] {
        const char *value = std::getenv("WEBKIT_EDEN_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

static bool edenAuthorRuleMemoVerificationEnabled() {
#ifdef WEBKIT_EDEN_VERIFY_PATCHES
    return true;
#else
    static bool enabled = [] {
        const char *value = std::getenv("WEBKIT_EDEN_VERIFY_AUTHOR_RULE_MEMO");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
#endif
}

static void edenAuthorRuleMemoCount(EdenAuthorRuleMemoEvent event, size_t reusedRules = 0) {
    if (!edenAuthorRuleMemoCountersEnabled()) {
        return;
    }

    static thread_local EdenAuthorRuleMemoCounters counters;
    ++counters.lookups;
    switch (event) {
    case EdenAuthorRuleMemoEvent::Hit:
        ++counters.hits;
        counters.reusedRuleCount += reusedRules;
        break;
    case EdenAuthorRuleMemoEvent::Miss:
        ++counters.misses;
        break;
    case EdenAuthorRuleMemoEvent::Bypass:
        ++counters.bypasses;
        break;
    }

    if (counters.reports >= 4096) {
        return;
    }
    bool firstHit = event == EdenAuthorRuleMemoEvent::Hit && counters.hits == 1;
    if (!firstHit && counters.lookups % 1024) {
        return;
    }
    ++counters.reports;
    WTFLogAlways(
        "Eden author rule memo: lookups %llu hits %llu misses %llu bypasses %llu reused-rules %llu",
        counters.lookups,
        counters.hits,
        counters.misses,
        counters.bypasses,
        counters.reusedRuleCount
    );
}

static bool edenAuthorRuleMemoHasLocalSelectorGrammar(const RuleData &ruleData) {
    if (ruleData.canMatchPseudoElement() || ruleData.descendantSelectorIdentifierHashes()[0] ||
        ruleData.styleRule().properties().isEmpty()) {
        return false;
    }

    for (auto *selector = &ruleData.selector(); selector; selector = selector->precedingInComplexSelector()) {
        if (selector->precedingInComplexSelector() && selector->relation() != CSSSelector::Relation::Subselector) {
            return false;
        }

        switch (selector->match()) {
        case CSSSelector::Match::Tag:
        case CSSSelector::Match::Id:
        case CSSSelector::Match::Class:
        case CSSSelector::Match::Exact:
        case CSSSelector::Match::Set:
        case CSSSelector::Match::List:
        case CSSSelector::Match::Hyphen:
        case CSSSelector::Match::Contain:
        case CSSSelector::Match::Begin:
        case CSSSelector::Match::End:
            if (selector->isAttributeSelector() &&
                selector->attribute().localNameLowercase() == HTMLNames::styleAttr->localName()) {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    return true;
}

static bool edenBuildAuthorRuleMemoKey(const Element &element, EdenAuthorRuleMemoEntry &key, unsigned &hash) {
    auto &document = element.document();
    auto &localName = element.localName();
    auto &namespaceURI = element.namespaceURI();
    auto id = element.idForStyleResolution();

    if (localName.length() > 128 || namespaceURI.length() > 128 || id.length() > 96 ||
        (element.hasClass() && element.classNames().size() > edenAuthorMemoMaximumClasses)) {
        return false;
    }

    key.localName = localName;
    key.namespaceURI = namespaceURI;
    key.id = id;

    uint8_t flags = 0;
    if (document.isHTMLDocument()) {
        flags |= edenAuthorShapeHTMLDocument;
    }
    if (document.inQuirksMode()) {
        flags |= edenAuthorShapeQuirksMode;
    }
    if (element.isHTMLElement()) {
        flags |= edenAuthorShapeHTMLElement;
    }
    if (id.isNull()) {
        flags |= edenAuthorShapeNullID;
    }
    if (element.isLink()) {
        flags |= edenAuthorShapeLink;
    }
    if (matchesFocusPseudoClass(element)) {
        flags |= edenAuthorShapeFocus;
    }
    if (matchesFocusVisiblePseudoClass(element)) {
        flags |= edenAuthorShapeFocusVisible;
    }
#if ENABLE(FULLSCREEN_API)
    if (auto *fullscreen = document.fullscreenIfExists(); fullscreen && fullscreen->isFullscreen()) {
        flags |= edenAuthorShapeFullscreen;
    }
#endif
    key.flags = flags;

    if (element.hasClass()) {
        for (auto &className : element.classNames()) {
            if (className.length() > 64) {
                return false;
            }
            key.classNames.append(className);
        }
    }

    if (element.hasAttributesWithoutUpdate()) {
        auto attributes = element.attributes();
        if (attributes.size() > edenAuthorMemoMaximumAttributes) {
            return false;
        }
        for (auto &attribute : attributes) {
            auto &name = attribute.name();
            if (name.localName().length() > 128 || name.namespaceURI().length() > 128 || name.prefix().length() > 128 ||
                attribute.value().length() > 96) {
                return false;
            }
            key.attributeNames.append(name);
            key.attributeValues.append(attribute.value());
        }
    }

    Hasher hasher;
    add(hasher, key.localName.impl(), key.namespaceURI.impl(), key.id.impl(), static_cast<unsigned>(key.flags));
    for (auto &className : key.classNames) {
        add(hasher, className.impl());
    }
    for (size_t i = 0; i < key.attributeNames.size(); ++i) {
        auto &name = key.attributeNames[i];
        add(hasher,
            name.localName().impl(),
            name.namespaceURI().impl(),
            name.prefix().impl(),
            key.attributeValues[i].impl());
    }
    hash = hasher.hash();
    if (!hash) {
        hash = 1;
    }
    hash = AlreadyHashed::avoidDeletedValue(hash);
    return true;
}

static bool edenAuthorRuleMemoKeyMatches(const EdenAuthorRuleMemoEntry &candidate, const EdenAuthorRuleMemoEntry &key) {
    if (candidate.localName != key.localName || candidate.namespaceURI != key.namespaceURI || candidate.id != key.id ||
        candidate.flags != key.flags || candidate.classNames.size() != key.classNames.size() ||
        candidate.attributeNames.size() != key.attributeNames.size()) {
        return false;
    }
    for (size_t i = 0; i < key.classNames.size(); ++i) {
        if (candidate.classNames[i] != key.classNames[i]) {
            return false;
        }
    }
    for (size_t i = 0; i < key.attributeNames.size(); ++i) {
        if (candidate.attributeNames[i] != key.attributeNames[i] ||
            candidate.attributeValues[i] != key.attributeValues[i]) {
            return false;
        }
    }
    return true;
}

static bool
edenAppendAuthorRuleMemoCandidates(Vector<const RuleData *, 64> &candidates, const RuleSet::RuleDataVector *rules) {
    if (!rules) {
        return true;
    }
    if (candidates.size() + rules->size() > edenAuthorMemoMaximumCandidates) {
        return false;
    }
    for (auto &ruleData : *rules) {
        candidates.append(&ruleData);
    }
    return true;
}

static bool edenAuthorRuleMemoCandidates(
    const Element &element,
    const RuleSet &rules,
    uint8_t flags,
    Vector<const RuleData *, 64> &candidates
) {
    bool isHTMLElement = element.isHTMLElement();
    bool isCaseInsensitiveForHTML = isHTMLElement && element.document().isHTMLDocument();

    auto &id = element.idForStyleResolution();
    if (!id.isNull() && !edenAppendAuthorRuleMemoCandidates(candidates, rules.idRules(id))) {
        return false;
    }
    if (element.hasClass()) {
        for (auto &className : element.classNames()) {
            if (!edenAppendAuthorRuleMemoCandidates(candidates, rules.classRules(className))) {
                return false;
            }
        }
    }
    if (element.hasAttributesWithoutUpdate() && rules.hasAttributeRules()) {
        for (auto &attribute : element.attributes()) {
            if (!edenAppendAuthorRuleMemoCandidates(
                    candidates,
                    rules.attributeRules(attribute.localName(), isCaseInsensitiveForHTML)
                )) {
                return false;
            }
        }
    }
    if ((flags & edenAuthorShapeLink) &&
        !edenAppendAuthorRuleMemoCandidates(candidates, &rules.linkPseudoClassRules())) {
        return false;
    }
    if ((flags & edenAuthorShapeFocus) &&
        !edenAppendAuthorRuleMemoCandidates(candidates, &rules.focusPseudoClassRules())) {
        return false;
    }
    if ((flags & edenAuthorShapeFocusVisible) &&
        !edenAppendAuthorRuleMemoCandidates(candidates, &rules.focusVisiblePseudoClassRules())) {
        return false;
    }
#if ENABLE(FULLSCREEN_API)
    if ((flags & edenAuthorShapeFullscreen) &&
        !edenAppendAuthorRuleMemoCandidates(candidates, &rules.fullscreenPseudoClassRules())) {
        return false;
    }
#endif
    if (!edenAppendAuthorRuleMemoCandidates(
            candidates,
            rules.tagRules(element.localName(), isCaseInsensitiveForHTML)
        ) ||
        !edenAppendAuthorRuleMemoCandidates(candidates, &rules.universalRules())) {
        return false;
    }
    return true;
}

static bool edenAuthorRuleMemoAdmissionEligible(const Vector<const RuleData *, 64> &candidates) {
    size_t staticCount = 0;
    size_t expensiveCount = 0;
    for (auto *ruleData : candidates) {
        if (!edenAuthorRuleMemoHasLocalSelectorGrammar(*ruleData)) {
            continue;
        }
        if (++staticCount > edenAuthorMemoMaximumStaticRules) {
            return false;
        }
        if (ruleData->matchBasedOnRuleHash() == MatchBasedOnRuleHash::None) {
            ++expensiveCount;
        }
    }
    return expensiveCount >= edenAuthorMemoMinimumExpensiveRules;
}

void ElementRuleCollector::edenCollectDynamicAuthorRule(const RuleData &ruleData, const MatchRequest &matchRequest) {
    if (!ruleData.isEnabled()) [[unlikely]] {
        return;
    }
    if (!ruleData.canMatchPseudoElement() && m_pseudoElementRequest) {
        return;
    }
    if (m_selectorMatchingState &&
        m_selectorMatchingState->selectorFilter.fastRejectSelector(ruleData.descendantSelectorIdentifierHashes())) {
        return;
    }
    if (ruleData.styleRule().properties().isEmpty() && !m_shouldIncludeEmptyRules) {
        return;
    }

    unsigned specificity;
    if (ruleMatches(ruleData, specificity, matchRequest.styleScopeOrdinal)) {
        addMatchedRule(ruleData, specificity, std::numeric_limits<unsigned>::max(), matchRequest);
    }
}

void ElementRuleCollector::edenVerifyAuthorRuleMemoHit(
    const MatchRequest &matchRequest,
    size_t matchedRuleCountBefore,
    size_t relationCountBefore,
    EnumSet<PseudoElementType> pseudoElementsBefore
) {
    Vector<MatchedRule, 64> memoRules;
    for (size_t i = matchedRuleCountBefore; i < m_matchedRules.size(); ++i) {
        memoRules.append(m_matchedRules[i]);
    }
    Relations memoRelations;
    for (size_t i = relationCountBefore; i < m_styleRelations.size(); ++i) {
        memoRelations.append(m_styleRelations[i]);
    }
    auto memoPseudoElements = m_matchedPseudoElements;

    m_matchedRules.shrink(matchedRuleCountBefore);
    m_styleRelations.shrink(relationCountBefore);
    m_matchedPseudoElements = pseudoElementsBefore;
    collectMatchingRules(matchRequest);

    Vector<MatchedRule, 64> stockRules;
    for (size_t i = matchedRuleCountBefore; i < m_matchedRules.size(); ++i) {
        stockRules.append(m_matchedRules[i]);
    }
    std::ranges::sort(memoRules, compareRules);
    std::ranges::sort(stockRules, compareRules);

    bool equal = memoRules.size() == stockRules.size() && memoPseudoElements == m_matchedPseudoElements &&
                 memoRelations.size() == m_styleRelations.size() - relationCountBefore;
    for (size_t i = 0; equal && i < memoRules.size(); ++i) {
        auto &memoRule = memoRules[i];
        auto &stockRule = stockRules[i];
        equal = memoRule.ruleData == stockRule.ruleData && memoRule.specificity == stockRule.specificity &&
                memoRule.scopingRootDistance == stockRule.scopingRootDistance &&
                memoRule.styleScopeOrdinal == stockRule.styleScopeOrdinal &&
                memoRule.cascadeLayerPriority == stockRule.cascadeLayerPriority;
    }
    for (size_t i = 0; equal && i < memoRelations.size(); ++i) {
        auto &memoRelation = memoRelations[i];
        auto &stockRelation = m_styleRelations[relationCountBefore + i];
        equal = memoRelation.element == stockRelation.element && memoRelation.type == stockRelation.type &&
                memoRelation.value == stockRelation.value;
    }
    if (!equal) {
        WTFLogAlways(
            "Eden author rule memo: mismatch for <%s>, memo rules %zu stock rules %zu, memo relations %zu stock "
            "relations %zu",
            element().localName().string().utf8().data(),
            memoRules.size(),
            stockRules.size(),
            memoRelations.size(),
            m_styleRelations.size() - relationCountBefore
        );
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool ElementRuleCollector::edenCollectAuthorRulesWithMemo(const MatchRequest &matchRequest) {
    if (!edenAuthorRuleMemoEnabled()) {
        return false;
    }

    auto bypass = [] {
        edenAuthorRuleMemoCount(EdenAuthorRuleMemoEvent::Bypass);
        return false;
    };

    auto &element = this->element();
    if (!isMainThread() || !element.isHTMLElement() || m_mode != SelectorChecker::Mode::ResolvingStyle ||
        m_pseudoElementRequest || m_shouldIncludeEmptyRules || matchRequest.matchingPartPseudoElementRules) {
        return bypass();
    }
    auto &document = element.document();
    if (&element == document.documentElement() || element.isInShadowTree() || element.assignedSlot() ||
        element.shadowRoot()) {
        return bypass();
    }
    if (auto *parent = element.parentElement(); parent && parent->shadowRoot()) {
        return bypass();
    }
    if (matchRequest.ruleSet.hasContainerQueries() || matchRequest.ruleSet.hasScopeRules()) {
        return bypass();
    }

    EdenAuthorRuleMemoEntry key;
    unsigned hash;
    if (!edenBuildAuthorRuleMemoKey(element, key, hash)) {
        return bypass();
    }

    auto &memo = matchRequest.ruleSet.edenAuthorRuleMemo();
    EdenAuthorRuleMemoEntry *entry = nullptr;
    if (auto iterator = memo.entries.find(hash); iterator != memo.entries.end()) {
        for (auto &candidate : iterator->value) {
            if (edenAuthorRuleMemoKeyMatches(*candidate, key)) {
                entry = candidate.get();
                break;
            }
        }
    }

    auto matchedRuleCountBefore = m_matchedRules.size();
    auto relationCountBefore = m_styleRelations.size();
    auto pseudoElementsBefore = m_matchedPseudoElements;

    if (entry) {
        size_t reusedRules = 0;
        for (auto &cachedRule : entry->rules) {
            if (!cachedRule.eligible) {
                edenCollectDynamicAuthorRule(*cachedRule.ruleData, matchRequest);
                continue;
            }
            ++reusedRules;
            if (cachedRule.matches && cachedRule.ruleData->isEnabled() &&
                !cachedRule.ruleData->styleRule().properties().isEmpty()) [[likely]] {
                addMatchedRule(
                    *cachedRule.ruleData,
                    cachedRule.specificity,
                    std::numeric_limits<unsigned>::max(),
                    matchRequest
                );
            }
        }

        auto &rules = matchRequest.ruleSet;
        auto pseudoElementTypes =
            element.isHTMLElement() ? rules.universalHTMLPseudoElementTypes() : rules.universalPseudoElementTypes();
        m_matchedPseudoElements.add(pseudoElementTypes & allPublicPseudoElementTypes);
        edenAuthorRuleMemoCount(EdenAuthorRuleMemoEvent::Hit, reusedRules);
        if (edenAuthorRuleMemoVerificationEnabled()) {
            edenVerifyAuthorRuleMemoHit(
                matchRequest,
                matchedRuleCountBefore,
                relationCountBefore,
                pseudoElementsBefore
            );
        }
        return true;
    }

    if (memo.entryCount >= edenAuthorMemoMaximumEntries) {
        return bypass();
    }
    if (auto iterator = memo.rejectedEntries.find(hash); iterator != memo.rejectedEntries.end()) {
        for (auto &candidate : iterator->value) {
            if (edenAuthorRuleMemoKeyMatches(*candidate, key)) {
                return bypass();
            }
        }
    }

    Vector<const RuleData *, 64> candidates;
    if (!edenAuthorRuleMemoCandidates(element, matchRequest.ruleSet, key.flags, candidates) ||
        !edenAuthorRuleMemoAdmissionEligible(candidates)) {
        if (memo.rejectedEntryCount >= edenAuthorMemoMaximumRejectedEntries) {
            auto evicted = memo.rejectedEntries.begin();
            memo.rejectedEntryCount -= evicted->value.size();
            memo.rejectedEntries.remove(evicted);
        }
        auto iterator = memo.rejectedEntries.add(hash, EdenAuthorRuleMemo::EntryBucket{}).iterator;
        iterator->value.append(makeUnique<EdenAuthorRuleMemoEntry>(WTF::move(key)));
        ++memo.rejectedEntryCount;
        return bypass();
    }

    collectMatchingRules(matchRequest);

    auto newEntry = makeUnique<EdenAuthorRuleMemoEntry>(WTF::move(key));
    newEntry->rules.reserveInitialCapacity(candidates.size());
    for (auto *candidate : candidates) {
        EdenAuthorRuleMemoResult cachedRule;
        cachedRule.ruleData = candidate;
        cachedRule.eligible = edenAuthorRuleMemoHasLocalSelectorGrammar(*candidate);
        if (cachedRule.eligible) {
            for (size_t i = matchedRuleCountBefore; i < m_matchedRules.size(); ++i) {
                auto &matchedRule = m_matchedRules[i];
                if (matchedRule.ruleData == candidate) {
                    cachedRule.matches = true;
                    cachedRule.specificity = matchedRule.specificity;
                    break;
                }
            }
        }
        newEntry->rules.append(cachedRule);
    }

    auto iterator = memo.entries.add(hash, EdenAuthorRuleMemo::EntryBucket{}).iterator;
    iterator->value.append(WTF::move(newEntry));
    ++memo.entryCount;
    edenAuthorRuleMemoCount(EdenAuthorRuleMemoEvent::Miss);
    return true;
}
