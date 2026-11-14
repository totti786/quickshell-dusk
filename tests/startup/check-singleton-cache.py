#!/usr/bin/env python3
import os,sys,tempfile,subprocess,time
from pathlib import Path
binary=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='qs-singleton-cache-') as tmp:
 root=Path(tmp);shared=root/'shared';shared.mkdir()
 state=shared/'FixtureState.qml'
 def src(value,extra=False):return 'pragma Singleton\nimport QtQuick\nQtObject {\n'+(' property int insertedBeforeValue: 42\n' if extra else '')+' property string value: "'+value+'"\n}\n'
 state.write_text(src('one'))
 (shared/'Reader.qml').write_text('import QtQuick\nimport "." as Local\nQtObject { property string value: Local.FixtureState.value }\n')
 (root/'shell.qml').write_text('''//@ pragma Env QML_DISK_CACHE=qmlc-read,qmlc-write
import QtQuick
import Quickshell
import "shared" as Shared
ShellRoot {
 Shared.Reader { id: reader }
 Component.onCompleted: console.info("SCHEMA_RESULT", reader.value)
}
''')
 log=root/'app.log'
 env={**os.environ,'QT_QPA_PLATFORM':os.environ.get('QT_QPA_PLATFORM','offscreen')}
 def wait(app,marker):
  until=time.monotonic()+10
  while time.monotonic()<until:
   text=log.read_text()
   if marker in text:return
   if app.poll() is not None:raise RuntimeError(text)
   time.sleep(.1)
  raise RuntimeError(log.read_text())
 with log.open('w') as f:
  app=subprocess.Popen([binary,'--no-color','-p',str(root)],env=env,stdout=f,stderr=subprocess.STDOUT)
  try:
   wait(app,'SCHEMA_RESULT one');state.write_text(src('two',True));wait(app,'SCHEMA_RESULT two')
   state.write_text(src('three'));wait(app,'SCHEMA_RESULT three')
  finally:app.terminate();app.wait(timeout=10)
 with log.open('w') as f:
  app=subprocess.Popen([binary,'--no-color','-p',str(root)],env=env,stdout=f,stderr=subprocess.STDOUT)
  try:wait(app,'SCHEMA_RESULT three')
  finally:app.terminate();app.wait(timeout=10)
 print('PASS: singleton property additions/removals update unchanged readers on reload and fresh cached launch.')
