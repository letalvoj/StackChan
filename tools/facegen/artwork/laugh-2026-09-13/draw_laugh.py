from pathlib import Path
import xml.etree.ElementTree as E,copy,subprocess,json,zipfile
p=Path(__file__).parent;base=p.parent;S='http://www.w3.org/2000/svg';H='{http://www.w3.org/1999/xlink}href'
E.register_namespace('',S);E.register_namespace('xlink','http://www.w3.org/1999/xlink')
def el(tag,**a):return E.Element('{'+S+'}'+tag,{k.replace('_','-'):str(v) for k,v in a.items()})
def path(d,**a):return el('path',d=d,**a)
def use(id,**a):e=el('use',**a);e.set(H,'#'+id);return e
style=dict(fill='none',stroke='#f4f1e9',stroke_width='4.2',stroke_linecap='round',stroke_linejoin='round')
def root(w,h,v=None):return el('svg',version='1.2',baseProfile='tiny',width=w,height=h,viewBox=v or f'0 0 {w} {h}')
def write(r,file):E.ElementTree(r).write(file,encoding='unicode')
def render(file):subprocess.run(['rsvg-convert',str(file),'-o',str(file.with_suffix('.png'))],check=True)
# Six hand-drawn openings; thin continuous rim and restrained pink tongue.
poses=[
('grin','M-23-4 C-12 13 11 17 24-5',''),
('catch','M-21-6 Q-1-4 22-7 C22 9 13 20 1 20 C-11 20-20 9-21-6Z',''),
('ha','M-24-8 Q0-6 25-9 C25 13 14 27 1 27 C-13 27-23 12-24-8Z',''),
('hee','M-25-5 Q-1-2 25-7 C23 10 11 20-1 19 C-14 18-23 8-25-5Z',''),
('haa','M-23-10 Q2-7 27-9 C27 14 16 30 2 30 C-13 30-23 12-23-10Z',''),
('settle','M-23-3 C-14 11 10 15 24-5','')]
tongues=[None,'M-8 17 Q0 10 9 17 Q1 22-8 17Z','M-10 23 C-6 16 6 16 12 23 Q1 29-10 23Z','M-10 16 Q0 10 10 16 Q0 22-10 16Z','M-10 26 C-6 17 8 18 14 26 Q2 33-10 26Z',None]
defs=el('defs');mouthids=[]
for i,(name,outline,_) in enumerate(poses):
 id=f'mouth-laugh-{i:02d}-{name}';mouthids.append(id);g=el('g',id=id)
 if tongues[i]:g.append(path(tongues[i],fill='#e67c89',stroke='none'))
 g.append(path(outline,stroke_width='4.2'));defs.append(g)
# Original squeeze gestures keep this extension in the approved visual vocabulary.
source=E.parse(base/'face-parts.svg');lookup={e.get('id'):e for e in source.iter() if e.get('id')}
for prefix in ['eyes-smile-squeeze','cheeks-smile-lift']:
 for i in range(6):
  id=f'{prefix}-{i:02d}'
  if id in lookup:defs.append(copy.deepcopy(lookup[id]))
# A stronger squeeze at each ha; retain original smile eyes for recovery.
peak=next(e for e in defs if e.get('id')=='eyes-smile-squeeze-04')
peak.clear();peak.set('id','eyes-laugh-squeeze-peak')
for side,d in [('left','M-58-12 Q-46-7-33 1 Q-45 5-57 13'),('right','M58-13 Q45-6 33 0 Q46 5 57 11')]:
 g=el('g',id='eyes-laugh-squeeze-peak-'+side);g.append(path(d,stroke_width='4.8'));peak.append(g)
# Frames are composable using the established mouth anchor.
(p/'frames').mkdir(exist_ok=True)
for id in mouthids:
 r=root(200,100,'-100 -45 200 100');g=el('g',**style);r.append(g);g.append(copy.deepcopy(next(e for e in defs if e.get('id')==id)));write(r,p/'frames'/(id+'.svg'))
def face(index,eye=3,cheek=3,dy=0):
 g=el('g',transform=f'translate(0 {dy})');g.append(use('eyes-laugh-squeeze-peak' if eye==4 else f'eyes-smile-squeeze-{eye:02d}',transform='translate(160 100) scale(1.5)'));g.append(use(f'cheeks-smile-lift-{cheek:02d}',transform='translate(160 140) scale(1.5)'));g.append(use(mouthids[index],transform='translate(160 161) scale(1.5)'));return g
# Six full-face key drawings, plus one independent mouth row.
r=root(960,620);r.append(el('rect',width=960,height=620,fill='#000'));g=el('g',**style);r.append(g);g.append(copy.deepcopy(defs))
for i,(name,_,_) in enumerate(poses):
 cell=el('g',transform=f'translate({(i%3)*320} {(i//3)*265})');g.append(cell);cell.append(face(i,[2,3,4,3,4,2][i],[2,3,4,3,4,2][i]));t=el('text',x=160,y=249,fill='#aaa69e',stroke='none',font_size=13,font_family='sans-serif',text_anchor='middle');t.text=name;cell.append(t)
for i,id in enumerate(mouthids):g.append(use(id,transform=f'translate({80+i*160} 571) scale(1.3)'))
write(r,p/'laugh-sheet.svg');render(p/'laugh-sheet.svg')
# A laugh is an event, with two uneven bursts and a recovery, not incessant jaw flapping.
sequence=[(0,2,2,0,600),(1,3,3,1,160),(2,4,4,-2,140),(3,3,4,1,100),(4,4,4,-3,180),(3,3,4,1,120),(2,4,4,-1,140),(5,3,3,1,260),(0,2,2,0,1000)]
(p/'preview-frames').mkdir(exist_ok=True)
for j,(m,e,c,y,ms) in enumerate(sequence):
 r=root(320,240);r.append(el('rect',width=320,height=240,fill='#000'));g=el('g',**style);r.append(g);g.append(copy.deepcopy(defs));g.append(face(m,e,c,y));f=p/'preview-frames'/f'{j:02d}.svg';write(r,f);render(f)
 if j==2:write(r,p/'laugh-face.svg');render(p/'laugh-face.svg')
args=['magick']
for j,(*_,ms) in enumerate(sequence):args+=['-delay',str(ms//10),str(p/'preview-frames'/f'{j:02d}.png')]
subprocess.run(args+['-loop','0',str(p/'laugh-stop-motion.gif')],check=True)
(p/'laugh-timing.json').write_text(json.dumps({'kind':'laugh event; preview repeats with a resting hold','duration_ms':sum(x[-1] for x in sequence),'mouth_anchor':[160,161],'eyes_anchor':[160,100],'cheeks_anchor':[160,140],'scale':1.5,'pupils':'hidden during squeezed eyes','interpolation':'discrete held drawings; no path morph','sequence':[dict(mouth=mouthids[m],eyes='eyes-laugh-squeeze-peak' if e==4 else f'eyes-smile-squeeze-{e:02d}',cheeks=f'cheeks-smile-lift-{c:02d}',face_y=y,hold_ms=ms) for m,e,c,y,ms in sequence]},indent=2))
print('Rendered laugh extension')
