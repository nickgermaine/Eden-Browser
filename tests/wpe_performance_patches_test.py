from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.performance_patches import (
    DEFAULT_PATCHES,
    DEFAULT_PROFILE_VARIANT,
    EXPERIMENTAL_PATCHES,
    names,
    resolve_patches,
    selections,
)


class PerformancePatchSetTest(unittest.TestCase):
    def test_default_set_is_the_measured_31_patch_configuration(self):
        defaults = names(resolve_patches(False))
        self.assertEqual(31, len(defaults))
        self.assertEqual(len(defaults), len(set(defaults)))
        self.assertEqual(defaults, names(resolve_patches(True)))
        self.assertEqual(EXPERIMENTAL_PATCHES, ())
        self.assertEqual(set(defaults), set(names(DEFAULT_PATCHES)) - {"canvas-replay"})
        self.assertNotIn("canvas-replay", defaults)
        self.assertIn("canvas-thread", defaults)
        self.assertIn("huge-page-alignment", defaults)
        self.assertEqual(
            defaults[-4:],
            ("collection-index", "svg-length-parse", "removal-aggregate", "selector-filter-storage"),
        )

    def test_default_build_selects_its_own_trained_profile(self):
        self.assertEqual(DEFAULT_PROFILE_VARIANT, "default")

    def test_patch_appliers_receive_explicit_selections(self):
        resolved = resolve_patches(True)
        self.assertEqual(selections(resolved, "jsc"), ())
        self.assertEqual(selections(resolved, "engine"), ())
        self.assertEqual(
            selections(resolved, "adopted"),
            (
                "editor_window",
                "scene_completion",
                "helper_priority",
                "jit_pages",
                "text_pages",
                "scaled_font",
                "sparse_paint",
                "jsc_assign",
                "jsc_rest",
                "jsc_regexp",
                "jsc_json_keys",
                "simple_text",
                "text_cache_keys",
                "bulk_teardown",
                "ua_rule_memo",
                "flex_invalidation",
                "text_control",
                "matched_declarations",
                "canvas_thread",
                "process_prewarm",
                "navigation_preconnect",
                "opportunistic_yield",
                "intrinsic_width_probe",
                "intrinsic_width",
                "list_renumbering",
                "author_rule_memo",
                "collection_index",
                "svg_length_parse",
                "removal_aggregate",
                "selector_filter_storage",
            ),
        )


if __name__ == "__main__":
    unittest.main()
