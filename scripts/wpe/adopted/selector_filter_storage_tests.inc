static String edenSelectorFilterFixture() {
    StringBuilder builder;
    builder.append(
        "<div id='root' class='outer' data-Mixed='x'>"
        "<section id='wide' class='c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11' data-one='1' data-two='2' title='wide'>"_s
    );
    for (unsigned depth = 0; depth < 30; ++depth) {
        builder.append("<div id='deep"_s, depth, "' class='level"_s, depth % 3, "' data-depth='"_s, depth, "'>"_s);
    }
    builder.append("<span id='leaf'></span>"_s);
    for (unsigned depth = 0; depth < 30; ++depth) {
        builder.append("</div>"_s);
    }
    builder.append(
        "</section>"
        "<div id='dup' class='same'><div id='dup-child' class='same'><span id='dup-leaf'></span></div></div>"
        "<svg id='vector' viewBox='0 0 10 10' preserveAspectRatio='none'>"
        "<g id='group' class='shape'><rect id='shape' width='4' height='4'></rect></g></svg></div>"_s
    );
    return builder.toString();
}

static Vector<unsigned> edenElementIdentifierHashes(const Element &element) {
    Vector<unsigned, 4> collected;
    SelectorFilter::collectElementIdentifierHashes(element, collected);
    Vector<unsigned> hashes;
    hashes.appendVector(collected);
    return hashes;
}

static Vector<Ref<Element>> edenAncestorChain(Element &element) {
    Vector<Ref<Element>> chain;
    for (RefPtr ancestor = &element; ancestor; ancestor = ancestor->parentElement()) {
        chain.insert(0, *ancestor);
    }
    return chain;
}

static bool edenFilterMayContain(const SelectorFilter &filter, unsigned hash) {
    return !filter.fastRejectSelector(SelectorFilter::Hashes{hash, 0, 0, 0});
}

static SelectorFilter::Hashes edenSelectorHashes(Document &document, const String &selector) {
    auto sheet = StyleSheetContents::create(CSSParserContext{document});
    EXPECT_TRUE(sheet->parseString(makeString(selector, " { color: green; }"_s)));
    EXPECT_EQ(1u, sheet->ruleCount());
    RefPtr rule = sheet->ruleCount() ? dynamicDowncast<StyleRule>(sheet->ruleAt(0)) : nullptr;
    EXPECT_TRUE(rule);
    if (!rule) {
        return {};
    }
    return SelectorFilter::collectHashes(rule->selectorList().first());
}

static Vector<unsigned> edenSelectorFilterProbeHashes(TestPageHarness &testPage) {
    Vector<unsigned> probes;
    for (auto &identifier : {"root"_s, "wide"_s, "dup"_s, "dup-child"_s, "vector"_s, "group"_s, "shape"_s}) {
        RefPtr element = testPage.getElementById(identifier);
        EXPECT_TRUE(element);
        if (element) {
            probes.appendVector(edenElementIdentifierHashes(*element));
        }
    }
    for (unsigned depth = 0; depth < 30; ++depth) {
        RefPtr element = testPage.getElementById(makeString("deep"_s, depth));
        EXPECT_TRUE(element);
        if (element) {
            probes.appendVector(edenElementIdentifierHashes(*element));
        }
    }
    for (unsigned value = 1; value <= 64; ++value) {
        probes.append(value * 2654435761u);
    }
    return probes;
}

TEST(SelectorFilterStorage, ElementHashOrderMatchesSelectorHashes) {
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(edenSelectorFilterFixture());
    RefPtr wide = testPage.getElementById("wide"_s);
    ASSERT_TRUE(wide);
    auto &document = testPage.document();

    auto hashes = edenElementIdentifierHashes(*wide);
    ASSERT_EQ(17u, hashes.size());
    EXPECT_EQ(edenSelectorHashes(document, "section *"_s)[0], hashes[0]);
    EXPECT_EQ(edenSelectorHashes(document, "#wide *"_s)[0], hashes[1]);
    for (unsigned index = 0; index < 12; ++index) {
        EXPECT_EQ(edenSelectorHashes(document, makeString(".c"_s, index, " *"_s))[0], hashes[2 + index]);
    }
    EXPECT_EQ(edenSelectorHashes(document, "[data-one] *"_s)[0], hashes[14]);
    EXPECT_EQ(edenSelectorHashes(document, "[data-two] *"_s)[0], hashes[15]);
    EXPECT_EQ(edenSelectorHashes(document, "[title] *"_s)[0], hashes[16]);
}

TEST(SelectorFilterStorage, OverflowingParentFrameKeepsEveryHash) {
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(edenSelectorFilterFixture());
    RefPtr wide = testPage.getElementById("wide"_s);
    ASSERT_TRUE(wide);
    auto &document = testPage.document();
    auto wideHashes = edenElementIdentifierHashes(*wide);
    EXPECT_GT(wideHashes.size(), 10u);

    SelectorFilter filter;
    filter.pushParentInitializingIfNeeded(*wide);
    EXPECT_TRUE(filter.parentStackIsConsistent(wide.get()));
    Vector<unsigned> ancestorHashes;
    for (auto &ancestor : edenAncestorChain(*wide)) {
        ancestorHashes.appendVector(edenElementIdentifierHashes(ancestor));
    }
    for (auto hash : ancestorHashes) {
        EXPECT_TRUE(edenFilterMayContain(filter, hash));
    }
    EXPECT_FALSE(filter.fastRejectSelector(edenSelectorHashes(document, "section.c11[data-two] span"_s)));
    EXPECT_FALSE(filter.fastRejectSelector(edenSelectorHashes(document, "#wide.c0.c10[TITLE] > div"_s)));
    EXPECT_FALSE(filter.fastRejectSelector(edenSelectorHashes(document, ".c9.c10.c11[data-one] div"_s)));

    filter.popParentsUntil(nullptr);
    EXPECT_TRUE(filter.parentStackIsEmpty());
    for (auto hash : ancestorHashes) {
        EXPECT_FALSE(edenFilterMayContain(filter, hash));
    }
    EXPECT_TRUE(filter.fastRejectSelector(edenSelectorHashes(document, "section.c11[data-two] span"_s)));
}

TEST(SelectorFilterStorage, DeepParentStackMatchesFreshFiltersAfterPops) {
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(edenSelectorFilterFixture());
    auto &document = testPage.document();
    RefPtr deepest = testPage.getElementById("deep29"_s);
    RefPtr middle = testPage.getElementById("deep9"_s);
    ASSERT_TRUE(deepest);
    ASSERT_TRUE(middle);
    auto chain = edenAncestorChain(*deepest);
    ASSERT_GT(chain.size(), 20u);
    auto probes = edenSelectorFilterProbeHashes(testPage);

    SelectorFilter incremental;
    incremental.pushParentInitializingIfNeeded(chain[0]);
    for (size_t index = 1; index < chain.size(); ++index) {
        incremental.pushParent(chain[index].ptr());
    }
    EXPECT_TRUE(incremental.parentStackIsConsistent(deepest.get()));
    SelectorFilter initialized;
    initialized.pushParentInitializingIfNeeded(*deepest);
    EXPECT_TRUE(initialized.parentStackIsConsistent(deepest.get()));
    for (auto hash : probes) {
        EXPECT_EQ(edenFilterMayContain(initialized, hash), edenFilterMayContain(incremental, hash));
    }
    for (auto &ancestor : chain) {
        for (auto hash : edenElementIdentifierHashes(ancestor)) {
            EXPECT_TRUE(edenFilterMayContain(incremental, hash));
        }
    }
    EXPECT_FALSE(incremental.fastRejectSelector(edenSelectorHashes(document, "#deep0 .level2[data-depth] span"_s)));
    EXPECT_FALSE(incremental.fastRejectSelector(edenSelectorHashes(document, "#deep29.level2 > span"_s)));

    incremental.popParentsUntil(middle.get());
    EXPECT_TRUE(incremental.parentStackIsConsistent(middle.get()));
    SelectorFilter fresh;
    fresh.pushParentInitializingIfNeeded(*middle);
    for (auto hash : probes) {
        EXPECT_EQ(edenFilterMayContain(fresh, hash), edenFilterMayContain(incremental, hash));
    }
    EXPECT_FALSE(incremental.fastRejectSelector(edenSelectorHashes(document, "#deep9.level0 span"_s)));

    incremental.popParentsUntil(nullptr);
    initialized.popParentsUntil(nullptr);
    EXPECT_TRUE(incremental.parentStackIsEmpty());
    EXPECT_TRUE(initialized.parentStackIsEmpty());
    for (auto hash : probes) {
        EXPECT_FALSE(edenFilterMayContain(incremental, hash));
        EXPECT_FALSE(edenFilterMayContain(initialized, hash));
    }
}

TEST(SelectorFilterStorage, DuplicateAncestorHashesSurviveChildPop) {
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(edenSelectorFilterFixture());
    auto &document = testPage.document();
    RefPtr duplicate = testPage.getElementById("dup"_s);
    RefPtr duplicateChild = testPage.getElementById("dup-child"_s);
    ASSERT_TRUE(duplicate);
    ASSERT_TRUE(duplicateChild);
    auto sameClass = edenSelectorHashes(document, ".same *"_s)[0];
    auto divTag = edenSelectorHashes(document, "div *"_s)[0];
    auto probes = edenSelectorFilterProbeHashes(testPage);

    SelectorFilter filter;
    filter.pushParentInitializingIfNeeded(*duplicate);
    filter.pushParent(duplicateChild.get());
    EXPECT_TRUE(edenFilterMayContain(filter, sameClass));
    EXPECT_TRUE(edenFilterMayContain(filter, divTag));
    filter.popParent();
    EXPECT_TRUE(filter.parentStackIsConsistent(duplicate.get()));
    EXPECT_TRUE(edenFilterMayContain(filter, sameClass));
    EXPECT_TRUE(edenFilterMayContain(filter, divTag));
    EXPECT_FALSE(filter.fastRejectSelector(edenSelectorHashes(document, "#dup.same span"_s)));

    SelectorFilter fresh;
    fresh.pushParentInitializingIfNeeded(*duplicate);
    for (auto hash : probes) {
        EXPECT_EQ(edenFilterMayContain(fresh, hash), edenFilterMayContain(filter, hash));
    }
}

TEST(SelectorFilterStorage, SvgAndHtmlAttributeCasingMatchesSelectors) {
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(edenSelectorFilterFixture());
    auto &document = testPage.document();
    RefPtr vector = testPage.getElementById("vector"_s);
    RefPtr group = testPage.getElementById("group"_s);
    RefPtr root = testPage.getElementById("root"_s);
    ASSERT_TRUE(vector);
    ASSERT_TRUE(group);
    ASSERT_TRUE(root);

    auto viewBox = edenSelectorHashes(document, "[viewBox] *"_s)[0];
    EXPECT_EQ(viewBox, edenSelectorHashes(document, "[viewbox] *"_s)[0]);
    EXPECT_TRUE(edenElementIdentifierHashes(*vector).contains(viewBox));
    auto mixed = edenSelectorHashes(document, "[DATA-MIXED] *"_s)[0];
    EXPECT_TRUE(edenElementIdentifierHashes(*root).contains(mixed));

    SelectorFilter filter;
    filter.pushParentInitializingIfNeeded(*group);
    EXPECT_FALSE(filter.fastRejectSelector(edenSelectorHashes(document, "svg[viewBox] rect"_s)));
    EXPECT_FALSE(filter.fastRejectSelector(edenSelectorHashes(document, "svg[viewbox][preserveAspectRatio] g.shape > rect"_s)));
    EXPECT_FALSE(filter.fastRejectSelector(edenSelectorHashes(document, "div.outer[data-mixed] rect"_s)));
    filter.popParentsUntil(nullptr);
    EXPECT_TRUE(filter.fastRejectSelector(edenSelectorHashes(document, "svg[viewBox] rect"_s)));
}
