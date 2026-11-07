#include "engine/wpe/wpeengineview.h"
#include "engine/wpe/wpeprofile.h"
#include "engine/wpe/wperuntime.h"
#include "engine/wpe/wpesurface.h"
#include "platform/waylandglibeventdispatcher.h"

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTimer>
#include <chrono>

using namespace eden::engine;

static QVariantMap rendererPageBacking(qint64 pid) {
    QFile mapping(QString("/proc/%1/smaps").arg(pid));
    if (!mapping.open(QIODevice::ReadOnly)) {
        return {{"readable", false}};
    }
    quint64 textFileHugeKiB = 0, textAnonymousHugeKiB = 0, anonymousHugeKiB = 0, textRssKiB = 0;
    bool text = false, textAdvised = false;
    for (const QByteArray &line : mapping.readAll().split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.value(0).contains('-')) {
            text = line.contains("libWPEWebKit-2.0.so") && fields.value(1).contains('x');
        }
        if (line.startsWith("AnonHugePages:")) {
            anonymousHugeKiB += fields.value(1).toULongLong();
            if (text) {
                textAnonymousHugeKiB += fields.value(1).toULongLong();
            }
        }
        if (text && line.startsWith("FilePmdMapped:")) {
            textFileHugeKiB += fields.value(1).toULongLong();
        }
        if (text && line.startsWith("Rss:")) {
            textRssKiB += fields.value(1).toULongLong();
        }
        if (text && line.startsWith("VmFlags:") && fields.contains("hg")) {
            textAdvised = true;
        }
    }
    return {
        {"readable", true},
        {"webkitTextFileHugeKiB", textFileHugeKiB},
        {"webkitTextAnonymousHugeKiB", textAnonymousHugeKiB},
        {"anonymousHugeKiB", anonymousHugeKiB},
        {"webkitTextRssKiB", textRssKiB},
        {"webkitTextAdvised", textAdvised}
    };
}

int main(int argc, char *argv[]) {
    qputenv("QT_NO_GLIB", "1");
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QCoreApplication::setApplicationName("EdenWebKitBenchmark");
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"probe-script", "Run a local probe instead of Speedometer", "file"});
    parser.addOption({"url", "Benchmark page", "url", "https://browserbench.org/Speedometer3.1/"});
    parser.addOption({"output", "Result JSON path", "file"});
    parser.addOption({"label", "Build and tuning label", "name"});
    parser.addOption({"timings", "Save the benchmark timing entries for trace correlation"});
    parser.addOption({"training-directory", "Directory for native PGO profiles", "path"});
    parser.addOption({"external-driver", "Leave benchmark control to an external inspector driver"});
    parser.addOption({"viewport-width", "Fixed page viewport width for shell comparisons", "pixels"});
    parser.addOption({"viewport-height", "Fixed page viewport height for shell comparisons", "pixels"});
    parser.process(application);
    if (parser.value("output").isEmpty() && !parser.isSet("external-driver")) {
        parser.showHelp(2);
    }
    const int viewportWidth = parser.value("viewport-width").toInt();
    const int viewportHeight = parser.value("viewport-height").toInt();
    if (parser.isSet("viewport-width") != parser.isSet("viewport-height") ||
        (parser.isSet("viewport-width") &&
         (viewportWidth < 1 || viewportWidth > 8192 || viewportHeight < 1 || viewportHeight > 8192))) {
        parser.showHelp(2);
    }
    const bool isProbe = parser.isSet("probe-script");
    QString probeScript;
    if (isProbe) {
        QFile input(parser.value("probe-script"));
        if (!input.open(QIODevice::ReadOnly)) {
            qCritical("Could not read the DOM probe");
            return 2;
        }
        probeScript = QString::fromUtf8(input.readAll());
    }
    eden::platform::WaylandGlibEventDispatcher dispatcher;
    if (!WpeRuntime::initialize("Eden", "benchmark")) {
        return 2;
    }
    const quint64 thumbnailRequestsBefore = WpeRuntime::counters().value("thumbnailRequests").toULongLong();
    int result = 1;
    {
        EngineProfileParameters parameters;
        parameters.backend = Backend::Wpe;
        parameters.profileId = "benchmark";
        parameters.privateProfile = true;
        WpeProfile profile(parameters);
        if (!parser.value("training-directory").isEmpty()) {
            const QString training = QDir(parser.value("training-directory")).absolutePath();
            if (!QDir().mkpath(training)) {
                return 2;
            }
            webkit_web_context_add_path_to_sandbox(profile.context(), training.toUtf8().constData(), FALSE);
        }
        WpeEngineView view(&profile);
        const gulong terminationHandler = g_signal_connect(
            view.nativeView(),
            "web-process-terminated",
            G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason reason, gpointer data) {
                qCritical("The benchmark renderer terminated: %d", int(reason));
                static_cast<QCoreApplication *>(data)->exit(6);
            }),
            &application
        );
        QQuickWindow window;
        window.resize(1280, 900);
        QQuickItem viewport(window.contentItem());
        if (viewportWidth > 0) {
            viewport.setSize(QSizeF(viewportWidth, viewportHeight));
        }
        view.attach(viewportWidth > 0 ? &viewport : window.contentItem());
        window.show();
        if (isProbe && view.surface()) {
            window.requestActivate();
            view.surface()->forceActiveFocus(Qt::OtherFocusReason);
        }
        view.load(isProbe && !parser.isSet("url") ? QUrl("about:blank") : QUrl(parser.value("url")));
        QElapsedTimer elapsed;
        elapsed.start();
        QTimer poll;
        poll.setInterval(isProbe ? 100 : 5000);
        bool started = false;
        bool pending = false;
        QObject::connect(&poll, &QTimer::timeout, &application, [&] {
            if (elapsed.elapsed() > 1800000 || (!started && elapsed.elapsed() > 60000)) {
                qCritical("Speedometer timed out");
                application.exit(3);
                return;
            }
            if (pending || view.isLoading()) {
                return;
            }
            pending = true;
            QString script = started ? QStringLiteral(R"JS((() => ({
                    complete: benchmarkClient._hasResults,
                    finished: benchmarkClient._finishedTestCount,
                    results: benchmarkClient._hasResults ? {
                        score: benchmarkClient._computeResults(benchmarkClient._measuredValuesList, "score"),
                        fullJSON: benchmarkClient._formattedJSONResult({modern: true}),
                        measuredValues: benchmarkClient._measuredValuesList,
                        userAgent: navigator.userAgent,
                        viewport: [innerWidth, innerHeight, devicePixelRatio],
                        visibility: document.visibilityState,
                        timeOrigin: performance.timeOrigin,
                        timings: %1 ? performance.getEntriesByType('measure').map(entry => ({
                            name: entry.name, startTime: entry.startTime, duration: entry.duration
                        })) : null
                    } : null
                }))())JS")
                                           .arg(parser.isSet("timings") ? "true" : "false")
                                     : QStringLiteral(R"JS((() => {
                    const button = document.querySelector('.start-tests-button');
                    if (document.readyState !== 'complete' || !button) { return false; }
                    button.click();
                    return true;
                })())JS");
            if (isProbe) {
                script = started ? QStringLiteral("globalThis.__edenProbeState") : probeScript + QStringLiteral(R"JS(
                    globalThis.__edenProbeState = {complete: false};
                    Promise.resolve().then(() => edenDOMProbe()).then(
                        results => globalThis.__edenProbeState = {complete: true, results},
                        error => globalThis.__edenProbeState = {complete: true, error: String(error.stack || error)}
                    );
                    true;
                )JS");
            }
            view.evaluate(
                script,
                [&](QVariant value) {
                    pending = false;
                    if (!started) {
                        started = value.toBool();
                        if (started) {
                            qInfo("%s started", isProbe ? "DOM probe" : "Speedometer");
                        }
                        return;
                    }
                    const QVariantMap state = value.toMap();
                    if (!isProbe) {
                        qInfo("Speedometer progress: %d", state.value("finished").toInt());
                    }
                    if (state.contains("error")) {
                        qCritical().noquote() << state.value("error").toString();
                        application.exit(5);
                        return;
                    }
                    if (!state.value("complete").toBool()) {
                        return;
                    }
                    poll.stop();
                    QVariantMap results = state.value("results").toMap();
                    results.insert("elapsedMilliseconds", elapsed.elapsed());
                    results.insert("displaySyncPulses", qulonglong(WpeRuntime::displayPulseCount()));
                    results.insert(
                        "thumbnailRequests",
                        WpeRuntime::counters().value("thumbnailRequests").toULongLong() - thumbnailRequestsBefore
                    );
                    results.insert("rendererPageBackingAfterCompletion", rendererPageBacking(view.rendererProcessId()));
                    results.insert("label", parser.value("label"));
                    results.insert("qtVersion", QString::fromLatin1(qVersion()));
                    results.insert("runtime", WpeRuntime::diagnostics());
                    results.insert(
                        "webkitVersion",
                        QStringLiteral("%1.%2.%3")
                            .arg(webkit_get_major_version())
                            .arg(webkit_get_minor_version())
                            .arg(webkit_get_micro_version())
                    );
                    results.insert("accelerated", view.surface() && view.surface()->accelerated());
                    results.insert("textureImports", view.surface() ? view.surface()->textureImportCount() : 0);
                    results.insert("explicitSync", bool(wpe_display_use_explicit_sync(WpeRuntime::display())));
                    results.insert("jitPolicyScale", qEnvironmentVariable("JSC_jitPolicyScale"));
                    results.insert("gpuPaintingThreads", qEnvironmentVariable("WEBKIT_SKIA_GPU_PAINTING_THREADS"));
                    QVariantMap runtimeOptions;
                    for (const char *name :
                         {"JSC_jitPolicyScale",
                          "JSC_forceUnlinkedDFG",
                          "JSC_thresholdForFTLOptimizeAfterWarmUp",
                          "WEBKIT_EDEN_DISABLE_CANVAS_THREAD",
                          "WEBKIT_EDEN_SIMPLE_TEXT",
                          "WEBKIT_EDEN_CANVAS_DAMAGE",
                          "WEBKIT_EDEN_CANVAS_CPU_PIXELS",
                          "WEBKIT_EDEN_CODE_CACHE_TRACE",
                          "JSC_useCodeCache",
                          "JSC_minNumberOfWorklistThreads",
                          "JSC_maxNumberOfWorklistThreads",
                          "JSC_numberOfBaselineCompilerThreads",
                          "JSC_numberOfDFGCompilerThreads",
                          "JSC_numberOfFTLCompilerThreads",
                          "WEBKIT_SKIA_GPU_PAINTING_THREADS",
                          "WEBKIT_EDEN_JSC_ASSIGN_CLONE",
                          "WEBKIT_EDEN_JSC_REST_CLONE",
                          "WEBKIT_EDEN_JSC_REGEXP_ALTERNATIVES",
                          "WEBKIT_EDEN_EDITOR_WINDOW",
                          "WEBKIT_EDEN_EARLY_SCENE_COMPLETION",
                          "WEBKIT_EDEN_HELPER_NICE",
                          "WEBKIT_EDEN_TEXT_HUGE_PAGES",
                          "WEBKIT_EDEN_JIT_HUGE_PAGES",
                          "WEBKIT_EDEN_REUSE_SCALED_FONT",
                          "WEBKIT_EDEN_PAINT_PARTITION_RATIO",
                          "WEBKIT_EDEN_EXPERIMENT_TRACE",
                          "EDEN_WPE_DISPLAY_SYNC",
                          "WEBKIT_SKIA_CANVAS_REPLAY_THREADS",
                          "WEBKIT_SKIA_CANVAS_REPLAY_BATCH",
                          "WEBKIT_SKIA_CANVAS_REPLAY_TRACE",
                          "WEBKIT_SKIA_CPU_PAINTING_THREADS",
                          "WEBKIT_SKIA_MSAA_SAMPLE_COUNT",
                          "WEBKIT_SKIA_ENABLE_DYNAMIC_MSAA",
                          "WEBKIT_SKIA_ENABLE_DDL",
                          "WEBKIT_SKIA_ENABLE_CPU_RENDERING",
                          "WEBKIT_A11Y_BUS_ADDRESS",
                          "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS",
                          "EDEN_WPE_FRAME_ACK",
                          "EDEN_WPE_CANVAS_ACCELERATION",
                          "EDEN_WPE_EXPLICIT_SYNC"}) {
                        if (qEnvironmentVariableIsSet(name)) {
                            runtimeOptions.insert(QString::fromLatin1(name), qEnvironmentVariable(name));
                        }
                    }
                    results.insert("runtimeOptions", runtimeOptions);
                    if (parser.isSet("timings")) {
                        const auto before = std::chrono::steady_clock::now();
                        const auto wall = std::chrono::system_clock::now();
                        const auto after = std::chrono::steady_clock::now();
                        results.insert(
                            "clockCorrelation",
                            QVariantMap{
                                {"wallMilliseconds",
                                 std::chrono::duration<double, std::milli>(wall.time_since_epoch()).count()},
                                {"monotonicMilliseconds",
                                 std::chrono::duration<double, std::milli>(
                                     (before + (after - before) / 2).time_since_epoch()
                                 )
                                     .count()},
                                {"samplingWindowMilliseconds",
                                 std::chrono::duration<double, std::milli>(after - before).count()}
                            }
                        );
                    }
                    QSaveFile output(parser.value("output"));
                    const QByteArray json = QJsonDocument::fromVariant(results).toJson();
                    if (!output.open(QIODevice::WriteOnly) || output.write(json) != json.size() || !output.commit()) {
                        qCritical("Could not save benchmark results");
                        application.exit(4);
                        return;
                    }
                    qInfo().noquote()
                        << QJsonDocument::fromVariant(results.value("score")).toJson(QJsonDocument::Compact);
                    const bool valid = !results.value("thumbnailRequests").toULongLong() &&
                                       results.value("accelerated").toBool() &&
                                       results.value("textureImports").toULongLong() > 0 &&
                                       (isProbe ? !results.value("probe").toString().isEmpty() &&
                                                      !results.value("measurements").toMap().isEmpty()
                                                : results.value("score").toMap().value("isValid").toBool() &&
                                                      results.value("measuredValues").toList().size() == 10) &&
                                       results.value("visibility").toString() == QStringLiteral("visible");
                    application.exit(valid ? 0 : 5);
                },
                nullptr
            );
        });
        if (!parser.isSet("external-driver")) {
            poll.start();
        }
        result = application.exec();
        g_signal_handler_disconnect(view.nativeView(), terminationHandler);
        view.attach(nullptr);
    }
    if (!parser.value("training-directory").isEmpty()) {
        QEventLoop profileFlush;
        QTimer::singleShot(1500, &profileFlush, &QEventLoop::quit);
        profileFlush.exec();
    }
    WpeRuntime::shutdown();
    return result;
}
