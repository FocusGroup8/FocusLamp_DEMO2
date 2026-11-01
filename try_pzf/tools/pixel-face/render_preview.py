import json
from pathlib import Path
from PIL import Image,ImageDraw
p=Path(__file__).parent
d=json.loads((p/'compiled-preview.json').read_text(encoding='utf-8'))
atlas=Image.new('RGB',(9*160,8*180),'#fffdf5')
for row,r in enumerate(d['roles']):
    for col,e in enumerate(d['expressions']):
        im=Image.new('RGB',(480,480),'black');draw=ImageDraw.Draw(im)
        for x,y,w,h,color,group in r['expressions'][e]['rects']:
            draw.rectangle((x,y,x+w-1,y+h-1),fill=((color>>16)&255,(color>>8)&255,color&255))
        atlas.paste(im.resize((150,150),Image.Resampling.NEAREST),(col*160,row*180))
        ImageDraw.Draw(atlas).text((col*160,row*180+151),r['id']+' / '+e,fill='black')
atlas.save(p/'compiled-atlas.png')
