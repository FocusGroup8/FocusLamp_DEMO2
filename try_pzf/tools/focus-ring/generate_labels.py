from pathlib import Path
from PIL import Image, ImageFont, ImageDraw
root=Path('head/main/display')
labels=['高度专注','普通专注','分心暂停','等待专注数据','语音暂停','已完成','数据过期','连接中断','数据无效']
font=ImageFont.truetype('C:/Windows/Fonts/simhei.ttf',12)
rows=[]
for text in labels:
    im=Image.new('1',(12*len(text),14));ImageDraw.Draw(im).text((0,0),text,font=font,fill=1)
    bits=[sum((1<<y) for y in range(14) if im.getpixel((x,y))) for x in range(im.width)]
    rows.append('{'+str(im.width)+', {'+','.join(str(v) for v in bits)+'}}')
(root/'focus_ring_labels.h').write_text('#pragma once\n// Generated 12px monochrome labels. No runtime font allocation.\nnamespace focus_ring {\nstruct Label {int width; unsigned short columns[72];};\nstatic constexpr Label labels[] = {\n'+',\n'.join(rows)+'\n};\n}\n',encoding='utf8')
