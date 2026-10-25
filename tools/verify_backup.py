"""Verify rollback archives; optionally restore the runtime into a NEW folder."""
import argparse,hashlib,json,zipfile
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('backup',type=Path)
parser.add_argument('--extract-runtime',type=Path)
args=parser.parse_args()
manifest=json.loads((args.backup/'manifest.json').read_text(encoding='utf-8'))
for name in sorted({item['archive'] for item in manifest}):
    with zipfile.ZipFile(args.backup/name) as archive:
        assert archive.testzip() is None,f'Corrupt archive: {name}'
        for item in [i for i in manifest if i['archive']==name]:
            data=archive.read(item['path'])
            assert len(data)==item['bytes'] and hashlib.sha256(data).hexdigest()==item['sha256'],item['path']
if args.extract_runtime:
    target=args.extract_runtime.resolve()
    if target.exists():raise SystemExit('Restore target must be a new folder')
    target.mkdir(parents=True)
    with zipfile.ZipFile(args.backup/'runtime.zip') as archive:
        for entry in archive.infolist():
            path=(target/entry.filename).resolve()
            assert path.is_relative_to(target),'Unsafe archive path'
        archive.extractall(target)
    for item in [i for i in manifest if i['archive']=='runtime.zip']:
        assert hashlib.sha256((target/item['path']).read_bytes()).hexdigest()==item['sha256']
    print('Runtime restored and verified:',target)
print('Verified manifest entries:',len(manifest))
