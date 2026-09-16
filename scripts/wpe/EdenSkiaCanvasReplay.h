#pragma once

#include "IntSize.h"

#include <cstdlib>
#include <optional>
#include <utility>

#include <skia/core/SkPaint.h>
#include <skia/core/SkPicture.h>
#include <skia/core/SkSurface.h>
#include <skia/private/chromium/GrDeferredDisplayListRecorder.h>
#include <skia/utils/SkNWayCanvas.h>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WorkerPool.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebCore {

    class SkiaSwitchableCanvas : public SkNWayCanvas {
        WTF_MAKE_TZONE_ALLOCATED(SkiaSwitchableCanvas);

      public:
        explicit SkiaSwitchableCanvas(const IntSize &size)
            : SkNWayCanvas(size.width(), size.height()) {}

        void switchToCanvas(SkCanvas *canvas) {
            SkNWayCanvas::removeAll();
            if (canvas) {
                SkNWayCanvas::addCanvas(canvas);
            }
        }

        virtual bool canReplayOffThread() const {
            return false;
        }
        virtual unsigned recordedDrawCount() const {
            return 0;
        }
        virtual void resetDrawCount() {}
    };

    class SkiaReplayTrackingCanvas final : public SkiaSwitchableCanvas {
        WTF_MAKE_TZONE_ALLOCATED(SkiaReplayTrackingCanvas);

      public:
        using SkiaSwitchableCanvas::SkiaSwitchableCanvas;
        bool canReplayOffThread() const final {
            return m_canReplayOffThread;
        }
        unsigned recordedDrawCount() const final {
            return m_recordedDrawCount;
        }
        void resetDrawCount() final {
            m_recordedDrawCount = 0;
        }

      private:
        void recordPaint(const SkPaint &paint) {
            ++m_recordedDrawCount;
            if (paint.getShader() || paint.getImageFilter()) {
                m_canReplayOffThread = false;
            }
        }

        SaveLayerStrategy getSaveLayerStrategy(const SaveLayerRec &record) final {
            m_canReplayOffThread = false;
            return SkNWayCanvas::getSaveLayerStrategy(record);
        }

        bool onDoSaveBehind(const SkRect *bounds) final {
            m_canReplayOffThread = false;
            return SkNWayCanvas::onDoSaveBehind(bounds);
        }

        void onDrawPaint(const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawPaint(paint);
        }

        void onDrawBehind(const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawBehind(paint);
        }

        void onDrawPoints(PointMode mode, size_t count, const SkPoint points[], const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawPoints(mode, count, points, paint);
        }

        void onDrawRect(const SkRect &rect, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawRect(rect, paint);
        }

        void onDrawRegion(const SkRegion &region, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawRegion(region, paint);
        }

        void onDrawOval(const SkRect &rect, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawOval(rect, paint);
        }

        void onDrawArc(const SkRect &rect, SkScalar start, SkScalar sweep, bool center, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawArc(rect, start, sweep, center, paint);
        }

        void onDrawRRect(const SkRRect &rect, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawRRect(rect, paint);
        }

        void onDrawDRRect(const SkRRect &outer, const SkRRect &inner, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawDRRect(outer, inner, paint);
        }

        void onDrawPath(const SkPath &path, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawPath(path, paint);
        }

        void onDrawTextBlob(const SkTextBlob *blob, SkScalar x, SkScalar y, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawTextBlob(blob, x, y, paint);
        }

        void onDrawGlyphRunList(const sktext::GlyphRunList &glyphs, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawGlyphRunList(glyphs, paint);
        }

        void onDrawPatch(
            const SkPoint cubics[12],
            const SkColor colors[4],
            const SkPoint textures[4],
            SkBlendMode blend,
            const SkPaint &paint
        ) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawPatch(cubics, colors, textures, blend, paint);
        }

        void onDrawVerticesObject(const SkVertices *vertices, SkBlendMode blend, const SkPaint &paint) final {
            recordPaint(paint);
            SkNWayCanvas::onDrawVerticesObject(vertices, blend, paint);
        }

        void onDrawImage2(
            const SkImage *image,
            SkScalar x,
            SkScalar y,
            const SkSamplingOptions &sampling,
            const SkPaint *paint
        ) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onDrawImage2(image, x, y, sampling, paint);
        }

        void onDrawImageRect2(
            const SkImage *image,
            const SkRect &source,
            const SkRect &destination,
            const SkSamplingOptions &sampling,
            const SkPaint *paint,
            SrcRectConstraint constraint
        ) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onDrawImageRect2(image, source, destination, sampling, paint, constraint);
        }

        void onDrawImageLattice2(
            const SkImage *image,
            const Lattice &lattice,
            const SkRect &destination,
            SkFilterMode filter,
            const SkPaint *paint
        ) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onDrawImageLattice2(image, lattice, destination, filter, paint);
        }

        void onDrawAtlas2(
            const SkImage *image,
            const SkRSXform transforms[],
            const SkRect rects[],
            const SkColor colors[],
            int count,
            SkBlendMode blend,
            const SkSamplingOptions &sampling,
            const SkRect *cull,
            const SkPaint *paint
        ) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onDrawAtlas2(image, transforms, rects, colors, count, blend, sampling, cull, paint);
        }

        void onDrawEdgeAAImageSet2(
            const ImageSetEntry images[],
            int count,
            const SkPoint clips[],
            const SkMatrix matrices[],
            const SkSamplingOptions &sampling,
            const SkPaint *paint,
            SrcRectConstraint constraint
        ) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onDrawEdgeAAImageSet2(images, count, clips, matrices, sampling, paint, constraint);
        }

        void onDrawPicture(const SkPicture *picture, const SkMatrix *matrix, const SkPaint *paint) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onDrawPicture(picture, matrix, paint);
        }

        void onDrawDrawable(SkDrawable *drawable, const SkMatrix *matrix) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onDrawDrawable(drawable, matrix);
        }

        void onDrawSlug(const sktext::gpu::Slug *slug, const SkPaint &paint) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onDrawSlug(slug, paint);
        }

        void onDrawShadowRec(const SkPath &path, const SkDrawShadowRec &shadow) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onDrawShadowRec(path, shadow);
        }

        void onClipShader(sk_sp<SkShader> shader, SkClipOp operation) final {
            m_canReplayOffThread = false;
            SkNWayCanvas::onClipShader(WTF::move(shader), operation);
        }

        unsigned m_recordedDrawCount{0};
        bool m_canReplayOffThread{true};
    };

    class SkiaCanvasReplayTask final : public ThreadSafeRefCounted<SkiaCanvasReplayTask> {
        WTF_MAKE_TZONE_ALLOCATED(SkiaCanvasReplayTask);

      public:
        static Ref<SkiaCanvasReplayTask>
        create(sk_sp<SkPicture> picture, const GrSurfaceCharacterization &characterization) {
            return adoptRef(*new SkiaCanvasReplayTask(WTF::move(picture), characterization));
        }

        void prepare() {
            WTFBeginSignpost(this, CanvasReplayPrepare);
            sk_sp<GrDeferredDisplayList> list;
            {
                GrDeferredDisplayListRecorder recorder(m_characterization);
                if (auto *canvas = recorder.getCanvas()) {
                    m_picture->playback(canvas);
                    list = recorder.detach();
                }
            }
            WTFEndSignpost(this, CanvasReplayPrepare);
            Locker locker{m_lock};
            m_list = WTF::move(list);
            m_ready = true;
            m_condition.notifyOne();
        }

        void wait() {
            WTFBeginSignpost(this, CanvasReplayWait);
            Locker locker{m_lock};
            m_condition.wait(m_lock, [this] { return m_ready; });
            WTFEndSignpost(this, CanvasReplayWait);
        }

        void replay(SkSurface &surface) {
            wait();
            WTFBeginSignpost(this, CanvasReplaySubmit);
            auto list = std::exchange(m_list, nullptr);
            if (list) {
                surface.notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
            }
            if (!list || !skgpu::ganesh::DrawDDL(&surface, list)) {
                auto *canvas = surface.getCanvas();
                auto saveCount = canvas->getSaveCount();
                canvas->save();
                canvas->resetMatrix();
                m_picture->playback(canvas);
                canvas->restoreToCount(saveCount);
            }
            WTFEndSignpost(this, CanvasReplaySubmit);
        }

      private:
        SkiaCanvasReplayTask(sk_sp<SkPicture> picture, const GrSurfaceCharacterization &characterization)
            : m_picture(WTF::move(picture)),
              m_characterization(characterization) {}

        sk_sp<SkPicture> m_picture;
        GrSurfaceCharacterization m_characterization;
        Lock m_lock;
        Condition m_condition;
        sk_sp<GrDeferredDisplayList> m_list;
        bool m_ready{false};
    };

    class SkiaCanvasReplay final {
        WTF_MAKE_TZONE_ALLOCATED(SkiaCanvasReplay);

      public:
        static unsigned workerCount() {
            static unsigned count = [] {
                const char *setting = getenv("WEBKIT_SKIA_CANVAS_REPLAY_THREADS");
                if (!setting) {
                    return 0U;
                }
                auto value = parseInteger<unsigned>(StringView::fromLatin1(setting));
                return value && *value <= 4 ? *value : 0U;
            }();
            return count;
        }

        static unsigned batchSize() {
            static unsigned count = [] {
                const char *setting = getenv("WEBKIT_SKIA_CANVAS_REPLAY_BATCH");
                auto value = setting ? parseInteger<unsigned>(StringView::fromLatin1(setting)) : std::nullopt;
                return value && *value >= 16 && *value <= 4096 ? *value : 256U;
            }();
            return count;
        }

        explicit SkiaCanvasReplay(GrSurfaceCharacterization characterization)
            : m_characterization(WTF::move(characterization)) {}

        ~SkiaCanvasReplay() {
            for (auto &task : m_pending) {
                task->wait();
            }
        }

        void submit(sk_sp<SkPicture> picture, SkSurface &surface) {
            if (m_pending.size() >= workerCount() * 2) {
                m_pending.takeFirst()->replay(surface);
            }
            auto task = SkiaCanvasReplayTask::create(WTF::move(picture), m_characterization);
            m_pending.append(task.copyRef());
            static NeverDestroyed<Ref<WorkerPool>> workers(WorkerPool::create("CanvasDDL"_s, workerCount()));
            workers.get()->postTask([task = WTF::move(task)] { task->prepare(); });
            if (getenv("WEBKIT_SKIA_CANVAS_REPLAY_TRACE")) {
                WTFLogAlways("Eden canvas replay: submitted batch");
            }
        }

        void drain(SkSurface &surface) {
            while (!m_pending.isEmpty()) {
                m_pending.takeFirst()->replay(surface);
            }
        }

      private:
        GrSurfaceCharacterization m_characterization;
        Deque<Ref<SkiaCanvasReplayTask>> m_pending;
    };
}
