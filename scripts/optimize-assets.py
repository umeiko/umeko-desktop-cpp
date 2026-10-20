"""Lossless WebP equivalents of the baseline PNGs, with decoded-pixel verification."""
import hashlib
import json
from pathlib import Path
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
rows=[]
for source in sorted((ROOT/'app/icons').rglob('*.png')):
    target=source.with_suffix('.webp')
    with Image.open(source) as image:
        rgba=image.convert('RGBA')
        options=dict(lossless=True,quality=100,method=6,exact=True)
        if image.info.get('icc_profile'): options['icc_profile']=image.info['icc_profile']
        rgba.save(target,'WEBP',**options)
        with Image.open(target) as encoded:
            assert encoded.convert('RGBA').tobytes()==rgba.tobytes(),source
        rows.append(dict(source=str(source.relative_to(ROOT)).replace('\\','/'),target=str(target.relative_to(ROOT)).replace('\\','/'),sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),decodedSha256=hashlib.sha256(rgba.tobytes()).hexdigest(),originalBytes=source.stat().st_size,packedBytes=target.stat().st_size))
(ROOT/'app/icons/lossless-assets.json').write_text(json.dumps(rows,indent=2)+'\n',encoding='utf-8')
print('Verified lossless assets:',len(rows),'bytes:',sum(r['originalBytes'] for r in rows),'->',sum(r['packedBytes'] for r in rows))
