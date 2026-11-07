from pathlib import Path

from wpe.patch_support import replace_once


STYLE = "Source/WebCore/style/"


def ruleset_header_patch(text):
    text = replace_once(
        text,
        '#include "CSSSelectorList.h"\n',
        '#include "CSSSelectorList.h"\n#include "EdenAuthorRuleMemo.h"\n',
    )
    text = replace_once(
        text,
        "    void shrinkToFit();\n",
        "    void shrinkToFit();\n"
        "    EdenAuthorRuleMemo &edenAuthorRuleMemo() const { return m_edenAuthorRuleMemo; }\n"
        "    void edenInvalidateAuthorRuleMemo() const { m_edenAuthorRuleMemo.clear(); }\n",
    )
    return replace_once(
        text,
        "    bool m_isBuilding { false };\n",
        "    bool m_isBuilding { false };\n"
        "    mutable EdenAuthorRuleMemo m_edenAuthorRuleMemo;\n",
    )


def ruleset_patch(text):
    text = replace_once(
        text,
        "RuleSet::~RuleSet()\n{\n    RELEASE_ASSERT(isMainThread());\n",
        "RuleSet::~RuleSet()\n{\n    edenInvalidateAuthorRuleMemo();\n    RELEASE_ASSERT(isMainThread());\n",
    )
    text = replace_once(
        text,
        "void RuleSet::addRule(RuleData&& ruleData, CascadeLayerIdentifier cascadeLayerIdentifier, ContainerQueryIdentifier containerQueryIdentifier, ScopeRuleIdentifier scopeRuleIdentifier, RuleFeatureSet::CollectionContext* featureCollectionContext)\n{\n    ASSERT(ruleData.position() == m_ruleCount);\n",
        "void RuleSet::addRule(RuleData&& ruleData, CascadeLayerIdentifier cascadeLayerIdentifier, ContainerQueryIdentifier containerQueryIdentifier, ScopeRuleIdentifier scopeRuleIdentifier, RuleFeatureSet::CollectionContext* featureCollectionContext)\n{\n    edenInvalidateAuthorRuleMemo();\n    ASSERT(ruleData.position() == m_ruleCount);\n",
    )
    text = replace_once(
        text,
        "    if (affectedRulePositionsAndResults.isEmpty())\n        return collectedChanges;\n\n    traverseRuleDatas([&](RuleData& ruleData) {\n",
        "    if (collectedChanges.requiredFullReset || !collectedChanges.changedQueryIndexes.isEmpty())\n"
        "        edenInvalidateAuthorRuleMemo();\n\n"
        "    if (affectedRulePositionsAndResults.isEmpty())\n"
        "        return collectedChanges;\n\n"
        "    traverseRuleDatas([&](RuleData& ruleData) {\n",
    )
    return replace_once(
        text,
        "void RuleSet::shrinkToFit()\n{\n    shrinkMapVectorsToFit(m_idRules);\n",
        "void RuleSet::shrinkToFit()\n{\n    edenInvalidateAuthorRuleMemo();\n    shrinkMapVectorsToFit(m_idRules);\n",
    )


def collector_header_patch(text):
    return replace_once(
        text,
        "    bool edenCollectUARulesWithMemo(const RuleSet&);\n",
        "    bool edenCollectUARulesWithMemo(const RuleSet&);\n"
        "    bool edenCollectAuthorRulesWithMemo(const MatchRequest&);\n"
        "    void edenCollectDynamicAuthorRule(const RuleData&, const MatchRequest&);\n"
        "    void edenVerifyAuthorRuleMemoHit(\n"
        "        const MatchRequest&,\n"
        "        size_t matchedRuleCountBefore,\n"
        "        size_t relationCountBefore,\n"
        "        EnumSet<PseudoElementType> pseudoElementsBefore\n"
        "    );\n",
    )


def collector_patch(text):
    text = replace_once(
        text,
        "        MatchRequest matchRequest(m_authorStyle);\n"
        "        collectMatchingRules(matchRequest);\n",
        "        MatchRequest matchRequest(m_authorStyle);\n"
        "        if (!edenCollectAuthorRulesWithMemo(matchRequest))\n"
        "            collectMatchingRules(matchRequest);\n",
    )
    implementation = Path(__file__).with_name("author_rule_memo.inc").read_text()
    return replace_once(
        text,
        "void ElementRuleCollector::matchAllRules(bool matchAuthorAndUserStyles, bool includeSMILProperties)\n",
        implementation + "\nvoid ElementRuleCollector::matchAllRules(bool matchAuthorAndUserStyles, bool includeSMILProperties)\n",
    )


PATCHES = {
    STYLE + "RuleSet.h": ruleset_header_patch,
    STYLE + "RuleSet.cpp": ruleset_patch,
    STYLE + "ElementRuleCollector.h": collector_header_patch,
    STYLE + "ElementRuleCollector.cpp": collector_patch,
}

FILES = {
    STYLE + "EdenAuthorRuleMemo.h": "author_rule_memo.h",
    "LayoutTests/fast/css/eden-author-rule-memo.html": "author_rule_memo_test.html",
    "LayoutTests/fast/css/eden-author-rule-memo-expected.txt": "author_rule_memo_test_expected.txt",
    "LayoutTests/resources/eden-author-rule-memo.js": "author_rule_memo_test.js",
}

SOURCES = [
    "author_rule_memo.h",
    "author_rule_memo.inc",
    "author_rule_memo_test.html",
    "author_rule_memo_test.js",
    "author_rule_memo_test_expected.txt",
]
