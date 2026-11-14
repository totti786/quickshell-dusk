#!/usr/bin/env python3
"""Exercise duplicate directory aliases and config hot reloads without windows."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='qs-scanner-regression-') as tmp:
    root = Path(tmp)
    config = root / 'config'
    shared = config / 'shared'
    shared.mkdir(parents=True)
    (config / 'alias').symlink_to(shared, target_is_directory=True)
    entry = root / 'entry'
    entry.symlink_to(config, target_is_directory=True)
    thing = shared / 'Thing.qml'
    def thing_source(value):
        return 'import QtQuick\nQtObject { property string value: \"' + value + '\" }\n'
    thing.write_text(thing_source('one'))
    shell = config / 'shell.qml'
    def shell_source(extra=False):
        return '''import QtQuick
import Quickshell
import "shared" as Shared
import "alias" as Alias
ShellRoot {
    Shared.Thing { id: first }
    Alias.Thing { id: second }
''' + ('    Shared.Added { id: added }\n' if extra else '') + '''    Component.onCompleted: console.info("SCANNER_CHECK", first.value, second.value''' + (', added.value' if extra else '') + ''')
}
'''
    shell.write_text(shell_source())
    output = root / 'app.log'
    with output.open('w') as stream:
        proc = subprocess.Popen([binary, '--no-color', '-p', str(entry)],
            stdout=stream, stderr=subprocess.STDOUT,
            env={**os.environ, 'QT_QPA_PLATFORM': os.environ.get('QT_QPA_PLATFORM', 'offscreen')})
        def wait_for(marker):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                text = output.read_text()
                if marker in text:
                    return
                if proc.poll() is not None:
                    raise RuntimeError('Quickshell exited:\n' + text)
                time.sleep(0.1)
            raise RuntimeError('Missing ' + marker + ':\n' + output.read_text())
        try:
            wait_for('SCANNER_CHECK one one')
            print('PASS: symlinked config root and two imports of one directory')
            thing.write_text(thing_source('two'))
            wait_for('SCANNER_CHECK two two')
            print('PASS: edited imported component is hot-reloaded')
            (shared / 'Added.qml').write_text('import QtQuick\nQtObject { property string value: "new-file" }\n')
            shell.write_text(shell_source(extra=True))
            wait_for('SCANNER_CHECK two two new-file')
            print('PASS: new component is discovered on reload')
            # Remove the import and file together, then confirm the new generation.
            shell.write_text(shell_source().replace('SCANNER_CHECK', 'SCANNER_REMOVAL'))
            (shared / 'Added.qml').unlink()
            wait_for('SCANNER_REMOVAL two two')
            print('PASS: deleted component is removed from the next scan')
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)
