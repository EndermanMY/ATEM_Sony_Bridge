"""Create a verified source and portable-runtime rollback before maintenance."""
from pathlib import Path
import hashlib, json, zipfile, datetime, argparse
parser=argparse.ArgumentParser()
parser.add_argument("--runtime",type=Path)
args=parser.parse_args()

root = Path(__file__).resolve().parents[2]
project = root / 'AtemSonyBridge'
runtime=args.runtime or root/'releases'/(root/'releases/current.txt').read_text().strip()
if not (runtime/'AtemSonyBridge.exe').exists():raise SystemExit('Runtime executable missing')
stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
dest = root / 'archives' / ('baseline-' + stamp)
dest.mkdir(parents=True)
manifest = []
for label, base, exclude in [('source', project, {'.git','__pycache__'}),
                              ('runtime', runtime, set())]:
    target = dest / (label + '.zip')
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in sorted(base.rglob('*')):
            rel = p.relative_to(base)
            if not p.is_file(): continue
            if label=='source' and (any(x in exclude or x.startswith('build') for x in rel.parts) or p.suffix in {'.log','.zip','.pyc'}): continue
            data = p.read_bytes()
            z.writestr(str(rel), data)
            manifest.append({'archive': target.name, 'path': rel.as_posix(), 'bytes': len(data),
                             'sha256': hashlib.sha256(data).hexdigest()})
    with zipfile.ZipFile(target) as z:
        assert z.testzip() is None
        for item in [m for m in manifest if m['archive'] == target.name]:
            assert hashlib.sha256(z.read(item['path'])).hexdigest() == item['sha256']
(dest / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
(dest / 'README.txt').write_text('source.zip: source snapshot with vendored dependencies.\nruntime.zip: COMPLETE selected portable runtime, configuration, rules, logs and WebView2 data.\nRestore runtime.zip into a separate folder and run AtemSonyBridge.exe.\nContains private runtime configuration; keep local.\n', encoding='utf-8')
print(dest)
print('Verified files:', len(manifest))
