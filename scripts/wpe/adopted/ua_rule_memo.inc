static bool edenUARuleMemoEnabled() {
    static bool enabled = [] {
        const char *value = std::getenv("WEBKIT_EDEN_DISABLE_UA_RULE_MEMO");
        return !value || std::strcmp(value, "1");
    }();
    return enabled;
}

static bool edenUARuleMemoCountersEnabled() {
    static bool enabled = [] {
        const char *value = std::getenv("WEBKIT_EDEN_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

enum class EdenUARuleMemoEvent : uint8_t { Hit, Miss, NonMemoizable };

static void edenUARuleMemoCount(EdenUARuleMemoEvent event) {
    if (!edenUARuleMemoCountersEnabled()) {
        return;
    }
    static std::array<unsigned long long, 3> counts{};
    static unsigned long long total = 0;
    ++counts[static_cast<size_t>(event)];
    if (++total % 4096) {
        return;
    }
    WTFLogAlways("Eden UA rule memo: hits %llu misses %llu non-memoizable %llu", counts[0], counts[1], counts[2]);
}

static constexpr uint8_t edenUAShapeHTMLDocument = 1u << 0;
static constexpr uint8_t edenUAShapeQuirksMode = 1u << 1;
static constexpr uint8_t edenUAShapeLink = 1u << 2;
static constexpr uint8_t edenUAShapeUserAgentShadowTree = 1u << 3;

static constexpr size_t edenUAMemoMaximumAttributes = 4;
static constexpr size_t edenUAMemoMaximumVariants = 16;
static constexpr unsigned edenUAMemoMaximumShapes = 512;

static bool edenUARuleSelectorIsStatic(const CSSSelector &firstSelector, bool &valuesMatter) {
    for (auto *selector = &firstSelector; selector; selector = selector->precedingInComplexSelector()) {
        if (selector->precedingInComplexSelector() && selector->relation() != CSSSelector::Relation::Subselector) {
            return false;
        }

        switch (selector->match()) {
        case CSSSelector::Match::Tag:
        case CSSSelector::Match::Set:
            continue;
        case CSSSelector::Match::Exact:
        case CSSSelector::Match::List:
        case CSSSelector::Match::Hyphen:
        case CSSSelector::Match::Contain:
        case CSSSelector::Match::Begin:
        case CSSSelector::Match::End:
            valuesMatter = true;
            continue;
        default:
            return false;
        }
    }
    return true;
}

struct EdenUARuleMemoVariant {
    Vector<AtomString, edenUAMemoMaximumAttributes> values;
    Vector<MatchedRule, 4> staticMatches;
};

struct EdenUARuleMemoShape {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(EdenUARuleMemoShape);

    AtomString localName;
    AtomString namespaceURI;
    AtomString userAgentPart;
    Vector<QualifiedName, edenUAMemoMaximumAttributes> attributeNames;
    uint8_t flags{0};
    bool broken{false};
    bool valuesMatter{false};
    Vector<const RuleData *> staticRules;
    Vector<const RuleData *> dynamicRules;
    Vector<EdenUARuleMemoVariant> variants;
};

struct EdenUARuleMemo {
    unsigned version{0};
    bool initialized{false};
    bool usable{false};
    unsigned shapeCount{0};
    HashMap<unsigned, Vector<std::unique_ptr<EdenUARuleMemoShape>>, AlreadyHashed> shapes;
};

class EdenUARuleMemoStore final : public Supplement<Document> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(EdenUARuleMemoStore);

  public:
    static EdenUARuleMemoStore &from(Document &document) {
        constexpr auto name = "EdenUARuleMemoStore"_s;
        auto *store = static_cast<EdenUARuleMemoStore *>(Supplement<Document>::from(&document, name));
        if (!store) {
            auto owned = makeUnique<EdenUARuleMemoStore>();
            store = owned.get();
            Supplement<Document>::provideTo(&document, name, WTF::move(owned));
        }
        return *store;
    }
    std::array<EdenUARuleMemo, 3> memos;
};

static EdenUARuleMemo *edenUARuleMemoFor(Document &document, const RuleSet &rules) {
    size_t index;
    if (&rules == UserAgentStyle::defaultStyle) {
        index = 0;
    } else if (&rules == UserAgentStyle::defaultPrintStyle) {
        index = 1;
    } else if (&rules == UserAgentStyle::defaultQuirksStyle) {
        index = 2;
    } else {
        return nullptr;
    }

    auto &memo = EdenUARuleMemoStore::from(document).memos[index];
    if (!memo.initialized || memo.version != UserAgentStyle::defaultStyleVersion) {
        memo.initialized = true;
        memo.version = UserAgentStyle::defaultStyleVersion;
        memo.shapes.clear();
        memo.shapeCount = 0;

        auto &features = rules.features();
        auto &styleAttributeName = HTMLNames::styleAttr->localName();
        bool referencesStyleAttribute = features.attributeLocalNamesInRules.contains(styleAttributeName) ||
                                        features.attributeLowercaseLocalNamesInRules.contains(styleAttributeName);
        memo.usable = !referencesStyleAttribute && !rules.hasContainerQueries() && !rules.hasScopeRules() &&
                      features.idsInRules.isEmpty() && features.idRules.isEmpty() && features.classRules.isEmpty();
    }
    return memo.usable ? &memo : nullptr;
}

static void edenUARuleMemoBuildShape(EdenUARuleMemoShape &shape, const RuleSet &rules, bool isCaseInsensitiveForHTML) {
    auto visit = [&](const RuleSet::RuleDataVector *bucket) {
        if (!bucket) {
            return;
        }
        for (auto &ruleData : *bucket) {
            if (!ruleData.isEnabled()) {
                shape.broken = true;
                continue;
            }
            if (ruleData.styleRule().properties().isEmpty()) {
                continue;
            }

            bool valuesMatter = false;
            bool isStatic = !ruleData.canMatchPseudoElement() && !ruleData.descendantSelectorIdentifierHashes()[0] &&
                            edenUARuleSelectorIsStatic(ruleData.selector(), valuesMatter);
            if (isStatic) {
                shape.staticRules.append(&ruleData);
                shape.valuesMatter = shape.valuesMatter || valuesMatter;
            } else {
                shape.dynamicRules.append(&ruleData);
            }
        }
    };

    if ((shape.flags & edenUAShapeUserAgentShadowTree) && !shape.userAgentPart.isEmpty()) {
        visit(rules.userAgentPartRules(shape.userAgentPart));
    }
    for (auto &name : shape.attributeNames) {
        visit(rules.attributeRules(name.localName(), isCaseInsensitiveForHTML));
    }
    if (shape.flags & edenUAShapeLink) {
        visit(&rules.linkPseudoClassRules());
    }
    visit(rules.tagRules(shape.localName, isCaseInsensitiveForHTML));
    visit(&rules.universalRules());
}

void ElementRuleCollector::edenCollectDynamicUARule(const RuleData &ruleData, const MatchRequest &matchRequest) {
    if (!ruleData.isEnabled()) [[unlikely]] {
        return;
    }
    if (m_selectorMatchingState &&
        m_selectorMatchingState->selectorFilter.fastRejectSelector(ruleData.descendantSelectorIdentifierHashes())) {
        return;
    }
    unsigned specificity;
    if (ruleMatches(ruleData, specificity, matchRequest.styleScopeOrdinal)) {
        addMatchedRule(ruleData, specificity, std::numeric_limits<unsigned>::max(), matchRequest);
    }
}

#ifdef WEBKIT_EDEN_VERIFY_PATCHES
void ElementRuleCollector::edenVerifyUARuleMemoHit(
    const RuleSet &rules,
    size_t relationCountBefore,
    EnumSet<PseudoElementType> pseudoElementsBefore
) {
    auto memoRules = m_matchedRules;
    Relations memoRelations;
    for (size_t i = relationCountBefore; i < m_styleRelations.size(); ++i) {
        memoRelations.append(m_styleRelations[i]);
    }
    auto memoPseudoElements = m_matchedPseudoElements;

    m_matchedRules.clear();
    m_styleRelations.shrink(relationCountBefore);
    m_matchedPseudoElements = pseudoElementsBefore;

    collectMatchingRules(MatchRequest(rules));

    auto stockRules = m_matchedRules;
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
            "Eden UA rule memo: mismatch for <%s>, memo rules %zu stock rules %zu, memo relations %zu stock relations "
            "%zu, using stock result",
            element().localName().string().utf8().data(),
            memoRules.size(),
            stockRules.size(),
            memoRelations.size(),
            m_styleRelations.size() - relationCountBefore
        );
        RELEASE_ASSERT_NOT_REACHED();
    }
}
#endif

bool ElementRuleCollector::edenCollectUARulesWithMemo(const RuleSet &rules) {
    if (!edenUARuleMemoEnabled()) {
        return false;
    }
    if (!isMainThread()) {
        return false;
    }

    auto nonMemoizable = [] {
        edenUARuleMemoCount(EdenUARuleMemoEvent::NonMemoizable);
        return false;
    };

    if (m_mode != SelectorChecker::Mode::ResolvingStyle || m_pseudoElementRequest || m_shouldIncludeEmptyRules) {
        return nonMemoizable();
    }

    auto &element = this->element();
    if (!element.isHTMLElement() || element.localName().length() > 128 || element.namespaceURI().length() > 128) {
        return nonMemoizable();
    }

    auto &document = element.document();
    if (&element == document.documentElement()) {
        return nonMemoizable();
    }
    if (matchesFocusPseudoClass(element) || matchesFocusVisiblePseudoClass(element)) {
        return nonMemoizable();
    }
#if ENABLE(FULLSCREEN_API)
    if (auto *fullscreen = document.fullscreenIfExists(); fullscreen && fullscreen->isFullscreen()) {
        return nonMemoizable();
    }
#endif

    auto *memo = edenUARuleMemoFor(document, rules);
    if (!memo) {
        return nonMemoizable();
    }

    uint8_t flags = 0;
    AtomString userAgentPart;
    if (auto *shadowRoot = element.containingShadowRoot();
        shadowRoot && shadowRoot->mode() == ShadowRootMode::UserAgent) {
#if ENABLE(VIDEO)
        if (element.isWebVTTElement() || element.userAgentPart() == UserAgentParts::cue()) {
            return nonMemoizable();
        }
#endif
        flags |= edenUAShapeUserAgentShadowTree;
        userAgentPart = element.userAgentPart();
        if (userAgentPart.length() > 128) {
            return nonMemoizable();
        }
    }

    bool isCaseInsensitiveForHTML = document.isHTMLDocument();
    if (isCaseInsensitiveForHTML) {
        flags |= edenUAShapeHTMLDocument;
    }
    if (document.inQuirksMode()) {
        flags |= edenUAShapeQuirksMode;
    }
    if (element.isLink()) {
        flags |= edenUAShapeLink;
    }

    Vector<QualifiedName, edenUAMemoMaximumAttributes> attributeNames;
    Vector<AtomString, edenUAMemoMaximumAttributes> attributeValues;
    if (element.hasAttributesWithoutUpdate()) {
        auto &features = rules.features();
        auto &names = isCaseInsensitiveForHTML ? features.attributeLowercaseLocalNamesInRules
                                               : features.attributeLocalNamesInRules;
        if (!names.isEmpty()) {
            for (auto &attribute : element.attributes()) {
                if (!names.contains(attribute.localName())) {
                    continue;
                }
                if (attributeNames.size() == edenUAMemoMaximumAttributes || attribute.value().length() > 256 ||
                    attribute.namespaceURI().length() > 128 || attribute.prefix().length() > 128) {
                    return nonMemoizable();
                }
                attributeNames.append(attribute.name());
                attributeValues.append(attribute.value());
            }
        }
    }

    auto &localName = element.localName();
    auto &namespaceURI = element.namespaceURI();

    Hasher hasher;
    add(hasher, localName.impl(), namespaceURI.impl(), userAgentPart.impl(), static_cast<unsigned>(flags));
    for (auto &name : attributeNames) {
        add(hasher, name.localName().impl(), name.namespaceURI().impl(), name.prefix().impl());
    }
    unsigned hash = hasher.hash();
    if (!hash) {
        hash = 1;
    }
    hash = AlreadyHashed::avoidDeletedValue(hash);

    auto shapeMatches = [&](const EdenUARuleMemoShape &candidate) {
        if (candidate.flags != flags || candidate.localName != localName || candidate.namespaceURI != namespaceURI ||
            candidate.userAgentPart != userAgentPart) {
            return false;
        }
        if (candidate.attributeNames.size() != attributeNames.size()) {
            return false;
        }
        for (size_t i = 0; i < attributeNames.size(); ++i) {
            if (candidate.attributeNames[i] != attributeNames[i]) {
                return false;
            }
        }
        return true;
    };

    auto findShape = [&]() -> EdenUARuleMemoShape * {
        auto it = memo->shapes.find(hash);
        if (it == memo->shapes.end()) {
            return nullptr;
        }
        for (auto &candidate : it->value) {
            if (shapeMatches(*candidate)) {
                return candidate.get();
            }
        }
        return nullptr;
    };

    auto findVariant = [&](EdenUARuleMemoShape &shape) -> EdenUARuleMemoVariant * {
        for (auto &candidate : shape.variants) {
            if (shape.valuesMatter && candidate.values != attributeValues) {
                continue;
            }
            return &candidate;
        }
        return nullptr;
    };

    auto *shape = findShape();
    if (shape && !shape->broken) {
        if (auto *variant = findVariant(*shape)) {
#ifdef WEBKIT_EDEN_VERIFY_PATCHES
            auto relationCountBefore = m_styleRelations.size();
            auto pseudoElementsBefore = m_matchedPseudoElements;
#endif
            for (auto &matchedRule : variant->staticMatches) {
                if (matchedRule.ruleData->isEnabled()) [[likely]] {
                    m_matchedRules.append(matchedRule);
                }
            }
            if (!shape->dynamicRules.isEmpty()) {
                MatchRequest matchRequest(rules);
                for (auto *ruleData : shape->dynamicRules) {
                    edenCollectDynamicUARule(*ruleData, matchRequest);
                }
            }
            m_matchedPseudoElements.add(rules.universalHTMLPseudoElementTypes() & allPublicPseudoElementTypes);
            edenUARuleMemoCount(EdenUARuleMemoEvent::Hit);
#ifdef WEBKIT_EDEN_VERIFY_PATCHES
            edenVerifyUARuleMemoHit(rules, relationCountBefore, pseudoElementsBefore);
#endif
            return true;
        }
    }

    collectMatchingRules(MatchRequest(rules));

    if (!shape) {
        if (memo->shapeCount >= edenUAMemoMaximumShapes) {
            memo->shapes.clear();
            memo->shapeCount = 0;
        }
        auto newShape = makeUnique<EdenUARuleMemoShape>();
        newShape->localName = localName;
        newShape->namespaceURI = namespaceURI;
        newShape->userAgentPart = userAgentPart;
        newShape->flags = flags;
        newShape->attributeNames = attributeNames;
        edenUARuleMemoBuildShape(*newShape, rules, isCaseInsensitiveForHTML);
        shape = newShape.get();
        memo->shapes.add(hash, Vector<std::unique_ptr<EdenUARuleMemoShape>>{})
            .iterator->value.append(WTF::move(newShape));
        ++memo->shapeCount;
    }

    if (shape->broken) {
        edenUARuleMemoCount(EdenUARuleMemoEvent::NonMemoizable);
        return true;
    }

    edenUARuleMemoCount(EdenUARuleMemoEvent::Miss);

    if (shape->variants.size() >= edenUAMemoMaximumVariants) {
        return true;
    }

    EdenUARuleMemoVariant variant;
    if (findVariant(*shape)) {
        return true;
    }

    for (auto &matchedRule : m_matchedRules) {
        if (shape->staticRules.contains(matchedRule.ruleData)) {
            variant.staticMatches.append(matchedRule);
            continue;
        }
        if (!shape->dynamicRules.contains(matchedRule.ruleData)) {
            shape->broken = true;
            shape->variants.clear();
            return true;
        }
    }

    if (shape->valuesMatter) {
        variant.values = attributeValues;
    }
    shape->variants.append(WTF::move(variant));
    return true;
}
