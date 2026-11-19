#include "memorytrim.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <dlfcn.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <private/qhooks_p.h>
#include <qcoreapplication.h>
#include <qelapsedtimer.h>
#include <qfile.h>
#include <qguiapplication.h>
#include <qevent.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qqmlengine.h>
#include <qtenvironmentvariables.h>
#include <qquickwindow.h>
#include <qsgrendererinterface.h>
#include <qstring.h>
#if QT_CONFIG(opengl)
#include <qopenglcontext.h>
#include <qopenglfunctions.h>
#endif
#include <qtimer.h>
#include <unistd.h>

#include "generation.hpp"
#include "logcat.hpp"

// Gives memory back after a panel closes, once the shell has gone quiet:
// a full JS garbage collection, then the allocator's free pages back to the OS.
//
// Why: closing a panel (a Loader going inactive, a window going away) frees thousands of
// objects, but nothing returns that memory. The JS wrappers stay until the GC next runs on
// allocation pressure, and jemalloc only decays its dirty pages while something allocates,
// so an idle shell keeps tens of MB of pages it no longer uses.
//
// Trigger: a destruction burst. Qt calls qtHookData[QHooks::RemoveQObject] (the hook
// GammaRay uses, present in release builds) from every QObject destructor; it counts, and
// when at least QS_IDLE_TRIM_BURST objects (default 500) die within ~2 s it arms a
// one-shot timer on the GUI thread. Every further burst restarts it, so the trim runs
// QS_IDLE_TRIM_DELAY_MS (default 10 s) after the last one, no closer than 60 s to the
// previous trim, and only once the shell has had no pointer or key input for that long either
// (an application event filter notes the time of the last input event). Panels kept loaded and
// merely hidden free nothing and trigger nothing.
// There is no periodic timer: an idle shell is never woken by this.
//
// Release: jemalloc's thread.tcache.flush + arena.<all>.purge, found with dlsym so a
// build without jemalloc (USE_JEMALLOC=OFF) falls back to glibc's malloc_trim(0).
//
// Component cache: after the GC, QQmlEngine::trimComponentCache() drops the compiled types
// no live object uses any more (a closed panel's files), when QS_IDLE_TRIM_COMPONENTS=1.
// Off by default: measured on a large config, the call itself blocked the GUI thread for
// 34-71 ms whenever it had types to drop, the first reopen of a trimmed panel was ~10-16 ms
// slower (its types load again: disk cache, but imports and type data are rebuilt), and the
// settled PSS showed no measurable gain.
//
// QS_NO_IDLE_TRIM=1 turns it off; the quickshell.memtrim debug category logs every trim.

namespace qs::memtrim {

namespace {

QS_LOGGING_CATEGORY(logMemTrim, "quickshell.memtrim");

using MallctlFn = int (*)(const char*, void*, size_t*, void*, size_t);

constexpr qint64 BURST_WINDOW_MS = 2000;
constexpr qint64 MIN_INTERVAL_MS = 60000;
// Lets the objects the GC handed to deleteLater() go before the pages are released.
constexpr int RELEASE_DELAY_MS = 250;
// MALLCTL_ARENAS_ALL
constexpr const char* PURGE_ALL_ARENAS = "arena.4096.purge";

// Written by the QObject destructor hook on any thread; approximate counting is fine.
std::atomic<quint32> gDestroyed {0};      // NOLINT
std::atomic<qint64> gWindowStart {0};     // NOLINT
std::atomic<quint32> gWindowBase {0};     // NOLINT
std::atomic<bool> gPosted {false};        // NOLINT
std::atomic<quint32> gBurst {500};        // NOLINT
std::atomic<QHooks::RemoveQObjectCallback> gPrevHook {nullptr}; // NOLINT

qint64 steadyMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch()
	)
	    .count();
}

// jemalloc's mallctl, but only when jemalloc is the allocator in use: `malloc` must resolve
// into the same object. Otherwise (another allocator preloaded over a jemalloc build, or a
// jemalloc-linked library in a build without it) a dormant jemalloc would be purged and
// malloc_trim skipped.
MallctlFn mallctlFn() {
	static const auto fn = []() -> MallctlFn {
		auto* ctl = dlsym(RTLD_DEFAULT, "mallctl");
		auto* alloc = dlsym(RTLD_DEFAULT, "malloc");
		Dl_info ctlInfo {};
		Dl_info allocInfo {};
		if (!ctl || !alloc || !dladdr(ctl, &ctlInfo) || !dladdr(alloc, &allocInfo)) return nullptr;
		if (ctlInfo.dli_fbase != allocInfo.dli_fbase) return nullptr;
		return reinterpret_cast<MallctlFn>(ctl);
	}();
	return fn;
}

size_t jeStat(const char* name) {
	auto* mallctl = mallctlFn();
	if (!mallctl) return 0;
	uint64_t epoch = 1;
	auto epochSize = sizeof(epoch);
	mallctl("epoch", &epoch, &epochSize, &epoch, epochSize);
	size_t value = 0;
	auto size = sizeof(value);
	if (mallctl(name, &value, &size, nullptr, 0) != 0) return 0;
	return value;
}

// Resident set from /proc/self/statm (pages), in MiB.
qint64 residentMiB() {
	QFile f("/proc/self/statm");
	if (!f.open(QFile::ReadOnly)) return -1;
	const auto fields = f.readAll().split(' ');
	if (fields.size() < 2) return -1;
	return fields[1].toLongLong() * sysconf(_SC_PAGESIZE) / (1024 * 1024);
}

// Installed on the application: notes the time of the last user input. A type switch and,
// for input events only, a monotonic clock read.
class InputWatcher: public QObject {
public:
	bool eventFilter(QObject* /*watched*/, QEvent* event) override {
		switch (event->type()) {
		case QEvent::KeyPress:
		case QEvent::MouseButtonPress:
		case QEvent::MouseMove:
		case QEvent::Wheel:
		case QEvent::TouchBegin:
		case QEvent::TouchUpdate:
		case QEvent::TabletPress:
		case QEvent::TabletMove:
		case QEvent::NativeGesture:
		case QEvent::InputMethod: this->last.start(); break; // IME / on-screen keyboard text
		default: break;
		}
		return false;
	}

	QElapsedTimer last;
};

InputWatcher* gInput = nullptr; // NOLINT

void ensureInputWatcher() {
	if (gInput != nullptr) return;
	gInput = new InputWatcher(); // leaked with the application
	QCoreApplication::instance()->installEventFilter(gInput);
}

class Trimmer: public QObject {
public:
	explicit Trimmer(int delayMs, bool trimComponents)
	    : delayMs(delayMs)
	    , trimComponents(trimComponents) {
		this->timer.setSingleShot(true);
		QObject::connect(&this->timer, &QTimer::timeout, this, &Trimmer::onTimeout);
	}

	// A destruction burst (queued from the hook): wait for the shell to go quiet again.
	void onBurst() {
		gPosted.store(false, std::memory_order_relaxed);
		qCDebug(logMemTrim) << "destruction burst," << gDestroyed.load(std::memory_order_relaxed)
		                    << "QObjects destroyed so far";
		auto wait = static_cast<qint64>(this->delayMs);
		if (this->lastTrim.isValid()) wait = qMax(wait, MIN_INTERVAL_MS - this->lastTrim.elapsed());
		this->timer.start(static_cast<int>(wait));
	}

private:
	void onTimeout() {
		if (const auto input = msSinceInput(); input >= 0 && input < this->delayMs) {
			// Someone is using the shell (pointer or keys on one of its windows): do not take a
			// GC pause under their hands. Look again once `delay` has passed since that input;
			// this re-arms only while input keeps coming, so an idle shell is not woken.
			this->timer.start(static_cast<int>(this->delayMs - input));
			return;
		}

		// Started now, not after the release: the collection hands parentless JS-owned objects
		// to deleteLater(), and that burst must not arm a second trim inside the interval.
		this->lastTrim.start();

		QElapsedTimer t;
		t.start();
		if (auto* generation = EngineGeneration::currentGeneration()) {
			if (generation->engine) generation->engine->collectGarbage();
		}
		this->gcMs = t.elapsed();

		QTimer::singleShot(RELEASE_DELAY_MS, this, &Trimmer::release);
	}

	void release() {
		// The statistics cost a jemalloc epoch refresh and /proc reads: only when logged.
		const bool debug = logMemTrim().isDebugEnabled();
		const auto rssBefore = debug ? residentMiB() : 0;
		const auto jeBefore = debug ? jeStat("stats.resident") : 0;

		QElapsedTimer t;
		t.start();
		if (this->trimComponents) {
			// After the GC, so components only dead JS objects referenced count as unused.
			if (auto* generation = EngineGeneration::currentGeneration()) {
				if (generation->engine) generation->engine->trimComponentCache();
			}
		}
		const auto componentsMs = t.elapsed();

		t.restart();
		if (auto* mallctl = mallctlFn()) {
			mallctl("thread.tcache.flush", nullptr, nullptr, nullptr, 0);
			mallctl(PURGE_ALL_ARENAS, nullptr, nullptr, nullptr, 0);
		} else {
#ifdef __GLIBC__
			malloc_trim(0);
#endif
		}
		const auto releaseMs = t.elapsed();

		++this->trims;

		if (!debug) return;
		const auto rssAfter = residentMiB();
		const auto jeAfter = jeStat("stats.resident");

		qCDebug(logMemTrim).nospace() << "trim #" << this->trims << ": gc " << this->gcMs
		                              << " ms, components " << componentsMs << " ms, release "
		                              << releaseMs << " ms, rss " << rssBefore
		                              << " -> " << rssAfter << " MiB, jemalloc resident "
		                              << jeBefore / (1024 * 1024) << " -> "
		                              << jeAfter / (1024 * 1024) << " MiB";
	}

	int delayMs;
	bool trimComponents;
	QTimer timer;
	QElapsedTimer lastTrim;
	qint64 gcMs = 0;
	int trims = 0;
};

std::atomic<Trimmer*> gTrimmer {nullptr}; // NOLINT

// Runs inside every QObject destructor, on whichever thread: a relaxed increment, and
// a clock read once per 128 objects.
void onObjectRemoved(QObject* object) {
	if (auto* prev = gPrevHook.load(std::memory_order_relaxed)) prev(object);

	const auto n = gDestroyed.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((n & 127) != 0) return;

	const auto now = steadyMs();
	if (now - gWindowStart.load(std::memory_order_relaxed) > BURST_WINDOW_MS) {
		gWindowStart.store(now, std::memory_order_relaxed);
		gWindowBase.store(n, std::memory_order_relaxed);
		return;
	}

	if (n - gWindowBase.load(std::memory_order_relaxed) < gBurst.load(std::memory_order_relaxed)) {
		return;
	}

	gWindowStart.store(now, std::memory_order_relaxed);
	gWindowBase.store(n, std::memory_order_relaxed);

	if (QCoreApplication::closingDown() || gPosted.exchange(true)) return;
	QMetaObject::invokeMethod(
	    gTrimmer.load(std::memory_order_acquire),
	    &Trimmer::onBurst,
	    Qt::QueuedConnection
	);
}

// Idle buffer trim (0009).
//
// Mesa's EGL Wayland platform (src/egl/drivers/dri2/platform_wayland.c) keeps up to 4 colour
// buffers per surface. When the compositor holds buffers during an animation it allocates a
// third (or fourth); it frees such a spare only in update_buffers(), i.e. while acquiring the
// next back buffer, once the spare's age, counted in swaps, exceeds BUFFER_TRIM_AGE_HYSTERESIS
// (20). A window that stops drawing right after an animation keeps its spares for as long as
// it stays idle; a full-screen 2880x1800 buffer is ~20 MB of GPU memory (system RAM on an iGPU).
//
// So: once a visible window of at least 4 MiB per buffer has presented no frame for
// QS_IDLE_BUFFER_TRIM_DELAY_MS (2 s) and the shell has had no input for as long, it redraws its
// unchanged scene QS_IDLE_BUFFER_TRIM_FRAMES (22) times, QS_IDLE_BUFFER_TRIM_PACE_MS (500 ms)
// apart, so the spares age out and Mesa frees them. Same content, same pixels. Only after at
// least 3 animation frames (under 50 ms apart) since the previous burst, at most once per
// QS_IDLE_BUFFER_TRIM_INTERVAL_MS (120 s, at least 1 s) per window, doubling up to 10 min for
// a window that keeps animating again right after, and abandoned on input. Each redraw is a
// commit the compositor recomposites (with blur), so a burst costs about as much as 22 frames
// of a small animation, spread over 11 s. A window that is hidden or gets no frame callbacks
// never presents, so the burst just stops. Only with OpenGL on Mesa under Wayland (detected
// once at the first scene-graph initialisation); elsewhere the trimmers remove themselves.
// QS_NO_IDLE_BUFFER_TRIM=1 turns it off (QS_IDLE_BUFFER_TRIM=1 on, if the default is off).

constexpr int BUFFER_TRIM_MIN_FRAMES = 3;
// Frames closer than this belong to an animation, the only time a spare can be allocated.
constexpr qint64 BUFFER_TRIM_ANIMATION_GAP_MS = 50;
// A spare of a smaller window is not worth the compositor's recomposites (bar, dock, popups).
// 4 MiB, not 8: a full-screen 1920x1080 buffer is 7.9 MiB, and 8 left out every 1080p screen.
constexpr qint64 BUFFER_TRIM_MIN_BUFFER_BYTES = 4ll * 1024 * 1024;
constexpr qint64 BUFFER_TRIM_MIN_INTERVAL_MS = 1000;
constexpr qint64 BUFFER_TRIM_MAX_INTERVAL_MS = 600000;

// Default on; to ship it off, make this false (QS_IDLE_BUFFER_TRIM=1 then turns it on).
constexpr bool BUFFER_TRIM_DEFAULT_ON = true;

struct BufferTrimConfig {
	bool enabled = BUFFER_TRIM_DEFAULT_ON;
	int delayMs = 2000;
	int frames = 22;
	int intervalMs = 120000;
	int paceMs = 500;
};

const BufferTrimConfig& bufferTrimConfig() {
	static const auto config = [] {
		BufferTrimConfig c;
		if (qEnvironmentVariableIntValue("QS_NO_IDLE_BUFFER_TRIM") == 1) c.enabled = false;
		else if (qEnvironmentVariableIntValue("QS_IDLE_BUFFER_TRIM") == 1) c.enabled = true;
		bool ok = false;
		if (auto v = qEnvironmentVariableIntValue("QS_IDLE_BUFFER_TRIM_DELAY_MS", &ok); ok && v > 0) {
			c.delayMs = v;
		}
		if (auto v = qEnvironmentVariableIntValue("QS_IDLE_BUFFER_TRIM_FRAMES", &ok); ok && v > 0) {
			c.frames = v;
		}
		if (auto v = qEnvironmentVariableIntValue("QS_IDLE_BUFFER_TRIM_INTERVAL_MS", &ok); ok) {
			c.intervalMs = static_cast<int>(qMax<qint64>(v, BUFFER_TRIM_MIN_INTERVAL_MS));
		}
		if (auto v = qEnvironmentVariableIntValue("QS_IDLE_BUFFER_TRIM_PACE_MS", &ok); ok && v > 0) {
			c.paceMs = v;
		}
		if (c.enabled) {
			qCInfo(logMemTrim).nospace() << "Idle buffer trim enabled (" << c.frames << " frames "
			                             << c.paceMs << " ms apart after " << c.delayMs
			                             << " ms idle, at most every " << c.intervalMs
			                             << " ms per window; OpenGL on Mesa only)";
		} else {
			qCInfo(logMemTrim) << "Idle buffer trim disabled";
		}
		return c;
	}();
	return config;
}

// Whether the windows render through Mesa's EGL Wayland platform, the one place the 20-swap
// rule exists: -1 not known yet, 0 no, 1 yes. Decided once, by the first window whose scene
// graph initialises (on its render thread, with its GL context current). Under Vulkan, the
// software renderer, X11 or another GL driver the trimmers remove themselves.
std::atomic<int> gMesaWayland {-1}; // NOLINT

void detectMesaWayland(QQuickWindow* window, bool wayland) {
	if (gMesaWayland.load(std::memory_order_acquire) != -1) return;

	auto result = 0;
	QString what = QStringLiteral("not Wayland");
	if (wayland) {
		auto* renderer = window->rendererInterface();
		const auto api = renderer ? renderer->graphicsApi() : QSGRendererInterface::Unknown;
		what = QStringLiteral("graphics API %1").arg(static_cast<int>(api));
#if QT_CONFIG(opengl)
		if (api == QSGRendererInterface::OpenGL) {
			const char* version = nullptr;
			if (auto* context = QOpenGLContext::currentContext()) {
				version = reinterpret_cast<const char*>(context->functions()->glGetString(GL_VERSION));
			}
			what = QStringLiteral("OpenGL %1").arg(QString::fromLatin1(version ? version : "(no context)"));
			result = version && std::strstr(version, "Mesa") ? 1 : 0;
		}
#endif
	}

	auto expected = -1;
	if (gMesaWayland.compare_exchange_strong(expected, result, std::memory_order_acq_rel)) {
		qCInfo(logMemTrim).nospace() << "Idle buffer trim " << (result ? "in effect" : "not in effect")
		                             << ": " << what;
	}
}

class BufferTrimmer: public QObject {
public:
	explicit BufferTrimmer(QQuickWindow* window): QObject(window), window(window) {
		this->paceTimer.setSingleShot(true);
		QObject::connect(&this->paceTimer, &QTimer::timeout, window, &QQuickWindow::update);
		this->timer.setSingleShot(true);
		QObject::connect(&this->timer, &QTimer::timeout, this, &BufferTrimmer::onTimeout);

		if (gMesaWayland.load(std::memory_order_acquire) == -1) {
			// Direct: runs on the render thread while its GL context is current.
			QObject::connect(
			    window,
			    &QQuickWindow::sceneGraphInitialized,
			    this,
			    [window, wayland = this->wayland]() { detectMesaWayland(window, wayland); },
			    Qt::DirectConnection
			);
		}

		// Emitted on the render thread with the threaded loop; handled on the GUI thread.
		QObject::connect(
		    window,
		    &QQuickWindow::frameSwapped,
		    this,
		    &BufferTrimmer::onFrameSwapped,
		    Qt::QueuedConnection
		);
	}

private:
	void onFrameSwapped() {
		const auto& config = bufferTrimConfig();

		auto mesa = gMesaWayland.load(std::memory_order_acquire);
		if (mesa == -1) {
			// Not every backend reports sceneGraphInitialized (the software one does not);
			// anything but OpenGL can be decided here without a GL context.
			auto* renderer = this->window->rendererInterface();
			const auto api = renderer ? renderer->graphicsApi() : QSGRendererInterface::Unknown;
			if (api != QSGRendererInterface::Unknown && api != QSGRendererInterface::OpenGL) {
				detectMesaWayland(this->window, this->wayland);
				mesa = gMesaWayland.load(std::memory_order_acquire);
			}
		}
		if (mesa == 0) {
			this->deleteLater(); // nothing to do on this backend: drop the per-frame work too
			return;
		}

		if (this->burstLeft > 0) {
			const auto input = msSinceInput();
			if (input >= 0 && input < config.delayMs) {
				this->burstLeft = 0; // the user is back; their own frames age the buffers anyway
			} else if (--this->burstLeft > 0) {
				// Paced, not back to back: a redraw requested right on the frame callback finds
				// the compositor still holding the previous buffer and takes the spare again.
				// A gap lets that buffer come back, so two buffers rotate and the spare only
				// ages. 500 ms also keeps a burst at 10 frames per 5 s or fewer, under the frame
				// counts QML watchdogs treat as busy.
				this->paceTimer.start(config.paceMs);
				return;
			}
			qCDebug(logMemTrim) << "buffer trim done for" << this->window;
			this->framesSinceBurst = 0;
			this->lastBurst.start();
			return;
		}

		// Only animation frames can have needed a spare; isolated updates (a clock tick) never
		// do, so they do not make the window eligible again.
		if (this->lastFrame.isValid() && this->lastFrame.elapsed() < BUFFER_TRIM_ANIMATION_GAP_MS) {
			++this->framesSinceBurst;
		}
		this->lastFrame.start();
		if (!this->timer.isActive()) this->timer.start(config.delayMs);
	}

	void onTimeout() {
		const auto& config = bufferTrimConfig();
		if (gMesaWayland.load(std::memory_order_acquire) != 1) return;
		if (this->framesSinceBurst < BUFFER_TRIM_MIN_FRAMES) return;
		if (!this->window->isVisible() || !this->window->isExposed()) return;

		const auto pixels = this->window->size() * this->window->devicePixelRatio();
		if (static_cast<qint64>(pixels.width()) * pixels.height() * 4 < BUFFER_TRIM_MIN_BUFFER_BYTES) {
			return;
		}

		// Wait for both the window and the user to be idle; each check re-arms only while
		// frames or input keep coming.
		auto wait = config.delayMs - this->lastFrame.elapsed();
		if (const auto input = msSinceInput(); input >= 0) {
			wait = qMax(wait, config.delayMs - input);
		}
		if (this->lastBurst.isValid()) {
			wait = qMax(wait, this->intervalMs - this->lastBurst.elapsed());
		}
		if (wait > 0) {
			this->timer.start(static_cast<int>(wait));
			return;
		}

		// A window that animates again soon after each burst (a periodic animation, like the
		// island's) takes its spare right back: back off, doubling the interval up to 10 min,
		// until it stays calm for twice the interval.
		if (this->lastBurst.isValid() && this->lastBurst.elapsed() < 2 * this->intervalMs) {
			this->intervalMs = qMin(this->intervalMs * 2, BUFFER_TRIM_MAX_INTERVAL_MS);
		} else {
			this->intervalMs = config.intervalMs;
		}

		qCDebug(logMemTrim) << "buffer trim:" << config.frames << "redraws of" << this->window
		                    << "after" << this->framesSinceBurst << "animation frames, next interval"
		                    << this->intervalMs << "ms";
		this->burstLeft = config.frames;
		this->window->update();
	}

	QQuickWindow* window;
	bool wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
	QTimer timer;
	QTimer paceTimer;
	QElapsedTimer lastFrame;
	QElapsedTimer lastBurst;
	int framesSinceBurst = 0;
	int burstLeft = 0;
	qint64 intervalMs = bufferTrimConfig().intervalMs;
};

} // namespace

qint64 msSinceInput() {
	if (gInput == nullptr || !gInput->last.isValid()) return -1;
	return gInput->last.elapsed();
}

void trackWindowBuffers(QQuickWindow* window) {
	if (window == nullptr || !bufferTrimConfig().enabled) return;
	if (gMesaWayland.load(std::memory_order_acquire) == 0) return;
	// A reload hands the same window to the new proxy, which connects it again.
	if (window->findChild<QObject*>("qs-buffer-trimmer", Qt::FindDirectChildrenOnly)) return;
	ensureInputWatcher();
	auto* trimmer = new BufferTrimmer(window); // owned by the window
	trimmer->setObjectName("qs-buffer-trimmer");
}

void install() {
	if (gTrimmer.load(std::memory_order_relaxed) != nullptr) return;
	if (qEnvironmentVariableIntValue("QS_NO_IDLE_TRIM") == 1) {
		static bool logged = false; // install() runs again on every reload
		if (!logged) qCInfo(logMemTrim) << "Idle memory trim disabled (QS_NO_IDLE_TRIM)";
		logged = true;
		return;
	}

	bool ok = false;
	auto delayMs = qEnvironmentVariableIntValue("QS_IDLE_TRIM_DELAY_MS", &ok);
	if (!ok || delayMs < 0) delayMs = 10000;
	auto burst = qEnvironmentVariableIntValue("QS_IDLE_TRIM_BURST", &ok);
	if (ok && burst > 0) gBurst.store(static_cast<quint32>(burst), std::memory_order_relaxed);

	const auto trimComponents = qEnvironmentVariableIntValue("QS_IDLE_TRIM_COMPONENTS") == 1;

	// Intentionally leaked: the hook can still fire during static destruction.
	gTrimmer.store(new Trimmer(delayMs, trimComponents), std::memory_order_release);
	ensureInputWatcher();

	gPrevHook.store(
	    reinterpret_cast<QHooks::RemoveQObjectCallback>(qtHookData[QHooks::RemoveQObject]),
	    std::memory_order_relaxed
	);
	// qtHookData is a plain array: publish everything the hook reads before installing it.
	std::atomic_thread_fence(std::memory_order_release);
	qtHookData[QHooks::RemoveQObject] = reinterpret_cast<quintptr>(&onObjectRemoved);

	if (auto* mallctl = mallctlFn()) {
		unsigned narenas = 0;
		auto size = sizeof(narenas);
		mallctl("opt.narenas", &narenas, &size, nullptr, 0);
		qCInfo(logMemTrim).nospace() << "Idle memory trim active (jemalloc, opt.narenas=" << narenas
		                             << ", delay " << delayMs << " ms, burst " << gBurst.load()
		                             << (trimComponents ? ", components" : "") << ")";
	} else {
		qCInfo(logMemTrim).nospace() << "Idle memory trim active (malloc_trim, delay " << delayMs
		                             << " ms, burst " << gBurst.load()
		                             << (trimComponents ? ", components" : "") << ")";
	}
}

} // namespace qs::memtrim
