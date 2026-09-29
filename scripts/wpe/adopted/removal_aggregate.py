from pathlib import Path

from wpe.patch_support import replace_once


DOM = "Source/WebCore/dom/"

PREDICATE_BLOCK = (
    "        updateCanDelayNodeDeletion(canDelayNodeDeletion, AsyncNodeDeletionQueue::canNodeBeDeletedAsync(*currentNode));\n"
    "        updateObservability(observability, observabilityOfRemovedNode(*currentNode));\n"
)

GUARDED_PREDICATE_BLOCK = (
    "        if (canDelayNodeDeletion != ContainerNode::CanDelayNodeDeletion::No) {\n"
    "            updateCanDelayNodeDeletion(canDelayNodeDeletion, AsyncNodeDeletionQueue::canNodeBeDeletedAsync(*currentNode));\n"
    "        }\n"
    "        if (observability != RemovedSubtreeObservability::MaybeObservableByRefPtr) {\n"
    "            updateObservability(observability, observabilityOfRemovedNode(*currentNode));\n"
    "        }\n"
)


def container_node_algorithms_patch(text):
    if text.count(PREDICATE_BLOCK) != 2:
        raise RuntimeError("Expected the connected and disconnected removal aggregate sites")
    return text.replace(PREDICATE_BLOCK, GUARDED_PREDICATE_BLOCK)


def removal_test_patch(text):
    if "TEST(RemovalAggregate," in text:
        raise RuntimeError("The removal aggregate regression tests are already present")
    text = replace_once(
        text,
        '#include "TestPageHarness.h"',
        '#include "TestPageHarness.h"\n#include "EdenRemovalAggregateFixture.h"\n'
        '#include <WebCore/AsyncNodeDeletionQueueInlines.h>\n'
        '#include <WebCore/ContainerNodeInlines.h>\n'
        '#include <WebCore/HTMLBodyElement.h>\n'
        '#include <WebCore/HTMLNames.h>\n'
        '#include <WebCore/GarbageCollectionController.h>\n'
        '#include <WebCore/ShadowRootInit.h>',
    )
    tests = Path(__file__).with_name("removal_aggregate_tests.inc").read_text()
    return replace_once(text, "TEST(CachedMatchFinder, CountMatchesFindsMatches)", tests + "\nTEST(CachedMatchFinder, CountMatchesFindsMatches)")


def headers_list_patch(text):
    if "    dom/AsyncNodeDeletionQueueInlines.h\n" in text or "    dom/ShadowRootInit.h\n" in text:
        raise RuntimeError("The removal aggregate test headers are already exported")
    text = replace_once(
        text,
        "    dom/AsyncNodeDeletionQueue.h\n",
        "    dom/AsyncNodeDeletionQueue.h\n    dom/AsyncNodeDeletionQueueInlines.h\n",
    )
    return replace_once(text, "    dom/ShadowRoot.h\n", "    dom/ShadowRoot.h\n    dom/ShadowRootInit.h\n")


PATCHES = {
    DOM + "ContainerNodeAlgorithms.cpp": container_node_algorithms_patch,
    "Source/WebCore/Headers.cmake": headers_list_patch,
    "Tools/TestWebKitAPI/Tests/WebCore/CachedMatchFinder.cpp": removal_test_patch,
}

FILES = {
    "Tools/TestWebKitAPI/Tests/WebCore/EdenRemovalAggregateFixture.h": "removal_aggregate_fixture.h",
}

SOURCES = ["removal_aggregate_fixture.h", "removal_aggregate_tests.inc"]
