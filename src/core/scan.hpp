#pragma once

#include <qbytearray.h>
#include <qcontainerfwd.h>
#include <qdir.h>
#include <qhash.h>
#include <qset.h>
#include <qjsengine.h>
#include <qloggingcategory.h>
#include <qvector.h>

#include "logcat.hpp"

QS_DECLARE_LOGGING_CATEGORY(logQmlScanner);

// expects canonical paths
class QmlScanner {
public:
	QmlScanner() = default;
	QmlScanner(const QDir& rootPath): rootPath(rootPath) {}

	void scanDir(const QDir& dir);
	void scanQmlRoot(const QString& path);

	QVector<QDir> scannedDirs;
	// scannedDirs.contains() compares QDirs, and QDir::operator== canonicalizes both paths
	// (a readlink per path component) whenever they differ. With a few hundred directories
	// and tens of thousands of imports that is hundreds of thousands of syscalls and seconds
	// of startup, so membership is tracked by canonical path, computed once per input path.
	QSet<QString> scannedDirKeys;
	QHash<QString, QString> dirKeyCache;
	QString dirKey(const QString& path);
	QVector<QString> scannedFiles;
	QSet<QString> scannedQmlFiles;
	QHash<QString, QByteArray> fileHashes;
	QHash<QString, QString> fileIntercepts;

	struct ScanError {
		QString file;
		QString message;
		int line;
	};

	QVector<ScanError> scanErrors;

	bool readAndHashFile(const QString& path, QByteArray& data);
	[[nodiscard]] bool hasFileContentChanged(const QString& path) const;

private:
	QDir rootPath;

	bool scanQmlFile(const QString& path, bool& singleton, bool& internal);
	bool scanQmlJson(const QString& path);
	[[nodiscard]] static QPair<QString, QString> jsonToQml(const QJsonValue& value, int indent = 0);

	static QJSEngine* preprocEngine();
};
