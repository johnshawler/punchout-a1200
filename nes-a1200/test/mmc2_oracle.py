import struct,sys
chr_=open('chr.bin','rb').read()
def load(f):
    d=open('gt.st%05d'%f,'rb').read()
    nt=[d[0:0x400],d[0x400:0x800]]
    pal=d[0x800:0x820]; oam=d[0x820:0x920]
    ctrl,mask,xo=d[0x920],d[0x921],d[0x922]
    creg=list(d[0x928:0x92c]); l0,l1,mirr=d[0x92c],d[0x92d],d[0x92e]
    vl=[(d[0x930+i*3]|d[0x931+i*3]<<8, d[0x932+i*3]) for i in range(240)]
    return dict(nt=nt,pal=pal,oam=oam,ctrl=ctrl,mask=mask,creg=creg,l0=l0,l1=l1,mirr=mirr,vl=vl)
def ntbank(mirr,n):   # fceumm mirr=1 -> vertical (NT0!=NT1)
    return (n&1) if mirr else (n>>1)
def oracle(S):
    """per line, per fetch index i: (bank regidx, col, row, bankno) for BG table = ctrl bit4; latch fceumm semantic 0=FD"""
    t1=(S['ctrl']>>4)&1
    l1=S['l1']; l0=S['l0']
    out=[]
    for L in range(240):
        v,fx=S['vl'][L]
        line=[]
        vv=v
        for i in range(34):
            n=(vv>>10)&3; col=vv&31; row=(vv>>5)&31
            b=ntbank(S['mirr'],n)
            tile=S['nt'][b][row*32+col] if row<30 else 0
            reg = (2 if l1==0 else 3) if t1 else (0 if l0==0 else 1)
            line.append((reg,b,row,col,tile))
            if tile==0xFD:
                if t1: l1=0
                else: l0=0
            elif tile==0xFE:
                if t1: l1=1
                else: l0=1
            if (vv&31)==31: vv=(vv&~31)^0x400
            else: vv+=1
        out.append(line)
        # sprite phase: 8x16 dummy sets latch1=FE(1); 8x8 nothing for BG (ignoring real sprite flips)
        if S['ctrl']&0x20: l1=1
    return out
def model(S,W,mode=0):
    t1=(S['ctrl']>>4)&1; sz16=(S['ctrl']>>5)&1
    ring=64 if mode==0 else 32
    rings=1 if mode==0 else 2
    res={}
    st_prev=(1 if S['l0']==0 else 0)|(2 if S['l1']==0 else 0)
    ends=[st_prev]*(rings*30)
    for pas in range(2):
        for q in range(rings*30):
            g=q//30; r=q%30
            st=ends[q-1] if q else ends[rings*30-1]
            if sz16: st&=~2
            n=max(ring,34)
            end_s=None
            for p in range(2):
                for i in range(n):
                    L=(W+i)&(ring-1); b=(L>>5) if mode==0 else g; col=L&31
                    tile=S['nt'][b][r*32+col]
                    if i<ring:
                        reg=(2 if (st&2) else 3) if t1 else (0 if (st&1) else 1)
                        res.setdefault((b,r,col),[0,0])[p]=reg
                    if tile==0xFD: st|= 2 if t1 else 1
                    elif tile==0xFE: st&= ~2 if t1 else ~1
                    if i==33 and p==0: end_s=st
                if p==0:
                    st=end_s
                    if sz16: st&=~2
            ends[q]=end_s
    return res
if __name__=='__main__':
    f=int(sys.argv[1]); S=load(f)
    orc=oracle(S)
    v0,fx=S['vl'][0]
    W=((v0>>10)&1)*32+(v0&31)
    mode=0 if S["mirr"] else 1
    if mode==1: W=v0&31
    mod=model(S,W,mode)
    bad=0; tot=0; badrows={}
    for L in range(240):
        row=L//8; s=L%8
        for i in range(32):
            reg,b,r,col,tile=orc[L][i]
            if r>=30: continue
            m=mod[(b,r,col)][0 if s==0 else 1]
            tot+=1
            if m!=reg:
                bad+=1; badrows.setdefault(r,[]).append((L,i,col,tile,reg,m))
    print('W',W,'mismatches',bad,'of',tot)
    for r in sorted(badrows)[:10]: print(' row',r,len(badrows[r]),badrows[r][:4])
