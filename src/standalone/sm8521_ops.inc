// Clean-room Sharp SM8521 instruction execution body included inside Sm8521Cpu::step().
// Implements all documented SM8521 / SM8500 instruction encodings, flag rules,
// addressing modes, and cycle timings in phi0 ticks without MAME code or macros.
{
    static constexpr uint8_t k_pair_reg[8] = { 0u, 8u, 2u, 10u, 4u, 12u, 6u, 14u };

    const auto fetch8 = [this]() -> uint8_t {
        return mem_readbyte(m_PC++);
    };
    const auto fetch16 = [this]() -> uint16_t {
        const uint16_t hi = mem_readbyte(m_PC++);
        const uint16_t lo = mem_readbyte(m_PC++);
        return static_cast<uint16_t>((hi << 8) | lo);
    };
    const auto sp16_mode = [this]() -> bool {
        return (m_SYS & 0x40u) != 0u;
    };
    const auto push8 = [this, &sp16_mode](uint8_t val) {
        --m_SP;
        if (!sp16_mode()) m_SP &= 0x00FFu;
        mem_writebyte(m_SP, val);
    };
    const auto pop8 = [this, &sp16_mode]() -> uint8_t {
        const uint8_t val = mem_readbyte(m_SP);
        ++m_SP;
        if (!sp16_mode()) m_SP &= 0x00FFu;
        return val;
    };

    const auto cond_true = [this](uint8_t cc) -> bool {
        const bool c = (m_PS1 & FLAG_C) != 0u;
        const bool z = (m_PS1 & FLAG_Z) != 0u;
        const bool s = (m_PS1 & FLAG_S) != 0u;
        const bool v = (m_PS1 & FLAG_V) != 0u;
        switch (cc & 0x0Fu) {
        case 0x00u: return false;            /* F   */
        case 0x01u: return s != v;           /* LT  */
        case 0x02u: return z || (s != v);    /* LE  */
        case 0x03u: return c || z;           /* ULE */
        case 0x04u: return v;                /* OV  */
        case 0x05u: return s;                /* MI  */
        case 0x06u: return z;                /* Z   */
        case 0x07u: return c;                /* C   */
        case 0x08u: return true;             /* T   */
        case 0x09u: return s == v;           /* GE  */
        case 0x0Au: return !z && (s == v);   /* GT  */
        case 0x0Bu: return !c && !z;         /* UGT */
        case 0x0Cu: return !v;               /* NOV */
        case 0x0Du: return !s;               /* PL  */
        case 0x0Eu: return !z;               /* NZ  */
        default:    return !c;               /* NC  */
        }
    };

    /* 8-bit binary ALU: kind 0=CMP, 1=ADD, 2=SUB, 3=ADC, 4=SBC, 5=AND, 6=OR, 7=XOR. */
    const auto alu8 = [this](unsigned kind, uint8_t lhs, uint8_t rhs) -> uint8_t {
        if (kind >= 5u) {
            const uint8_t out = (kind == 5u) ? static_cast<uint8_t>(lhs & rhs)
                              : (kind == 6u) ? static_cast<uint8_t>(lhs | rhs)
                                             : static_cast<uint8_t>(lhs ^ rhs);
            uint8_t ps = static_cast<uint8_t>(m_PS1 & (FLAG_C | FLAG_D | FLAG_H | FLAG_B | FLAG_I));
            if (out == 0u) ps |= FLAG_Z;
            if ((out & 0x80u) != 0u) ps |= FLAG_S;
            m_PS1 = ps;
            return out;
        }

        const bool sub = (kind == 0u || kind == 2u || kind == 4u);
        const uint16_t cin = ((kind == 3u || kind == 4u) && (m_PS1 & FLAG_C) != 0u) ? 1u : 0u;
        const uint16_t wide = sub
            ? static_cast<uint16_t>(static_cast<uint16_t>(lhs) - static_cast<uint16_t>(rhs) - cin)
            : static_cast<uint16_t>(static_cast<uint16_t>(lhs) + static_cast<uint16_t>(rhs) + cin);
        const uint8_t out = static_cast<uint8_t>(wide);

        uint8_t ps = (kind == 0u)
            ? static_cast<uint8_t>(m_PS1 & (FLAG_B | FLAG_I | FLAG_H | FLAG_D))
            : static_cast<uint8_t>(m_PS1 & (FLAG_B | FLAG_I));
        if (sub && kind != 0u) ps |= FLAG_D;
        if ((wide & 0x0100u) != 0u) ps |= FLAG_C;
        if (out == 0u) ps |= FLAG_Z;
        if ((out & 0x80u) != 0u) ps |= FLAG_S;

        const uint8_t sign_match = sub ? static_cast<uint8_t>(lhs ^ rhs)
                                       : static_cast<uint8_t>(~(lhs ^ rhs));
        if (((sign_match & (lhs ^ out)) & 0x80u) != 0u) ps |= FLAG_V;
        if (kind != 0u && (((lhs ^ rhs ^ out) & 0x10u) != 0u)) ps |= FLAG_H;
        m_PS1 = ps;
        return out;
    };

    /* 16-bit binary ALU: kind 0=CMPW, 1=ADDW, 2=SUBW, 3=ADCW, 4=SBCW, 5=ANDW, 6=ORW, 7=XORW. */
    const auto alu16 = [this](unsigned kind, uint16_t lhs, uint16_t rhs) -> uint16_t {
        if (kind >= 5u) {
            const uint16_t out = (kind == 5u) ? static_cast<uint16_t>(lhs & rhs)
                               : (kind == 6u) ? static_cast<uint16_t>(lhs | rhs)
                                              : static_cast<uint16_t>(lhs ^ rhs);
            /* 16-bit logical operations update Z and clear V while preserving S/C/D/H/B/I. */
            uint8_t ps = static_cast<uint8_t>(m_PS1 & (FLAG_C | FLAG_S | FLAG_D | FLAG_H | FLAG_B | FLAG_I));
            if (out == 0u) ps |= FLAG_Z;
            m_PS1 = ps;
            return out;
        }

        const bool sub = (kind == 0u || kind == 2u || kind == 4u);
        const uint32_t cin = ((kind == 3u || kind == 4u) && (m_PS1 & FLAG_C) != 0u) ? 1u : 0u;
        const uint32_t wide = sub
            ? (static_cast<uint32_t>(lhs) - static_cast<uint32_t>(rhs) - cin)
            : (static_cast<uint32_t>(lhs) + static_cast<uint32_t>(rhs) + cin);
        const uint16_t out = static_cast<uint16_t>(wide);

        uint8_t ps = (kind == 0u)
            ? static_cast<uint8_t>(m_PS1 & (FLAG_B | FLAG_I | FLAG_H | FLAG_D))
            : static_cast<uint8_t>(m_PS1 & (FLAG_B | FLAG_I));
        if (sub && kind != 0u) ps |= FLAG_D;
        if ((wide & 0x10000u) != 0u) ps |= FLAG_C;
        if (out == 0u) ps |= FLAG_Z;
        if ((out & 0x8000u) != 0u) ps |= FLAG_S;

        const uint16_t sign_match = sub ? static_cast<uint16_t>(lhs ^ rhs)
                                        : static_cast<uint16_t>(~(lhs ^ rhs));
        if (((sign_match & (lhs ^ out)) & 0x8000u) != 0u) ps |= FLAG_V;
        if (kind != 0u && (((lhs ^ rhs ^ out) & 0x0010u) != 0u)) ps |= FLAG_H;
        m_PS1 = ps;
        return out;
    };

    /* Single-byte unary ALU:
       1=NEG, 2=COM, 3=RR, 4=RL, 5=RRC, 6=RLC, 7=SRL, 8=INC, 9=DEC, 10=SRA, 11=SLL, 12=DA, 13=SWAP. */
    const auto unary8 = [this](unsigned ukind, uint8_t val) -> uint8_t {
        if (ukind == 13u) /* SWAP leaves all flags untouched. */
            return static_cast<uint8_t>((val << 4) | (val >> 4));

        if (ukind == 12u) { /* DA (BCD adjust) */
            uint16_t adj = val;
            uint8_t ps = m_PS1;
            const bool c = (ps & FLAG_C) != 0u;
            const bool d = (ps & FLAG_D) != 0u;
            const bool h = (ps & FLAG_H) != 0u;
            if (d) {
                if (c) adj = static_cast<uint16_t>(adj + (h ? 0x9Au : 0xA0u));
                else if (h) adj = static_cast<uint16_t>(adj + 0xFAu);
            } else if (c) {
                adj = static_cast<uint16_t>(adj + ((h || (adj & 0x0Fu) >= 10u) ? 0x66u : 0x60u));
            } else if (h) {
                if ((adj & 0xF0u) < 0xA0u) {
                    adj = static_cast<uint16_t>(adj + 0x06u);
                } else {
                    adj = static_cast<uint16_t>(adj + 0x66u);
                    ps |= FLAG_C;
                }
            } else if ((adj & 0x0Fu) < 10u) {
                if ((adj & 0xF0u) >= 0xA0u) {
                    adj = static_cast<uint16_t>(adj + 0x60u);
                    ps |= FLAG_C;
                }
            } else if ((adj & 0xF0u) < 0x90u) {
                adj = static_cast<uint16_t>(adj + 0x06u);
            } else {
                adj = static_cast<uint16_t>(adj + 0x66u);
                ps |= FLAG_C;
            }
            const uint8_t out = static_cast<uint8_t>(adj);
            ps &= static_cast<uint8_t>(~(FLAG_Z | FLAG_S));
            if (out == 0u) ps |= FLAG_Z;
            if ((out & 0x80u) != 0u) ps |= FLAG_S;
            m_PS1 = ps;
            return out;
        }

        uint8_t ps = static_cast<uint8_t>(m_PS1 & (FLAG_D | FLAG_H | FLAG_B | FLAG_I));
        uint8_t out = 0u;
        bool update_sign = true;

        switch (ukind) {
        case 1u: /* NEG */
            out = static_cast<uint8_t>(0u - val);
            if (out == 0u) ps |= FLAG_C;
            if (out == 0x80u) ps |= FLAG_V;
            break;
        case 2u: /* COM */
            out = static_cast<uint8_t>(~val);
            ps |= static_cast<uint8_t>(m_PS1 & FLAG_C);
            break;
        case 3u: /* RR */
            out = static_cast<uint8_t>((val >> 1) | (val << 7));
            if ((val & 0x01u) != 0u) ps |= FLAG_C;
            if ((((val ^ out) & 0x80u) != 0u) && ((out & 0x80u) == 0u)) ps |= FLAG_V;
            break;
        case 4u: /* RL */
            out = static_cast<uint8_t>((val << 1) | (val >> 7));
            if ((val & 0x80u) != 0u) ps |= FLAG_C;
            if (((val ^ out) & 0x80u) != 0u) ps |= FLAG_V;
            break;
        case 5u: /* RRC */
            out = static_cast<uint8_t>((val >> 1) | ((m_PS1 & FLAG_C) ? 0x80u : 0x00u));
            if ((val & 0x01u) != 0u) ps |= FLAG_C;
            if ((((val ^ out) & 0x80u) != 0u) && ((out & 0x80u) == 0u)) ps |= FLAG_V;
            break;
        case 6u: /* RLC */
            out = static_cast<uint8_t>((val << 1) | ((m_PS1 & FLAG_C) ? 0x01u : 0x00u));
            if ((val & 0x80u) != 0u) ps |= FLAG_C;
            if (((val ^ out) & 0x80u) != 0u) ps |= FLAG_V;
            break;
        case 7u: /* SRL (S always cleared) */
            out = static_cast<uint8_t>(val >> 1);
            if ((val & 0x01u) != 0u) ps |= FLAG_C;
            update_sign = false;
            break;
        case 8u: /* INC */
            out = static_cast<uint8_t>(val + 1u);
            ps |= static_cast<uint8_t>(m_PS1 & FLAG_C);
            if (val == 0x7Fu) ps |= FLAG_V;
            break;
        case 9u: /* DEC (signed overflow on -128 -> +127, i.e. 0x80 -> 0x7F) */
            out = static_cast<uint8_t>(val - 1u);
            ps |= static_cast<uint8_t>(m_PS1 & FLAG_C);
            if (val == 0x80u) ps |= FLAG_V;
            break;
        case 10u: /* SRA */
            out = static_cast<uint8_t>((val >> 1) | (val & 0x80u));
            if ((val & 0x01u) != 0u) ps |= FLAG_C;
            break;
        default: /* 11u: SLL */
            out = static_cast<uint8_t>(val << 1);
            if ((val & 0x80u) != 0u) ps |= FLAG_C;
            break;
        }

        if (out == 0u) ps |= FLAG_Z;
        if (update_sign && (out & 0x80u) != 0u) ps |= FLAG_S;
        m_PS1 = ps;
        return out;
    };

    /* 8-bit register-indirect addressing (opcodes 0x20..0x29). */
    const auto resolve_rmb = [this, &fetch8](uint8_t desc, unsigned& cycles_out, bool is_store) -> uint16_t {
        const uint8_t ptr_reg = static_cast<uint8_t>(desc & 0x07u);
        uint8_t ea = 0u;
        switch (desc & 0xC0u) {
        case 0x00u:
            ea = mem_readbyte(ptr_reg);
            cycles_out = is_store ? 6u : 7u;
            break;
        case 0x40u:
            ea = mem_readbyte(ptr_reg);
            mem_writebyte(ptr_reg, static_cast<uint8_t>(ea + 1u));
            cycles_out = 8u;
            break;
        case 0x80u:
            ea = fetch8();
            if (ptr_reg != 0u) {
                ea = static_cast<uint8_t>(ea + mem_readbyte(ptr_reg));
                cycles_out = is_store ? 7u : 8u;
            } else {
                cycles_out = is_store ? 9u : 10u;
            }
            break;
        default: /* 0xC0u */
            ea = static_cast<uint8_t>(mem_readbyte(ptr_reg) - 1u);
            mem_writebyte(ptr_reg, ea);
            cycles_out = 9u;
            break;
        }
        return ea;
    };

    /* 16-bit pair-indirect addressing for byte ops (opcodes 0x30..0x39). */
    const auto resolve_rmw = [this, &fetch16](uint8_t desc, unsigned& cycles_out) -> uint16_t {
        const uint8_t pair = k_pair_reg[desc & 0x07u];
        uint16_t ea = 0u;
        switch (desc & 0xC0u) {
        case 0x00u:
            ea = mem_readword(pair);
            cycles_out = 8u;
            break;
        case 0x40u:
            ea = mem_readword(pair);
            mem_writeword(pair, static_cast<uint16_t>(ea + 1u));
            cycles_out = 13u;
            break;
        case 0x80u:
            ea = fetch16();
            if ((desc & 0x07u) != 0u) {
                ea = static_cast<uint16_t>(ea + mem_readword(pair));
                cycles_out = 11u;
            } else {
                cycles_out = 15u;
            }
            break;
        default: /* 0xC0u */
            ea = static_cast<uint16_t>(mem_readword(pair) - 1u);
            mem_writeword(pair, ea);
            cycles_out = 13u;
            break;
        }
        return ea;
    };

    /* 16-bit pair-indirect addressing for word MOVW ops (opcodes 0x3A..0x3B). */
    const auto resolve_smw = [this, &fetch16](uint8_t desc, unsigned& cycles_out) -> uint16_t {
        const uint8_t pair = k_pair_reg[desc & 0x07u];
        uint16_t ea = 0u;
        switch (desc & 0xC0u) {
        case 0x00u:
            ea = mem_readword(pair);
            cycles_out = 11u;
            break;
        case 0x40u:
            ea = mem_readword(pair);
            mem_writeword(pair, static_cast<uint16_t>(ea + 2u));
            cycles_out = 16u;
            break;
        case 0x80u:
            ea = fetch16();
            if ((desc & 0x07u) != 0u) {
                ea = static_cast<uint16_t>(ea + mem_readword(pair));
                cycles_out = 18u;
            } else {
                cycles_out = 14u;
            }
            break;
        default: /* 0xC0u */
            ea = static_cast<uint16_t>(mem_readword(pair) - 2u);
            mem_writeword(pair, ea);
            cycles_out = 16u;
            break;
        }
        return ea;
    };

    /* Indexed bit-operation effective address: 0xFF00+disp when reg==0, else zero-page (disp+R)&0xFF. */
    const auto resolve_bit_idx = [this, &fetch8](uint8_t& desc_out, uint8_t& mask_out) -> uint16_t {
        const uint8_t desc = fetch8();
        desc_out = desc;
        mask_out = static_cast<uint8_t>(1u << (desc & 0x07u));
        const uint8_t disp = fetch8();
        if ((desc & 0x38u) != 0u)
            return static_cast<uint16_t>((disp + mem_readbyte((desc >> 3) & 0x07u)) & 0xFFu);
        return static_cast<uint16_t>(0xFF00u + disp);
    };

    switch (op) {
    case 0x00u: { /* CLR R */
        const uint8_t dst = fetch8();
        mem_writebyte(dst, 0u);
        mycycles += 4;
        break;
    }
    case 0x01u: case 0x02u: case 0x03u: case 0x04u:
    case 0x05u: case 0x06u: case 0x07u: case 0x08u:
    case 0x09u: case 0x0Au: case 0x0Bu: case 0x0Cu:
    case 0x0Du: { /* Direct unary: NEG..SWAP R */
        const uint8_t dst = fetch8();
        const uint8_t out = unary8(op, mem_readbyte(dst));
        mem_writebyte(dst, out);
        mycycles += (op == 0x01u) ? 5 : (op == 0x0Du) ? 7 : 4;
        break;
    }
    case 0x0Eu: { /* PUSH R */
        const uint8_t src = fetch8();
        push8(mem_readbyte(src));
        mycycles += sp16_mode() ? 12 : 10;
        break;
    }
    case 0x0Fu: { /* POP R */
        const uint8_t dst = fetch8();
        mem_writebyte(dst, pop8());
        mycycles += sp16_mode() ? 9 : 8;
        break;
    }
    case 0x10u: case 0x11u: case 0x12u: case 0x13u:
    case 0x14u: case 0x15u: case 0x16u: case 0x17u: { /* Compact ALU r, s */
        const uint8_t desc = fetch8();
        const uint8_t dst = ((desc & 0xC0u) == 0u) ? static_cast<uint8_t>((desc >> 3) & 0x07u) : 0u;
        const uint8_t src = ((desc & 0xC0u) == 0u) ? static_cast<uint8_t>(desc & 0x07u) : 0u;
        const unsigned kind = op & 0x07u;
        const uint8_t out = alu8(kind, mem_readbyte(dst), mem_readbyte(src));
        if (kind != 0u) mem_writebyte(dst, out);
        mycycles += 5;
        break;
    }
    case 0x18u:
    case 0x19u: { /* INCW / DECW RR */
        const uint8_t dst = fetch8();
        const uint16_t before = mem_readword(dst);
        const bool is_dec = (op == 0x19u);
        const uint16_t after = is_dec ? static_cast<uint16_t>(before - 1u)
                                      : static_cast<uint16_t>(before + 1u);
        uint8_t ps = static_cast<uint8_t>(m_PS1 & (FLAG_C | FLAG_D | FLAG_H | FLAG_B | FLAG_I));
        if (after == 0u) ps |= FLAG_Z;
        if ((after & 0x8000u) != 0u) ps |= FLAG_S;
        if ((!is_dec && before == 0x7FFFu) || (is_dec && before == 0x8000u)) ps |= FLAG_V;
        m_PS1 = ps;
        mem_writeword(dst, after);
        mycycles += 8;
        break;
    }
    case 0x1Au: { /* Indirect unary group A: CLR/NEG/COM/RR/RL/RRC/RLC/SRL @r */
        const uint8_t desc = fetch8();
        const uint8_t ptr_reg = ((desc & 0xC0u) == 0u) ? static_cast<uint8_t>((desc >> 3) & 0x07u) : 0u;
        const uint8_t subop = ((desc & 0xC0u) == 0u) ? static_cast<uint8_t>(desc & 0x07u) : 0u;
        const uint8_t addr = mem_readbyte(ptr_reg);
        if (subop == 0u) {
            mem_writebyte(addr, 0u); /* CLR @r does not read the destination byte. */
            mycycles += 7;
        } else {
            const uint8_t out = unary8(subop, mem_readbyte(addr));
            mem_writebyte(addr, out);
            mycycles += (subop == 1u) ? 8 : (subop == 7u) ? 6 : 7;
        }
        break;
    }
    case 0x1Bu: { /* Indirect unary group B: INC/DEC/SRA/SLL/DA/SWAP/PUSH/POP @r */
        const uint8_t desc = fetch8();
        const uint8_t ptr_reg = ((desc & 0xC0u) == 0u) ? static_cast<uint8_t>((desc >> 3) & 0x07u) : 0u;
        const uint8_t subop = ((desc & 0xC0u) == 0u) ? static_cast<uint8_t>(desc & 0x07u) : 0u;
        const uint8_t addr = mem_readbyte(ptr_reg);
        if (subop == 6u) {
            push8(mem_readbyte(addr));
            mycycles += sp16_mode() ? 13 : 8;
        } else if (subop == 7u) {
            mem_writebyte(addr, pop8());
            mycycles += sp16_mode() ? 12 : 11;
        } else {
            const uint8_t out = unary8(8u + subop, mem_readbyte(addr));
            mem_writebyte(addr, out);
            mycycles += (subop == 2u || subop == 3u) ? 6 : 7;
        }
        break;
    }
    case 0x1Cu:
    case 0x1Du: { /* BCLR / BSET 0xFFdd or d8(r), #b */
        uint8_t desc = 0u, mask = 0u;
        const uint16_t addr = resolve_bit_idx(desc, mask);
        const uint8_t cur = mem_readbyte(addr);
        mem_writebyte(addr, (op == 0x1Cu) ? static_cast<uint8_t>(cur & ~mask)
                                          : static_cast<uint8_t>(cur | mask));
        mycycles += ((desc & 0x38u) != 0u) ? 8 : 12;
        break;
    }
    case 0x1Eu: { /* PUSHW RR */
        const uint8_t pair = fetch8();
        push8(mem_readbyte(static_cast<uint32_t>(pair) + 1u));
        push8(mem_readbyte(pair));
        mycycles += sp16_mode() ? 12 : 9;
        break;
    }
    case 0x1Fu: { /* POPW RR */
        const uint8_t pair = fetch8();
        const uint8_t hi = pop8();
        mem_writebyte(pair, hi);
        const uint8_t lo = pop8();
        mem_writebyte(static_cast<uint32_t>(pair) + 1u, lo);
        mycycles += sp16_mode() ? 12 : 13;
        break;
    }
    case 0x20u: case 0x21u: case 0x22u: case 0x23u:
    case 0x24u: case 0x25u: case 0x26u: case 0x27u:
    case 0x28u: { /* Byte ALU / MOV r, rmb */
        const uint8_t desc = fetch8();
        unsigned cyc = 0u;
        const uint16_t ea = resolve_rmb(desc, cyc, false);
        const uint8_t dst_reg = static_cast<uint8_t>((desc >> 3) & 0x07u);
        const uint8_t rhs = mem_readbyte(ea);
        if (op == 0x28u) {
            mem_writebyte(dst_reg, rhs);
        } else {
            const unsigned kind = op & 0x07u;
            const uint8_t out = alu8(kind, mem_readbyte(dst_reg), rhs);
            if (kind != 0u) mem_writebyte(dst_reg, out);
        }
        mycycles += static_cast<int>(cyc);
        break;
    }
    case 0x29u: { /* MOV rmb, r */
        const uint8_t desc = fetch8();
        unsigned cyc = 0u;
        const uint16_t ea = resolve_rmb(desc, cyc, true);
        const uint8_t src_reg = static_cast<uint8_t>((desc >> 3) & 0x07u);
        mem_writebyte(ea, mem_readbyte(src_reg));
        mycycles += static_cast<int>(cyc);
        break;
    }
    case 0x2Au:
    case 0x2Bu: { /* BBC / BBS 0xFFdd or d8(r), #b, rel8 */
        uint8_t desc = 0u, mask = 0u;
        const uint16_t addr = resolve_bit_idx(desc, mask);
        const int8_t rel = static_cast<int8_t>(fetch8());
        const uint16_t target = static_cast<uint16_t>(m_PC + rel);
        const bool bit_set = (mem_readbyte(addr) & mask) != 0u;
        const bool take = (op == 0x2Au) ? !bit_set : bit_set;
        if (take) {
            m_PC = target;
            mycycles += 10;
        } else {
            mycycles += 6;
        }
        if ((desc & 0x38u) == 0u) mycycles += 2;
        break;
    }
    case 0x2Cu: { /* EXTS RR (sign-extend low byte into full 16-bit pair) */
        const uint8_t pair = fetch8();
        const uint8_t lo = mem_readbyte(static_cast<uint32_t>(pair) + 1u);
        mem_writeword(pair, static_cast<uint16_t>(((lo & 0x80u) ? 0xFF00u : 0x0000u) | lo));
        mycycles += 6;
        break;
    }
    case 0x2Du:
    case 0x3Du: { /* DM operand (2-byte hardware NOP, 4 cycles) */
        (void)fetch8();
        mycycles += 4;
        break;
    }
    case 0x2Eu: { /* MOV PS0, #imm */
        m_PS0 = fetch8();
        mem_writebyte(0x1Eu, m_PS0);
        mycycles += 4;
        break;
    }
    case 0x2Fu: { /* BTST R, #mask */
        const uint8_t dst = fetch8();
        const uint8_t mask = fetch8();
        uint8_t ps = static_cast<uint8_t>(m_PS1 & ~(FLAG_Z | FLAG_V));
        if ((mem_readbyte(dst) & mask) == 0u) ps |= FLAG_Z;
        m_PS1 = ps;
        mycycles += 6;
        break;
    }
    case 0x30u: case 0x31u: case 0x32u: case 0x33u:
    case 0x34u: case 0x35u: case 0x36u: case 0x37u:
    case 0x38u: { /* Byte ALU / MOV r, rmw */
        const uint8_t desc = fetch8();
        unsigned cyc = 0u;
        const uint16_t ea = resolve_rmw(desc, cyc);
        const uint8_t dst_reg = static_cast<uint8_t>((desc >> 3) & 0x07u);
        const uint8_t rhs = mem_readbyte(ea);
        if (op == 0x38u) {
            mem_writebyte(dst_reg, rhs);
        } else {
            const unsigned kind = op & 0x07u;
            const uint8_t out = alu8(kind, mem_readbyte(dst_reg), rhs);
            if (kind != 0u) mem_writebyte(dst_reg, out);
        }
        mycycles += static_cast<int>(cyc);
        break;
    }
    case 0x39u: { /* MOV rmw, r */
        const uint8_t desc = fetch8();
        unsigned cyc = 0u;
        const uint16_t ea = resolve_rmw(desc, cyc);
        const uint8_t src_reg = static_cast<uint8_t>((desc >> 3) & 0x07u);
        mem_writebyte(ea, mem_readbyte(src_reg));
        mycycles += static_cast<int>(cyc);
        break;
    }
    case 0x3Au: { /* MOVW RR, smw */
        const uint8_t desc = fetch8();
        unsigned cyc = 0u;
        const uint16_t ea = resolve_smw(desc, cyc);
        const uint8_t dst_pair = k_pair_reg[(desc >> 3) & 0x07u];
        mem_writeword(dst_pair, mem_readword(ea));
        mycycles += static_cast<int>(cyc);
        break;
    }
    case 0x3Bu: { /* MOVW smw, RR */
        const uint8_t desc = fetch8();
        unsigned cyc = 0u;
        const uint16_t ea = resolve_smw(desc, cyc);
        const uint8_t src_pair = k_pair_reg[(desc >> 3) & 0x07u];
        mem_writeword(ea, mem_readword(src_pair));
        mycycles += static_cast<int>(cyc);
        break;
    }
    case 0x3Cu: { /* Compact MOVW RRr, RRs */
        const uint8_t desc = fetch8();
        const uint8_t dst_pair = ((desc & 0xC0u) == 0u) ? k_pair_reg[(desc >> 3) & 0x07u] : 0u;
        const uint8_t src_pair = ((desc & 0xC0u) == 0u) ? k_pair_reg[desc & 0x07u] : 0u;
        mem_writeword(dst_pair, mem_readword(src_pair));
        mycycles += 7;
        break;
    }
    case 0x3Eu:
    case 0x3Fu: { /* JMP / CALL @RR or @ww or ww(r) */
        const uint8_t desc = fetch8();
        uint16_t target = 0u;
        if ((desc & 0xC0u) == 0x00u) {
            target = mem_readword(k_pair_reg[desc & 0x07u]);
        } else if ((desc & 0xC0u) == 0x40u) {
            uint16_t ptr = fetch16();
            if ((desc & 0x38u) != 0u)
                ptr = static_cast<uint16_t>(ptr + mem_readbyte((desc >> 3) & 0x07u));
            target = mem_readword(ptr);
        }
        if (op == 0x3Fu) {
            push8(static_cast<uint8_t>(m_PC));
            push8(static_cast<uint8_t>(m_PC >> 8));
        }
        m_PC = target;
        if (op == 0x3Eu) {
            if ((desc & 0xC0u) == 0x00u) mycycles += 7;
            else if ((desc & 0xC0u) == 0x40u) mycycles += ((desc & 0x38u) != 0u) ? 19 : 15;
            else mycycles += 4;
        } else {
            if ((desc & 0xC0u) == 0x00u) mycycles += sp16_mode() ? 14 : 11;
            else if ((desc & 0xC0u) == 0x40u)
                mycycles += ((desc & 0x38u) != 0u) ? (sp16_mode() ? 26 : 23)
                                                   : (sp16_mode() ? 22 : 19);
            else mycycles += 4;
        }
        break;
    }
    case 0x40u: case 0x41u: case 0x42u: case 0x43u:
    case 0x44u: case 0x45u: case 0x46u: case 0x47u: { /* Direct byte ALU Rr, Rs */
        const uint8_t src = fetch8();
        const uint8_t dst = fetch8();
        const unsigned kind = op & 0x07u;
        const uint8_t out = alu8(kind, mem_readbyte(dst), mem_readbyte(src));
        if (kind != 0u) mem_writebyte(dst, out);
        mycycles += 6;
        break;
    }
    case 0x48u: { /* MOV Rr, Rs (destination is write-only) */
        const uint8_t src = fetch8();
        const uint8_t dst = fetch8();
        mem_writebyte(dst, mem_readbyte(src));
        mycycles += 6;
        break;
    }
    case 0x49u: { /* CALL ad16 */
        const uint16_t target = fetch16();
        push8(static_cast<uint8_t>(m_PC));
        push8(static_cast<uint8_t>(m_PC >> 8));
        m_PC = target;
        mycycles += sp16_mode() ? 12 : 10;
        break;
    }
    case 0x4Au: { /* MOVW RRr, RRs */
        const uint8_t src = fetch8();
        const uint8_t dst = fetch8();
        mem_writeword(dst, mem_readword(src));
        mycycles += 8;
        break;
    }
    case 0x4Bu: { /* MOVW RRr, #imm16 */
        const uint8_t dst = fetch8();
        const uint16_t imm = fetch16();
        mem_writeword(dst, imm);
        mycycles += 9;
        break;
    }
    case 0x4Cu: { /* MULT RRr, Rs (8x8 -> 16: low byte of RRr * Rs) */
        const uint8_t src = fetch8();
        const uint8_t dst = fetch8();
        const uint16_t prod = static_cast<uint16_t>(mem_readbyte(dst | 1u)) *
                              static_cast<uint16_t>(mem_readbyte(src));
        mem_writeword(dst, prod);
        m_PS1 = static_cast<uint8_t>((m_PS1 & ~(FLAG_Z | FLAG_V)) | ((prod == 0u) ? FLAG_Z : 0u));
        mycycles += 24;
        break;
    }
    case 0x4Du: { /* MULT RRr, #imm8 */
        const uint8_t imm = fetch8();
        const uint8_t dst = fetch8();
        const uint16_t prod = static_cast<uint16_t>(mem_readbyte(dst | 1u)) *
                              static_cast<uint16_t>(imm);
        mem_writeword(dst, prod);
        m_PS1 = static_cast<uint8_t>((m_PS1 & ~(FLAG_Z | FLAG_V)) | ((prod == 0u) ? FLAG_Z : 0u));
        mycycles += 24;
        break;
    }
    case 0x4Eu: { /* BMOV BF, Rr, #b  /  BMOV Rr, #b, BF */
        const uint8_t desc = fetch8();
        const uint8_t addr = fetch8();
        const uint8_t mask = static_cast<uint8_t>(1u << (desc & 0x07u));
        if ((desc & 0xC0u) == 0x40u) {
            const uint8_t cur = mem_readbyte(addr);
            const uint8_t out = (m_PS1 & FLAG_B) ? static_cast<uint8_t>(cur | mask)
                                                 : static_cast<uint8_t>(cur & ~mask);
            mem_writebyte(addr, out);
        } else if ((desc & 0xC0u) == 0x00u) {
            uint8_t ps = static_cast<uint8_t>(m_PS1 & (FLAG_C | FLAG_S | FLAG_D | FLAG_H | FLAG_I));
            if ((mem_readbyte(addr) & mask) != 0u) ps |= FLAG_B;
            else ps |= FLAG_Z;
            m_PS1 = ps;
        }
        mycycles += 6;
        break;
    }
    case 0x4Fu: { /* BCMP / BAND / BOR / BXOR BF, Rr, #b */
        const uint8_t desc = fetch8();
        const uint8_t addr = fetch8();
        const bool rbit = (mem_readbyte(addr) & (1u << (desc & 0x07u))) != 0u;
        const bool bbit = (m_PS1 & FLAG_B) != 0u;
        switch (desc & 0xC0u) {
        case 0x00u: /* BCMP */
            m_PS1 = static_cast<uint8_t>((m_PS1 & ~(FLAG_Z | FLAG_V)) | ((rbit == bbit) ? FLAG_Z : 0u));
            break;
        case 0x40u: /* BAND */
            m_PS1 = static_cast<uint8_t>((m_PS1 & ~(FLAG_Z | FLAG_V | FLAG_B)) | ((rbit && bbit) ? FLAG_B : FLAG_Z));
            break;
        case 0x80u: /* BOR */
            m_PS1 = static_cast<uint8_t>((m_PS1 & ~(FLAG_Z | FLAG_V | FLAG_B)) | ((rbit || bbit) ? FLAG_B : FLAG_Z));
            break;
        default: /* 0xC0u: BXOR */
            m_PS1 = static_cast<uint8_t>((m_PS1 & ~(FLAG_Z | FLAG_V | FLAG_B)) | ((rbit != bbit) ? FLAG_B : FLAG_Z));
            break;
        }
        mycycles += 6;
        break;
    }
    case 0x50u: case 0x51u: case 0x52u: case 0x53u:
    case 0x54u: case 0x55u: case 0x56u: case 0x57u: { /* Direct immediate ALU Rr, #imm8 */
        const uint8_t imm = fetch8();
        const uint8_t dst = fetch8();
        const unsigned kind = op & 0x07u;
        const uint8_t out = alu8(kind, mem_readbyte(dst), imm);
        if (kind != 0u) mem_writebyte(dst, out);
        mycycles += 6;
        break;
    }
    case 0x58u: { /* MOV Rr, #imm8 (destination is write-only) */
        const uint8_t imm = fetch8();
        const uint8_t dst = fetch8();
        mem_writebyte(dst, imm);
        mycycles += 6;
        break;
    }
    case 0x5Au:
    case 0x5Bu: { /* Indirect immediate CMP / MOV @r / (r)+ / d8(r) / -(r), #imm8 */
        const uint8_t desc = fetch8();
        const uint8_t first = fetch8();
        const uint8_t ptr_reg = static_cast<uint8_t>(desc & 0x07u);
        uint8_t ea = 0u;
        uint8_t imm = first;
        int extra = 0;
        switch (desc & 0xC0u) {
        case 0x00u:
            ea = mem_readbyte(ptr_reg);
            break;
        case 0x40u:
            ea = mem_readbyte(ptr_reg);
            if (op == 0x5Au) (void)alu8(0u, mem_readbyte(ea), imm);
            else mem_writebyte(ea, imm);
            mem_writebyte(ptr_reg, static_cast<uint8_t>(ea + 1u));
            mycycles += (op == 0x5Au) ? 8 : 7;
            break;
        case 0x80u:
            imm = fetch8();
            ea = static_cast<uint8_t>(first + mem_readbyte(ptr_reg));
            extra = 4;
            break;
        default: /* 0xC0u */
            ea = static_cast<uint8_t>(mem_readbyte(ptr_reg) - 1u);
            mem_writebyte(ptr_reg, ea);
            extra = 2;
            break;
        }
        if ((desc & 0xC0u) != 0x40u) {
            if (op == 0x5Au) (void)alu8(0u, mem_readbyte(ea), imm);
            else mem_writebyte(ea, imm);
            mycycles += ((op == 0x5Au) ? 7 : 6) + extra;
        }
        break;
    }
    case 0x5Cu: { /* DIV RRr, RRs (16 / 8: divisor in low byte of RRs, remainder in high byte of RRs) */
        const uint8_t src = fetch8();
        const uint8_t dst = fetch8();
        const uint8_t divisor = mem_readbyte(src | 1u);
        uint8_t ps = static_cast<uint8_t>(m_PS1 & ~(FLAG_Z | FLAG_V));
        if (divisor != 0u) {
            const uint16_t dividend = mem_readword(dst);
            const uint16_t quot = static_cast<uint16_t>(dividend / divisor);
            const uint8_t rem = static_cast<uint8_t>(dividend % divisor);
            mem_writebyte(static_cast<uint8_t>(src & ~1u), rem);
            mem_writeword(dst, quot);
            if (quot == 0u) ps |= FLAG_Z;
        } else {
            ps |= FLAG_V;
        }
        m_PS1 = ps;
        mycycles += 47;
        break;
    }
    case 0x5Du: { /* DIV RRr, #imm8 */
        const uint8_t divisor = fetch8();
        const uint8_t dst = fetch8();
        uint8_t ps = static_cast<uint8_t>(m_PS1 & ~(FLAG_Z | FLAG_V));
        if (divisor != 0u) {
            const uint16_t dividend = mem_readword(dst);
            const uint16_t quot = static_cast<uint16_t>(dividend / divisor);
            mem_writeword(dst, quot);
            if (quot == 0u) ps |= FLAG_Z;
        } else {
            ps |= FLAG_V;
        }
        m_PS1 = ps;
        mycycles += 44;
        break;
    }
    case 0x5Eu: { /* MOVM Rr, #mask, Rs */
        const uint8_t dst = fetch8();
        const uint8_t mask = fetch8();
        const uint8_t src = fetch8();
        mem_writebyte(dst, static_cast<uint8_t>((mem_readbyte(dst) & mask) | mem_readbyte(src)));
        mycycles += 9;
        break;
    }
    case 0x5Fu: { /* MOVM Rr, #mask, #imm8 */
        const uint8_t dst = fetch8();
        const uint8_t mask = fetch8();
        const uint8_t imm = fetch8();
        mem_writebyte(dst, static_cast<uint8_t>((mem_readbyte(dst) & mask) | imm));
        mycycles += 8;
        break;
    }
    case 0x60u: case 0x61u: case 0x62u: case 0x63u:
    case 0x64u: case 0x65u: case 0x66u: case 0x67u: { /* 16-bit ALU RRr, RRs */
        const uint8_t src = fetch8();
        const uint8_t dst = fetch8();
        const unsigned kind = op & 0x07u;
        const uint16_t out = alu16(kind, mem_readword(dst), mem_readword(src));
        if (kind != 0u) mem_writeword(dst, out);
        mycycles += (kind == 0u) ? 9 : (kind >= 5u) ? 14 : 10;
        break;
    }
    case 0x68u: case 0x69u: case 0x6Au: case 0x6Bu:
    case 0x6Cu: case 0x6Du: case 0x6Eu: case 0x6Fu: { /* 16-bit ALU RRr, #imm16 */
        const uint8_t dst = fetch8();
        const uint16_t imm = fetch16();
        const unsigned kind = op & 0x07u;
        const uint16_t out = alu16(kind, mem_readword(dst), imm);
        if (kind != 0u) mem_writeword(dst, out);
        mycycles += (kind == 0u) ? 9 : (kind >= 5u) ? 13 : 10;
        break;
    }
    case 0x70u: case 0x71u: case 0x72u: case 0x73u:
    case 0x74u: case 0x75u: case 0x76u: case 0x77u: { /* DBNZ r, rel8 */
        const int8_t rel = static_cast<int8_t>(fetch8());
        const uint16_t target = static_cast<uint16_t>(m_PC + rel);
        const uint8_t reg = static_cast<uint8_t>(op & 0x07u);
        const uint8_t next = static_cast<uint8_t>(mem_readbyte(reg) - 1u);
        mem_writebyte(reg, next);
        if (next != 0u) {
            m_PC = target;
            mycycles += 10;
        } else {
            mycycles += 6;
        }
        break;
    }
    case 0x78u: case 0x79u: case 0x7Au: case 0x7Bu:
    case 0x7Cu: case 0x7Du: case 0x7Eu: case 0x7Fu: { /* MOVW RRx, #imm16 */
        const uint8_t pair = k_pair_reg[op & 0x07u];
        const uint16_t imm = fetch16();
        mem_writeword(pair, imm);
        mycycles += 6;
        break;
    }
    case 0x80u: case 0x81u: case 0x82u: case 0x83u:
    case 0x84u: case 0x85u: case 0x86u: case 0x87u:
    case 0x88u: case 0x89u: case 0x8Au: case 0x8Bu:
    case 0x8Cu: case 0x8Du: case 0x8Eu: case 0x8Fu: { /* BBC / BBS R, #b, rel8 */
        const uint8_t addr = fetch8();
        const int8_t rel = static_cast<int8_t>(fetch8());
        const uint16_t target = static_cast<uint16_t>(m_PC + rel);
        const bool bit_set = (mem_readbyte(addr) & (1u << (op & 0x07u))) != 0u;
        const bool take = (op < 0x88u) ? !bit_set : bit_set;
        if (take) {
            m_PC = target;
            mycycles += 10;
        } else {
            mycycles += 6;
        }
        break;
    }
    case 0x90u: case 0x91u: case 0x92u: case 0x93u:
    case 0x94u: case 0x95u: case 0x96u: case 0x97u:
    case 0x98u: case 0x99u: case 0x9Au: case 0x9Bu:
    case 0x9Cu: case 0x9Du: case 0x9Eu: case 0x9Fu: { /* JMP cc, ad16 */
        const uint16_t target = fetch16();
        if (cond_true(op)) m_PC = target;
        mycycles += 6;
        break;
    }
    case 0xA0u: case 0xA1u: case 0xA2u: case 0xA3u:
    case 0xA4u: case 0xA5u: case 0xA6u: case 0xA7u:
    case 0xA8u: case 0xA9u: case 0xAAu: case 0xABu:
    case 0xACu: case 0xADu: case 0xAEu: case 0xAFu: { /* BCLR / BSET R, #b */
        const uint8_t addr = fetch8();
        const uint8_t mask = static_cast<uint8_t>(1u << (op & 0x07u));
        const uint8_t cur = mem_readbyte(addr);
        mem_writebyte(addr, (op < 0xA8u) ? static_cast<uint8_t>(cur & ~mask)
                                         : static_cast<uint8_t>(cur | mask));
        mycycles += 4;
        break;
    }
    case 0xB0u: case 0xB1u: case 0xB2u: case 0xB3u:
    case 0xB4u: case 0xB5u: case 0xB6u: case 0xB7u: { /* MOV r, R */
        const uint8_t src = fetch8();
        mem_writebyte(op & 0x07u, mem_readbyte(src));
        mycycles += 4;
        break;
    }
    case 0xB8u: case 0xB9u: case 0xBAu: case 0xBBu:
    case 0xBCu: case 0xBDu: case 0xBEu: case 0xBFu: { /* MOV R, r */
        const uint8_t dst = fetch8();
        mem_writebyte(dst, mem_readbyte(op & 0x07u));
        mycycles += 4;
        break;
    }
    case 0xC0u: case 0xC1u: case 0xC2u: case 0xC3u:
    case 0xC4u: case 0xC5u: case 0xC6u: case 0xC7u: { /* MOV r, #imm8 */
        const uint8_t imm = fetch8();
        mem_writebyte(op & 0x07u, imm);
        mycycles += 4;
        break;
    }
    case 0xC8u: case 0xC9u: case 0xCAu: case 0xCBu:
    case 0xCCu: case 0xCDu: case 0xCEu: case 0xCFu: { /* MOV SFR(0x10..0x17), #imm8 */
        const uint8_t imm = fetch8();
        mem_writebyte(0x10u + (op & 0x07u), imm);
        mycycles += 4;
        break;
    }
    case 0xD0u: case 0xD1u: case 0xD2u: case 0xD3u:
    case 0xD4u: case 0xD5u: case 0xD6u: case 0xD7u:
    case 0xD8u: case 0xD9u: case 0xDAu: case 0xDBu:
    case 0xDCu: case 0xDDu: case 0xDEu: case 0xDFu: { /* BR cc, rel8 */
        const int8_t rel = static_cast<int8_t>(fetch8());
        const uint16_t target = static_cast<uint16_t>(m_PC + rel);
        if (cond_true(op)) {
            m_PC = target;
            mycycles += 8;
        } else {
            mycycles += 4;
        }
        break;
    }
    case 0xE0u: case 0xE1u: case 0xE2u: case 0xE3u:
    case 0xE4u: case 0xE5u: case 0xE6u: case 0xE7u:
    case 0xE8u: case 0xE9u: case 0xEAu: case 0xEBu:
    case 0xECu: case 0xEDu: case 0xEEu: case 0xEFu: { /* CALS 0x1000..0x1FFF */
        const uint8_t lo = fetch8();
        const uint16_t target = static_cast<uint16_t>(0x1000u | ((op & 0x0Fu) << 8) | lo);
        push8(static_cast<uint8_t>(m_PC));
        push8(static_cast<uint8_t>(m_PC >> 8));
        m_PC = target;
        mycycles += sp16_mode() ? 12 : 9;
        break;
    }
    case 0xF0u: /* STOP */
        mycycles += 2;
        if ((m_CKC & 0x80u) != 0u) commit_ckc_prescale();
        if (m_stop_mode_enabled) {
            m_stopped = 1;
            m_machine.cpu.stopped = true;
        }
        m_clock_changed = 0;
        break;
    case 0xF1u: /* HALT */
        m_halted = 1;
        mycycles += 2;
        break;
    case 0xF8u: { /* RET */
        const uint8_t hi = pop8();
        const uint8_t lo = pop8();
        m_PC = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
        mycycles += sp16_mode() ? 10 : 8;
        break;
    }
    case 0xF9u: { /* IRET */
        m_PS1 = pop8();
        const uint8_t hi = pop8();
        const uint8_t lo = pop8();
        m_PC = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
        m_irq_defer_one = true;
        mycycles += sp16_mode() ? 12 : 10;
        break;
    }
    case 0xFAu: /* CLRC */
        m_PS1 &= static_cast<uint8_t>(~FLAG_C);
        mycycles += 2;
        break;
    case 0xFBu: /* COMC */
        m_PS1 ^= FLAG_C;
        mycycles += 2;
        break;
    case 0xFCu: /* SETC */
        m_PS1 |= FLAG_C;
        mycycles += 2;
        break;
    case 0xFDu: /* EI */
        m_PS1 |= FLAG_I;
        mycycles += 2;
        break;
    case 0xFEu: /* DI */
        m_PS1 &= static_cast<uint8_t>(~FLAG_I);
        mycycles += 2;
        break;
    case 0xFFu: /* NOP */
        mycycles += 2;
        break;
    default: /* Undefined opcodes (0x59, 0xF2..0xF7) */
        mycycles += 2;
        break;
    }
}
