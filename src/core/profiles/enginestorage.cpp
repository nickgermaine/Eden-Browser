#include "core/profiles/enginestorage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>

#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")
#include <sodium.h>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace eden::core {

    namespace {

        const SecretSchema storageSchema = {
            "browser.eden.engine-storage",
            SECRET_SCHEMA_NONE,
            {{"root", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}
        };

        void wipe(QByteArray &bytes) {
            sodium_memzero(bytes.data(), static_cast<size_t>(bytes.capacity()));
            bytes.clear();
        }

        bool safeDirectory(const QString &path, bool create) {
            const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
            QString current = clean;
            while (current != QLatin1String("/")) {
                const QFileInfo entry(current);
                if (entry.isSymbolicLink() || (entry.exists() && !entry.isDir())) {
                    return false;
                }
                current = entry.absolutePath();
            }
            if (create && !QDir().mkpath(clean)) {
                return false;
            }
            const QFileInfo entry(clean);
            return entry.isDir() && entry.ownerId() == static_cast<uint>(getuid());
        }

        bool syncDirectory(const QString &path) {
            const int descriptor =
                open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (descriptor < 0) {
                return false;
            }
            const bool success = fsync(descriptor) == 0;
            close(descriptor);
            return success;
        }

        bool phaseFile(const QString &path, const QByteArray &phase) {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
                file.write(phase) != phase.size() || !file.commit()) {
                return false;
            }
            return syncDirectory(QFileInfo(path).absolutePath());
        }

        QByteArray readKey(const QString &root, bool existing) {
            const QByteArray identity = QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex();
            GError *error = nullptr;
            gchar *stored =
                secret_password_lookup_sync(&storageSchema, nullptr, &error, "root", identity.constData(), nullptr);
            if (error) {
                g_error_free(error);
                if (stored) {
                    secret_password_free(stored);
                }
                return {};
            }
            if (stored) {
                QByteArray encoded(stored);
                secret_password_free(stored);
                auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
                wipe(encoded);
                if (!decoded || decoded.decoded.size() != 32) {
                    wipe(decoded.decoded);
                    return {};
                }
                return std::move(decoded.decoded);
            }
            if (existing) {
                return {};
            }
            QByteArray key(32, Qt::Uninitialized);
            randombytes_buf(key.data(), 32);
            QByteArray encoded = key.toBase64();
            const bool saved = secret_password_store_sync(
                &storageSchema,
                SECRET_COLLECTION_DEFAULT,
                "Eden encrypted site storage",
                encoded.constData(),
                nullptr,
                &error,
                "root",
                identity.constData(),
                nullptr
            );
            wipe(encoded);
            if (error) {
                g_error_free(error);
            }
            if (!saved) {
                wipe(key);
            }
            return key;
        }

        bool sameFile(const QString &left, const QString &right) {
            QFile source(left);
            QFile destination(right);
            if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::ReadOnly) ||
                source.size() != destination.size()) {
                return false;
            }
            while (!source.atEnd()) {
                QByteArray a = source.read(1024 * 1024);
                QByteArray b = destination.read(a.size());
                const bool equal = !a.isEmpty() && a == b;
                wipe(a);
                wipe(b);
                if (!equal) {
                    return false;
                }
            }
            return source.error() == QFile::NoError && destination.error() == QFile::NoError;
        }

        bool copyVerified(const QString &sourcePath, const QString &destinationPath) {
            const QFileInfo source(sourcePath);
            const QFileInfo destination(destinationPath);
            if (source.isSymbolicLink() &&
                (source.fileName() == "SingletonLock" || source.fileName() == "SingletonCookie" ||
                 source.fileName() == "SingletonSocket")) {
                return true;
            }
            if (source.isSymbolicLink() || destination.isSymbolicLink()) {
                return false;
            }
            if (source.isDir()) {
                const int sourceDescriptor =
                    open(QFile::encodeName(sourcePath).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (sourceDescriptor < 0) {
                    return false;
                }
                DIR *directory = fdopendir(sourceDescriptor);
                if (!directory) {
                    close(sourceDescriptor);
                    return false;
                }
                const auto closeDirectory = qScopeGuard([directory] { closedir(directory); });
                if (!safeDirectory(destinationPath, true) || !ProfilePaths::restrictDirectory(destinationPath)) {
                    return false;
                }
                for (;;) {
                    errno = 0;
                    const dirent *entry = readdir(directory);
                    if (!entry) {
                        if (errno != 0) {
                            return false;
                        }
                        break;
                    }
                    const QByteArray name(entry->d_name);
                    if (name == "." || name == "..") {
                        continue;
                    }
                    const QString fileName = QFile::decodeName(name);
                    if (!copyVerified(sourcePath + '/' + fileName, destinationPath + '/' + fileName)) {
                        return false;
                    }
                }
                return syncDirectory(destinationPath);
            }
            if (!source.isFile() || source.ownerId() != static_cast<uint>(getuid())) {
                return false;
            }
            if (destination.exists() && sameFile(sourcePath, destinationPath)) {
                return true;
            }
            const int input = open(QFile::encodeName(sourcePath).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (input < 0) {
                return false;
            }
            const auto closeInput = qScopeGuard([input] { close(input); });
            struct stat before{};
            if (fstat(input, &before) != 0 || !S_ISREG(before.st_mode)) {
                return false;
            }
            QFile reader;
            if (!reader.open(input, QIODevice::ReadOnly)) {
                return false;
            }
            QSaveFile writer(destinationPath);
            if (!writer.open(QIODevice::WriteOnly) || !writer.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
                return false;
            }
            while (!reader.atEnd()) {
                QByteArray bytes = reader.read(1024 * 1024);
                const bool written = !bytes.isEmpty() && writer.write(bytes) == bytes.size();
                wipe(bytes);
                if (!written) {
                    return false;
                }
            }
            struct stat after{};
            if (reader.error() != QFile::NoError || fstat(input, &after) != 0 || before.st_size != after.st_size ||
                before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
                !writer.commit() || !sameFile(sourcePath, destinationPath)) {
                return false;
            }
            return true;
        }

        bool verifiedMount(const QString &view, const QString &cipher) {
            const QStorageInfo storage(view);
            return storage.isValid() && storage.isReady() && storage.rootPath() == view &&
                   storage.fileSystemType() == "fuse.gocryptfs" && storage.device() == QFile::encodeName(cipher);
        }

        int existingMount(const QString &view, const QString &cipher) {
            QFile mounts(QStringLiteral("/proc/self/mountinfo"));
            if (!mounts.open(QIODevice::ReadOnly)) {
                return -1;
            }
            const auto decode = [](QByteArray value) {
                value.replace("\\040", " ");
                value.replace("\\011", "\t");
                value.replace("\\012", "\n");
                value.replace("\\134", "\\");
                return value;
            };
            const auto lines = mounts.readAll().split('\n');
            if (mounts.error() != QFile::NoError) {
                return -1;
            }
            for (const QByteArray &line : lines) {
                const qsizetype separator = line.indexOf(" - ");
                if (separator < 0) {
                    continue;
                }
                const auto fields = line.left(separator).split(' ');
                if (fields.size() < 5 || decode(fields[4]) != QFile::encodeName(view)) {
                    continue;
                }
                const auto type = line.mid(separator + 3).split(' ');
                return type.size() >= 2 && type[0] == "fuse.gocryptfs" && decode(type[1]) == QFile::encodeName(cipher)
                           ? 1
                           : -1;
            }
            return 0;
        }

    }

    class EngineStorage::Private {
      public:
        struct Mount {
            QString view;
            QString cipher;
            QProcess *process = nullptr;
            int underlying = -1;
        };

        explicit Private(EngineStorage *owner)
            : q(owner),
              worker(new QObject) {
            worker->moveToThread(&thread);
            QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
            thread.start();
        }

        bool startProcess(QProcess &process, const QStringList &arguments, const QByteArray &password) {
            process.setProgram(QCoreApplication::applicationDirPath() + QStringLiteral("/eden-gocryptfs"));
            process.setArguments(arguments);
            process.setStandardOutputFile(QProcess::nullDevice());
            process.setStandardErrorFile(QProcess::nullDevice());
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("GOMAXPROCS", "2");
            process.setProcessEnvironment(environment);
            process.start();
            if (!process.waitForStarted(10000) || process.write(password) != password.size()) {
                return false;
            }
            process.closeWriteChannel();
            return true;
        }

        QString mountTree(const QString &view, const QString &purpose, const QByteArray &key) {
            const QString cipher = view + QStringLiteral(".encrypted");
            const QString pending = view + QStringLiteral(".plaintext-migration");
            const QString journal = view + QStringLiteral(".encryption-state");
            if (!safeDirectory(QFileInfo(view).absolutePath(), true) || QFileInfo(view).isSymbolicLink() ||
                QFileInfo(cipher).isSymbolicLink() || QFileInfo(pending).isSymbolicLink() ||
                QFileInfo(journal).isSymbolicLink()) {
                return QStringLiteral("Site storage contains an unsafe directory or link.");
            }
            QFile phase(journal);
            QByteArray state;
            if (phase.exists()) {
                if (!phase.open(QIODevice::ReadOnly) || phase.size() > 32) {
                    return QStringLiteral("The site storage migration state cannot be read.");
                }
                state = phase.readAll();
                if (state != "migrate" && state != "ready") {
                    return QStringLiteral("The site storage migration state is invalid.");
                }
            } else if (QFileInfo::exists(cipher) || QFileInfo::exists(pending)) {
                return QStringLiteral(
                    "Encrypted site storage exists without its migration state. The original files were preserved."
                );
            } else if (!phaseFile(journal, "migrate")) {
                return QStringLiteral("The site storage migration state could not be saved.");
            }
            if (state == "ready" && QFileInfo::exists(pending)) {
                return QStringLiteral("A previous site storage migration needs recovery.");
            }
            if (!safeDirectory(cipher, true) || !ProfilePaths::restrictDirectory(cipher)) {
                return QStringLiteral("The encrypted site storage directory is unavailable.");
            }
            QByteArray password(32, Qt::Uninitialized);
            const QByteArray context = purpose.toUtf8();
            crypto_generichash(
                reinterpret_cast<unsigned char *>(password.data()),
                32,
                reinterpret_cast<const unsigned char *>(context.constData()),
                context.size(),
                reinterpret_cast<const unsigned char *>(key.constData()),
                key.size()
            );
            QByteArray encoded = password.toHex() + '\n';
            wipe(password);
            const auto clearPassword = qScopeGuard([&encoded] { wipe(encoded); });
            if (!QFileInfo::exists(cipher + "/gocryptfs.conf")) {
                if (state == "ready" || !QDir(cipher).isEmpty()) {
                    return QStringLiteral("The encrypted site storage configuration is missing.");
                }
                QProcess initialize;
                if (!startProcess(initialize, {"-q", "-init", "-passfile", "/dev/stdin", cipher}, encoded) ||
                    !initialize.waitForFinished(30000) || initialize.exitStatus() != QProcess::NormalExit ||
                    initialize.exitCode() != 0) {
                    return QStringLiteral(
                        "Encrypted site storage could not be created. Check that the encryption helper is installed."
                    );
                }
                if (!syncDirectory(cipher)) {
                    return QStringLiteral("Encrypted site storage could not be saved to disk.");
                }
            }
            const int mounted = existingMount(view, cipher);
            if (mounted < 0) {
                return QStringLiteral("The site storage mount could not be identified safely.");
            }
            if (mounted > 0) {
                QProcess unmount;
                unmount.start("fusermount3", {"-u", view});
                if (!unmount.waitForFinished(10000) || unmount.exitCode() != 0) {
                    return QStringLiteral(
                        "Site storage is still mounted by another process. Close other Eden processes and retry."
                    );
                }
            }
            if (QFileInfo::exists(view)) {
                if (!safeDirectory(view, false) || !ProfilePaths::restrictDirectory(view)) {
                    return QStringLiteral("The site storage mount directory is unavailable.");
                }
                if (!QDir(view).isEmpty()) {
                    if (state == "ready" || QFileInfo::exists(pending) || !QDir().rename(view, pending) ||
                        !syncDirectory(QFileInfo(view).absolutePath())) {
                        return QStringLiteral(
                            "Existing site storage could not be prepared for encryption. The files were preserved."
                        );
                    }
                }
            }
            if (!safeDirectory(view, true) || !ProfilePaths::restrictDirectory(view)) {
                return QStringLiteral("The encrypted site storage mount could not be prepared.");
            }
            Mount mount{
                view,
                cipher,
                new QProcess(worker),
                open(QFile::encodeName(view).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
            };
            if (mount.underlying < 0) {
                delete mount.process;
                return QStringLiteral("The site storage mount directory cannot be secured.");
            }
            mounts.append(mount);
            if (!startProcess(
                    *mount.process,
                    {"-q", "-fg", "-passfile", "/dev/stdin", "-fsname", cipher, cipher, view},
                    encoded
                )) {
                return QStringLiteral("The site storage encryption helper could not start.");
            }
            QElapsedTimer timer;
            timer.start();
            while (!verifiedMount(view, cipher) && timer.elapsed() < 30000 &&
                   mount.process->state() != QProcess::NotRunning) {
                mount.process->waitForFinished(25);
            }
            if (!verifiedMount(view, cipher) || fchmod(mount.underlying, 0) != 0) {
                return QStringLiteral(
                    "Encrypted site storage could not be unlocked. Check the keyring and FUSE installation."
                );
            }
            if (QFileInfo::exists(pending)) {
                if (!safeDirectory(pending, false) || !copyVerified(pending, view) || !syncDirectory(view)) {
                    return QStringLiteral(
                        "Site storage encryption did not verify. The original files were preserved for retry."
                    );
                }
                if (!QDir(pending).removeRecursively() || !syncDirectory(QFileInfo(pending).absolutePath())) {
                    return QStringLiteral(
                        "Encryption verified, but the old site storage could not be removed. Retry to finish cleanup."
                    );
                }
            }
            if (!phaseFile(journal, "ready")) {
                return QStringLiteral("The completed encryption migration could not be recorded.");
            }
            QObject::connect(mount.process, &QProcess::finished, worker, [this](int, QProcess::ExitStatus) {
                if (!stopping) {
                    {
                        const QMutexLocker lock(&mutex);
                        protectedRoots.clear();
                    }
                    emit q->storageFailed(QStringLiteral(
                        "Encrypted site storage disconnected. Quit Eden and reopen it to restore access."
                    ));
                }
            });
            return {};
        }

        void stop() {
            stopping = true;
            {
                const QMutexLocker lock(&mutex);
                protectedRoots.clear();
            }
            for (Mount &mount : mounts) {
                if (mount.underlying >= 0) {
                    fchmod(mount.underlying, 0);
                }
                QProcess unmount;
                unmount.setStandardErrorFile(QProcess::nullDevice());
                unmount.start("fusermount3", {"-u", mount.view});
                unmount.waitForFinished(10000);
                if (mount.process->state() != QProcess::NotRunning) {
                    mount.process->terminate();
                    if (!mount.process->waitForFinished(5000)) {
                        mount.process->kill();
                        mount.process->waitForFinished(5000);
                    }
                }
                delete mount.process;
                if (mount.underlying >= 0) {
                    close(mount.underlying);
                }
            }
            mounts.clear();
            storageLock.reset();
            stopping = false;
        }

        QString prepare(const ProfilePaths::Roots &roots) {
            if (sodium_init() < 0) {
                return QStringLiteral("The encryption library could not initialize.");
            }
            const QString data = QDir::cleanPath(QFileInfo(roots.dataRoot).absoluteFilePath());
            const QString cache = QDir::cleanPath(QFileInfo(roots.cacheRoot).absoluteFilePath());
            {
                const QMutexLocker lock(&mutex);
                if (protectedRoots.contains(data + "/engine-data") && protectedRoots.contains(cache + "/profiles")) {
                    return {};
                }
            }
            if (!mounts.isEmpty()) {
                return QStringLiteral("A different site storage directory is already open.");
            }
            if (!safeDirectory(data, true) || !safeDirectory(cache, true) || !ProfilePaths::restrictDirectory(data) ||
                !ProfilePaths::restrictDirectory(cache)) {
                return QStringLiteral("Site storage directories could not be secured.");
            }
            storageLock = std::make_unique<QLockFile>(data + "/engine-storage.lock");
            if (!storageLock->tryLock()) {
                return QStringLiteral("Another Eden process owns the encrypted site storage.");
            }
            QByteArray key = readKey(
                data,
                QFileInfo::exists(data + "/engine-data.encrypted") || QFileInfo::exists(cache + "/profiles.encrypted")
            );
            if (key.size() != 32) {
                return QStringLiteral(
                    "The keyring could not unlock site storage. Existing keys and files were preserved."
                );
            }
            sodium_mlock(key.data(), static_cast<size_t>(key.size()));
            const auto clearKey = qScopeGuard([&key] {
                sodium_munlock(key.data(), static_cast<size_t>(key.size()));
                wipe(key);
            });
            QString error = mountTree(data + "/engine-data", "eden-site-data-v1", key);
            if (error.isEmpty()) {
                error = mountTree(cache + "/profiles", "eden-site-cache-v1", key);
            }
            QList<QPair<QString, QString>> legacy{
                {data + "/webengine", "eden-legacy-qt-data-v1"},
                {cache + "/webengine", "eden-legacy-qt-cache-v1"}
            };
            const auto standard = ProfilePaths::standardRoots();
            if (data == QDir::cleanPath(standard.dataRoot)) {
                legacy.append(
                    {QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/eden/cef",
                     "eden-legacy-cef-v1"}
                );
            }
            for (const auto &entry : legacy) {
                if (error.isEmpty() &&
                    (QFileInfo::exists(entry.first) || QFileInfo::exists(entry.first + ".encryption-state"))) {
                    error = mountTree(entry.first, entry.second, key);
                }
            }
            if (!error.isEmpty()) {
                stop();
                return error;
            }
            const QMutexLocker lock(&mutex);
            for (const Mount &mount : mounts) {
                protectedRoots.insert(mount.view, mount.cipher);
            }
            return {};
        }

        EngineStorage *q;
        QThread thread;
        QObject *worker;
        QList<Mount> mounts;
        std::unique_ptr<QLockFile> storageLock;
        mutable QMutex mutex;
        QHash<QString, QString> protectedRoots;
        bool stopping = false;
    };

    EngineStorage *EngineStorage::instance() {
        static EngineStorage storage;
        return &storage;
    }

    EngineStorage::EngineStorage(QObject *parent)
        : QObject(parent),
          d(std::make_unique<Private>(this)) {}

    EngineStorage::~EngineStorage() {
        shutdown();
        d->thread.quit();
        d->thread.wait();
    }

    void EngineStorage::prepare(const ProfilePaths::Roots &roots, Completion completion) {
        QMetaObject::invokeMethod(
            d->worker,
            [this, roots, completion = std::move(completion)] {
                const QString error = d->prepare(roots);
                QMetaObject::invokeMethod(
                    this,
                    [completion, error] {
                        if (completion) {
                            completion(error);
                        }
                    },
                    Qt::QueuedConnection
                );
            },
            Qt::QueuedConnection
        );
    }

    bool EngineStorage::migrateLegacyPath(const QString &source, const QString &destination) {
        if (source == destination || !copyVerified(source, destination) ||
            !syncDirectory(QFileInfo(destination).absolutePath())) {
            return false;
        }
        bool removed = QFileInfo(source).isDir() ? QDir(source).removeRecursively() : QFile::remove(source);
        if (!removed && QStorageInfo(source).rootPath() == source && QDir(source).isEmpty()) {
            removed = true;
        }
        return removed && syncDirectory(QFileInfo(source).absolutePath());
    }

    bool EngineStorage::protects(const QString &path) const {
        const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        const QMutexLocker lock(&d->mutex);
        for (auto entry = d->protectedRoots.cbegin(); entry != d->protectedRoots.cend(); ++entry) {
            if ((clean == entry.key() || clean.startsWith(entry.key() + '/')) &&
                verifiedMount(entry.key(), entry.value())) {
                QString ancestor = clean;
                while (ancestor != entry.key()) {
                    if (QFileInfo(ancestor).isSymbolicLink()) {
                        return false;
                    }
                    ancestor = QFileInfo(ancestor).absolutePath();
                }
                return true;
            }
        }
        return false;
    }

    void EngineStorage::shutdown() {
        if (d->thread.isRunning()) {
            QMetaObject::invokeMethod(d->worker, [this] { d->stop(); }, Qt::BlockingQueuedConnection);
        }
    }

}
