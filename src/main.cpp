#include <dlfcn.h>

#include "launch/main.hpp"

#ifdef QS_JEMALLOC_MALLOC_CONF
// jemalloc's application default, read on the first allocation (before main()) and before
// MALLOC_CONF, which overrides it option by option. It has to live in the executable and be
// exported (--export-dynamic-symbol=malloc_conf) so that the shared libjemalloc's weak
// malloc_conf resolves to it. A "//@ pragma Env MALLOC_CONF=..." is applied long after
// jemalloc has initialised and never reaches the shell's own allocator.
//
// narenas:1: jemalloc's default is 4 arenas per CPU, and Quickshell's ~50 threads (QML loader,
// one render thread per window, Mesa, PipeWire, D-Bus...) each pin one, every arena keeping its
// own dirty pages and partly used slabs. One arena cut settled native memory by ~100 MB on a large
// config with no measurable startup or frame-time cost (lock contention stayed negligible).
extern "C" {
__attribute__((visibility("default"), used)) const char* malloc_conf = QS_JEMALLOC_MALLOC_CONF;
}
#endif

namespace {

// Keep OpenSSL's "default" provider activated for the life of the process, before any
// thread exists. Qt 6.12.0's QCryptographicHash loads and unloads that provider around
// every hash (OSSL_PROVIDER_try_load(..., retain_fallbacks=1) / OSSL_PROVIDER_unload).
// When a hash is the first OpenSSL user, this leaves two "default" entries in the
// provider store, and the one Qt finds drops to zero activations after each hash:
// OpenSSL then removes its methods and flushes the decoder cache while the TLS code on
// another thread is walking them (use-after-free in collect_keymgmt, ossl_provider_free,
// ...). With an activation held here, the count never reaches zero. Same remedy as
// upstream Qt a64dfe20a4b4 ("QCHash/openssl: Don't unload loaded providers",
// QTBUG-148575, 6.12 branch). Looked up at run time: libcrypto is already loaded through
// QtCore, so Quickshell needs no OpenSSL headers or link dependency; with no libcrypto
// in the process this does nothing.
void pinOpenSslDefaultProvider() {
	using TryLoad = void* (*)(void*, const char*, int);
	auto* tryLoad = reinterpret_cast<TryLoad>(dlsym(RTLD_DEFAULT, "OSSL_PROVIDER_try_load"));
	if (tryLoad != nullptr) tryLoad(nullptr, "default", 1);
}

} // namespace

int main(int argc, char** argv) {
	pinOpenSslDefaultProvider();
	return qs::launch::main(argc, argv);
}
