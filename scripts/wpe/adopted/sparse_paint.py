from wpe.patch_support import replace_once


def paint_patch(text):
    text = replace_once(text, '    unsigned dirtyTilesCount = 0;', '    unsigned dirtyTilesCount = 0;\n    uint64_t dirtyArea = 0;')
    text = replace_once(text, '        tileDirtyRectUnion.unite(tile.dirtyRect);', '        tileDirtyRectUnion.unite(tile.dirtyRect);\n        dirtyArea += static_cast<uint64_t>(tile.dirtyRect.width()) * tile.dirtyRect.height();')
    text = replace_once(text, '        auto recording = paintingEngine.record(*layer.owner(), tileDirtyRectUnion, contentsOpaque, contentsScale, dirtyTilesCount);', '''        static unsigned partitionRatio = [] {
            const char* value = getenv("WEBKIT_EDEN_PAINT_PARTITION_RATIO");
            auto ratio = value ? parseInteger<unsigned>(StringView::fromLatin1(value)) : std::optional<unsigned> { 2 };
            return ratio && *ratio <= 64 ? *ratio : 0U;
        }();
        uint64_t unionArea = static_cast<uint64_t>(tileDirtyRectUnion.width()) * tileDirtyRectUnion.height();
        bool partition = partitionRatio && dirtyTilesCount > 1 && unionArea > dirtyArea * partitionRatio;
        RefPtr<SkiaRecordingResult> recording;
        if (!partition) {
            recording = paintingEngine.record(*layer.owner(), tileDirtyRectUnion, contentsOpaque, contentsScale, dirtyTilesCount);
        }
        if (getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
            WTFLogAlways("Eden paint regions: tiles=%u union=%llu dirty=%llu partition=%d", dirtyTilesCount, static_cast<unsigned long long>(unionArea), static_cast<unsigned long long>(dirtyArea), partition);
        }''')
    return replace_once(text, '            auto buffer = paintingEngine.replay(*layer.owner(), recording.copyRef(), tile.rect, tile.dirtyRect);', '''            auto tileRecording = partition ? paintingEngine.record(*layer.owner(), tile.dirtyRect, contentsOpaque, contentsScale, 1) : Ref { *recording };
            auto buffer = paintingEngine.replay(*layer.owner(), WTF::move(tileRecording), tile.rect, tile.dirtyRect);''')


PATCHES = {
    'Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedBackingStoreProxy.cpp': paint_patch,
}
