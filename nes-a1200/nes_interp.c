/* nes_interp.c - 6502 interpreter used for code that has no translation (data decoded as code is never executed; paths that the code map
 * missed are).  Runs until the program counter reaches an address that has translated code.  Official opcodes only (as the game). */
#include "nes.h"
#include "nes_ops.h"
#include "platform.h"
static int interp_seen;

#define RD(ad) rd(c, (ad))
static inline uint32_t rd(Cpu *c, uint32_t a) { uint32_t v = nes_rd8(a, c->a6); c->a6 += nes_stall; nes_stall = 0; return v; }
static inline void wr(Cpu *c, uint32_t a, uint32_t v) { nes_wr8(a, v, c->a6); c->a6 += nes_stall; nes_stall = 0; }

int nes_interp(Cpu *c)
{
    nes_stat[6]++;
    uint32_t pc = c->pc;
    uint32_t A = c->a, X = c->x, Y = c->y, S = c->s;
    uint32_t N = (c->ccr & 8) != 0, Z = (c->ccr & 4) != 0, C = c->c != 0, V = c->v != 0;
    int first = 1;
#define SYNC_OUT() do { c->a = (uint8_t)A; c->x = (uint8_t)X; c->y = (uint8_t)Y; c->s = (uint8_t)S; c->c = C ? 0xFF : 0; c->v = V ? 0xFF : 0; \
                        c->ccr = (uint16_t)((N ? 8 : 0) | (Z ? 4 : 0)); c->pc = (uint16_t)pc; } while (0)
#define SYNC_IN()  do { A = c->a; X = c->x; Y = c->y; S = c->s; C = c->c != 0; V = c->v != 0; N = (c->ccr & 8) != 0; Z = (c->ccr & 4) != 0; pc = c->pc; } while (0)
#define NZ(v) do { uint32_t _v = (v) & 0xFF; N = _v >> 7; Z = (_v == 0); } while (0)
#define PUSH(v) do { nes_ram[0x100 + S] = (uint8_t)(v); S = (S - 1) & 0xFF; } while (0)
#define PULL() (S = (S + 1) & 0xFF, nes_ram[0x100 + S])
    for (;;) {
        if (!first && nes_has_entry(pc)) break;
        first = 0;
        uint32_t op = RD(pc); pc = (pc + 1) & 0xFFFF; nes_stat[1]++;
        uint32_t cyc = op_cycles[op];
        uint32_t ad = 0, v;
        uint32_t cc = op & 3, bbb = (op >> 2) & 7, aaa = op >> 5;
        if ((op & 0x1F) == 0x10) {                              /* branches */
            int off = (int8_t)RD(pc); pc = (pc + 1) & 0xFFFF;
            uint32_t flag = (aaa >> 1) == 0 ? N : (aaa >> 1) == 1 ? V : (aaa >> 1) == 2 ? C : Z;
            if (flag == (aaa & 1)) { pc = (pc + off) & 0xFFFF; cyc++; }
            goto done;
        }
        switch (op) {
        case 0x00: { uint32_t r = (pc + 1) & 0xFFFF; PUSH(r >> 8); PUSH(r); PUSH(0x30 | (N << 7) | (V << 6) | (Z << 1) | C);
                     pc = nes_prg[0x1FFFE] | nes_prg[0x1FFFF] << 8; goto done; }
        case 0x20: { uint32_t t = RD(pc); t |= RD(pc + 1) << 8; uint32_t r = (pc + 1) & 0xFFFF; PUSH(r >> 8); PUSH(r); pc = t; goto done; }
        case 0x40: { uint32_t p = PULL(); N = p >> 7 & 1; V = p >> 6 & 1; Z = p >> 1 & 1; C = p & 1; uint32_t l = PULL(); pc = l | PULL() << 8; goto done; }
        case 0x60: { uint32_t l = PULL(); pc = ((l | PULL() << 8) + 1) & 0xFFFF; goto done; }
        case 0x4C: { uint32_t t = RD(pc); t |= RD(pc + 1) << 8; pc = t; goto done; }
        case 0x6C: { uint32_t p = RD(pc); p |= RD(pc + 1) << 8; uint32_t l = RD(p); pc = l | RD((p & 0xFF00) | ((p + 1) & 0xFF)) << 8; goto done; }
        case 0x08: PUSH(0x30 | (N << 7) | (V << 6) | (Z << 1) | C); goto done;
        case 0x28: { uint32_t p = PULL(); N = p >> 7 & 1; V = p >> 6 & 1; Z = p >> 1 & 1; C = p & 1; goto done; }
        case 0x48: PUSH(A); goto done;
        case 0x68: A = PULL(); NZ(A); goto done;
        case 0x18: C = 0; goto done;  case 0x38: C = 1; goto done;
        case 0xB8: V = 0; goto done;
        case 0x58: case 0x78: case 0xD8: case 0xF8: case 0xEA: goto done;
        case 0xAA: X = A; NZ(X); goto done;  case 0x8A: A = X; NZ(A); goto done;
        case 0xA8: Y = A; NZ(Y); goto done;  case 0x98: A = Y; NZ(A); goto done;
        case 0xBA: X = S; NZ(X); goto done;  case 0x9A: S = X; goto done;
        case 0xCA: X = (X - 1) & 0xFF; NZ(X); goto done;  case 0x88: Y = (Y - 1) & 0xFF; NZ(Y); goto done;
        case 0xE8: X = (X + 1) & 0xFF; NZ(X); goto done;  case 0xC8: Y = (Y + 1) & 0xFF; NZ(Y); goto done;
        }
        /* addressing modes */
        if (cc == 1) {
            switch (bbb) {
            case 0: { uint32_t z = (RD(pc) + X) & 0xFF; pc++; ad = RD(z) | RD((z + 1) & 0xFF) << 8; break; }
            case 1: ad = RD(pc); pc++; break;
            case 2: ad = pc; pc++; break;
            case 3: ad = RD(pc); ad |= RD(pc + 1) << 8; pc += 2; break;
            case 4: { uint32_t z = RD(pc); pc++; ad = ((RD(z) | RD((z + 1) & 0xFF) << 8) + Y) & 0xFFFF; break; }
            case 5: ad = (RD(pc) + X) & 0xFF; pc++; break;
            case 6: ad = RD(pc); ad |= RD(pc + 1) << 8; ad = (ad + Y) & 0xFFFF; pc += 2; break;
            default: ad = RD(pc); ad |= RD(pc + 1) << 8; ad = (ad + X) & 0xFFFF; pc += 2; break;
            }
            pc &= 0xFFFF;
            switch (aaa) {
            case 0: A |= RD(ad); NZ(A); break;
            case 1: A &= RD(ad); NZ(A); break;
            case 2: A ^= RD(ad); NZ(A); break;
            case 3: case 7: { v = RD(ad); if (aaa == 7) v ^= 0xFF; uint32_t tt = A + v + C; V = ((~(A ^ v) & (A ^ tt)) >> 7) & 1; C = tt > 255; A = tt & 0xFF; NZ(A); break; }
            case 4: wr(c, ad, A); break;
            case 5: A = RD(ad); NZ(A); break;
            case 6: { v = RD(ad); C = A >= v; NZ(A - v); break; }
            }
        } else if (cc == 2) {
            int acc = (bbb == 2);
            if (!acc) {
                switch (bbb) {
                case 0: ad = pc; pc++; break;
                case 1: ad = RD(pc); pc++; break;
                case 3: ad = RD(pc); ad |= RD(pc + 1) << 8; pc += 2; break;
                case 5: ad = (RD(pc) + ((aaa == 4 || aaa == 5) ? Y : X)) & 0xFF; pc++; break;
                default: ad = RD(pc); ad |= RD(pc + 1) << 8; ad = (ad + ((aaa == 5) ? Y : X)) & 0xFFFF; pc += 2; break;
                }
                pc &= 0xFFFF;
            }
            if (aaa == 4) wr(c, ad, X);
            else if (aaa == 5) { X = RD(ad); NZ(X); }
            else {
                v = acc ? A : RD(ad);
                switch (aaa) {
                case 0: C = v >> 7; v = (v << 1) & 0xFF; break;
                case 1: { uint32_t cin = C; C = v >> 7; v = ((v << 1) | cin) & 0xFF; break; }
                case 2: C = v & 1; v >>= 1; break;
                case 3: { uint32_t cin = C; C = v & 1; v = (v >> 1) | (cin << 7); break; }
                case 6: v = (v - 1) & 0xFF; break;
                default: v = (v + 1) & 0xFF; break;
                }
                NZ(v);
                if (acc) A = v; else wr(c, ad, v);
            }
        } else {                                                /* cc == 0: LDY STY CPY CPX BIT */
            switch (bbb) {
            case 0: ad = pc; pc++; break;
            case 1: ad = RD(pc); pc++; break;
            case 3: ad = RD(pc); ad |= RD(pc + 1) << 8; pc += 2; break;
            case 5: ad = (RD(pc) + X) & 0xFF; pc++; break;
            default: ad = RD(pc); ad |= RD(pc + 1) << 8; ad = (ad + X) & 0xFFFF; pc += 2; break;
            }
            pc &= 0xFFFF;
            switch (aaa) {
            case 1: v = RD(ad); N = v >> 7; V = v >> 6 & 1; Z = (v & A) == 0; break;
            case 4: wr(c, ad, Y); break;
            case 5: Y = RD(ad); NZ(Y); break;
            case 6: v = RD(ad); C = Y >= v; NZ(Y - v); break;
            case 7: v = RD(ad); C = X >= v; NZ(X - v); break;
            default: break;
            }
        }
done:
        c->a6 += (int32_t)cyc;
        if (c->a6 > 0) {
            SYNC_OUT();
            nes_event(c);
            SYNC_IN();
        }
    }
    SYNC_OUT();
    return 1;
}
