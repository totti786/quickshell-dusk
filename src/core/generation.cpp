#include "generation.hpp"
#include <atomic>
#include <utility>

#include <dlfcn.h>
#include <private/qqmlimport_p.h>
#include <qcontainerfwd.h>
#include <qcoreapplication.h>
#include <qdebug.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qfilesystemwatcher.h>
#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qqmlcontext.h>
#include <qqmlengine.h>
#include <qqmlerror.h>
#include <qqmlincubator.h>
#include <qquickwindow.h>
#include <qtmetamacros.h>

#include "iconimageprovider.hpp"
#include "imageprovider.hpp"
#include "incubator.hpp"
#include "logcat.hpp"
#include "memorytrim.hpp"
#include "plugin.hpp"
#include "qsintercept.hpp"
#include "reload.hpp"
#include "scan.hpp"
#include "paths.hpp"
#include <qbytearray.h>
#include <qfile.h>
#include <qqml.h>
#include <qset.h>
#include <qstringview.h>
#include <qtenvironmentvariables.h>
#include <qstringbuilder.h>

namespace {
QS_LOGGING_CATEGORY(logScene, "scene");
}


namespace {

void mirrorConfigDir(
    const QString& src,
    const QString& dst,
    const QString& rel,
    const QSet<QString>& qmlDirs,
    const QHash<QString, QString>& intercepts
) {
	QDir().mkpath(dst);
	constexpr auto filter = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;

	for (const auto& e: QDir(dst).entryInfoList(filter)) {
		if (e.isSymLink() || !e.isDir()) QFile::remove(e.filePath());
		else if (!qmlDirs.contains(rel.isEmpty() ? e.fileName() : rel % '/' % e.fileName())) {
			QDir(e.filePath()).removeRecursively();
		}
	}

	for (const auto& e: QDir(src).entryInfoList(filter)) {
		const auto name = e.fileName();
		const auto childRel = rel.isEmpty() ? name : rel % '/' % name;
		const auto s = src % '/' % name;
		const auto t = dst % '/' % name;

		if (e.isDir() && qmlDirs.contains(childRel)) {
			mirrorConfigDir(s, t, childRel, qmlDirs, intercepts);
		} else if (auto it = intercepts.constFind(s); it != intercepts.cend()) {
			QFile f(t);
			if (f.open(QFile::WriteOnly)) f.write(it->toUtf8());
		} else {
			QFile::link(s, t);
		}
	}

	const auto qmldir = src % "/qmldir";
	if (auto it = intercepts.constFind(qmldir); it != intercepts.cend() && !QFileInfo::exists(qmldir)) {
		QFile f(dst % "/qmldir");
		if (f.open(QFile::WriteOnly)) f.write(it->toUtf8());
	}
}

QString buildFileVfs(const QDir& rootPath, const QmlScanner& scanner) {
	auto* vfs = QsPaths::instance()->shellVfsDir();
	if (!vfs) return QString();

	const auto root = rootPath.path();
	QSet<QString> qmlDirs;
	auto addDirOf = [&](const QString& path) {
		if (!path.startsWith(root % '/')) return;
		auto rel = path.sliced(root.length() + 1);
		while (true) {
			auto i = rel.lastIndexOf('/');
			if (i == -1) break;
			rel = rel.first(i);
			if (qmlDirs.contains(rel)) break;
			qmlDirs.insert(rel);
		}
	};
	for (const auto& f: scanner.scannedFiles) addDirOf(f);
	for (auto it = scanner.fileIntercepts.cbegin(); it != scanner.fileIntercepts.cend(); ++it) {
		addDirOf(it.key());
	}

	const auto out = vfs->filePath("filevfs");
	mirrorConfigDir(root, out % "/qs", QString(), qmlDirs, scanner.fileIntercepts);
	return out;
}

// Register synthesized qs* modules with Qt before loading the root component.
// Without a registered QQmlTypeModule, Qt's finalizeLibraryImport runs
// matchingQmldirVersion (an O(n^2) duplicate check over qmldir components on
// every import). The synthesized qmldirs list types as version 1.0 and the
// imports are unversioned, so registering 1.0 lets Qt skip that check.
void registerSynthesizedModules(const QHash<QString, QString>& fileIntercepts) {
	static QSet<QByteArray> registeredUris;
	int registeredCount = 0;

	for (auto it = fileIntercepts.cbegin(); it != fileIntercepts.cend(); ++it) {
		if (!it.key().endsWith(QLatin1StringView("/qmldir"))) continue;

		const auto& text = it.value();
		if (!text.startsWith(QLatin1StringView("module "))) continue;

		const auto lineEnd = text.indexOf('\n');
		const auto firstLine = lineEnd == -1 ? QStringView(text) : QStringView(text).first(lineEnd);
		const auto uriView = firstLine.sliced(7).trimmed();
		if (uriView.isEmpty()) continue;

		const auto uri = uriView.toUtf8();
		if (registeredUris.contains(uri)) continue;

		// Keep the QByteArray alive in registeredUris since qmlRegisterModule takes const char*.
		const auto inserted = registeredUris.insert(uri);
		qmlRegisterModule(inserted->constData(), 1, 0);
		++registeredCount;
	}

	if (registeredCount > 0) {
		qCInfo(logScene) << "Registered" << registeredCount << "synthesized QML modules";
	}
}

QSet<QString> upperIdentifiersOf(const QByteArray& src) {
	QSet<QString> ids;
	const char* p = src.constData();
	const char* end = p + src.size();
	while (p < end) {
		const unsigned char c = *p;
		if (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
			const char* s = p;
			while (p < end) {
				const unsigned char d = *p;
				if (d == '_' || (d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') || (d >= '0' && d <= '9')) {
					++p;
				} else {
					break;
				}
			}
			// Singleton type names start upper-case; keep the set small.
			if (*s >= 'A' && *s <= 'Z') {
				ids.insert(QString::fromLatin1(s, p - s));
			}
		} else {
			++p;
		}
	}
	return ids;
}

} // namespace

// Interpose QQmlImports::resolvedCompositeSingletons() so each QML document
// only receives the composite singletons it actually names.
//
// Why: Qt's QQmlTypeData::resolveTypes() iterates over
// QQmlImports::resolvedCompositeSingletons() and, for every composite singleton
// of every imported module, resolves it, adding a dependency and a
// type-name-cache entry to the document. With ~200 singletons across ~2000
// documents that fans out to ~400k entries (~42 MB RAM) and forces every
// singleton to load at boot. libQt6Qml calls this symbol through the PLT, so
// defining it here (always linked into the quickshell executable) and exporting
// it with --export-dynamic-symbol lets us interpose it and delegate via dlsym.
//
// Filtering & fail-open: Scans the document source for ASCII identifiers
// [A-Za-z_][A-Za-z0-9_]* starting with an upper-case letter (including comments
// and string literals; over-keeping is safe) and drops entries whose typeName is
// absent. Fails open (returns the unfiltered list) when QS_NO_SINGLETON_FILTER
// is set, the list is empty, baseUrl() is not a local file, or the file cannot
// be read.
//
// Known limit: A singleton used only from a non-library .js file imported by
// the document, or only through a string built at runtime, would be dropped
// (none exist in this config).
QList<QQmlImports::CompositeSingletonReference> QQmlImports::resolvedCompositeSingletons() const {
	using OrigFn = QList<CompositeSingletonReference> (*)(const QQmlImports*);
	static const auto orig = []() {
		auto* fn = reinterpret_cast<OrigFn>(
		    dlsym(RTLD_NEXT, "_ZNK11QQmlImports27resolvedCompositeSingletonsEv")
		);
		if (!fn) {
			qFatal(
			    "QQmlImports::resolvedCompositeSingletons: dlsym(RTLD_NEXT, "
			    "\"_ZNK11QQmlImports27resolvedCompositeSingletonsEv\") returned null"
			);
		}
		return fn;
	}();

	static const bool disabled = qEnvironmentVariableIsSet("QS_NO_SINGLETON_FILTER");

	auto list = orig(this);
	if (disabled || list.isEmpty()) return list;

	const auto url = this->baseUrl();
	if (!url.isLocalFile()) return list;

	QFile f(url.toLocalFile());
	if (!f.open(QIODevice::ReadOnly)) return list;

	const auto ids = upperIdentifiersOf(f.readAll());
	const auto before = list.size();
	list.removeIf([&](const CompositeSingletonReference& r) { return !ids.contains(r.typeName); });

	static std::atomic<bool> loggedActive{false};
	if (!loggedActive.exchange(true, std::memory_order_relaxed)) {
		qCInfo(logScene) << "singleton filter active";
	}

	static std::atomic<long> docs{0};
	static std::atomic<long> total{0};
	static std::atomic<long> kept{0};
	total.fetch_add(before, std::memory_order_relaxed);
	kept.fetch_add(list.size(), std::memory_order_relaxed);
	const auto n = docs.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n % 250 == 0) {
		qCDebug(logScene).nospace()
		    << "singleton filter: docs=" << n << " total=" << total.load(std::memory_order_relaxed)
		    << " kept=" << kept.load(std::memory_order_relaxed);
	}

	return list;
}

static QHash<const QQmlEngine*, EngineGeneration*> g_generations; // NOLINT

EngineGeneration::EngineGeneration(const QDir& rootPath, QmlScanner scanner)
    : rootPath(rootPath)
    , scanner(std::move(scanner))
    , urlInterceptor(this->rootPath)
    , interceptNetFactory(this->rootPath, this->scanner.fileIntercepts)
    , engine(new QQmlEngine()) {
	g_generations.insert(this->engine, this);

	this->engine->setOutputWarningsToStandardError(false);
	QObject::connect(this->engine, &QQmlEngine::warnings, this, &EngineGeneration::onEngineWarnings);

	this->engine->addUrlInterceptor(&this->urlInterceptor);

	if (!qEnvironmentVariableIsSet("QS_NO_FILE_VFS") && !this->scanner.scannedFiles.isEmpty()) {
		this->urlInterceptor.fileVfsRoot = buildFileVfs(this->rootPath, this->scanner);
	}

	if (this->urlInterceptor.fileVfsRoot.isEmpty()) this->engine->addImportPath("qs:@/");
	else this->engine->addImportPath(this->urlInterceptor.fileVfsRoot);

	registerSynthesizedModules(this->scanner.fileIntercepts);

	this->engine->setNetworkAccessManagerFactory(&this->interceptNetFactory);
	this->incubationController.initLoop();
	this->engine->setIncubationController(&this->incubationController);

	this->engine->addImageProvider("icon", new IconImageProvider());
	this->engine->addImageProvider("qsimage", new QsImageProvider());
	this->engine->addImageProvider("qspixmap", new QsPixmapProvider());

	qs::memtrim::install();

	QsEnginePlugin::runConstructGeneration(*this);
}

EngineGeneration::EngineGeneration(): EngineGeneration(QDir(), QmlScanner()) {}

EngineGeneration::~EngineGeneration() {
	for (auto* extension: this->extensions.values()) {
		delete extension;
	}

	if (this->engine != nullptr) {
		qFatal() << this << "destroyed without calling destroy()";
	}
}

void EngineGeneration::destroy() {
	if (this->destroying) return;
	this->destroying = true;

	if (this->watcher != nullptr) {
		// Multiple generations can detect a reload at the same time.
		QObject::disconnect(this->watcher, nullptr, this, nullptr);
		this->watcher->deleteLater();
		this->watcher = nullptr;
	}

	if (this->root != nullptr) {
		// prevent further js execution during teardown of the root's children, gc and the engine.
		QObject::connect(this->root, &QObject::destroyed, this, [this]() {
			this->engine->setInterrupted(true);
		});

		// QObject::destroyed is emitted before the root's children are destroyed.
		// The engine must outlive them, as objects such as Loaders with in-flight
		// incubations access it in their destructors.
		QObject::connect(
		    this->root,
		    &QObject::destroyed,
		    this,
		    [this]() {
			    g_generations.remove(this->engine);

			    // Garbage is not collected during engine destruction.
			    this->engine->collectGarbage();

			    delete this->engine;
			    this->engine = nullptr;

			    auto terminate = this->shouldTerminate;
			    auto code = this->exitCode;
			    delete this;

			    if (terminate) QCoreApplication::exit(code);
		    },
		    Qt::QueuedConnection
		);

		this->root->deleteLater();
		this->root = nullptr;
	} else {
		g_generations.remove(this->engine);

		// the engine has never been used, no need to clean up
		delete this->engine;
		this->engine = nullptr;

		auto terminate = this->shouldTerminate;
		auto code = this->exitCode;
		delete this;

		if (terminate) QCoreApplication::exit(code);
	}
}

void EngineGeneration::shutdown() {
	if (this->destroying) return;

	delete this->root;
	this->root = nullptr;
	delete this->engine;
	this->engine = nullptr;
	delete this;
}

void EngineGeneration::onReload(EngineGeneration* old) {
	if (old != nullptr) {
		// if the old generation holds the window incubation controller as the
		// new generation acquires it then incubators will hang intermittently
		qCDebug(logIncubator) << "Locking incubation controllers of old generation" << old;
		old->incubationControllersLocked = true;
		old->updateIncubationMode();
	}

	QObject::connect(this->engine, &QQmlEngine::quit, this, &EngineGeneration::quit);
	QObject::connect(this->engine, &QQmlEngine::exit, this, &EngineGeneration::exit);

	if (auto* reloadable = qobject_cast<Reloadable*>(this->root)) {
		reloadable->reload(old ? old->root : nullptr);
	}

	this->singletonRegistry.onReload(old == nullptr ? nullptr : &old->singletonRegistry);
	this->reloadComplete = true;
	emit this->reloadFinished();

	if (old != nullptr) {
		QObject::connect(old, &QObject::destroyed, this, [this]() { this->postReload(); });
		old->destroy();
	} else {
		this->postReload();
	}
}

void EngineGeneration::postReload() {
	// This can be called on a generation during its destruction.
	if (this->engine == nullptr || this->root == nullptr) return;

	QsEnginePlugin::runOnReload();

	emit this->firePostReload();
	QObject::disconnect(this, &EngineGeneration::firePostReload, nullptr, nullptr);
}

void EngineGeneration::setWatchingFiles(bool watching) {
	if (watching) {
		if (this->watcher == nullptr) {
			this->watcher = new QFileSystemWatcher();

			for (auto& file: this->scanner.scannedFiles) {
				this->watcher->addPath(file);
				this->watcher->addPath(QFileInfo(file).dir().absolutePath());
			}

			for (auto& file: this->extraWatchedFiles) {
				this->watcher->addPath(file);
				this->watcher->addPath(QFileInfo(file).dir().absolutePath());
			}

			QObject::connect(
			    this->watcher,
			    &QFileSystemWatcher::fileChanged,
			    this,
			    &EngineGeneration::onFileChanged
			);

			QObject::connect(
			    this->watcher,
			    &QFileSystemWatcher::directoryChanged,
			    this,
			    &EngineGeneration::onDirectoryChanged
			);
		}
	} else {
		if (this->watcher != nullptr) {
			QObject::disconnect(this->watcher, nullptr, this, nullptr);
			this->watcher->deleteLater();
			this->watcher = nullptr;
		}
	}
}

bool EngineGeneration::setExtraWatchedFiles(const QVector<QString>& files) {
	this->extraWatchedFiles.clear();
	for (const auto& file: files) {
		if (!this->scanner.scannedFiles.contains(file)) {
			this->extraWatchedFiles.append(file);
			QByteArray data;
			this->scanner.readAndHashFile(file, data);
		}
	}

	if (this->watcher) {
		this->setWatchingFiles(false);
		this->setWatchingFiles(true);
	}

	return !this->extraWatchedFiles.isEmpty();
}

void EngineGeneration::onFileChanged(const QString& name) {
	if (!this->watcher->files().contains(name)) {
		this->deletedWatchedFiles.push_back(name);
	} else {
		// some editors (e.g vscode) perform file saving in two steps: truncate + write
		// ignore the first event (truncate) with size 0 to prevent incorrect live reloading
		auto fileInfo = QFileInfo(name);
		if (fileInfo.isFile() && fileInfo.size() == 0) return;

		if (!this->scanner.hasFileContentChanged(name)) {
			qCDebug(logQmlScanner) << "Ignoring file change with unchanged content:" << name;
			return;
		}

		emit this->filesChanged();
	}
}

void EngineGeneration::onDirectoryChanged() {
	// try to find any files that were just deleted from a replace operation
	for (auto& file: this->deletedWatchedFiles) {
		if (QFileInfo(file).exists()) {
			if (!this->scanner.hasFileContentChanged(file)) {
				qCDebug(logQmlScanner) << "Ignoring restored file with unchanged content:" << file;
				continue;
			}

			emit this->filesChanged();
			break;
		}
	}
}

void EngineGeneration::onEngineWarnings(const QList<QQmlError>& warnings) {
	for (const auto& error: warnings) {
		const auto& url = error.url();
		auto rel = url.scheme() == "qs" && url.path().startsWith("@/qs/") ? "@" % url.path().sliced(5)
		                                                                  : url.toString();

		QString objectName;
		auto desc = error.description();
		if (auto i = desc.indexOf(": "); i != -1 && desc.startsWith("QML ")) {
			objectName = desc.first(i) + " at ";
			desc = desc.sliced(i + 2);
		}

		qCWarning(logScene).noquote().nospace()
		    << objectName << rel << '[' << error.line() << ':' << error.column() << "]: " << desc;
	}
}

void EngineGeneration::registerExtension(const void* key, EngineGenerationExt* extension) {
	if (this->extensions.contains(key)) {
		delete this->extensions.value(key);
	}

	this->extensions.insert(key, extension);
}

EngineGenerationExt* EngineGeneration::findExtension(const void* key) {
	return this->extensions.value(key);
}

void EngineGeneration::quit() {
	this->shouldTerminate = true;
	this->destroy();
}

void EngineGeneration::exit(int code) {
	this->shouldTerminate = true;
	this->exitCode = code;
	this->destroy();
}

void EngineGeneration::trackWindowIncubationController(QQuickWindow* window) {
	if (this->trackedWindows.contains(window)) return;

	QObject::connect(window, &QObject::destroyed, this, &EngineGeneration::onTrackedWindowDestroyed);
	this->trackedWindows.append(window);
	this->updateIncubationMode();
}

void EngineGeneration::onTrackedWindowDestroyed(QObject* object) {
	this->trackedWindows.removeAll(static_cast<QQuickWindow*>(object)); // NOLINT
	this->updateIncubationMode();
}

void EngineGeneration::updateIncubationMode() {
	// If we're in a situation with only hidden but tracked windows this might be wrong,
	// but it seems to at least work.
	this->incubationController.setIncubationMode(!this->trackedWindows.empty());
}

EngineGeneration* EngineGeneration::currentGeneration() {
	if (g_generations.size() == 1) {
		return *g_generations.begin();
	} else return nullptr;
}

EngineGeneration* EngineGeneration::findEngineGeneration(const QQmlEngine* engine) {
	return g_generations.value(engine);
}

EngineGeneration* EngineGeneration::findObjectGeneration(const QObject* object) {
	// Objects can still attempt to find their generation after it has been destroyed.
	// if (g_generations.size() == 1) return EngineGeneration::currentGeneration();

	while (object != nullptr) {
		auto* context = QQmlEngine::contextForObject(object);

		if (context != nullptr) {
			if (auto* generation = EngineGeneration::findEngineGeneration(context->engine())) {
				return generation;
			}
		}

		object = object->parent();
	}

	return nullptr;
}
