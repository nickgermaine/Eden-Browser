from pathlib import Path
import os
import subprocess
import sys
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.adopted.removal_aggregate import FILES, PATCHES, SOURCES, container_node_algorithms_patch
from wpe.adopted_patches import render
from wpe.patch_support import without_cmake_comments, without_comments


SOURCE_ROOT = ROOT / "third_party/wpe/source/wpewebkit-2.53.92-combined3"
ALGORITHM_PATH = "Source/WebCore/dom/ContainerNodeAlgorithms.cpp"
TEST_PATH = "Tools/TestWebKitAPI/Tests/WebCore/CachedMatchFinder.cpp"
HEADERS_PATH = "Source/WebCore/Headers.cmake"
ARCHIVE = ROOT / "third_party/wpe/packages/wpewebkit-2.53.92.tar.xz"


class RemovalAggregatePatchTest(unittest.TestCase):
    def test_composed_source_only_guards_the_terminal_predicates(self):
        original = (SOURCE_ROOT / ALGORITHM_PATH).read_text()
        transformed = container_node_algorithms_patch(original)
        rendered = render(ROOT, original, [container_node_algorithms_patch], ALGORITHM_PATH)

        self.assertEqual(transformed.count("AsyncNodeDeletionQueue::canNodeBeDeletedAsync(*currentNode)"), 2)
        self.assertEqual(transformed.count("observabilityOfRemovedNode(*currentNode)"), 2)
        self.assertEqual(transformed.count("if (canDelayNodeDeletion != ContainerNode::CanDelayNodeDeletion::No)"), 2)
        self.assertEqual(
            transformed.count(
                "if (observability != RemovedSubtreeObservability::MaybeObservableByRefPtr)"
            ),
            2,
        )
        self.assertEqual(transformed.count("currentNode->removingSteps("), 2)
        self.assertEqual(transformed.count("++subTreeSize;"), 2)
        document_start = transformed.index("static RemovedSubtreeResult notifyNodeRemovedFromDocument")
        tree_start = transformed.index("static RemovedSubtreeResult notifyNodeRemovedFromTree", document_start)
        public_start = transformed.index("RemovedSubtreeResult notifyChildNodeRemoved", tree_start)
        document_walk = transformed[document_start:tree_start]
        disconnected_walk = transformed[tree_start:public_start]
        for walk in (document_walk, disconnected_walk):
            self.assertEqual(walk.count("currentNode->removingSteps("), 1)
            self.assertEqual(walk.count("++subTreeSize;"), 1)
            self.assertEqual(walk.count("if (RefPtr root = currentNode->shadowRoot())"), 1)
            self.assertEqual(walk.count("subTreeSize += shadowTreeSize;"), 1)
            self.assertIn("updateCanDelayNodeDeletion(canDelayNodeDeletion, canBeDelayed);", walk)
            self.assertIn("updateObservability(observability, newObservability);", walk)
        self.assertEqual(without_comments(rendered), rendered)
        self.assertLess(
            transformed.index("currentNode->removingSteps("),
            transformed.index("if (canDelayNodeDeletion != ContainerNode::CanDelayNodeDeletion::No)"),
        )
        self.assertLess(
            transformed.index("currentNode->removingSteps(", transformed.index("static RemovedSubtreeResult notifyNodeRemovedFromTree")),
            transformed.index("if (canDelayNodeDeletion != ContainerNode::CanDelayNodeDeletion::No)", transformed.index("static RemovedSubtreeResult notifyNodeRemovedFromTree")),
        )
        self.assertEqual(
            without_comments(transformed),
            without_comments(original).replace(
                "        updateCanDelayNodeDeletion(canDelayNodeDeletion, AsyncNodeDeletionQueue::canNodeBeDeletedAsync(*currentNode));\n"
                "        updateObservability(observability, observabilityOfRemovedNode(*currentNode));\n",
                "        if (canDelayNodeDeletion != ContainerNode::CanDelayNodeDeletion::No) {\n"
                "            updateCanDelayNodeDeletion(canDelayNodeDeletion, AsyncNodeDeletionQueue::canNodeBeDeletedAsync(*currentNode));\n"
                "        }\n"
                "        if (observability != RemovedSubtreeObservability::MaybeObservableByRefPtr) {\n"
                "            updateObservability(observability, observabilityOfRemovedNode(*currentNode));\n"
                "        }\n",
            ),
        )
        with self.assertRaises(RuntimeError):
            container_node_algorithms_patch(transformed)

    def test_patch_inventory_and_native_regression_payload(self):
        self.assertEqual(set(PATCHES), {ALGORITHM_PATH, HEADERS_PATH, TEST_PATH})
        with tarfile.open(ARCHIVE) as package:
            headers = package.extractfile("wpewebkit-2.53.92/" + HEADERS_PATH).read().decode()
        exported = render(ROOT, headers, [PATCHES[HEADERS_PATH]], HEADERS_PATH)
        self.assertEqual(exported.count("    dom/AsyncNodeDeletionQueueInlines.h\n"), 1)
        self.assertEqual(exported.count("    dom/ShadowRootInit.h\n"), 1)
        self.assertNotIn("#", exported)
        self.assertEqual(
            exported.replace("    dom/AsyncNodeDeletionQueueInlines.h\n", "").replace("    dom/ShadowRootInit.h\n", ""),
            without_cmake_comments(headers),
        )
        with self.assertRaises(RuntimeError):
            PATCHES[HEADERS_PATH](exported)
        self.assertEqual(
            FILES,
            {
                "Tools/TestWebKitAPI/Tests/WebCore/EdenRemovalAggregateFixture.h": "removal_aggregate_fixture.h",
            },
        )
        self.assertEqual(set(SOURCES), {"removal_aggregate_fixture.h", "removal_aggregate_tests.inc"})

        test_source = (SOURCE_ROOT / TEST_PATH).read_text()
        patched_tests = PATCHES[TEST_PATH](test_source)
        rendered_tests = render(ROOT, test_source, [PATCHES[TEST_PATH]], TEST_PATH)
        self.assertIn('#include "EdenRemovalAggregateFixture.h"', patched_tests)
        self.assertIn("ObservableShadowNodeDoesNotSkipLaterCallbacks", patched_tests)
        self.assertEqual(without_comments(rendered_tests), rendered_tests)
        with self.assertRaises(RuntimeError):
            PATCHES[TEST_PATH](patched_tests)

        tests = (ROOT / "scripts/wpe/adopted/removal_aggregate_tests.inc").read_text()
        fixture = (ROOT / "scripts/wpe/adopted/removal_aggregate_fixture.h").read_text()
        self.assertIn("ConnectedEarlyTerminalStillRunsEveryRemovalStep", tests)
        self.assertIn("ConnectedLateTerminalStillRunsEveryRemovalStep", tests)
        self.assertIn("DisconnectedShadowTreeRunsEveryRemovalStep", tests)
        self.assertIn("ObservableShadowNodeDoesNotSkipLaterCallbacks", tests)
        self.assertIn("LargeShadowNodeKeepsSynchronousDeletionChoice", tests)
        self.assertIn("SmallShadowTreeKeepsDeferredDeletionChoice", tests)
        self.assertIn("tree.elements.size(), removalStepsCount", tests)
        self.assertIn("tree.elements.size(), destructionCount", tests)
        self.assertIn("WebCore::HTMLElement::removingSteps(removalType, oldParentOfRemovedTree)", fixture)
        self.assertIn("++m_removalStepsCount", fixture)
        self.assertIn("++m_destructionCount", fixture)
        self.assertEqual(without_comments(tests), tests)
        self.assertEqual(without_comments(fixture), fixture)

    def test_selected_testwebcore_removal_aggregate_regressions(self):
        binary = os.environ.get("EDEN_TEST_WEBCORE_BINARY")
        if not binary:
            self.skipTest("Set EDEN_TEST_WEBCORE_BINARY to the candidate TestWebCore executable")
        result = subprocess.run(
            [binary, "--gtest_filter=RemovalAggregate.*", "--gtest_brief=0"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        tests = [
            "ConnectedEarlyTerminalStillRunsEveryRemovalStep",
            "ConnectedLateTerminalStillRunsEveryRemovalStep",
            "DisconnectedShadowTreeRunsEveryRemovalStep",
            "ObservableShadowNodeDoesNotSkipLaterCallbacks",
        ]
        if "LargeShadowNodeKeepsSynchronousDeletionChoice" in result.stdout:
            tests.extend(["LargeShadowNodeKeepsSynchronousDeletionChoice", "SmallShadowTreeKeepsDeferredDeletionChoice"])
        for test_name in tests:
            self.assertIn(test_name, result.stdout)
        self.assertIn(f"[  PASSED  ] {len(tests)} tests.", result.stdout)
        self.assertNotIn("[  SKIPPED ]", result.stdout)


if __name__ == "__main__":
    unittest.main()
