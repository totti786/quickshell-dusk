#pragma once

#include <qtypes.h>

class QQuickWindow;

namespace qs::memtrim {

// Starts the idle memory trimmer (once; later calls do nothing). Call on the GUI thread
// after the QGuiApplication exists. QS_NO_IDLE_TRIM=1 leaves it off.
void install();

// Milliseconds since the last key or pointer input to any shell window, -1 if none yet.
qint64 msSinceInput();

// Idle buffer trim (see memorytrim.cpp): call for every new QQuickWindow of the shell.
// QS_NO_IDLE_BUFFER_TRIM=1 makes it a no-op.
void trackWindowBuffers(QQuickWindow* window);

} // namespace qs::memtrim
