static void edenExpectConnectedRemovalAggregate(bool largeShadowChildFirst) {
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto tree =
        edenMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, largeShadowChildFirst);
    auto *body = testPage.document().body();
    ASSERT_TRUE(body);

    auto appendResult = body->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());
    for (auto *element : tree.elements) {
        EXPECT_TRUE(element->isConnected());
    }
    EXPECT_TRUE(tree.shadowRoot->isConnected());

    auto removalResult = body->removeChild(*tree.root);
    ASSERT_FALSE(removalResult.hasException());
    EXPECT_EQ(tree.elements.size(), removalStepsCount);
    for (auto *element : tree.elements) {
        EXPECT_FALSE(element->isConnected());
    }
    EXPECT_FALSE(tree.shadowRoot->isConnected());

    tree.root = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}

TEST(RemovalAggregate, ConnectedEarlyTerminalStillRunsEveryRemovalStep) {
    edenExpectConnectedRemovalAggregate(true);
}

TEST(RemovalAggregate, ConnectedLateTerminalStillRunsEveryRemovalStep) {
    edenExpectConnectedRemovalAggregate(false);
}

TEST(RemovalAggregate, DisconnectedShadowTreeRunsEveryRemovalStep) {
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto container = HTMLElement::create(HTMLNames::divTag, testPage.document());
    auto tree = edenMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, true);
    auto appendResult = container->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());
    for (auto *element : tree.elements) {
        EXPECT_FALSE(element->isConnected());
    }

    auto removalResult = container->removeChild(*tree.root);
    ASSERT_FALSE(removalResult.hasException());
    EXPECT_EQ(tree.elements.size(), removalStepsCount);
    EXPECT_FALSE(tree.shadowRoot->isConnected());

    tree.root = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}

TEST(RemovalAggregate, ObservableShadowNodeDoesNotSkipLaterCallbacks) {
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto tree = edenMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, true);
    RefPtr heldShadowChild = tree.elements[5];
    auto *body = testPage.document().body();
    ASSERT_TRUE(body);
    auto appendResult = body->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());

    auto removalResult = body->removeChild(*tree.root);
    ASSERT_FALSE(removalResult.hasException());
    EXPECT_EQ(tree.elements.size(), removalStepsCount);

    tree.root = nullptr;
    ASSERT_TRUE(heldShadowChild->parentNode());
    EXPECT_TRUE(heldShadowChild->parentNode()->isShadowRoot());
    heldShadowChild = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}

#if HAVE(OPPORTUNISTIC_TASK_SCHEDULER)
TEST(RemovalAggregate, LargeShadowNodeKeepsSynchronousDeletionChoice) {
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto container = HTMLElement::create(HTMLNames::divTag, testPage.document());
    auto tree = edenMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, true);
    auto appendResult = container->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());

    container->removeChildren();
    EXPECT_EQ(tree.elements.size(), removalStepsCount);
    tree.root = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
    testPage.document().asyncNodeDeletionQueue().deleteNodesNow();
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}

TEST(RemovalAggregate, SmallShadowTreeKeepsDeferredDeletionChoice) {
    unsigned removalStepsCount = 0;
    unsigned destructionCount = 0;
    auto testPage = TestPageHarness::create();
    auto container = HTMLElement::create(HTMLNames::divTag, testPage.document());
    auto tree = edenMakeRemovalAggregateTree(testPage.document(), removalStepsCount, destructionCount, true, false);
    auto appendResult = container->appendChild(*tree.root);
    ASSERT_FALSE(appendResult.hasException());

    container->removeChildren();
    EXPECT_EQ(tree.elements.size(), removalStepsCount);
    tree.root = nullptr;
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(0u, destructionCount);
    testPage.document().asyncNodeDeletionQueue().deleteNodesNow();
    GarbageCollectionController::singleton().garbageCollectNow();
    EXPECT_EQ(tree.elements.size(), destructionCount);
}
#endif
