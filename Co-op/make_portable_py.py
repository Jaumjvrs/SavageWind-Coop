"""make_portable_py.py (2026-11-15): a portable Python for the co-op package, so the other PC needs no Python install.

Players need no Python install: the co-op setup must work with nothing installed. This copies the build PC's Python 3.12
(the base install of the venv that has oead) into an output folder (COOP_PY_OUT) in the "embeddable" layout:
python.exe + python312.dll + the runtime DLLs, DLLs/*.pyd, the standard library zipped (python312.zip, minus tests /
tkinter / idle), and site-packages with only oead (build_overlay.py's one dependency). A python312._pth file makes it
ignore any other Python on the PC (no registry, PATH or user site-packages). The package ships it as Co-op/py.
No game files involved. Run with the oead venv's python (Co-op/build_coop.sh does this).
"""
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
# (0.1.1) build_coop.sh (building from source) passes its own output folder and venv
OUT = os.environ.get('COOP_PY_OUT') or os.path.join(HERE, 'local', 'portable-py')
BASE = sys.base_prefix  # the Python install the venv was made from
VENV_SITE = os.environ.get('COOP_PY_SITE') or os.path.join(HERE, 'tools-ext', 'oead-venv', 'Lib', 'site-packages')
assert sys.version_info[:2] == (3, 12), 'run with the oead venv (Python 3.12)'

SKIP_DIRS = {'test', 'tests', 'idle_test', 'tkinter', 'idlelib', 'turtledemo', 'ensurepip', 'site-packages',
             'lib2to3', '__pycache__', 'venv', 'pydoc_data'}
SKIP_DLLS = ('_tkinter', 'tcl', 'tk', '_test', 'xxlimited', '_ctypes_test', 'py.ico', 'pyc.ico')

if os.path.exists(OUT):
    shutil.rmtree(OUT)
os.makedirs(os.path.join(OUT, 'DLLs'))
os.makedirs(os.path.join(OUT, 'site-packages'))
for f in ('python.exe', 'python312.dll', 'python3.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt'):
    src = os.path.join(BASE, f)
    if os.path.exists(src):
        shutil.copy2(src, OUT)
for f in os.listdir(os.path.join(BASE, 'DLLs')):
    if f.lower().startswith(SKIP_DLLS):
        continue
    shutil.copy2(os.path.join(BASE, 'DLLs', f), os.path.join(OUT, 'DLLs'))
lib = os.path.join(BASE, 'Lib')
n = 0
with zipfile.ZipFile(os.path.join(OUT, 'python312.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(lib):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            if f.endswith('.py'):
                p = os.path.join(root, f)
                z.write(p, os.path.relpath(p, lib))
                n += 1
for f in os.listdir(VENV_SITE):  # oead and the C++ runtime DLL its module was built with
    if f.startswith('oead') and f.endswith('.pyd') or f.startswith('msvcp140') or f == 'oead-1.3.0.dist-info':
        src = os.path.join(VENV_SITE, f)
        dst = os.path.join(OUT, 'site-packages', f)
        (shutil.copytree if os.path.isdir(src) else shutil.copy2)(src, dst)
with open(os.path.join(OUT, 'python312._pth'), 'w') as f:
    f.write('python312.zip\n.\nDLLs\nsite-packages\n')
size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(OUT) for f in fs)
print(f'portable Python {sys.version.split()[0]} in {OUT}: {n} stdlib files, {size / 1e6:.1f} MB')
print(sorted(os.listdir(os.path.join(OUT, 'site-packages'))))
