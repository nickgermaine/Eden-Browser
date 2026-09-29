static String edenListMarkerText(TestPageHarness &testPage, const String &identifier) {
    auto element = testPage.getElementById(identifier);
    if (!element) {
        return {};
    }
    auto *listItem = dynamicDowncast<RenderListItem>(element->renderer());
    return listItem ? listItem->markerTextWithSuffix() : String{};
}

TEST(ListRenumbering, PreservesMarkersAcrossMutation) {
    auto testPage = TestPageHarness::create();
    testPage.loadHTML(edenListRenumberingFixture());
    testPage.document().updateLayoutIgnorePendingStylesheets();

    EXPECT_EQ(edenListMarkerText(testPage, "reverse-one"_s), "3. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "reverse-two"_s), "2. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "reverse-three"_s), "1. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "explicit-one"_s), "7. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "explicit-value"_s), "12. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "explicit-three"_s), "13. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "outer-one"_s), "4. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "outer-two"_s), "5. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "inner-one"_s), "1. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "inner-two"_s), "2. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "css-one"_s), "21. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "css-set"_s), "7. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "css-three"_s), "8. "_s);

    auto descending = dynamicDowncast<HTMLOListElement>(testPage.getElementById("descending"_s));
    auto reverseThree = testPage.getElementById("reverse-three"_s);
    ASSERT_TRUE(descending);
    ASSERT_TRUE(reverseThree);
    auto removed = descending->removeChild(*reverseThree);
    ASSERT_FALSE(removed.hasException());
    testPage.document().updateLayoutIgnorePendingStylesheets();

    EXPECT_EQ(edenListMarkerText(testPage, "reverse-one"_s), "2. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "reverse-two"_s), "1. "_s);

    auto inserted = descending->appendChild(*reverseThree);
    ASSERT_FALSE(inserted.hasException());
    testPage.document().updateLayoutIgnorePendingStylesheets();

    EXPECT_EQ(edenListMarkerText(testPage, "reverse-one"_s), "3. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "reverse-two"_s), "2. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "reverse-three"_s), "1. "_s);

    auto explicitList = testPage.getElementById("explicit"_s);
    ASSERT_TRUE(explicitList);
    explicitList->setAttribute(HTMLNames::startAttr, "3"_s);
    testPage.document().updateLayoutIgnorePendingStylesheets();

    EXPECT_EQ(edenListMarkerText(testPage, "explicit-one"_s), "3. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "explicit-value"_s), "12. "_s);
    EXPECT_EQ(edenListMarkerText(testPage, "explicit-three"_s), "13. "_s);
}

TEST(ListRenumbering, CacheCountTracksRendererTeardownAndAdoption) {
    auto sourcePage = TestPageHarness::create();
    sourcePage.loadHTML("<ol><li id='moving-item'>item</li></ol>"_s);
    sourcePage.document().updateLayoutIgnorePendingStylesheets();
    EXPECT_EQ(edenListMarkerText(sourcePage, "moving-item"_s), "1. "_s);
    EXPECT_GT(sourcePage.document().listItemsWithCachedValues(), 0u);

    auto movingItem = sourcePage.getElementById("moving-item"_s);
    ASSERT_TRUE(movingItem);
    auto destinationPage = TestPageHarness::create();
    destinationPage.loadHTML("<ol id='host'></ol>"_s);
    auto host = destinationPage.getElementById("host"_s);
    ASSERT_TRUE(host);
    auto adopted = destinationPage.document().adoptNode(*movingItem);
    ASSERT_FALSE(adopted.hasException());
    EXPECT_EQ(sourcePage.document().listItemsWithCachedValues(), 0u);
    EXPECT_EQ(destinationPage.document().listItemsWithCachedValues(), 0u);

    auto adoptedItem = adopted.releaseReturnValue();
    auto inserted = host->appendChild(adoptedItem.get());
    ASSERT_FALSE(inserted.hasException());
    destinationPage.document().updateLayoutIgnorePendingStylesheets();
    EXPECT_EQ(edenListMarkerText(destinationPage, "moving-item"_s), "1. "_s);
    EXPECT_GT(destinationPage.document().listItemsWithCachedValues(), 0u);

    auto removed = host->removeChild(adoptedItem.get());
    ASSERT_FALSE(removed.hasException());
    EXPECT_EQ(destinationPage.document().listItemsWithCachedValues(), 0u);
}
