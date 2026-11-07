#pragma once

#include "CoordinatedPlatformLayer.h"
#include "CoordinatedPlatformLayerBufferNativeImage.h"
#include "CoordinatedPlatformLayerBufferSkiaImage.h"
#include "FontRenderOptions.h"
#include "GLContext.h"
#include "GraphicsLayerContentsDisplayDelegate.h"
#include "NativeImage.h"
#include "PlatformDisplay.h"
#include "SkiaUtilities.h"

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkPicture.h>
#include <skia/core/SkSurface.h>
#include <skia/gpu/ganesh/GrContextThreadSafeProxy.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#include <wtf/Condition.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SharedTask.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/Vector.h>
#include <wtf/WorkQueue.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace WebCore {

    static bool edenCanvasThreadEnabled() {
        static const bool enabled = [] {
            const char *value = std::getenv("WEBKIT_EDEN_DISABLE_CANVAS_THREAD");
            return !value || !*value || !std::strcmp(value, "0");
        }();
        return enabled;
    }

    class EdenCanvasPresentation final : public ThreadSafeRefCounted<EdenCanvasPresentation> {
      public:
        struct Target {
            sk_sp<GrContextThreadSafeProxy> context;
            uint64_t generation;
        };
        using RefreshTask = SharedTask<void(Target, Ref<EdenCanvasPresentation>)>;

        static Ref<EdenCanvasPresentation> create() {
            return adoptRef(*new EdenCanvasPresentation);
        }

        void invalidate() {
            m_valid.store(false, std::memory_order_release);
            {
                Locker locker{m_lock};
                m_pendingBuffer = nullptr;
                m_refreshTask = nullptr;
            }
            callOnMainThread([protectedThis = Ref{*this}] { protectedThis->m_layer = nullptr; });
        }

        void setTargetLayer(CoordinatedPlatformLayer *layer) {
            ASSERT(isMainThread());
            m_layer = layer;
        }

        void setContext(const sk_sp<GrContextThreadSafeProxy> &context) {
            RefPtr<RefreshTask> refresh;
            uint64_t generation;
            {
                Locker locker{m_lock};
                if (m_context == context) {
                    return;
                }
                m_context = context;
                generation = ++m_contextGeneration;
                m_pendingBuffer = nullptr;
                refresh = m_refreshTask;
            }
            if (refresh && m_valid.load(std::memory_order_acquire)) {
                refresh->run({context, generation}, Ref{*this});
            }
        }

        Target target() const {
            Locker locker{m_lock};
            return {m_context, m_contextGeneration};
        }

        void setRefreshTask(Ref<RefreshTask> &&task) {
            Locker locker{m_lock};
            m_refreshTask = WTF::move(task);
        }

        void enqueue(std::unique_ptr<CoordinatedPlatformLayerBuffer> &&buffer, uint64_t generation = 0) {
            {
                Locker locker{m_lock};
                if (!m_valid.load(std::memory_order_acquire) || (generation && generation != m_contextGeneration)) {
                    return;
                }
                m_pendingBuffer = WTF::move(buffer);
                if (m_deliveryScheduled) {
                    return;
                }
                m_deliveryScheduled = true;
            }
            callOnMainThread([protectedThis = Ref{*this}] { protectedThis->deliver(); });
        }

        bool display(CoordinatedPlatformLayer &layer, std::optional<Damage> &&damage) {
            ASSERT(isMainThread());
            assertIsHeld(layer.lock());
            if (auto buffer = takeBuffer()) {
                layer.setContentsBuffer(WTF::move(buffer), WTF::move(damage));
            }
            return true;
        }

      private:
        EdenCanvasPresentation() = default;

        std::unique_ptr<CoordinatedPlatformLayerBuffer> takeBuffer() {
            Locker locker{m_lock};
            if (!m_valid.load(std::memory_order_acquire)) {
                m_pendingBuffer = nullptr;
            }
            return WTF::move(m_pendingBuffer);
        }

        void deliver() {
            ASSERT(isMainThread());
            {
                Locker locker{m_lock};
                m_deliveryScheduled = false;
            }
            if (!m_valid.load(std::memory_order_acquire)) {
                return;
            }
            if (auto layer = m_layer.get()) {
                auto buffer = takeBuffer();
                if (buffer) {
                    Locker locker{layer->lock()};
                    layer->setContentsBuffer(WTF::move(buffer));
                }
            }
        }

        mutable Lock m_lock;
        sk_sp<GrContextThreadSafeProxy> m_context WTF_GUARDED_BY_LOCK(m_lock);
        RefPtr<RefreshTask> m_refreshTask WTF_GUARDED_BY_LOCK(m_lock);
        uint64_t m_contextGeneration WTF_GUARDED_BY_LOCK(m_lock){1};
        std::unique_ptr<CoordinatedPlatformLayerBuffer> m_pendingBuffer WTF_GUARDED_BY_LOCK(m_lock);
        bool m_deliveryScheduled WTF_GUARDED_BY_LOCK(m_lock){false};
        std::atomic<bool> m_valid{true};
        ThreadSafeWeakPtr<CoordinatedPlatformLayer> m_layer;
    };

    class EdenCanvasDisplayDelegate final : public GraphicsLayerContentsDisplayDelegate {
      public:
        static Ref<EdenCanvasDisplayDelegate> create(Ref<EdenCanvasPresentation> &&presentation) {
            return adoptRef(*new EdenCanvasDisplayDelegate(WTF::move(presentation)));
        }

        void setTargetLayer(CoordinatedPlatformLayer *layer) final {
            m_presentation->setTargetLayer(layer);
        }

        void setThreadSafeGrContext(const sk_sp<GrContextThreadSafeProxy> &context) final {
            m_presentation->setContext(context);
        }

        void setDisplayBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer> &&buffer) final {
            m_presentation->enqueue(WTF::move(buffer));
        }

        bool display(CoordinatedPlatformLayer &layer, std::optional<Damage> &&damage) final {
            return m_presentation->display(layer, WTF::move(damage));
        }

      private:
        explicit EdenCanvasDisplayDelegate(Ref<EdenCanvasPresentation> &&presentation)
            : m_presentation(WTF::move(presentation)) {}

        Ref<EdenCanvasPresentation> m_presentation;
    };

    class EdenCanvasGPUState final : public ThreadSafeRefCounted<EdenCanvasGPUState> {
      public:
        static RefPtr<EdenCanvasGPUState>
        create(const SkImageInfo &imageInfo, const SkSurfaceProps &properties, bool enableDynamicMSAA) {
            if (contextFailed().load(std::memory_order_acquire)) {
                return nullptr;
            }
            auto state = adoptRef(*new EdenCanvasGPUState(imageInfo, properties, enableDynamicMSAA));
            state->m_queue->dispatchSync([state = state.copyRef()] { state->initialize(); });
            if (!state->m_surface) {
                state->shutdown();
                return nullptr;
            }
            return state.ptr();
        }

        ~EdenCanvasGPUState() {
            ASSERT(!m_surface);
            ASSERT(!m_lossHandler);
        }

        const sk_sp<GrContextThreadSafeProxy> &threadSafeProxy() const {
            return m_threadSafeProxy;
        }

        Ref<WorkQueue> gpuQueue() const {
            return m_queue.copyRef();
        }

        GrDirectContext *contextForImageTransfer() {
            ASSERT(m_queue->isCurrent());
            if (m_stopped.load(std::memory_order_acquire)) {
                if (isLost() || contextFailed().load(std::memory_order_acquire)) {
                    return nullptr;
                }
                auto &display = PlatformDisplay::sharedDisplay();
                auto *glContext = display.skiaGLContext();
                if (!glContext || !glContext->makeContextCurrent()) {
                    return nullptr;
                }
                auto *grContext = display.skiaGrContext();
                return grContext && !grContext->abandoned() ? grContext : nullptr;
            }
            return makeCurrent() ? m_grContext : nullptr;
        }

        bool isLost() const {
            return m_lost.load(std::memory_order_acquire);
        }

        void forceLossForTesting() {
            m_queue->dispatchSync([this] {
                if (m_grContext) {
                    m_grContext->abandonContext();
                }
                markLost();
            });
        }

        void setLossHandler(Function<void()> &&handler) {
            m_queue->dispatchSync([this, handler = WTF::move(handler)]() mutable {
                if (isLost()) {
                    handler();
                } else {
                    m_lossHandler = WTF::move(handler);
                }
            });
        }

        void submit(
            sk_sp<SkPicture> &&picture,
            EdenCanvasPresentation::Target &&target,
            Ref<EdenCanvasPresentation> &&presentation
        ) {
            auto &budget = queueBudget();
            auto queuedAt = MonotonicTime::now();
            size_t bytes = picture ? picture->approximateBytesUsed() : 0;
            {
                Locker locker{budget.lock};
                if (m_stopped.load(std::memory_order_acquire)) {
                    return;
                }
                if (m_pendingCount >= maximumPendingFrames || !budget.canAccept(bytes)) {
                    ++m_backpressureCount;
                    auto waitStarted = MonotonicTime::now();
                    budget.condition.wait(budget.lock, [&] {
                        return m_stopped.load(std::memory_order_acquire) ||
                               (m_pendingCount < maximumPendingFrames && budget.canAccept(bytes));
                    });
                    m_backpressureMicroseconds += elapsedMicroseconds(waitStarted);
                }
                if (m_stopped.load(std::memory_order_acquire)) {
                    return;
                }
                ++m_pendingCount;
                ++budget.count;
                budget.bytes += bytes;
                m_peakPendingCount = std::max(m_peakPendingCount, m_pendingCount);
            }
            queuedAt = MonotonicTime::now();
            m_queue->dispatch([state = Ref{*this},
                               picture = WTF::move(picture),
                               target = WTF::move(target),
                               presentation = WTF::move(presentation),
                               queuedAt,
                               bytes]() mutable {
                state->m_queueMicroseconds += elapsedMicroseconds(queuedAt);
                ++state->m_submittedCount;
                if (state->makeCurrent()) {
                    state->replay(picture);
                    state->m_grContext->flush(state->m_surface.get());
                    auto image = state->m_surface->makeImageSnapshot();
                    std::unique_ptr<CoordinatedPlatformLayerBuffer> buffer;
                    if (image && target.context) {
                        auto promiseImage = SkiaUtilities::createPromiseImageIfNeeded(image, target.context);
                        if (promiseImage) {
                            image = WTF::move(promiseImage);
                            OptionSet<TextureMapperFlags> flags;
                            if (!image->isOpaque()) {
                                flags.add(TextureMapperFlags::ShouldBlend);
                            }
                            buffer = makeUnique<CoordinatedPlatformLayerBufferSkiaImage>(WTF::move(image), flags);
                        }
                    }
                    if (!buffer && image && !state->m_grContext->abandoned()) {
                        auto rasterImage = image->makeRasterImage(state->m_grContext);
                        if (rasterImage) {
                            if (auto nativeImage = NativeImage::create(WTF::move(rasterImage), nullptr)) {
                                buffer = CoordinatedPlatformLayerBufferNativeImage::create(
                                    nativeImage.releaseNonNull(),
                                    nullptr
                                );
                            }
                        }
                    }
                    if (buffer) {
                        presentation->enqueue(WTF::move(buffer), target.generation);
                    } else if (state->m_grContext->abandoned()) {
                        state->markLost();
                    }
                }
                picture = nullptr;
                auto &budget = queueBudget();
                Locker locker{budget.lock};
                --state->m_pendingCount;
                --budget.count;
                budget.bytes -= bytes;
                budget.condition.notifyAll();
            });
        }

        bool executeSync(sk_sp<SkPicture> &&picture, Function<bool(SkSurface &, GrDirectContext &)> &&operation) {
            auto started = MonotonicTime::now();
            bool succeeded = false;
            m_queue->dispatchSync([&, picture = WTF::move(picture), operation = WTF::move(operation)]() mutable {
                if (makeCurrent()) {
                    replay(picture);
                    succeeded = operation(*m_surface, *m_grContext) && !m_grContext->abandoned();
                }
                ++m_syncCount;
                m_syncMicroseconds += elapsedMicroseconds(started);
            });
            return succeeded;
        }

        uint64_t synchronousOperationsForTesting() const {
            uint64_t count = 0;
            m_queue->dispatchSync([&] { count = m_syncCount; });
            return count;
        }

        void shutdown() {
            m_stopped.store(true, std::memory_order_release);
            {
                auto &budget = queueBudget();
                Locker locker{budget.lock};
                budget.condition.notifyAll();
            }
            m_queue->dispatchSync([this] {
                liveStates().remove(this);
                m_lossHandler = nullptr;
                m_surface = nullptr;
                if (m_grContext && !m_grContext->abandoned()) {
                    if (auto *context = PlatformDisplay::sharedDisplay().skiaGLContext();
                        context && context->makeContextCurrent()) {
                        m_grContext->performDeferredCleanup(std::chrono::milliseconds(0));
                    }
                }
                m_grContext = nullptr;
                if (traceEnabled()) {
                    auto &budget = queueBudget();
                    Locker locker{budget.lock};
                    WTFLogAlways(
                        "Eden canvas GPU: frames=%llu peakPending=%u queueUs=%llu backpressure=%llu "
                        "backpressureUs=%llu "
                        "sync=%llu syncUs=%llu lost=%d",
                        static_cast<unsigned long long>(m_submittedCount),
                        m_peakPendingCount,
                        static_cast<unsigned long long>(m_queueMicroseconds),
                        static_cast<unsigned long long>(m_backpressureCount),
                        static_cast<unsigned long long>(m_backpressureMicroseconds),
                        static_cast<unsigned long long>(m_syncCount),
                        static_cast<unsigned long long>(m_syncMicroseconds),
                        isLost()
                    );
                }
            });
        }

      private:
        struct QueueBudget {
            bool canAccept(size_t additionalBytes) const {
                return count < maximumFrameCount &&
                       (!count || (additionalBytes <= byteLimit && bytes <= byteLimit - additionalBytes));
            }
            static constexpr size_t byteLimit = 64 * 1024 * 1024;
            static constexpr unsigned maximumFrameCount = 3;
            Lock lock;
            Condition condition;
            unsigned count{0};
            size_t bytes{0};
        };

        static QueueBudget &queueBudget() {
            static NeverDestroyed<QueueBudget> budget;
            return budget.get();
        }

        static std::atomic<bool> &contextFailed() {
            static std::atomic<bool> failed{false};
            return failed;
        }

        static HashSet<EdenCanvasGPUState *> &liveStates() {
            static NeverDestroyed<HashSet<EdenCanvasGPUState *>> states;
            return states.get();
        }

        static uint64_t elapsedMicroseconds(MonotonicTime start) {
            return static_cast<uint64_t>((MonotonicTime::now() - start).microseconds());
        }

        static bool traceEnabled() {
            static const bool enabled = [] {
                const char *value = std::getenv("WEBKIT_EDEN_CANVAS_TRACE");
                return value && !std::strcmp(value, "1");
            }();
            return enabled;
        }

        static Ref<WorkQueue> queue() {
            static NeverDestroyed<Ref<WorkQueue>> queue(WorkQueue::create("EdenCanvasGPU"_s));
            return queue.get().copyRef();
        }

        EdenCanvasGPUState(const SkImageInfo &imageInfo, const SkSurfaceProps &properties, bool enableDynamicMSAA)
            : m_queue(queue()),
              m_imageInfo(imageInfo),
              m_properties(properties),
              m_enableDynamicMSAA(enableDynamicMSAA) {}

        void initialize() {
            auto &display = PlatformDisplay::sharedDisplay();
            auto *glContext = display.skiaGLContext();
            if (!glContext || !glContext->makeContextCurrent()) {
                contextFailed().store(true, std::memory_order_release);
                return;
            }
            m_grContext = display.skiaGrContext();
            if (!m_grContext || m_grContext->abandoned()) {
                contextFailed().store(true, std::memory_order_release);
                return;
            }
            m_grContext->setResourceCacheLimit(64 * 1024 * 1024);
            auto sampleCount = display.msaaSampleCount();
            auto properties = m_properties;
            if (sampleCount && m_enableDynamicMSAA) {
                properties = SkSurfaceProps(
                    properties.flags() | SkSurfaceProps::kDynamicMSAA_Flag,
                    properties.pixelGeometry(),
                    properties.textContrast(),
                    properties.textGamma()
                );
                sampleCount = 1;
            }
            m_surface = SkSurfaces::RenderTarget(
                m_grContext,
                skgpu::Budgeted::kYes,
                m_imageInfo,
                sampleCount,
                kTopLeft_GrSurfaceOrigin,
                &properties
            );
            if (!m_surface) {
                return;
            }
            m_surface->getCanvas()->clear(SK_ColorTRANSPARENT);
            m_threadSafeProxy = m_grContext->threadSafeProxy();
            if (!m_threadSafeProxy) {
                m_surface = nullptr;
            } else {
                liveStates().add(this);
            }
        }

        void markLost() {
            contextFailed().store(true, std::memory_order_release);
            for (auto *state : liveStates()) {
                if (!state->m_lost.exchange(true, std::memory_order_acq_rel)) {
                    if (auto handler = std::exchange(state->m_lossHandler, nullptr)) {
                        handler();
                    }
                }
            }
        }

        bool makeCurrent() {
            if (m_stopped.load(std::memory_order_acquire) || isLost()) {
                return false;
            }
            if (!contextFailed().load(std::memory_order_acquire) && m_surface && m_grContext &&
                !m_grContext->abandoned()) {
                auto *context = PlatformDisplay::sharedDisplay().skiaGLContext();
                if (context && context->makeContextCurrent()) {
                    return true;
                }
            }
            markLost();
            return false;
        }

        void replay(const sk_sp<SkPicture> &picture) {
            if (!picture) {
                return;
            }
            auto *canvas = m_surface->getCanvas();
            auto saveCount = canvas->getSaveCount();
            canvas->save();
            canvas->resetMatrix();
            picture->playback(canvas);
            canvas->restoreToCount(saveCount);
        }

        static constexpr unsigned maximumPendingFrames = 3;
        Ref<WorkQueue> m_queue;
        SkImageInfo m_imageInfo;
        SkSurfaceProps m_properties;
        bool m_enableDynamicMSAA;
        sk_sp<SkSurface> m_surface;
        GrDirectContext *m_grContext{nullptr};
        sk_sp<GrContextThreadSafeProxy> m_threadSafeProxy;
        Function<void()> m_lossHandler;
        std::atomic<bool> m_lost{false};
        std::atomic<bool> m_stopped{false};
        unsigned m_pendingCount{0};
        unsigned m_peakPendingCount{0};
        uint64_t m_submittedCount{0};
        uint64_t m_queueMicroseconds{0};
        uint64_t m_backpressureCount{0};
        uint64_t m_backpressureMicroseconds{0};
        uint64_t m_syncCount{0};
        uint64_t m_syncMicroseconds{0};
    };

    class EdenCanvasImageProvider final : public NativeImageSkiaImageProvider {
      public:
        EdenCanvasImageProvider(
            Ref<EdenCanvasGPUState> &&state,
            sk_sp<SkImage> image,
            sk_sp<GrContextThreadSafeProxy> sourceContext
        )
            : m_queue(state->gpuQueue()),
              m_state(WTF::move(state)),
              m_image(WTF::move(image)),
              m_sourceContext(WTF::move(sourceContext)) {}

        ~EdenCanvasImageProvider() final {
            if (m_queue->isCurrent()) {
                return;
            }
            auto image = WTF::move(m_image);
            auto images = WTF::move(m_images);
            auto raster = WTF::move(m_rasterImage);
            m_queue->dispatch(
                [image = WTF::move(image), images = WTF::move(images), raster = WTF::move(raster)]() mutable {
                    image = nullptr;
                    images.clear();
                    raster = nullptr;
                }
            );
        }

        sk_sp<SkImage> rasterImage() const final {
            if (!m_image) {
                return nullptr;
            }
            if (!m_image->isTextureBacked()) {
                return m_image;
            }
            {
                Locker locker{m_lock};
                if (m_rasterImage) {
                    return m_rasterImage;
                }
            }
            sk_sp<SkImage> image;
            auto createImage = [&] {
                auto *grContext = m_state->contextForImageTransfer();
                if (grContext && !grContext->abandoned()) {
                    image = m_image->makeRasterImage(grContext);
                }
            };
            if (m_queue->isCurrent()) {
                createImage();
            } else {
                m_queue->dispatchSync(WTF::move(createImage));
            }
            if (!image) {
                return nullptr;
            }
            Locker locker{m_lock};
            if (!m_rasterImage) {
                m_rasterImage = WTF::move(image);
            }
            return m_rasterImage;
        }

        sk_sp<SkImage> imageForThreadSafeContext(const sk_sp<GrContextThreadSafeProxy> &context) const final {
            if (!context || !m_image) {
                return nullptr;
            }
            if (context.get() == m_sourceContext.get()) {
                return m_image;
            }
            {
                Locker locker{m_lock};
                for (const auto &entry : m_images) {
                    if (entry.first.get() == context.get()) {
                        return entry.second;
                    }
                }
            }
            sk_sp<SkImage> image;
            auto createImage = [&] {
                auto *grContext = m_state->contextForImageTransfer();
                if (!grContext || grContext->abandoned()) {
                    return;
                }
                if (grContext->threadSafeProxy().get() == context.get()) {
                    image = m_image;
                    return;
                }
                image = SkiaUtilities::createPromiseImageIfNeeded(m_image, context);
                if (!image) {
                    image = m_image->makeRasterImage(grContext);
                }
            };
            if (m_queue->isCurrent()) {
                createImage();
            } else {
                m_queue->dispatchSync(WTF::move(createImage));
            }
            if (!image) {
                return nullptr;
            }
            Locker locker{m_lock};
            for (const auto &entry : m_images) {
                if (entry.first.get() == context.get()) {
                    return entry.second;
                }
            }
            m_images.append({context, image});
            return image;
        }

      private:
        Ref<WorkQueue> m_queue;
        Ref<EdenCanvasGPUState> m_state;
        sk_sp<SkImage> m_image;
        sk_sp<GrContextThreadSafeProxy> m_sourceContext;
        mutable Lock m_lock;
        mutable Vector<std::pair<sk_sp<GrContextThreadSafeProxy>, sk_sp<SkImage>>> m_images;
        mutable sk_sp<SkImage> m_rasterImage;
    };

}
