"""Compile the supplied orthogonal SVG design into small colored rectangles.
No SVG interpreter or full-frame image assets are needed on the device.
"""
import json, re, sys, zipfile, xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WEB = Path('C:/Users/Administrator/Desktop/focus_code/focus-studio-pzf')
ROLES = ['yoyo','diggy','dilly','dash','scout','phew','chacha','tock']
EXPRS = ['neutral','happy','sad','angry','surprised','sleepy','bored','wink_left','wink_right']

def polygon(d):
    tokens = re.findall(r'[MLHVZmlhvz]|-?\d+(?:\.\d+)?', d)
    points=[]; x=y=0; i=0; command=''
    while i < len(tokens):
        if tokens[i].isalpha(): command=tokens[i]; i+=1
        if command=='Z': break
        if command not in ('M','L','H','V'): raise ValueError('Unsupported SVG command '+command)
        if command in ('M','L'): x,y=float(tokens[i]),float(tokens[i+1]); i+=2
        elif command=='H': x=float(tokens[i]); i+=1
        else: y=float(tokens[i]); i+=1
        points.append((x,y))
    return points

def inside(points,x,y):
    hit=False
    for a,b in zip(points,points[1:]+points[:1]):
        if (a[1]>y)!=(b[1]>y) and x < (b[0]-a[0])*(y-a[1])/(b[1]-a[1])+a[0]: hit=not hit
    return hit

def compile_svg(svg):
    root=ET.fromstring(svg); clips={}
    for e in root.iter():
        if e.tag.endswith('clipPath'): clips[e.attrib['id']]=polygon(next(iter(e)).attrib['d'])
    pixels=[[None]*80 for _ in range(80)]
    def walk(e,tx=0,ty=0,clip=None,group=0):
        if e.tag.endswith(('defs','title')): return
        if 'transform' in e.attrib:
            m=re.fullmatch(r'translate\(([-\d.]+)[ ,]+([-\d.]+)\)',e.attrib['transform'])
            if not m: raise ValueError('Unsupported transform')
            tx+=float(m[1]); ty+=float(m[2])
        ident=e.attrib.get('id','')
        if ident=='eye_robot_right': group=1
        if ident=='eye_robot_left': group=2
        if 'clip-path' in e.attrib: clip=clips[e.attrib['clip-path'][5:-1]]
        if e.tag.endswith('path'):
            p=polygon(e.attrib['d']); fill=e.attrib.get('fill','#000000'); color=int(fill[1:],16)
            for gy in range(80):
                y=gy*6+3-ty
                for gx in range(80):
                    x=gx*6+3-tx
                    if inside(p,x,y) and (clip is None or inside(clip,x,y)):
                        pixels[gy][gx]=None if color==0 else (color,group)
        for child in e: walk(child,tx,ty,clip,group)
    walk(root)
    rects=[]; active={}
    for y,row in enumerate(pixels):
        next_active={}; x=0
        while x<80:
            item=row[x]; start=x; x+=1
            while x<80 and row[x]==item: x+=1
            if item is None: continue
            key=(start,x-start,*item)
            if key in active: index=active[key]; rects[index][3]+=6
            else: index=len(rects); rects.append([start*6,y*6,(x-start)*6,6,*item])
            next_active[key]=index
        active=next_active
    return rects

def main(source):
    output=[]; arrays=[]; refs=[]
    with zipfile.ZipFile(source) as z:
        for role in ROLES:
            mapping=json.loads(z.read(role+'/mapping.json'))
            record={'id':role,'name':mapping['role']['name'],'visual':mapping['visual'],'expressions':{}}
            for expr in EXPRS:
                rects=compile_svg(z.read(f'{role}/svg/{expr}.svg'))
                record['expressions'][expr]={'rects':rects,'motion':next(e['motion'] for e in mapping['expressions'] if e['id']==expr)}
                name=f'face_{role}_{expr}'
                arrays.append('static const PixelRect '+name+'[] = {'+','.join('{'+','.join(map(str,r[:4]))+',0x%06x,%d}'%(r[4],r[5]) for r in rects)+'};')
                refs.append('{'+name+', sizeof('+name+')/sizeof(PixelRect)}')
            output.append(record)
    data={'version':1,'canvas':[480,480],'expressions':EXPRS,'roles':output}
    (WEB/'src/pixel-faces-data.js').write_text('export default '+json.dumps(data,ensure_ascii=False,separators=(',',':'))+';\n',encoding='utf-8')
    header='#pragma once\n#include <stdint.h>\nstruct PixelRect {int16_t x,y,w,h; uint32_t color; uint8_t group;};\nstruct PixelFace {const PixelRect *rects; unsigned count;};\n'
    header+='\n'.join(arrays)+'\nstatic const PixelFace pixel_faces[8][9] = {'+','.join('{'+','.join(refs[i:i+9])+'}' for i in range(0,72,9))+'};\n'
    (ROOT/'head/main/display/pixel_faces_data.h').write_text(header,encoding='utf-8')
    print(json.dumps({'roles':8,'expressions':72,'rectangles':sum(len(e['rects']) for r in output for e in r['expressions'].values()),'header_bytes':len(header)}))

if __name__=='__main__': main(sys.argv[1])
