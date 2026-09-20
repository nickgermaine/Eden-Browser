#pragma once

#include "EdenSkiaCanvasReplay.h"
#include "GLContext.h"
#include "PlatformDisplay.h"
#include <skia/core/SkPixmap.h>

namespace WebCore {
    class SkiaCanvasCpuRaster final : public ThreadSafeRefCounted<SkiaCanvasCpuRaster> {
      public:
        static RefPtr<SkiaCanvasCpuRaster> create(SkSurface &target) {
            static unsigned pixelLimit = [] {
                auto *setting = std::getenv("WEBKIT_EDEN_CANVAS_CPU_PIXELS");
                auto value = setting ? parseInteger<unsigned>(StringView::fromLatin1(setting)) : std::nullopt;
                return value && *value <= 4194304 ? *value : 0U;
            }();
            if (!pixelLimit || static_cast<uint64_t>(target.width()) * target.height() > pixelLimit) {
                return nullptr;
            }
            if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent()) {
                return nullptr;
            }
            auto surface = SkSurfaces::Raster(target.imageInfo(), &target.props());
            SkPixmap pixels;
            if (!surface || !surface->peekPixels(&pixels) || !target.readPixels(pixels, 0, 0)) {
                return nullptr;
            }
            return adoptRef(*new SkiaCanvasCpuRaster(WTF::move(surface)));
        }

        void submit(sk_sp<SkPicture> picture) {
            wait();
            {
                Locker locker{m_lock};
                m_ready = false;
            }
            m_dirty = true;
            static NeverDestroyed<Ref<WorkerPool>> workers(WorkerPool::create("CanvasRaster"_s, 2));
            workers.get()->postTask([self = Ref{*this}, picture = WTF::move(picture)] {
                WTFBeginSignpost(self.ptr(), CanvasCpuRaster);
                auto *canvas = self->m_surface->getCanvas();
                auto saveCount = canvas->getSaveCount();
                canvas->save();
                canvas->resetMatrix();
                picture->playback(canvas);
                canvas->restoreToCount(saveCount);
                WTFEndSignpost(self.ptr(), CanvasCpuRaster);
                Locker locker{self->m_lock};
                self->m_ready = true;
                self->m_condition.notifyOne();
            });
        }

        bool upload(SkSurface &target) {
            wait();
            if (!m_dirty) {
                return true;
            }
            SkPixmap pixels;
            if (!m_surface->peekPixels(&pixels)) {
                return false;
            }
            target.writePixels(pixels, 0, 0);
            m_dirty = false;
            return true;
        }

        void wait() {
            Locker locker{m_lock};
            m_condition.wait(m_lock, [this] { return m_ready; });
        }

      private:
        explicit SkiaCanvasCpuRaster(sk_sp<SkSurface> surface)
            : m_surface(WTF::move(surface)) {}

        sk_sp<SkSurface> m_surface;
        Lock m_lock;
        Condition m_condition;
        bool m_ready{true};
        bool m_dirty{false};
    };
}
