# PyInstaller spec. Build with: pyinstaller donky.spec
# Result: dist/donky/donky.exe + dist/donky/examples/ + dist/donky/lib/ + dist/donky/docs/

a = Analysis(
    ['entry.py'],
    pathex=[],
    binaries=[],
    datas=[
        ('examples', 'examples'),
        ('lib', 'lib'),
        ('compiler', 'compiler'),
    ],
    hiddenimports=[],
    hookspath=[],
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='donky',
    debug=False,
    strip=False,
    upx=False,
    console=True,
    icon='donky.ico',
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    name='donky',
)