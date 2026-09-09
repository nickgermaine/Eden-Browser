#include "engine/enginefactory.h"
#include "core/profiles/profilepaths.h"
#include "engine/engineplugin.h"
#include "engine/engineprofile.h"
#include "engine/engineregistry.h"
#include "engine/engineview.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>

#include <map>
#include <vector>

namespace eden::engine {

    struct BackendModule {
        std::unique_ptr<QLibrary> library;
        const EnginePluginApi *api = nullptr;
        bool prepared = false;
        bool initialized = false;
    };

    struct EngineFactoryState {
        QList<QByteArray> arguments;
        std::vector<char *> argumentPointers;
        std::map<Backend, BackendModule> modules;
    };

    static EngineFactoryState &factoryState() {
        static EngineFactoryState state;
        return state;
    }

    static QString moduleName(Backend backend) {
        if (backend == Backend::Cef) {
            return "eden-engine-cef";
        }
        if (backend == Backend::QtWebEngine) {
            return "eden-engine-qtwebengine";
        }
        return {};
    }

    static QString executableDirectory() {
        if (QCoreApplication::instance()) {
            return QCoreApplication::applicationDirPath();
        }
        const QString executablePath = QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget();
        return executablePath.isEmpty() ? QString() : QFileInfo(executablePath).absolutePath();
    }

    static BackendModule *loadBackendModule(Backend backend) {
        EngineFactoryState &state = factoryState();
        BackendModule &module = state.modules[backend];
        if (module.library) {
            return &module;
        }
        const QString name = moduleName(backend);
        const QString directory = executableDirectory();
        if (name.isEmpty() || directory.isEmpty()) {
            return nullptr;
        }
        const QString path = QDir(directory).filePath(name);
        module.library = std::make_unique<QLibrary>(path);
        module.library->setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
        if (!module.library->load()) {
            qCritical().noquote() << "The engine module could not be loaded:" << module.library->errorString();
            module.library.reset();
            return nullptr;
        }
        const auto resolve = reinterpret_cast<ResolveEnginePlugin>(module.library->resolve("eden_engine_plugin"));
        module.api = resolve ? resolve() : nullptr;
        if (!module.api || module.api->abiVersion != enginePluginAbiVersion || !module.api->initialize ||
            !module.api->createView || !module.api->createProfile || !module.api->shutdown) {
            qCritical().noquote() << "The engine module has an incompatible interface:" << path;
            module.api = nullptr;
            module.library.reset();
            return nullptr;
        }
        return &module;
    }

    void EngineFactory::configureApplicationArguments(int argc, char *argv[]) {
        EngineFactoryState &state = factoryState();
        state.arguments.clear();
        state.argumentPointers.clear();
        state.arguments.reserve(argc);
        state.argumentPointers.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            state.arguments.append(argv && argv[index] ? QByteArray(argv[index]) : QByteArray());
        }
        for (QByteArray &argument : state.arguments) {
            state.argumentPointers.push_back(argument.data());
        }
    }

    bool EngineFactory::prepareApplication(Backend backend) {
        EngineFactoryState &state = factoryState();
        BackendModule *module = loadBackendModule(backend);
        if (!module) {
            return false;
        }
        if (module->prepared) {
            return true;
        }
        if (state.arguments.isEmpty()) {
            return false;
        }
        if (module->api->prepareApplication && !module->api->prepareApplication(
                                                   static_cast<int>(state.argumentPointers.size()),
                                                   state.argumentPointers.data()
                                               )) {
            return false;
        }
        module->prepared = true;
        return true;
    }

    bool EngineFactory::initialize(Backend backend) {
        if (!prepareApplication(backend)) {
            return false;
        }
        EngineFactoryState &state = factoryState();
        BackendModule &module = state.modules[backend];
        if (module.initialized) {
            return true;
        }

        const QByteArray product{"Eden"};
        const QByteArray version = QCoreApplication::applicationVersion().toUtf8();
        const QString backendId = EngineRegistry::instance()->idForBackend(backend);
        const QByteArray engineDataRoot =
            core::ProfilePaths::engineDataDirectory(core::ProfilePaths::standardRoots(), backendId).toUtf8();
        const EngineIdentity identity{
            .product = product.constData(),
            .version = version.constData(),
            .engineDataRoot = engineDataRoot.constData(),
        };
        module.initialized = module.api->initialize(
            static_cast<int>(state.argumentPointers.size()),
            state.argumentPointers.data(),
            &identity
        );
        return module.initialized;
    }

    bool EngineFactory::initializeCef(int argc, char *argv[]) {
        configureApplicationArguments(argc, argv);
        return initialize(Backend::Cef);
    }

    bool EngineFactory::isBackendLoaded(Backend backend) {
        const EngineFactoryState &state = factoryState();
        const auto found = state.modules.find(backend);
        return found != state.modules.cend() && found->second.initialized;
    }

    void EngineFactory::shutdown() {
        EngineFactoryState &state = factoryState();
        for (auto &[backend, module] : state.modules) {
            Q_UNUSED(backend)
            if (module.initialized && module.api) {
                module.api->shutdown();
                module.initialized = false;
            }
        }
        state.modules.clear();
    }

    std::unique_ptr<EngineView> EngineFactory::create(Backend backend, EngineProfile *profile) {
        if (!initialize(backend)) {
            return nullptr;
        }
        const EnginePluginApi *api = factoryState().modules.at(backend).api;
        return std::unique_ptr<EngineView>(api->createView(profile));
    }

    std::shared_ptr<EngineProfile>
    EngineFactory::create(const EngineProfileParameters &parameters, QQmlEngine *engine) {
        if (!initialize(parameters.backend)) {
            return nullptr;
        }
        const EnginePluginApi *api = factoryState().modules.at(parameters.backend).api;
        return std::shared_ptr<EngineProfile>(api->createProfile(&parameters, engine, nullptr));
    }

}
