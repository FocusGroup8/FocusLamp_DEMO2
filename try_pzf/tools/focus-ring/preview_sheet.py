from PIL import Image,ImageDraw,ImageFont
from pathlib import Path
p=Path('tools/focus-ring/previews');names=['green','orange','red','waiting','voice','completed'];out=Image.new('RGB',(1440,1020),'#171a20');draw=ImageDraw.Draw(out);font=ImageFont.truetype('C:/Windows/Fonts/simhei.ttf',24)
for i,n in enumerate(names):
    source=p/(n+'.ppm')
    im=Image.open(source);im.save(p/(n+'.png'));x=i%3*480;y=i//3*510;out.paste(im,(x,y+30));draw.text((x+16,y),n,font=font,fill='white')
out.save(p/'six-states.png')
