#!/usr/bin/env python3
import os,sys,tempfile,subprocess,time
from pathlib import Path
binary=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='qs-preprocessor-') as tmp:
 root=Path(tmp);(root/'shared').mkdir()
 thing=root/'shared/Masked.qml'
 def source(value):
  return '''import QtQuick
QtObject {
//@ if false
 INVALID_QML_MUST_BE_MASKED
//@ endif
 property string value: "'''+value+'"\n}\n'
 thing.write_text(source('one'))
 (root/'shell.qml').write_text('''import QtQuick
import Quickshell
import "shared" as Shared
ShellRoot {
 Shared.Masked { id: masked }
 Component.onCompleted: console.info("MASK_RESULT", masked.value)
}
''')
 log=root/'log'
 with log.open('w') as stream:
  app=subprocess.Popen([binary,'--no-color','-p',str(root)],env={**os.environ,'QT_QPA_PLATFORM':os.environ.get('QT_QPA_PLATFORM','offscreen')},stdout=stream,stderr=subprocess.STDOUT)
  def wait(marker):
   end=time.monotonic()+10
   while time.monotonic()<end:
    text=log.read_text()
    if marker in text:return
    if app.poll() is not None:raise RuntimeError(text)
    time.sleep(.1)
   raise RuntimeError(log.read_text())
  try:
   wait('MASK_RESULT one');thing.write_text(source('two'));wait('MASK_RESULT two')
   print('PASS: conditional masking and edits use the preprocessed source, including reloads.')
  finally:
   if app.poll() is None:app.terminate();app.wait(timeout=10)
