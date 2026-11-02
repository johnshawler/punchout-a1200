import oracle, sys, glob
def ev_model(S,W,mode):
    t1=(S['ctrl']>>4)&1; sz=(S['ctrl']>>5)&1
    ln=64 if mode==0 else 32
    rings=1 if mode==0 else 2
    res={}
    st_prev=(1 if S['l0']==0 else 0)|(2 if S['l1']==0 else 0)
    ends=[st_prev]*(rings*30)
    for pas in range(2):
        for q in range(rings*30):
            g=q//30; r=q%30
            st0=ends[q-1] if q else ends[rings*30-1]
            if sz: st0&=~2
            ei=[];et=[]
            for bk in range(1 if mode else 2):
                nt=S['nt'][g if mode else bk]
                for c in range(32):
                    t=nt[r*32+c]
                    if t in (0xFD,0xFE): ei.append(((bk*32+c)-W)&(ln-1)); et.append(t==0xFD)
            ev=sorted(zip(ei,et))
            if mode:
                ev=ev+[(i+32,t) for i,t in ev if i<2]
            end33=st0
            outs=[{},{}]
            st1=None
            for p in range(2):
                st=st1 if p else st0; idx=0
                for i,t in ev:
                    if i<ln and i>=idx:
                        reg=((2 if st&2 else 3) if t1 else (0 if st&1 else 1))
                        for k in range(idx,i+1):
                            L=(W+k)&(ln-1); b=(L>>5) if mode==0 else g
                            outs[p][(b,r,L&31)]=reg
                        idx=i+1
                    elif i>=ln and idx<ln:
                        reg=((2 if st&2 else 3) if t1 else (0 if st&1 else 1))
                        for k in range(idx,ln):
                            L=(W+k)&(ln-1); b=(L>>5) if mode==0 else g
                            outs[p][(b,r,L&31)]=reg
                        idx=ln
                    if t: st|= 2 if t1 else 1
                    else: st&= ~2 if t1 else ~1
                    if p==0 and i<=33: end33=st
                if idx<ln:
                    reg=((2 if st&2 else 3) if t1 else (0 if st&1 else 1))
                    for k in range(idx,ln):
                        L=(W+k)&(ln-1); b=(L>>5) if mode==0 else g
                        outs[p][(b,r,L&31)]=reg
                if p==0:
                    st1=end33
                    if sz: st1&=~2
            ends[q]=end33
            for key in outs[0]: res[key]=[outs[0][key],outs[1][key]]
    return res
bad=0;n=0
frames=sorted(int(f[-5:]) for f in glob.glob('gtall/g.st*'))
import os
os.chdir('.')
for f in frames[::7]:
    d=open('gtall/g.st%05d'%f,'rb').read()
    S=dict(nt=[d[0:0x400],d[0x400:0x800]],ctrl=d[0x920],l0=d[0x92c],l1=d[0x92d],mirr=d[0x92e])
    mode=0 if S['mirr'] else 1
    vl=(d[0x930]|d[0x931]<<8)
    W=((vl>>10)&1)*32+(vl&31) if mode==0 else vl&31
    a=oracle.model(S,W,mode); b=ev_model(S,W,mode)
    n+=1
    if a!=b:
        bad+=1
        if bad<4: 
            diff=[k for k in a if a[k]!=b.get(k)]
            print('frame',f,'mode',mode,'W',W,'diffs',len(diff),diff[:5],[ (a[k],b.get(k)) for k in diff[:5]])
print('frames',n,'bad',bad)
