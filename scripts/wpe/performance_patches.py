from dataclasses import dataclass


@dataclass(frozen=True)
class PerformancePatch:
    name: str
    applier: str
    selections: tuple[str, ...] = ()
    supersedes: tuple[str, ...] = ()


DEFAULT_PATCHES = (
    PerformancePatch("canvas-replay", "canvas-replay"),
    PerformancePatch("huge-page-alignment", "linker"),
    PerformancePatch("editor-window", "adopted", ("editor_window",)),
    PerformancePatch("early-scene-completion", "adopted", ("scene_completion",)),
    PerformancePatch("helper-thread-priority", "adopted", ("helper_priority",)),
    PerformancePatch("jit-huge-page-advice", "adopted", ("jit_pages",)),
    PerformancePatch("text-huge-page-advice", "adopted", ("text_pages",)),
    PerformancePatch("scaled-font-reuse", "adopted", ("scaled_font",)),
    PerformancePatch("sparse-paint-partition", "adopted", ("sparse_paint",)),
    PerformancePatch("jsc-object-assign-clone", "adopted", ("jsc_assign",)),
    PerformancePatch("jsc-object-rest-clone", "adopted", ("jsc_rest",)),
    PerformancePatch("jsc-regexp-alternatives", "adopted", ("jsc_regexp",)),
    PerformancePatch("jsc-json-property-keys", "adopted", ("jsc_json_keys",)),
    PerformancePatch("simple-text", "adopted", ("simple_text",)),
    PerformancePatch("text-cache-key-length", "adopted", ("text_cache_keys",)),
    PerformancePatch("bulk-teardown", "adopted", ("bulk_teardown",)),
    PerformancePatch("ua-rule-memo", "adopted", ("ua_rule_memo",)),
    PerformancePatch("flex-invalidation", "adopted", ("flex_invalidation",)),
    PerformancePatch("text-control", "adopted", ("text_control",)),
    PerformancePatch("matched-declarations", "adopted", ("matched_declarations",)),
    PerformancePatch(
        "canvas-thread",
        "adopted",
        ("canvas_thread",),
        ("canvas-replay",),
    ),
    PerformancePatch("jsc-policy-scale", "eden-runtime"),
    PerformancePatch("process-prewarm", "adopted", ("process_prewarm",)),
    PerformancePatch("navigation-preconnect", "adopted", ("navigation_preconnect",)),
    PerformancePatch("opportunistic-yield", "adopted", ("opportunistic_yield",)),
    PerformancePatch("intrinsic-width", "adopted", ("intrinsic_width_probe", "intrinsic_width")),
    PerformancePatch("list-renumbering", "adopted", ("list_renumbering",)),
    PerformancePatch("author-rule-memo", "adopted", ("author_rule_memo",)),
    PerformancePatch("collection-index", "adopted", ("collection_index",)),
    PerformancePatch("svg-length-parse", "adopted", ("svg_length_parse",)),
    PerformancePatch("removal-aggregate", "adopted", ("removal_aggregate",)),
    PerformancePatch("selector-filter-storage", "adopted", ("selector_filter_storage",)),
)

DEFAULT_PROFILE_VARIANT = "default"


EXPERIMENTAL_PATCHES = ()


def resolve_patches(experiments):
    selected = list(DEFAULT_PATCHES)
    if experiments:
        selected.extend(EXPERIMENTAL_PATCHES)
    superseded = {name for patch in selected for name in patch.supersedes}
    resolved = tuple(patch for patch in selected if patch.name not in superseded)
    names = [patch.name for patch in resolved]
    if len(names) != len(set(names)):
        raise RuntimeError("The performance patch lists contain duplicate names")
    return resolved


def selections(patches, applier):
    return tuple(selection for patch in patches if patch.applier == applier for selection in patch.selections)


def names(patches):
    return tuple(patch.name for patch in patches)
