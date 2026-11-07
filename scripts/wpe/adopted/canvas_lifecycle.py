from wpe.patch_support import replace_once


GRAPHICS = "Source/WebCore/platform/graphics/"
CANVAS = "Source/WebCore/html/canvas/"


def surface_header(text):
    text = replace_once(text, "#include <wtf/Forward.h>", "#include <wtf/Forward.h>\n#include <optional>")
    text = replace_once(text, "GraphicsContext& context() LIFETIME_BOUND override { return m_context; }",
                        "GraphicsContext& context() LIFETIME_BOUND override { ASSERT(m_context); return *m_context; }")
    return replace_once(text, "    GraphicsContextSkia m_context;", "    std::optional<GraphicsContextSkia> m_context;")


def surface_implementation(text):
    text = replace_once(text,
        "    , m_context(*m_surface->getCanvas(), renderingMode, parameters.purpose)\n{\n    m_context.applyDeviceScaleFactor(parameters.resolutionScale);",
        """{
    if (m_surface) {
        m_context.emplace(*m_surface->getCanvas(), renderingMode, parameters.purpose);
        m_context->applyDeviceScaleFactor(parameters.resolutionScale);
    }""")
    text = replace_once(text, "    return m_surface->imageInfo().minRowBytes64();",
                        "    return calculateBytesPerRow(size());")
    return replace_once(text, "    return true;", "    return !!m_surface;")


def backend_header(text):
    text = replace_once(text, "#include <wtf/RefPtr.h>", "#include <wtf/RefPtr.h>\n#include <wtf/Function.h>")
    return replace_once(text, "    virtual void flushContext() { }", """    virtual void flushContext() { }
    virtual bool isContextLost() const { return false; }
    virtual bool isCanvasThreadForTesting() const { return false; }
    virtual uint64_t synchronousCanvasOperationsForTesting() const { return 0; }
    virtual void setContextLossHandler(Function<void()>&&) { }
    virtual bool loseContextForTesting() { return false; }""")


def image_buffer_header(text):
    text = replace_once(text, "    ImageBufferCreationContext() = default;", """#if USE(SKIA)
    bool canvasThreadEnabled { true };
#endif
    ImageBufferCreationContext() = default;""")
    return replace_once(text, "    WEBCORE_EXPORT virtual void flushDrawingContext();", """    WEBCORE_EXPORT virtual void flushDrawingContext();
    bool isContextLost() const { return m_backend && m_backend->isContextLost(); }
    bool isCanvasThreadForTesting() const { return m_backend && m_backend->isCanvasThreadForTesting(); }
    uint64_t synchronousCanvasOperationsForTesting() const { return m_backend ? m_backend->synchronousCanvasOperationsForTesting() : 0; }
    void setContextLossHandler(Function<void()>&& handler) {
        if (m_backend) {
            m_backend->setContextLossHandler(WTF::move(handler));
        }
    }
    bool loseContextForTesting() { return m_backend && m_backend->loseContextForTesting(); }""")


def delegate_header(text):
    text = replace_once(text, "    virtual bool display(CoordinatedPlatformLayer&, std::optional<Damage>&&) = 0;",
        "    virtual bool display(CoordinatedPlatformLayer&, std::optional<Damage>&&) = 0;\n    virtual void setTargetLayer(CoordinatedPlatformLayer*) { }")
    return replace_once(text, "    void setThreadSafeGrContext(const sk_sp<GrContextThreadSafeProxy>&);",
                        "    virtual void setThreadSafeGrContext(const sk_sp<GrContextThreadSafeProxy>&);")


def graphics_layer(text):
    text = replace_once(text, "GraphicsLayerCoordinated::~GraphicsLayerCoordinated()\n{", """GraphicsLayerCoordinated::~GraphicsLayerCoordinated()
{
    if (m_contentsDisplayDelegate) {
        m_contentsDisplayDelegate->setTargetLayer(nullptr);
    }""")
    text = replace_once(text, "        m_contentsDisplayDelegate = nullptr;", """        if (m_contentsDisplayDelegate) {
            m_contentsDisplayDelegate->setTargetLayer(nullptr);
            m_contentsDisplayDelegate = nullptr;
        }""")
    text = replace_once(text, "    m_contentsDisplayDelegate = WTF::move(delegate);", """    if (m_contentsDisplayDelegate) {
        m_contentsDisplayDelegate->setTargetLayer(nullptr);
    }
    m_contentsDisplayDelegate = WTF::move(delegate);""")
    return replace_once(text, "        m_contentsDisplayDelegate->setThreadSafeGrContext(m_platformLayer->threadSafeGrContext());",
        "        m_contentsDisplayDelegate->setThreadSafeGrContext(m_platformLayer->threadSafeGrContext());\n        m_contentsDisplayDelegate->setTargetLayer(m_platformLayer.ptr());")


def promise_images(text):
    text = replace_once(text, "    if (!image->isTextureBacked())\n        return image;",
                        "    if (!image)\n        return nullptr;\n    if (!image->isTextureBacked())\n        return image;\n    if (!threadSafeGrContext)\n        return nullptr;")
    text = replace_once(text, "    if (!context)\n        return image;", "    if (!context)\n        return nullptr;")
    return replace_once(text, "    ASSERT(backendFormat.isValid());", "    if (!backendFormat.isValid())\n        return nullptr;")


def layer_buffer(text):
    return replace_once(text, "    return makeUnique<CoordinatedPlatformLayerBufferSkiaImage>(WTF::move(skiaImage), flags);",
        "    if (!skiaImage) {\n        return nullptr;\n    }\n    return makeUnique<CoordinatedPlatformLayerBufferSkiaImage>(WTF::move(skiaImage), flags);")


def context_header(text):
    text = replace_once(text, "#include <wtf/Vector.h>", "#include <wtf/Vector.h>\n#include <atomic>")
    text = replace_once(text, "    void reset();", """    void reset();
    bool isContextLost() const;
    bool loseContextForTesting();""")
    text = replace_once(text, "    bool loseContextForTesting();", """    bool loseContextForTesting();
    bool isCanvasThreadForTesting() const;
    uint64_t synchronousCanvasOperationsForTesting() const;
    bool canDispatchContextLossEvents() const { return m_canDispatchContextLossEvents.load(std::memory_order_relaxed); }""")
    return replace_once(text, "    mutable bool m_hasCreatedImageBuffer { false };", """    mutable bool m_hasCreatedImageBuffer { false };
    mutable std::atomic<bool> m_canDispatchContextLossEvents { false };
    bool m_contextLost { false };
    void didLoseContext();""")


def context_implementation(text):
    text = replace_once(text, '#include "CanvasRenderingContext2DBase.h"',
                        '#include "CanvasRenderingContext2DBase.h"\n#include "Event.h"\n#include "EventNames.h"\n#include "ScriptExecutionContext.h"\n#if USE(SKIA)\n#include "ImageBufferSkiaAcceleratedBackend.h"\n#endif')
    text = replace_once(text, "    m_stateStack.first() = State();\n    m_path.clear();",
                        "    m_stateStack.first() = State();\n    m_hasInvertibleTransform = true;\n    m_path.clear();")
    text = replace_once(text, "    return ImageBuffer::create(canvasBase().size(), renderingMode, RenderingPurpose::Canvas, 1, colorSpace(), pixelFormat(), scriptExecutionContext->graphicsClient());", """#if USE(SKIA)
    if (m_settings.disableCanvasThreadForTesting && renderingMode == RenderingMode::Accelerated) {
        ImageBufferCreationContext creationContext;
        creationContext.canvasThreadEnabled = false;
        if (auto buffer = ImageBuffer::create<ImageBufferSkiaAcceleratedBackend>(canvasBase().size(), 1, colorSpace(), ImageBufferFormat { pixelFormat() }, RenderingPurpose::Canvas, creationContext)) {
            return buffer;
        }
    }
#endif
    return ImageBuffer::create(canvasBase().size(), renderingMode, RenderingPurpose::Canvas, 1, colorSpace(), pixelFormat(), scriptExecutionContext->graphicsClient());""")
    text = replace_once(text, "ImageBuffer* CanvasRenderingContext2DBase::buffer() const\n{", """ImageBuffer* CanvasRenderingContext2DBase::buffer() const
{
    if (isContextLost()) {
        return nullptr;
    }""")
    text = replace_once(text, "    if (sizeChanged) {\n        m_hasCreatedImageBuffer = false;",
                        "    if (sizeChanged) {\n        if (!m_contextLost && (!m_buffer || !m_buffer->isContextLost())) {\n            m_canDispatchContextLossEvents.store(false, std::memory_order_relaxed);\n        }\n        m_hasCreatedImageBuffer = false;")
    text = replace_once(text, "    m_buffer = buffer;", """    m_buffer = buffer;
    m_canDispatchContextLossEvents.store(
        m_canDispatchContextLossEvents.load(std::memory_order_relaxed) || buffer->isCanvasThreadForTesting(),
        std::memory_order_relaxed
    );
    if (auto* scriptContext = canvasBase().scriptExecutionContext()) {
        buffer->setContextLossHandler([identifier = scriptContext->identifier(),
            weakThis = WeakPtr { const_cast<CanvasRenderingContext2DBase&>(*this) }]() mutable {
            ScriptExecutionContext::postTaskTo(identifier, [weakThis = WTF::move(weakThis)](auto&) {
                if (RefPtr context = weakThis.get()) {
                    context->didLoseContext();
                }
            });
        });
    }""")
    return replace_once(text, "void CanvasRenderingContext2DBase::reset()", """bool CanvasRenderingContext2DBase::isContextLost() const
{
    return m_contextLost || (m_buffer && m_buffer->isContextLost());
}

bool CanvasRenderingContext2DBase::loseContextForTesting()
{
    RefPtr buffer = this->buffer();
    return buffer && buffer->loseContextForTesting();
}

bool CanvasRenderingContext2DBase::isCanvasThreadForTesting() const
{
    RefPtr buffer = this->buffer();
    return buffer && buffer->isCanvasThreadForTesting();
}

uint64_t CanvasRenderingContext2DBase::synchronousCanvasOperationsForTesting() const
{
    RefPtr buffer = this->buffer();
    return buffer ? buffer->synchronousCanvasOperationsForTesting() : 0;
}

void CanvasRenderingContext2DBase::didLoseContext()
{
    if (m_contextLost) {
        return;
    }
    m_contextLost = true;
    didUpdateCanvasSizeProperties(true);
    m_canDispatchContextLossEvents.store(true, std::memory_order_relaxed);
    if (RefPtr canvasElement = dynamicDowncast<HTMLCanvasElement>(canvasBase())) {
        canvasElement->invalidateStyleAndLayerComposition();
    }
    protect(canvasBase())->queueTaskKeepingObjectAlive(TaskSource::DOMManipulation, [weakThis = WeakPtr { *this }](auto& canvas) {
        RefPtr context = weakThis.get();
        if (!context || !context->m_contextLost) {
            return;
        }
        auto event = Event::create(eventNames().contextlostEvent, Event::CanBubble::No, Event::IsCancelable::Yes);
        canvas.dispatchEvent(event);
        if (event->defaultPrevented()) {
            context->m_canDispatchContextLossEvents.store(false, std::memory_order_relaxed);
            return;
        }
        canvas.queueTaskKeepingObjectAlive(TaskSource::DOMManipulation, [weakThis](auto& canvas) {
            RefPtr context = weakThis.get();
            if (!context || !context->m_contextLost) {
                return;
            }
            context->didUpdateCanvasSizeProperties(true);
            context->m_contextLost = false;
            if (!context->buffer()) {
                context->m_contextLost = true;
                context->m_canDispatchContextLossEvents.store(false, std::memory_order_relaxed);
                return;
            }
            context->didDrawEntireCanvas();
            canvas.dispatchEvent(Event::create(eventNames().contextrestoredEvent, Event::CanBubble::No, Event::IsCancelable::No));
            context->m_canDispatchContextLossEvents.store(false, std::memory_order_relaxed);
        });
    });
}

void CanvasRenderingContext2DBase::reset()""")


def canvas_state_idl(text):
    return replace_once(text, "    undefined reset();", """    boolean isContextLost();
    [EnabledBySetting=DOMTestingAPIsEnabled] boolean loseContextForTesting();
    [EnabledBySetting=DOMTestingAPIsEnabled] boolean isCanvasThreadForTesting();
    [EnabledBySetting=DOMTestingAPIsEnabled] unsigned long long synchronousCanvasOperationsForTesting();
    undefined reset();""")


def event_handlers(text):
    for name in ("contextlost", "contextrestored"):
        text = replace_once(text, chr(47) * 2 + f" attribute EventHandler on{name};", f"attribute EventHandler on{name};")
    return text


def event_names(text):
    return replace_once(text, '    "webglcontextlost": { },',
                        '    "contextlost": { },\n    "contextrestored": { },\n    "webglcontextlost": { },')


def html_attribute_names(text):
    return replace_once(text, "oncontextmenu\n", "oncontextlost\noncontextmenu\noncontextrestored\n")


def pattern(text):
    text = replace_once(text, "#include <skia/core/SkImage.h>",
                        "#include <skia/core/SkImage.h>\n#include <skia/core/SkShader.h>")
    return replace_once(text, "    return platformImage->makeShader(",
                        "    if (!platformImage) {\n        return SkShaders::Color(SK_ColorTRANSPARENT);\n    }\n    return platformImage->makeShader(")


def offscreen_header(text):
    return replace_once(text, "    bool m_hasScheduledCommit { false };",
                        "    bool m_hasScheduledCommit { false };\n    bool m_hasRelevantCanvasEventListener { false };")


def offscreen_implementation(text):
    text = replace_once(text, "bool OffscreenCanvas::virtualHasPendingActivity() const\n{", """bool OffscreenCanvas::virtualHasPendingActivity() const
{
    if (m_hasRelevantCanvasEventListener) {
        SUPPRESS_UNCOUNTED_LOCAL auto* context = dynamicDowncast<OffscreenCanvasRenderingContext2D>(m_context.get());
        if (context && context->canDispatchContextLossEvents()) {
            return true;
        }
    }""")
    return replace_once(text, "void OffscreenCanvas::eventListenersDidChange()\n{", """void OffscreenCanvas::eventListenersDidChange()
{
    m_hasRelevantCanvasEventListener = hasEventListeners(WebCore::eventNames().contextlostEvent)
        || hasEventListeners(WebCore::eventNames().contextrestoredEvent);""")


def settings_header(text):
    return replace_once(text, "    bool desynchronized { false };",
                        "    bool desynchronized { false };\n    bool disableCanvasThreadForTesting { false };")


def settings_idl(text):
    return replace_once(text, "    boolean desynchronized = false;",
                        "    boolean desynchronized = false;\n    [EnabledBySetting=DOMTestingAPIsEnabled] boolean disableCanvasThreadForTesting = false;")


PATCHES = {
    GRAPHICS + "skia/ImageBufferSkiaSurfaceBackend.h": surface_header,
    GRAPHICS + "skia/ImageBufferSkiaSurfaceBackend.cpp": surface_implementation,
    GRAPHICS + "ImageBufferBackend.h": backend_header,
    GRAPHICS + "ImageBuffer.h": image_buffer_header,
    GRAPHICS + "GraphicsLayerContentsDisplayDelegate.h": delegate_header,
    GRAPHICS + "texmap/coordinated/GraphicsLayerCoordinated.cpp": graphics_layer,
    GRAPHICS + "skia/SkiaUtilities.cpp": promise_images,
    GRAPHICS + "skia/PatternSkia.cpp": pattern,
    GRAPHICS + "texmap/coordinated/CoordinatedPlatformLayerBufferSkiaImage.cpp": layer_buffer,
    CANVAS + "CanvasRenderingContext2DBase.h": context_header,
    CANVAS + "CanvasRenderingContext2DBase.cpp": context_implementation,
    CANVAS + "CanvasState.idl": canvas_state_idl,
    CANVAS + "CanvasRenderingContext2DSettings.h": settings_header,
    CANVAS + "CanvasRenderingContext2DSettings.idl": settings_idl,
    "Source/WebCore/html/OffscreenCanvas.idl": event_handlers,
    "Source/WebCore/html/OffscreenCanvas.h": offscreen_header,
    "Source/WebCore/html/OffscreenCanvas.cpp": offscreen_implementation,
    "Source/WebCore/dom/GlobalEventHandlers.idl": event_handlers,
    "Source/WebCore/dom/EventNames.json": event_names,
    "Source/WebCore/html/HTMLAttributeNames.in": html_attribute_names,
}
