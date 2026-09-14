#include "engine/wpe/wpeengineview.h"
#include "engine/wpe/wpeprofile.h"
#include "engine/wpe/wperuntime.h"
#include "engine/wpe/wpesurface.h"
#include "platform/waylandglibeventdispatcher.h"

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTimer>

using namespace eden::engine;

int main(int argc, char *argv[]) {
    qputenv("QT_NO_GLIB", "1");
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QCoreApplication::setApplicationName("EdenWebKitBenchmark");
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"url", "Benchmark page", "url", "https://browserbench.org/Speedometer3.1/"});
    parser.addOption({"output", "Result JSON path", "file"});
    parser.addOption({"label", "Build and tuning label", "name"});
    parser.addOption({"training-directory", "Directory for native PGO profiles", "path"});
    parser.process(application);
    if (parser.value("output").isEmpty()) {
        parser.showHelp(2);
    }
    eden::platform::WaylandGlibEventDispatcher dispatcher;
    if (!WpeRuntime::initialize("Eden", "benchmark")) {
        return 2;
    }
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
        view.attach(window.contentItem());
        window.show();
        view.load(QUrl(parser.value("url")));
        QElapsedTimer elapsed;
        elapsed.start();
        QTimer poll;
        poll.setInterval(5000);
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
            const QString script = started ? QStringLiteral(R"JS((() => ({
                    complete: benchmarkClient._hasResults,
                    finished: benchmarkClient._finishedTestCount,
                    results: benchmarkClient._hasResults ? {
                        score: benchmarkClient._computeResults(benchmarkClient._measuredValuesList, "score"),
                        fullJSON: benchmarkClient._formattedJSONResult({modern: true}),
                        measuredValues: benchmarkClient._measuredValuesList,
                        userAgent: navigator.userAgent,
                        viewport: [innerWidth, innerHeight, devicePixelRatio],
                        visibility: document.visibilityState
                    } : null
                }))())JS")
                                           : QStringLiteral(R"JS((() => {
                    const button = document.querySelector('.start-tests-button');
                    if (document.readyState !== 'complete' || !button) { return false; }
                    button.click();
                    return true;
                })())JS");
            view.evaluate(
                script,
                [&](QVariant value) {
                    pending = false;
                    if (!started) {
                        started = value.toBool();
                        if (started) {
                            qInfo("Speedometer started");
                        }
                        return;
                    }
                    const QVariantMap state = value.toMap();
                    qInfo("Speedometer progress: %d", state.value("finished").toInt());
                    if (!state.value("complete").toBool()) {
                        return;
                    }
                    poll.stop();
                    QVariantMap results = state.value("results").toMap();
                    results.insert("elapsedMilliseconds", elapsed.elapsed());
                    results.insert("label", parser.value("label"));
                    results.insert("qtVersion", QString::fromLatin1(qVersion()));
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
                          "JSC_minNumberOfWorklistThreads",
                          "JSC_maxNumberOfWorklistThreads",
                          "JSC_numberOfBaselineCompilerThreads",
                          "JSC_numberOfDFGCompilerThreads",
                          "JSC_numberOfFTLCompilerThreads",
                          "WEBKIT_SKIA_GPU_PAINTING_THREADS",
                          "WEBKIT_SKIA_CPU_PAINTING_THREADS",
                          "EDEN_WPE_EXPLICIT_SYNC"}) {
                        if (qEnvironmentVariableIsSet(name)) {
                            runtimeOptions.insert(QString::fromLatin1(name), qEnvironmentVariable(name));
                        }
                    }
                    results.insert("runtimeOptions", runtimeOptions);
                    QSaveFile output(parser.value("output"));
                    const QByteArray json = QJsonDocument::fromVariant(results).toJson();
                    if (!output.open(QIODevice::WriteOnly) || output.write(json) != json.size() || !output.commit()) {
                        qCritical("Could not save benchmark results");
                        application.exit(4);
                        return;
                    }
                    qInfo().noquote()
                        << QJsonDocument::fromVariant(results.value("score")).toJson(QJsonDocument::Compact);
                    const bool valid = results.value("accelerated").toBool() &&
                                       results.value("textureImports").toULongLong() > 0 &&
                                       results.value("score").toMap().value("isValid").toBool() &&
                                       results.value("measuredValues").toList().size() == 10 &&
                                       results.value("visibility").toString() == QStringLiteral("visible");
                    application.exit(valid ? 0 : 5);
                },
                nullptr
            );
        });
        poll.start();
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
