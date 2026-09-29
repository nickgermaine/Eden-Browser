from pathlib import Path

from wpe.patch_support import replace_once


STYLE = "Source/WebCore/style/"


def collector_header_patch(text):
    return replace_once(
        text,
        "    void collectMatchingRulesForListSlow(const RuleSet::RuleDataVector&, const MatchRequest&);\n",
        "    void collectMatchingRulesForListSlow(const RuleSet::RuleDataVector&, const MatchRequest&);\n"
        "    bool edenCollectUARulesWithMemo(const RuleSet&);\n"
        "    void edenCollectDynamicUARule(const RuleData&, const MatchRequest&);\n"
        "#ifdef WEBKIT_EDEN_VERIFY_PATCHES\n"
        "    void edenVerifyUARuleMemoHit(const RuleSet&, size_t relationCountBefore, EnumSet<PseudoElementType> pseudoElementsBefore);\n"
        "#endif\n",
    )


def collector_patch(text):
    text = replace_once(
        text,
        '#include "config.h"\n#include "ElementRuleCollector.h"\n',
        '#include "config.h"\n#include <cstdlib>\n#include <cstring>\n#include "ElementRuleCollector.h"\n',
    )
    text = replace_once(
        text,
        '#include "HTMLSlotElement.h"\n',
        '#include "Supplementable.h"\n#include "HTMLNames.h"\n#include "HTMLSlotElement.h"\n',
    )
    text = replace_once(
        text,
        "#include <ranges>\n#include <wtf/SetForScope.h>\n",
        "#include <array>\n"
        "#include <limits>\n"
        "#include <ranges>\n"
        "#include <wtf/HashMap.h>\n"
        "#include <wtf/Hasher.h>\n"
        "#include <wtf/MainThread.h>\n"
        "#include <wtf/NeverDestroyed.h>\n"
        "#include <wtf/SetForScope.h>\n"
        "#include <wtf/text/StringHash.h>\n",
    )
    text = replace_once(
        text,
        "    clearMatchedRules();\n"
        "\n"
        "    collectMatchingRules(MatchRequest(rules));\n"
        "\n"
        "    sortAndTransferMatchedRules(DeclarationOrigin::UserAgent);\n",
        "    clearMatchedRules();\n"
        "\n"
        "    if (!edenCollectUARulesWithMemo(rules))\n"
        "        collectMatchingRules(MatchRequest(rules));\n"
        "\n"
        "    sortAndTransferMatchedRules(DeclarationOrigin::UserAgent);\n",
    )
    implementation = Path(__file__).with_name("ua_rule_memo.inc").read_text()
    return replace_once(
        text,
        "void ElementRuleCollector::matchAllRules(bool matchAuthorAndUserStyles, bool includeSMILProperties)\n",
        implementation + "\nvoid ElementRuleCollector::matchAllRules(bool matchAuthorAndUserStyles, bool includeSMILProperties)\n",
    )


PATCHES = {
    STYLE + "ElementRuleCollector.h": collector_header_patch,
    STYLE + "ElementRuleCollector.cpp": collector_patch,
}

SOURCES = ["ua_rule_memo.inc"]
