#!/usr/bin/env python3
import csv, collections, json
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from PIL import Image

EDIT=75; FRAMES=150
rows=list(csv.DictReader(open('out/results.csv')))
D=collections.defaultdict(dict)  # D[method][budget] = arrays
for r in rows:
    m,b=r['method'],float(r['budget'])
    e=D[m].setdefault(b,{'f':[],'p':[],'r':[],'ms':[],'st':[]})
    e['f'].append(int(r['frame'])); e['p'].append(float(r['psnr']))
    e['r'].append(int(r['rays'])); e['ms'].append(float(r['ms'])); e['st'].append(int(r['steps']))

COL={'PT':'#d62728','DDGI':'#1f77b4','FCGI':'#2ca02c'}
LBL={'PT':'Path traced 1spp + temporal/spatial filter (Teardown-class)',
     'DDGI':'DDGI / RTXGI-style probe grid (Chebyshev + probe feedback)',
     'FCGI':'FaceCache-GI (proposed)'}

def stats(m,b):
    e=D[m][b]; p=np.array(e['p']); f=np.array(e['f'])
    pre=p[(f>=55)&(f<EDIT)].mean()
    post=p[f>=135].mean()
    # recovery: first frame >= post-1.0 dB after edit
    rec=None
    for fr in range(EDIT,FRAMES):
        if p[f==fr][0]>=post-1.0: rec=fr-EDIT; break
    drop=p[f==EDIT+1][0]
    rays=np.mean(e['r']); ms=np.mean(e['ms']); st=np.mean(e['st'])
    return dict(pre=pre,post=post,rec=rec,drop=drop,rays=rays,ms=ms,steps=st)

summary={m:{b:stats(m,b) for b in sorted(D[m])} for m in D}
json.dump(summary,open('out/summary.json','w'),indent=1,default=float)

# ---- chart 1: PSNR vs frame at 1x budget ----
plt.figure(figsize=(9,4.6))
for m in ['PT','DDGI','FCGI']:
    e=D[m][1.0]
    plt.plot(e['f'],e['p'],color=COL[m],label=LBL[m],lw=1.6)
plt.axvline(EDIT,color='k',ls='--',lw=1,alpha=0.6)
plt.text(EDIT+1.0,plt.ylim()[0]+0.5,'wall destroyed',fontsize=8,rotation=90)
plt.xlabel('frame'); plt.ylabel('PSNR vs converged reference (dB)')
plt.title('Quality over time at equal ray budget (≈40k rays/frame, 128×80)')
plt.legend(fontsize=8,loc='lower right'); plt.grid(alpha=0.3); plt.tight_layout()
plt.savefig('out/chart_psnr_time.png',dpi=150)

# ---- chart 2: Pareto rays/frame vs converged PSNR ----
plt.figure(figsize=(6.4,4.6))
for m in ['PT','DDGI','FCGI']:
    bs=sorted(D[m]); xs=[summary[m][b]['rays'] for b in bs]
    ys=[summary[m][b]['pre'] for b in bs]
    plt.plot(xs,ys,'o-',color=COL[m],label=m if m!='FCGI' else 'FaceCache-GI (ours)')
    ys2=[summary[m][b]['post'] for b in bs]
    plt.plot(xs,ys2,'o--',color=COL[m],alpha=0.45)
plt.xscale('log'); plt.xlabel('rays per frame (log)'); plt.ylabel('converged PSNR (dB)')
plt.title('Quality vs ray budget (solid: pre-edit, dashed: post-edit)')
plt.legend(fontsize=9); plt.grid(alpha=0.3,which='both'); plt.tight_layout()
plt.savefig('out/chart_pareto.png',dpi=150)

# ---- chart 3: response to destruction (zoom) at 1x ----
plt.figure(figsize=(7.2,4.4))
for m in ['PT','DDGI','FCGI']:
    e=D[m][1.0]; f=np.array(e['f']); p=np.array(e['p'])
    sel=(f>=EDIT-6)&(f<=EDIT+45)
    plt.plot(f[sel]-EDIT,p[sel],color=COL[m],label=m if m!='FCGI' else 'FaceCache-GI (ours)',lw=1.8)
plt.xlabel('frames after destruction event'); plt.ylabel('PSNR (dB)')
plt.title('Lighting response to destruction (hole blasted in wall), 1× budget')
plt.legend(fontsize=9); plt.grid(alpha=0.3); plt.tight_layout()
plt.savefig('out/chart_response.png',dpi=150)

# ---- chart 4: CPU ms/frame ----
plt.figure(figsize=(6.4,4.2))
bs=sorted(D['PT'])
w=0.25
for i,m in enumerate(['PT','DDGI','FCGI']):
    plt.bar(np.arange(len(bs))+i*w,[summary[m][b]['ms'] for b in bs],w,color=COL[m],
            label=m if m!='FCGI' else 'FaceCache-GI (ours)')
plt.xticks(np.arange(len(bs))+w,[f'{b}×' for b in bs])
plt.xlabel('ray budget'); plt.ylabel('CPU ms/frame (single thread, same traversal code)')
plt.title('Wall-clock cost per frame'); plt.legend(fontsize=9); plt.grid(alpha=0.3,axis='y')
plt.tight_layout(); plt.savefig('out/chart_ms.png',dpi=150)

# ---- comparison montage at 1x ----
def lab(im,txt):
    from PIL import ImageDraw
    d=ImageDraw.Draw(im); d.rectangle([0,0,im.width,11],fill=(0,0,0))
    d.text((3,1),txt,fill=(255,255,255)); return im
S=2; W,H=128,80
names=[('out/reference_pre.png','REFERENCE (pre-edit)'),('out/PT_b1.0x_f74.png','PT+filter f74'),
       ('out/DDGI_b1.0x_f74.png','DDGI f74'),('out/FCGI_b1.0x_f74.png','FaceCache-GI f74'),
       ('out/reference_post.png','REFERENCE (post-edit)'),('out/PT_b1.0x_f90.png','PT+filter f90 (+15)'),
       ('out/DDGI_b1.0x_f90.png','DDGI f90 (+15)'),('out/FCGI_b1.0x_f90.png','FaceCache-GI f90 (+15)')]
mont=Image.new('RGB',(W*S*4+6,H*S*2+4),(20,20,20))
for i,(p,t) in enumerate(names):
    im=Image.open(p).resize((W*S,H*S),Image.NEAREST)
    im=lab(im,t)
    mont.paste(im,((i%4)*(W*S+2),(i//4)*(H*S+2)))
mont.save('out/comparison.png')

# ---- print summary table ----
print(f"{'method':18s}{'budget':>7s}{'rays/f':>9s}{'pre dB':>8s}{'post dB':>8s}{'drop dB':>8s}{'recover':>8s}{'ms':>7s}")
for m in ['PT','DDGI','FCGI']:
    for b in sorted(summary[m]):
        s=summary[m][b]
        rec='%d f'%s['rec'] if s['rec'] is not None else '>75 f'
        print(f"{m:18s}{b:>6.1f}x{s['rays']:>9.0f}{s['pre']:>8.2f}{s['post']:>8.2f}{s['drop']:>8.2f}{rec:>8s}{s['ms']:>7.1f}")
