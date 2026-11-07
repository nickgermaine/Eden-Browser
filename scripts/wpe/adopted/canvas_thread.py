from wpe.patch_support import replace_once
from wpe.adopted.canvas_lifecycle import PATCHES as LIFECYCLE_PATCHES


SKIA = "Source/WebCore/platform/graphics/skia/"


def image_buffer_backend_patch(text):
    text = LIFECYCLE_PATCHES["Source/WebCore/platform/graphics/ImageBufferBackend.h"](text)
    return replace_once(
        text,
        "    virtual void flushContext() { }\n",
        "    virtual void flushContext() { }\n"
        "    virtual bool preparesForDisplayWithoutFlushing() const { return false; }\n",
    )


def image_buffer_patch(text):
    return replace_once(
        text,
        "void ImageBuffer::prepareForDisplay()\n{\n    flushDrawingContextAsync();\n",
        "void ImageBuffer::prepareForDisplay()\n{\n"
        "    if (!m_backend || !m_backend->preparesForDisplayWithoutFlushing())\n"
        "        flushDrawingContextAsync();\n",
    )


def image_buffer_header_patch(text):
    text = LIFECYCLE_PATCHES["Source/WebCore/platform/graphics/ImageBuffer.h"](text)
    return replace_once(
        text,
        "    WEBCORE_EXPORT virtual bool flushDrawingContextAsync();",
        "    bool preparesForDisplayWithoutFlushing() const { return m_backend && m_backend->preparesForDisplayWithoutFlushing(); }\n"
        "    WEBCORE_EXPORT virtual bool flushDrawingContextAsync();",
    )


def canvas_context_patch(text):
    text = LIFECYCLE_PATCHES["Source/WebCore/html/canvas/CanvasRenderingContext2DBase.cpp"](text)
    return replace_once(
        text,
        "    if (RefPtr buffer = this->buffer())\n        buffer->flushDrawingContextAsync();\n",
        "    if (RefPtr buffer = this->buffer(); buffer && !buffer->preparesForDisplayWithoutFlushing())\n"
        "        buffer->flushDrawingContextAsync();\n",
    )


def backend_patch(text):
    text = replace_once(
        text,
        "    auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();",
        """    auto imageInfo = SkImageInfo::Make(backendSize.width(), backendSize.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, parameters.colorSpace.platformColorSpace());
#if USE(COORDINATED_GRAPHICS)
    if (parameters.purpose == RenderingPurpose::Canvas && creationContext.canvasThreadEnabled && edenCanvasThreadEnabled()) {
        auto properties = FontRenderOptions::singleton().createSurfaceProps();
        if (auto canvasThread = EdenCanvasGPUState::create(imageInfo, properties, shouldEnableDynamicMSAA())) {
            return std::unique_ptr<ImageBufferSkiaAcceleratedBackend>(new ImageBufferSkiaAcceleratedBackend(parameters, nullptr, WTF::move(canvasThread)));
        }
    }
#endif
    auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();""",
    )
    text = replace_once(
        text,
        "    auto imageInfo = SkImageInfo::Make(backendSize.width(), backendSize.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, parameters.colorSpace.platformColorSpace());\n    auto msaaSampleCount = PlatformDisplay::sharedDisplay().msaaSampleCount();",
        "    auto msaaSampleCount = PlatformDisplay::sharedDisplay().msaaSampleCount();",
    )
    text = replace_once(
        text,
        "ImageBufferSkiaAcceleratedBackend::ImageBufferSkiaAcceleratedBackend(const Parameters& parameters, sk_sp<SkSurface>&& surface)\n    : ImageBufferSkiaSurfaceBackend(parameters, WTF::move(surface), RenderingMode::Accelerated)",
        """ImageBufferSkiaAcceleratedBackend::ImageBufferSkiaAcceleratedBackend(const Parameters& parameters, sk_sp<SkSurface>&& surface, RefPtr<EdenCanvasGPUState>&& canvasThread)
    : ImageBufferSkiaSurfaceBackend(parameters, WTF::move(surface), RenderingMode::Accelerated)
    , m_canvasThread(WTF::move(canvasThread))""",
    )
    text = replace_once(
        text,
        '#include "GraphicsContextSkia.h"',
        '#include "GraphicsContextSkia.h"\n#include "EdenCanvasThread.h"',
    )
    text = replace_once(
        text,
        "    if (parameters.purpose == RenderingPurpose::Canvas)\n        m_layerContentsDisplayDelegate = GraphicsLayerContentsDisplayDelegateCoordinated::create();",
        """    if (parameters.purpose == RenderingPurpose::Canvas) {
        if (m_canvasThread) {
            m_canvasPresentation = EdenCanvasPresentation::create();
            m_canvasPresentation->setRefreshTask(createSharedTask<void(EdenCanvasPresentation::Target, Ref<EdenCanvasPresentation>)>(
                [state = Ref { *m_canvasThread }](auto target, auto presentation) {
                    state->submit(nullptr, WTF::move(target), WTF::move(presentation));
                }));
            m_layerContentsDisplayDelegate = EdenCanvasDisplayDelegate::create(Ref { *m_canvasPresentation });
        } else {
            m_layerContentsDisplayDelegate = GraphicsLayerContentsDisplayDelegateCoordinated::create();
        }
    }""",
    )
    text = replace_once(text, "    auto* recordingContext = m_surface->recordingContext();",
        "    auto* recordingContext = m_surface ? m_surface->recordingContext() : nullptr;")
    text = replace_once(
        text,
        "ImageBufferSkiaAcceleratedBackend::~ImageBufferSkiaAcceleratedBackend()\n{",
        """ImageBufferSkiaAcceleratedBackend::~ImageBufferSkiaAcceleratedBackend()
{
    if (m_canvasPresentation) {
        m_canvasPresentation->invalidate();
    }
    if (m_canvasThread) {
        m_canvasThread->shutdown();
    }""",
    )
    text = replace_once(
        text,
        """    m_canvasRecordingContext->applyDeviceScaleFactor(resolutionScale());
    m_canvasRecordingContext->beginRecording(GraphicsContextSkia::RecordingMode::Canvas);
    m_hasActiveRecording = true;""",
        """    m_canvasRecordingContext->applyDeviceScaleFactor(resolutionScale());
    m_canvasRecordingContext->beginRecording(GraphicsContextSkia::RecordingMode::Canvas, m_canvasThread ? m_canvasThread->threadSafeProxy() : nullptr);
    m_hasActiveRecording = true;""",
    )
    text = replace_once(
        text,
        "void ImageBufferSkiaAcceleratedBackend::replayCanvasRecordingContextIfNeeded()\n{",
        """sk_sp<SkPicture> ImageBufferSkiaAcceleratedBackend::finishCanvasRecording()
{
    if (!m_canvasRecordingContext || !m_hasActiveRecording) {
        return nullptr;
    }
    m_canvasRecordingContext->endRecording();
    m_hasActiveRecording = false;
    return m_pictureRecorder.finishRecordingAsPicture();
}

void ImageBufferSkiaAcceleratedBackend::restartCanvasRecording()
{
    if (!m_canvasRecordingContext || m_hasActiveRecording) {
        return;
    }
    auto* recordingCanvas = m_pictureRecorder.beginRecording(size().width(), size().height());
    m_switchableCanvas->switchToCanvas(recordingCanvas);
    m_canvasRecordingContext->replayStateOnCanvas(*recordingCanvas);
    m_canvasRecordingContext->beginRecording(GraphicsContextSkia::RecordingMode::Canvas, m_canvasThread ? m_canvasThread->threadSafeProxy() : nullptr);
    m_hasActiveRecording = true;
}

bool ImageBufferSkiaAcceleratedBackend::executeCanvasThreadSync(Function<bool(SkSurface&, GrDirectContext&)>&& operation)
{
    auto picture = finishCanvasRecording();
    bool succeeded = m_canvasThread->executeSync(WTF::move(picture), WTF::move(operation));
    restartCanvasRecording();
    return succeeded;
}

bool ImageBufferSkiaAcceleratedBackend::isContextLost() const
{
    return m_canvasThread && m_canvasThread->isLost();
}

bool ImageBufferSkiaAcceleratedBackend::isCanvasThreadForTesting() const
{
    return !!m_canvasThread;
}

uint64_t ImageBufferSkiaAcceleratedBackend::synchronousCanvasOperationsForTesting() const
{
    return m_canvasThread ? m_canvasThread->synchronousOperationsForTesting() : 0;
}

void ImageBufferSkiaAcceleratedBackend::setContextLossHandler(Function<void()>&& handler)
{
    if (m_canvasThread) {
        m_canvasThread->setLossHandler(WTF::move(handler));
    }
}

bool ImageBufferSkiaAcceleratedBackend::loseContextForTesting()
{
    if (!m_canvasThread) {
        return false;
    }
    m_canvasThread->forceLossForTesting();
    return true;
}

void ImageBufferSkiaAcceleratedBackend::replayCanvasRecordingContextIfNeeded()
{""",
    )
    text = replace_once(
        text,
        "    IntRect recordRect(IntPoint(), size());\n    auto recordingData = m_canvasRecordingContext->endRecording();\n    auto picture = m_pictureRecorder.finishRecordingAsPicture();",
        """    IntRect recordRect(IntPoint(), size());
    auto recordingData = m_canvasRecordingContext->endRecording();
    auto picture = m_pictureRecorder.finishRecordingAsPicture();""",
    )
    text = replace_once(
        text,
        "void ImageBufferSkiaAcceleratedBackend::flushContext()\n{\n    replayCanvasRecordingContextIfNeeded();",
        """void ImageBufferSkiaAcceleratedBackend::flushContext()
{
    if (m_canvasThread) {
        executeCanvasThreadSync([](SkSurface& surface, GrDirectContext& context) {
            context.flushAndSubmit(&surface, GrSyncCpu::kNo);
            return !context.abandoned();
        });
        return;
    }

    replayCanvasRecordingContextIfNeeded();""",
    )
    start = text.index("void ImageBufferSkiaAcceleratedBackend::prepareForDisplay()")
    end = text.index("RefPtr<NativeImage> ImageBufferSkiaAcceleratedBackend::copyNativeImage()", start)
    replacement = """void ImageBufferSkiaAcceleratedBackend::prepareForDisplay()
{
#if USE(COORDINATED_GRAPHICS)
    if (!m_layerContentsDisplayDelegate) {
        return;
    }

    if (m_canvasThread) {
        auto presentationContext = m_canvasPresentation->target();
        auto picture = finishCanvasRecording();
        if (!picture) {
            return;
        }
        m_canvasThread->submit(WTF::move(picture), WTF::move(presentationContext), Ref { *m_canvasPresentation });
        restartCanvasRecording();
        return;
    }

    auto image = createNativeImageReference();
    if (!image) {
        return;
    }

    auto* grContext = this->grContext();
    if (!grContext) {
        return;
    }

    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent()) {
        return;
    }

    ASSERT(grContext == PlatformDisplay::sharedDisplay().skiaGrContext());

    if (auto threadSafeGrContext = m_layerContentsDisplayDelegate->threadSafeGrContext()) {
        m_layerContentsDisplayDelegate->setDisplayBuffer(CoordinatedPlatformLayerBufferSkiaImage::create(image->platformImage(), threadSafeGrContext));
    } else {
        m_layerContentsDisplayDelegate->setDisplayBuffer(CoordinatedPlatformLayerBufferNativeImage::create(image.releaseNonNull(),
            SkiaUtilities::flushAndSubmitSurfaceWithFence(grContext, m_surface.get())));
    }

    if (m_canvasRecordingContext) {
        m_surface->getCanvas()->restoreToCount(1);
        restartCanvasRecording();
    }
#endif
}

"""
    text = text[:start] + replacement + text[end:]
    text = replace_once(
        text,
        "RefPtr<NativeImage> ImageBufferSkiaAcceleratedBackend::createNativeImageReference()\n{\n    replayCanvasRecordingContextIfNeeded();",
        """RefPtr<NativeImage> ImageBufferSkiaAcceleratedBackend::createNativeImageReference()
{
    if (m_canvasThread) {
        GrDirectContext* consumerContext = nullptr;
        sk_sp<GrContextThreadSafeProxy> consumerProxy;
        if (isMainThread()) {
            auto* consumerGLContext = PlatformDisplay::sharedDisplay().skiaGLContext();
            if (consumerGLContext && consumerGLContext->makeContextCurrent()) {
                consumerContext = PlatformDisplay::sharedDisplay().skiaGrContext();
                if (consumerContext) {
                    consumerProxy = consumerContext->threadSafeProxy();
                }
            }
        }
        sk_sp<SkImage> sourceImage;
        if (!executeCanvasThreadSync([&](SkSurface& surface, GrDirectContext& context) {
                context.flush(&surface);
                sourceImage = surface.makeImageSnapshot();
                return !!sourceImage;
            }) || !sourceImage) {
            return nullptr;
        }
        auto sourceProxy = m_canvasThread->threadSafeProxy();
        auto provider = std::make_shared<EdenCanvasImageProvider>(Ref { *m_canvasThread }, sourceImage, sourceProxy);
        sk_sp<SkImage> image = provider->imageForThreadSafeContext(consumerProxy);
        if (!image) {
            if (!executeCanvasThreadSync([&](SkSurface&, GrDirectContext& context) {
                    image = sourceImage->makeRasterImage(&context);
                    return !!image;
                }) || !image) {
                return nullptr;
            }
        }
        auto* nativeImageContext = image->isTextureBacked() ? consumerContext : nullptr;
        auto nativeImage = NativeImage::create(WTF::move(image), nativeImageContext);
        if (nativeImage) {
            nativeImage->setSkiaImageProvider(WTF::move(provider));
        }
        return nativeImage;
    }

    replayCanvasRecordingContextIfNeeded();""",
    )
    start = text.index("void ImageBufferSkiaAcceleratedBackend::getPixelBuffer(")
    end = text.index("static std::span<uint8_t> mutableSpan", start)
    tail = text[text.index("    const IntRect backendRect", start):end]
    tail = tail.replace("        return;", "        return sourceRectClipped.isEmpty();")
    replacement = """void ImageBufferSkiaAcceleratedBackend::getPixelBuffer(const IntRect& srcRect, PixelBuffer& destination)
{
    if (m_canvasThread) {
        if (!executeCanvasThreadSync([&](SkSurface& surface, GrDirectContext&) {
            return readPixelBuffer(surface, srcRect, destination);
        })) {
            destination.zeroFill();
        }
        return;
    }

    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent()) {
        destination.zeroFill();
        return;
    }

    replayCanvasRecordingContextIfNeeded();
    if (!readPixelBuffer(*m_surface, srcRect, destination)) {
        destination.zeroFill();
    }
}

bool ImageBufferSkiaAcceleratedBackend::readPixelBuffer(SkSurface& surface, const IntRect& srcRect, PixelBuffer& destination)
{
""" + tail
    text = text[:start] + replacement + text[end:]
    text = replace_once(text, "    m_surface->readPixels(dstPixmap, sourceRectClipped.x(), sourceRectClipped.y());", "    return surface.readPixels(dstPixmap, sourceRectClipped.x(), sourceRectClipped.y());")
    start = text.index("void ImageBufferSkiaAcceleratedBackend::putPixelBuffer(")
    end = text.index("#if USE(COORDINATED_GRAPHICS)", start)
    tail = text[text.index("    UNUSED_PARAM(destFormat);", start):end]
    tail = tail.replace("        return;", "        return sourceRectClipped.isEmpty();")
    tail = replace_once(tail,
        "        m_surface->writePixels(srcPixmap, destinationRect.x(), destinationRect.y());\n        return sourceRectClipped.isEmpty();",
        "        m_surface->writePixels(srcPixmap, destinationRect.x(), destinationRect.y());\n        return true;")
    tail = tail.replace("        return;", "        return true;")
    tail = replace_once(tail, "    auto data = SkData::MakeUninitialized(srcPixmap.computeByteSize());",
        "    auto data = SkData::MakeUninitialized(srcPixmap.computeByteSize());\n    if (!data) {\n        return false;\n    }")
    tail = replace_once(tail, "    m_surface->writePixels(convertedSrcPixmap, destinationRect.x(), destinationRect.y());",
        "    m_surface->writePixels(convertedSrcPixmap, destinationRect.x(), destinationRect.y());\n    return true;")
    replacement = """void ImageBufferSkiaAcceleratedBackend::putPixelBuffer(const PixelBufferSourceView& pixelBuffer, const IntRect& srcRect, const IntPoint& destPoint, AlphaPremultiplication destFormat)
{
    if (m_canvasThread) {
        executeCanvasThreadSync([&](SkSurface& surface, GrDirectContext&) {
            return writePixelBuffer(surface, pixelBuffer, srcRect, destPoint, destFormat);
        });
        return;
    }

    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent()) {
        return;
    }

    replayCanvasRecordingContextIfNeeded();
    writePixelBuffer(*m_surface, pixelBuffer, srcRect, destPoint, destFormat);
}

bool ImageBufferSkiaAcceleratedBackend::writePixelBuffer(SkSurface& surface, const PixelBufferSourceView& pixelBuffer, const IntRect& srcRect, const IntPoint& destPoint, AlphaPremultiplication destFormat)
{
""" + tail
    text = text[:start] + replacement + text[end:]
    text = text.replace("m_surface->imageInfo()", "surface.imageInfo()")
    text = text.replace("m_surface->writePixels", "surface.writePixels")
    text = text.replace("auto imageInfo = surface.imageInfo();", "auto imageInfo = m_surface->imageInfo();")
    return text


def backend_header_patch(text):
    text = replace_once(text, "class SkiaSwitchableCanvas;", "class EdenCanvasGPUState;\nclass EdenCanvasPresentation;\nclass SkiaSwitchableCanvas;")
    text = replace_once(
        text,
        "    ImageBufferSkiaAcceleratedBackend(const Parameters&, sk_sp<SkSurface>&&);",
        """    ImageBufferSkiaAcceleratedBackend(const Parameters&, sk_sp<SkSurface>&&, RefPtr<EdenCanvasGPUState>&& = nullptr);
    bool preparesForDisplayWithoutFlushing() const final { return !!m_canvasThread; }
    bool isContextLost() const final;
    bool isCanvasThreadForTesting() const final;
    uint64_t synchronousCanvasOperationsForTesting() const final;
    void setContextLossHandler(Function<void()>&&) final;
    bool loseContextForTesting() final;""",
    )
    text = replace_once(
        text,
        "    void replayCanvasRecordingContextIfNeeded();\n    void ensureCanvasRecordingContext();",
        """    void replayCanvasRecordingContextIfNeeded();
    void ensureCanvasRecordingContext();
    sk_sp<SkPicture> finishCanvasRecording();
    void restartCanvasRecording();
    bool executeCanvasThreadSync(Function<bool(SkSurface&, GrDirectContext&)>&&);
    bool readPixelBuffer(SkSurface&, const IntRect&, PixelBuffer&);
    bool writePixelBuffer(SkSurface&, const PixelBufferSourceView&, const IntRect&, const IntPoint&, AlphaPremultiplication);""",
    )
    return replace_once(
        text,
        "    std::unique_ptr<GraphicsContextSkia> m_canvasRecordingContext;",
        """    std::unique_ptr<GraphicsContextSkia> m_canvasRecordingContext;
    RefPtr<EdenCanvasGPUState> m_canvasThread;
    RefPtr<EdenCanvasPresentation> m_canvasPresentation;""",
    )


def native_image_header_patch(text):
    text = replace_once(
        text,
        "#include <wtf/TZoneMalloc.h>",
        "#include <wtf/Lock.h>\n#include <wtf/TZoneMalloc.h>\n#include <memory>",
    )
    text = replace_once(
        text,
        "#if USE(SKIA)\nclass GrDirectContext;\n#endif",
        """#if USE(SKIA)
class GrDirectContext;
class GrContextThreadSafeProxy;
class SkImage;
class NativeImageSkiaImageProvider {
public:
    virtual ~NativeImageSkiaImageProvider() = default;
    virtual sk_sp<SkImage> imageForThreadSafeContext(const sk_sp<GrContextThreadSafeProxy>&) const = 0;
    virtual sk_sp<SkImage> rasterImage() const = 0;
};
#endif""",
    )
    text = replace_once(
        text,
        "    GrDirectContext* grContext() const { return m_grContext; }",
        """    GrDirectContext* grContext() const { return m_grContext; }
    bool hasSkiaImageProvider() const {
        Locker locker { m_skiaImageProviderLock };
        return !!m_skiaImageProvider;
    }
    sk_sp<SkImage> imageForThreadSafeContext(const sk_sp<GrContextThreadSafeProxy>& context) const {
        std::shared_ptr<NativeImageSkiaImageProvider> provider;
        {
            Locker locker { m_skiaImageProviderLock };
            provider = m_skiaImageProvider;
        }
        return provider ? provider->imageForThreadSafeContext(context) : nullptr;
    }
    sk_sp<SkImage> skiaRasterImage() const {
        std::shared_ptr<NativeImageSkiaImageProvider> provider;
        {
            Locker locker { m_skiaImageProviderLock };
            provider = m_skiaImageProvider;
        }
        return provider ? provider->rasterImage() : nullptr;
    }
    void setSkiaImageProvider(std::shared_ptr<NativeImageSkiaImageProvider>&& provider) {
        Locker locker { m_skiaImageProviderLock };
        m_skiaImageProvider = WTF::move(provider);
    }""",
    )
    return replace_once(
        text,
        "    GrDirectContext* m_grContext { nullptr };",
        "    GrDirectContext* m_grContext { nullptr };\n    mutable Lock m_skiaImageProviderLock;\n    mutable std::shared_ptr<NativeImageSkiaImageProvider> m_skiaImageProvider WTF_GUARDED_BY_LOCK(m_skiaImageProviderLock);",
    )


def native_image_patch(text):
    return replace_once(
        text,
        "    m_platformImage = WTF::move(platformImage);\n    computeHeadroom();",
        """#if USE(SKIA)
    {
        Locker locker { m_skiaImageProviderLock };
        m_platformImage = WTF::move(platformImage);
        m_skiaImageProvider = nullptr;
    }
#else
    m_platformImage = WTF::move(platformImage);
#endif
    computeHeadroom();""",
    )


def graphics_context_header_patch(text):
    return replace_once(
        text,
        "    sk_sp<SkImage> imageForCurrentThread(const sk_sp<SkImage>&) const;",
        "    sk_sp<SkImage> imageForCurrentThread(const sk_sp<SkImage>&) const;\n    sk_sp<SkImage> imageForCurrentThread(const NativeImage&) const;",
    )


def gl_image_extractor_patch(text):
    text = replace_once(
        text,
        "    auto platformImage = nativeImage->platformImage();\n    if (!platformImage)",
        "    auto platformImage = nativeImage->hasSkiaImageProvider() ? nativeImage->skiaRasterImage() : nativeImage->platformImage();\n    if (!platformImage)",
    )
    return replace_once(
        text,
        "        m_imageSourceFormat = DataFormat::BGRA8;",
        "        m_imageSourceFormat = pixmap.colorType() == kRGBA_8888_SkColorType ? DataFormat::RGBA8 : DataFormat::BGRA8;",
    )


def image_utilities_skia_patch(text):
    return replace_once(
        text,
        "    auto image = nativeImage.platformImage();\n    if (!image)",
        "    auto image = nativeImage.hasSkiaImageProvider() ? nativeImage.skiaRasterImage() : nativeImage.platformImage();\n    if (!image)",
    )


def coordinated_image_backing_store_patch(text):
    return replace_once(
        text,
        """    : m_buffer(CoordinatedPlatformLayerBufferSkiaImage::create(nativeImage->platformImage(), threadSafeGrContext))
    , m_uniqueID(nativeImage->uniqueID())
{
}""",
        """    : m_uniqueID(nativeImage->uniqueID())
{
    if (!nativeImage->hasSkiaImageProvider()) {
        m_buffer = CoordinatedPlatformLayerBufferSkiaImage::create(nativeImage->platformImage(), threadSafeGrContext);
        return;
    }
    auto image = nativeImage->imageForThreadSafeContext(threadSafeGrContext);
    if (!image) {
        auto platformImage = nativeImage->platformImage();
        if (!platformImage || platformImage->isTextureBacked()) {
            return;
        }
        image = WTF::move(platformImage);
    }
    OptionSet<TextureMapperFlags> flags;
    if (!image->isOpaque()) {
        flags.add(TextureMapperFlags::ShouldBlend);
    }
    m_buffer = makeUnique<CoordinatedPlatformLayerBufferSkiaImage>(WTF::move(image), flags);
}""",
    )


def async_contents_delegate_patch(text):
    return replace_once(
        text,
        """    if (m_threadSafeGrContext) {
        m_delegate->setDisplayBuffer(CoordinatedPlatformLayerBufferSkiaImage::create(image->platformImage(), m_threadSafeGrContext));
        return true;
    }""",
        """    if (m_threadSafeGrContext) {
        if (image->hasSkiaImageProvider()) {
            auto platformImage = image->platformImage();
            auto transferredImage = image->imageForThreadSafeContext(m_threadSafeGrContext);
            if (transferredImage) {
                platformImage = WTF::move(transferredImage);
            } else if (!platformImage || platformImage->isTextureBacked()) {
                return false;
            }
            OptionSet<TextureMapperFlags> flags;
            if (!platformImage->isOpaque()) {
                flags.add(TextureMapperFlags::ShouldBlend);
            }
            m_delegate->setDisplayBuffer(makeUnique<CoordinatedPlatformLayerBufferSkiaImage>(WTF::move(platformImage), flags));
            return true;
        }
        auto buffer = CoordinatedPlatformLayerBufferSkiaImage::create(image->platformImage(), m_threadSafeGrContext);
        if (!buffer) {
            return false;
        }
        m_delegate->setDisplayBuffer(WTF::move(buffer));
        return true;
    }""",
    )


def pattern_skia_patch(text):
    text = replace_once(
        text,
        "const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContext) const",
        "const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContext, bool isAccelerated) const",
    )
    text = replace_once(
        text,
        "#include <skia/core/SkImage.h>",
        "#include <skia/core/SkImage.h>\n#include <skia/core/SkShader.h>",
    )
    return replace_once(
        text,
        """    if (threadSafeGrContext)
        platformImage = SkiaUtilities::createPromiseImageIfNeeded(platformImage, threadSafeGrContext);

    return platformImage->makeShader(""",
        """    if (nativeImage->hasSkiaImageProvider()) {
        if (!isAccelerated) {
            platformImage = nativeImage->skiaRasterImage();
        } else if (threadSafeGrContext) {
            auto providerImage = nativeImage->imageForThreadSafeContext(threadSafeGrContext);
            if (providerImage) {
                platformImage = WTF::move(providerImage);
            } else if (platformImage->isTextureBacked()) {
                return nullptr;
            }
        }
    } else if (threadSafeGrContext) {
        platformImage = SkiaUtilities::createPromiseImageIfNeeded(platformImage, threadSafeGrContext);
    }
    if (!platformImage) {
        return SkShaders::Color(SK_ColorTRANSPARENT);
    }

    return platformImage->makeShader(""",
    )


def pattern_header_patch(text):
    return replace_once(
        text,
        "PlatformPatternPtr createPlatformPattern(const SkSamplingOptions&, const sk_sp<GrContextThreadSafeProxy>&) const;",
        "PlatformPatternPtr createPlatformPattern(const SkSamplingOptions&, const sk_sp<GrContextThreadSafeProxy>&, bool isAccelerated) const;",
    )


def graphics_context_patch(text):
    for pattern in ("fillPattern", "strokePattern"):
        text = replace_once(
            text,
            f"{pattern}->createPlatformPattern(toSkSamplingOptions(imageInterpolationQuality()), m_threadSafeGrContext)",
            f"{pattern}->createPlatformPattern(toSkSamplingOptions(imageInterpolationQuality()), m_threadSafeGrContext, renderingMode() == RenderingMode::Accelerated)",
        )
    text = replace_once(
        text,
        "    case RecordingMode::Canvas:\n        ASSERT(!threadSafeGrContext);\n        m_contextMode = ContextMode::CanvasRecordingMode;",
        """    case RecordingMode::Canvas:
        m_contextMode = ContextMode::CanvasRecordingMode;
        m_threadSafeGrContext = threadSafeGrContext;""",
    )
    text = replace_once(
        text,
        "    case ContextMode::CanvasRecordingMode:\n        ASSERT(!m_threadSafeGrContext);\n        return { };",
        """    case ContextMode::CanvasRecordingMode:
        m_threadSafeGrContext = nullptr;
        return { };""",
    )
    helper = """    sk_sp<SkImage> GraphicsContextSkia::imageForCurrentThread(const NativeImage& nativeImage) const {
        if (!nativeImage.hasSkiaImageProvider()) {
            return imageForCurrentThread(nativeImage.platformImage());
        }
        if (renderingMode() == RenderingMode::Unaccelerated) {
            return nativeImage.skiaRasterImage();
        }

        auto threadSafeGrContext = m_threadSafeGrContext;
        if (!threadSafeGrContext) {
            if (!makeGLContextCurrentIfNeeded()) {
                return nullptr;
            }
            auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
            if (!grContext) {
                return nullptr;
            }
            threadSafeGrContext = grContext->threadSafeProxy();
        }
        auto image = nativeImage.imageForThreadSafeContext(threadSafeGrContext);
        if (image) {
            return image;
        }
        auto fallbackImage = nativeImage.platformImage();
        return fallbackImage && !fallbackImage->isTextureBacked() ? fallbackImage : nullptr;
    }

"""
    anchor = "void GraphicsContextSkia::drawNativeImage("
    if anchor not in text:
        raise RuntimeError("GraphicsContextSkia drawNativeImage anchor missing")
    text = text.replace(anchor, helper + anchor, 1)
    text = replace_once(
        text,
        """    auto imageInThisThread = imageForCurrentThread(image);
    if (m_threadSafeGrContext)
        imageInThisThread = SkiaUtilities::createPromiseImageIfNeeded(imageInThisThread, m_threadSafeGrContext);
    else
        trackAcceleratedRenderingFenceIfNeeded(imageInThisThread, nativeImage.grContext());""",
        """    bool hasImageProvider = nativeImage.hasSkiaImageProvider();
    auto imageInThisThread = hasImageProvider ? imageForCurrentThread(nativeImage) : imageForCurrentThread(image);
    if (!imageInThisThread) {
        if (options.orientation() != ImageOrientation::Orientation::None) {
            m_canvas.restore();
        }
        return;
    }
    if (m_threadSafeGrContext && !hasImageProvider) {
        imageInThisThread = SkiaUtilities::createPromiseImageIfNeeded(imageInThisThread, m_threadSafeGrContext);
        if (!imageInThisThread) {
            if (options.orientation() != ImageOrientation::Orientation::None) {
                m_canvas.restore();
            }
            return;
        }
    } else if (!hasImageProvider) {
        trackAcceleratedRenderingFenceIfNeeded(imageInThisThread, nativeImage.grContext());
    }""",
    )
    text = replace_once(
        text,
        """    if (auto nativeImage = nativeImageForDrawing(buffer)) {
        auto image = nativeImage->platformImage();
        if (m_threadSafeGrContext)
            image = SkiaUtilities::createPromiseImageIfNeeded(image, m_threadSafeGrContext);
        else
            trackAcceleratedRenderingFenceIfNeeded(image, nativeImage->grContext());
        auto shader = image->makeShader(SkTileMode::kDecal, SkTileMode::kDecal, { }, SkMatrix::Translate(SkFloatToScalar(destRect.x()), SkFloatToScalar(destRect.y())));""",
        """    if (auto nativeImage = nativeImageForDrawing(buffer)) {
        bool hasImageProvider = nativeImage->hasSkiaImageProvider();
        auto image = hasImageProvider ? imageForCurrentThread(*nativeImage) : nativeImage->platformImage();
        if (!image) {
            clip(FloatRect { });
            return;
        }
        if (!hasImageProvider) {
            if (m_threadSafeGrContext)
                image = SkiaUtilities::createPromiseImageIfNeeded(image, m_threadSafeGrContext);
            else
                trackAcceleratedRenderingFenceIfNeeded(image, nativeImage->grContext());
        }
        if (!image) {
            clip(FloatRect { });
            return;
        }
        auto shader = image->makeShader(SkTileMode::kDecal, SkTileMode::kDecal, { }, SkMatrix::Translate(SkFloatToScalar(destRect.x()), SkFloatToScalar(destRect.y())));
        if (!shader) {
            clip(FloatRect { });
            return;
        }""",
    )
    text = replace_once(
        text,
        "    image = imageForCurrentThread(image);",
        """    bool hasImageProvider = nativeImage.hasSkiaImageProvider();
    image = hasImageProvider ? imageForCurrentThread(nativeImage) : imageForCurrentThread(image);
    if (!image) {
        return;
    }""",
    )
    patternStart = text.index("void GraphicsContextSkia::drawPattern(")
    patternEnd = text.index("\nvoid GraphicsContextSkia::beginRecording(", patternStart)
    pattern = text[patternStart:patternEnd]
    pattern = replace_once(
        pattern,
        """        if (m_threadSafeGrContext)
            image = SkiaUtilities::createPromiseImageIfNeeded(image, m_threadSafeGrContext);""",
        """        if (m_threadSafeGrContext && !hasImageProvider)
            image = SkiaUtilities::createPromiseImageIfNeeded(image, m_threadSafeGrContext);""",
    )
    pattern = replace_once(
        pattern,
        "        if (!m_threadSafeGrContext)\n            trackAcceleratedRenderingFenceIfNeeded(image, nativeImage.grContext());",
        "        if (!m_threadSafeGrContext && !hasImageProvider)\n            trackAcceleratedRenderingFenceIfNeeded(image, nativeImage.grContext());",
    )
    pattern = replace_once(
        pattern,
        "        paint.setShader(image->makeShader(",
        "        if (!image) {\n            return;\n        }\n        paint.setShader(image->makeShader(",
    )
    pattern = replace_once(
        pattern,
        "                paint.setShader(tileImage->makeShader(",
        "                if (!tileImage) {\n                    return;\n                }\n                paint.setShader(tileImage->makeShader(",
    )
    branchStart = pattern.index("        if (image->isTextureBacked()) {")
    branchMarker = "        } else {\n            auto dstRect = SkRect::MakeWH(tileRect.width(), tileRect.height());"
    branchEnd = pattern.index(branchMarker, branchStart)
    branchPrefix = "        if (image->isTextureBacked()) {\n"
    originalTextureBranch = pattern[branchStart + len(branchPrefix):branchEnd]
    originalTextureBody = originalTextureBranch
    threadedTextureBranch = """        if (image->isTextureBacked() && m_threadSafeGrContext) {
            if (!hasImageProvider) {
                image = SkiaUtilities::createPromiseImageIfNeeded(image, m_threadSafeGrContext);
                if (!image) {
                    return;
                }
            }
            auto enclosingTileIntRect = enclosingIntRect(tileRect);
            auto dstRect = SkRect::MakeWH(enclosingTileIntRect.width(), enclosingTileIntRect.height());
            auto clipRect = enclosingIntRect(tileFloatRectWithSpacing);
            auto pictureClipRect = SkRect::MakeWH(clipRect.width(), clipRect.height());
            SkPictureRecorder recorder;
            auto* recordCanvas = recorder.beginRecording(pictureClipRect);
            recordCanvas->drawImageRect(image, tileRect, dstRect, samplingOptions, nullptr, SkCanvas::kStrict_SrcRectConstraint);
            auto picture = recorder.finishRecordingAsPicture();
            if (!picture) {
                return;
            }
            auto shader = picture->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, samplingOptions.filter, &shaderMatrix, nullptr);
            if (!shader) {
                return;
            }
            paint.setShader(shader);
        } else if (image->isTextureBacked()) {
"""
    pattern = pattern[:branchStart] + threadedTextureBranch + originalTextureBody + pattern[branchEnd:]
    text = text[:patternStart] + pattern + text[patternEnd:]
    return text


def canvas_header_patch(text):
    text = replace_once(
        text,
        "#include <wtf/Forward.h>",
        "#include <wtf/Forward.h>\n#include <wtf/WeakPtr.h>",
    )
    return replace_once(
        text,
        "    bool m_ignoreDidUpdateSizeProperties",
        "    SingleThreadWeakPtr<RenderBox> m_edenFullyDirtyRenderer;\n    bool m_ignoreDidUpdateSizeProperties",
    )


def canvas_patch(text):
    start = text.index("void HTMLCanvasElement::didDraw(")
    end = text.index("\nvoid HTMLCanvasElement::didUpdateSizeProperties()", start)
    implementation = """void HTMLCanvasElement::didDraw(const std::optional<FloatRect>& rect, ShouldApplyPostProcessingToDirtyRect shouldApplyPostProcessingToDirtyRect)
{
    clearCopiedImage();
    if (CheckedPtr renderer = renderBox()) {
        bool layerContents = usesContentsAsLayerContents();
        bool eligible = layerContents && is<CanvasRenderingContext2D>(m_context.get());
        if (!eligible || m_edenFullyDirtyRenderer.get() != renderer.get()) {
            const std::optional<FloatRect> dirtyRect = computeDirtyRectangleIfNeeded(rect);
            if (layerContents) {
                renderer->contentChanged(ContentChangeType::CanvasPixels, dirtyRect);
                if (eligible && (!rect || rect->contains(FloatRect { { }, size() }))) {
                    m_edenFullyDirtyRenderer = renderer.get();
                }
            } else if (dirtyRect) {
                renderer->repaintRectangle(enclosingIntRect(*dirtyRect));
            }
        }
    }
    CanvasBase::didDraw(rect, shouldApplyPostProcessingToDirtyRect);
}
"""
    text = text[:start] + implementation + text[end:]
    for anchor in [
        "void HTMLCanvasElement::didUpdateSizeProperties()\n{",
        "void HTMLCanvasElement::paint(GraphicsContext& context, const LayoutRect& r)\n{",
        "void HTMLCanvasElement::didMoveToNewDocument(Document& oldDocument, Document& newDocument)\n{",
        "void HTMLCanvasElement::prepareForDisplay()\n{",
    ]:
        text = replace_once(text, anchor, anchor + "\n    m_edenFullyDirtyRenderer = nullptr;")
    return text


def ddl_order_patch(text):
    return replace_once(
        text,
        "    this->setFlag(kClosed_Flag);",
        "    this->setFlag(kBlocksReordering_Flag);\n    this->setFlag(kClosed_Flag);",
    )


PATCHES = {
    **LIFECYCLE_PATCHES,
    "Source/ThirdParty/skia/src/gpu/ganesh/GrDDLTask.cpp": ddl_order_patch,
    "Source/WebCore/platform/graphics/ImageBuffer.cpp": image_buffer_patch,
    "Source/WebCore/platform/graphics/ImageBuffer.h": image_buffer_header_patch,
    "Source/WebCore/platform/graphics/ImageBufferBackend.h": image_buffer_backend_patch,
    "Source/WebCore/html/canvas/CanvasRenderingContext2DBase.cpp": canvas_context_patch,
    SKIA + "GraphicsContextSkia.cpp": graphics_context_patch,
    SKIA + "GraphicsContextSkia.h": graphics_context_header_patch,
    SKIA + "GraphicsContextGLSkia.cpp": gl_image_extractor_patch,
    SKIA + "ImageUtilitiesSkia.cpp": image_utilities_skia_patch,
    SKIA + "ImageBufferSkiaAcceleratedBackend.cpp": backend_patch,
    SKIA + "ImageBufferSkiaAcceleratedBackend.h": backend_header_patch,
    "Source/WebCore/platform/graphics/NativeImage.h": native_image_header_patch,
    "Source/WebCore/platform/graphics/NativeImage.cpp": native_image_patch,
    SKIA + "PatternSkia.cpp": pattern_skia_patch,
    "Source/WebCore/platform/graphics/Pattern.h": pattern_header_patch,
    "Source/WebCore/platform/graphics/texmap/coordinated/GraphicsLayerAsyncContentsDisplayDelegateCoordinated.cpp": async_contents_delegate_patch,
    "Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedImageBackingStore.cpp": coordinated_image_backing_store_patch,
    "Source/WebCore/html/HTMLCanvasElement.cpp": canvas_patch,
    "Source/WebCore/html/HTMLCanvasElement.h": canvas_header_patch,
}

FILES = {SKIA + "EdenCanvasThread.h": "canvas_thread.inc"}
SOURCES = ["canvas_thread.inc", "canvas_lifecycle.py"]
