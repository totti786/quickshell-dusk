#include "qsintercept.hpp"
#include <cstring>

#include <qdir.h>
#include <qhash.h>
#include <qiodevice.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qminmax.h>
#include <qnetworkaccessmanager.h>
#include <qnetworkrequest.h>
#include <qobject.h>
#include <qqmlabstracturlinterceptor.h>
#include <qstring.h>
#include <qtypes.h>
#include <qurl.h>

#include "logcat.hpp"

QS_LOGGING_CATEGORY(logQsIntercept, "quickshell.interceptor", QtWarningMsg);

QUrl QsUrlInterceptor::intercept(
    const QUrl& originalUrl,
    QQmlAbstractUrlInterceptor::DataType type
) {
	auto url = originalUrl;

	// Qt only keeps its compiled-QML disk cache for local files. Every type served on the
	// qs: scheme is therefore recompiled from source on every start (~1600 files, ~13 s of
	// the QML thread for a large shell). Hand Qt the real file: URL for any type that has no
	// preprocessed override, so the cache applies. The directory qmldirs are still synthesized
	// by the scanner, so a file: base asks for its qmldir through qs: below.
	if (url.scheme() == "file" && type == QQmlAbstractUrlInterceptor::DataType::QmldirFile) {
		auto path = url.path();
		if (path.startsWith(this->configRoot.path() % '/') && this->fileIntercepts.contains(path)) {
			url.setScheme("qs");
			return url;
		}
	}

	if (url.scheme() == "qs" && type == QQmlAbstractUrlInterceptor::DataType::QmlFile) {
		auto path = url.path();
		if (path.startsWith("@/qs/")) path = this->configRoot.filePath(path.sliced(5));
		else if (!path.startsWith("/")) path = QString();

		// The root file is where the engine starts from; keep it on qs: (one file, never cached).
		if (!path.isEmpty() && !this->fileIntercepts.contains(path) && path != this->configRoot.filePath("shell.qml")) {
			auto fileUrl = url;
			fileUrl.setScheme("file");
			fileUrl.setPath(path);
			return fileUrl;
		}
	}

	if (url.scheme() == "root") {
		url.setScheme("qs");

		auto path = url.path();
		if (path.startsWith('/')) path = path.sliced(1);
		url.setPath("@/qs/" % path);

		qCDebug(logQsIntercept) << "Rewrote root intercept" << originalUrl << "to" << url;
	}

	if (url.scheme() == "qs") {
		auto path = url.path();

		// Our import path is on "qs:@/".
		// We want to blackhole any import resolution outside of the config folder as it breaks Qt
		// but NOT file lookups that might be on "qs:/" due to a missing "file:/" prefix.
		if (path.startsWith("@/qs/")) {
			path = this->configRoot.filePath(path.sliced(5));
		} else if (!path.startsWith("/")) {
			qCDebug(logQsIntercept) << "Blackholed import URL" << url;
			return QUrl("qrc:/qs-blackhole");
		}

		// Some types such as Image take into account where they are loading from, and force
		// asynchronous loading over a network. qs: is considered to be over a network.
		// In those cases we want to return a file:// url so asynchronous loading is not forced.
		if (type == QQmlAbstractUrlInterceptor::DataType::UrlString) {
			// Qt.resolvedUrl and context->resolvedUrl can use this on qml files, in which
			// case we want to keep the intercept, otherwise objects created from those paths
			// will not be able to use singletons.
			// Experimental: files without a preprocessed override are served as file: URLs too,
			// so that one type has one URL (a type loaded under two URLs is two types).
			if (path.endsWith(".qml") && this->fileIntercepts.contains(path)) return url;

			auto newUrl = url;
			newUrl.setScheme("file");
			// above check asserts path starts with /qs/
			newUrl.setPath(path);
			qCDebug(logQsIntercept) << "Rewrote intercept" << url << "to" << newUrl;
			return newUrl;
		}
	}

	return url;
}

QsInterceptDataReply::QsInterceptDataReply(const QString& data, QObject* parent)
    : QNetworkReply(parent)
    , content(data.toUtf8()) {
	this->setOpenMode(QIODevice::ReadOnly);
	this->setFinished(true);
}

qint64 QsInterceptDataReply::readData(char* data, qint64 maxSize) {
	auto size = qMin(maxSize, this->content.length() - this->offset);
	if (size == 0) return -1;
	memcpy(data, this->content.constData() + this->offset, size); // NOLINT
	this->offset += size;
	return size;
}

QsInterceptNetworkAccessManager::QsInterceptNetworkAccessManager(
    const QDir& configRoot,
    const QHash<QString, QString>& fileIntercepts,
    QObject* parent
)
    : QNetworkAccessManager(parent)
    , configRoot(configRoot)
    , fileIntercepts(fileIntercepts) {}

QNetworkReply* QsInterceptNetworkAccessManager::createRequest(
    QNetworkAccessManager::Operation op,
    const QNetworkRequest& req,
    QIODevice* outgoingData
) {
	auto url = req.url();

	if (url.scheme() == "qs") {
		auto path = url.path();

		if (path.startsWith("@/qs/")) path = this->configRoot.filePath(path.sliced(5));
		// otherwise pass through to fs

		qCDebug(logQsIntercept) << "Got intercept for" << path << "contains"
		                        << this->fileIntercepts.value(path);

		if (auto data = this->fileIntercepts.value(path); !data.isEmpty()) {
			return new QsInterceptDataReply(data, this);
		}

		auto fileReq = req;
		auto fileUrl = req.url();
		fileUrl.setScheme("file");
		fileUrl.setPath(path);
		qCDebug(logQsIntercept) << "Passing through intercept" << url << "to" << fileUrl;

		fileReq.setUrl(fileUrl);
		return this->QNetworkAccessManager::createRequest(op, fileReq, outgoingData);
	}

	return this->QNetworkAccessManager::createRequest(op, req, outgoingData);
}

QNetworkAccessManager* QsInterceptNetworkAccessManagerFactory::create(QObject* parent) {
	return new QsInterceptNetworkAccessManager(this->configRoot, this->fileIntercepts, parent);
}
