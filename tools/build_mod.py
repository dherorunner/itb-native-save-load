"""Standalone public-source build. Standard library only; never installs or deploys."""
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    config = json.loads((root / 'release.json').read_text(encoding='utf-8'))
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file():
        raise SystemExit('Existing Visual Studio C++ tools not found; no automatic installation.')
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    if not install:
        raise SystemExit('Existing MSVC x86/x64 C++ tools missing; no automatic installation.')
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars32.bat'
    if not vcvars.is_file():
        raise SystemExit('MSVC x86 environment script not found.')
    out = root / 'build' / datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    out.mkdir(parents=True, exist_ok=False)
    print('OUTPUT', out, flush=True)
    native = root / 'src/native'
    commands = ['@echo off', 'chcp 65001 > nul',
        'call "%s" > environment.log 2>&1' % vcvars, 'if errorlevel 1 exit /b 1',
        'cl /nologo /c /EHsc /W4 /utf-8 /Od /MT /DITB_SL_NATIVE_EXPERIMENT=1 /DITB_SL_P04_SERVICE=1 /Fo:native.obj "%s" > native.log 2>&1' % (native / 'native_roundtrip.cpp'),
        'if errorlevel 1 exit /b 1',
        'cl /nologo /c /EHsc /W4 /utf-8 /Od /MT /Fo:service.obj "%s" > service.log 2>&1' % (native / 'p04_service.cpp'),
        'if errorlevel 1 exit /b 1',
        'link /nologo /DLL /OUT:itb_sl_p04.dll native.obj service.obj bcrypt.lib ole32.lib > link.log 2>&1',
        'if errorlevel 1 exit /b 1']
    batch = out / 'compile.cmd'
    batch.write_text('\r\n'.join(commands) + '\r\n', encoding='utf-8')
    result = subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=out,
        env=dict(os.environ, TEMP=str(out), TMP=str(out), VSCMD_SKIP_SENDTELEMETRY='1'), timeout=240)
    batch.unlink()
    if result.returncode:
        raise SystemExit('Compilation failed; inspect logs in %s' % out)
    package = out / 'package/itb_sl'
    (package / 'native').mkdir(parents=True)
    (package / 'scripts').mkdir()
    shutil.copy2(out / 'itb_sl_p04.dll', package / 'native/itb_sl_p04.dll')
    source = root / 'src/mod/scripts'
    parts = ['-- generated P04 bundle\nlocal __p04 = {}\n']
    for name in ['reader', 'store', 'controller', 'ui', 'escape_menu']:
        parts.append('\n__p04[%r] = (function()\n%s\nend)()\n' %
                     (name, (source / (name + '.lua')).read_text(encoding='utf-8')))
    parts.append('\n' + (source / 'init.lua').read_text(encoding='utf-8'))
    (package / 'scripts/init.lua').write_text(''.join(parts), encoding='utf-8')
    for name in config['documents']:
        shutil.copy2(root / name, package / name)
    (package / 'VERSION').write_text(config['version'] + '\n', encoding='utf-8')
    manifest = dict(config)
    manifest['artifact_kind'] = 'local-source-build'
    manifest['validation'] = {'build': 'compiled only; no in-game validation of this new binary'}
    manifest['files'] = {p.relative_to(package).as_posix(): sha256(p)
                         for p in sorted(package.rglob('*')) if p.is_file()}
    (package / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    archive = out / ('ITB-Native-Save-Load-v' + config['version'] + '-local-build.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as target:
        for path in sorted(package.rglob('*')):
            if path.is_file():
                target.write(path, 'itb_sl/' + path.relative_to(package).as_posix())
    print('BUILT', archive, flush=True)
    print('New local DLL; not the historically validated release DLL. No deployment or game execution.', flush=True)


if __name__ == '__main__':
    main()
