#!/usr/bin/env python3
import os,sys,tempfile,subprocess
from pathlib import Path
binary=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='qs-readonly-cache-') as tmp:
 root=Path(tmp);shared=root/'shared';shared.mkdir()
 (shared/'FixtureState.qml').write_text('pragma Singleton\nimport QtQuick\nQtObject { property string value: "readonly-ok" }\n')
 (shared/'Reader.qml').write_text('''import QtQuick
import "." as Local
QtObject {
//@ if false
 INVALID_QML_MUST_BE_MASKED
//@ endif
 property string value: Local.FixtureState.value
}
''')
 (root/'shell.qml').write_text('''import QtQuick
import Quickshell
import "shared" as Shared
ShellRoot {
 Shared.Reader { id: reader }
 Component.onCompleted: Qt.callLater(function() { console.info("READONLY_RESULT",reader.value); Qt.quit(); })
}
''')
 for p in root.rglob('*.qml'):p.chmod(0o444)
 shared.chmod(0o555);root.chmod(0o555)
 try:
  r=subprocess.run([binary,'--no-color','-p',str(root)],env={**os.environ,'QT_QPA_PLATFORM':os.environ.get('QT_QPA_PLATFORM','offscreen')},capture_output=True,text=True,timeout=10)
  text=r.stdout+r.stderr
  assert r.returncode==0 and 'READONLY_RESULT readonly-ok' in text,text
  assert not (root/'qmldir').exists() and not(shared/'qmldir').exists()
  print('PASS: read-only configs retain virtual module metadata, singleton imports and conditional preprocessing.')
 finally:root.chmod(0o755);shared.chmod(0o755)
