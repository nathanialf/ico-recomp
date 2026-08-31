//! Unit tests for tricky encodings. Expected strings follow the
//! spimdisasm output conventions the verify-decode gate diffs against.
//!
//! All test words are synthetic: constructed here from the documented
//! field layouts (or spot-checked against the MIT rabbitizer library's
//! output for the same word). Nothing is copied from game disassembly.

use crate::decode;

fn fmt(word: u32) -> String {
    decode(word, 0x0010_0000).to_string()
}

fn fmt_at(word: u32, vram: u32) -> String {
    decode(word, vram).to_string()
}

/// Build a COP2 special word: opcode 0x12, bit 25 set, then
/// dest/ft/fs/fd fields and the 6-bit funct.
fn c2(dest: u32, ft: u32, fs: u32, fd: u32, funct: u32) -> u32 {
    (0x12 << 26) | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct
}

#[test]
fn basic_integer() {
    assert_eq!(fmt(0x27A3FFC0), "addiu $3, $29, -0x40");
    assert_eq!(fmt(0x24020123), "addiu $2, $0, 0x123");
    assert_eq!(fmt(0x3C02BEEF), "lui $2, 0xBEEF");
    assert_eq!(fmt(0x3082FFFF), "andi $2, $4, 0xFFFF");
    assert_eq!(fmt(0x2882FFFF), "slti $2, $4, -0x1");
    assert_eq!(fmt(0x2C82FFFF), "sltiu $2, $4, -0x1");
    assert_eq!(fmt(0x00000000), "nop");
    assert_eq!(fmt(0x0004243C), "dsll32 $4, $4, 16");
    assert_eq!(fmt(0x00041022), "neg $2, $4");
    assert_eq!(fmt(0x00041023), "negu $2, $4");
    // No move pseudo: or/daddu with a zero operand stay raw.
    assert_eq!(fmt(0x00802025), "or $4, $4, $0");
    assert_eq!(fmt(0x0060282D), "daddu $5, $3, $0");
    assert_eq!(fmt(0x0004102F), "dsubu $2, $0, $4");
}

#[test]
fn loads_stores() {
    assert_eq!(fmt(0x8C82FFF0), "lw $2, -0x10($4)");
    assert_eq!(fmt(0x7D270030), "sq $7, 0x30($9)");
    assert_eq!(fmt(0x79A1FFF0), "lq $1, -0x10($13)");
    assert_eq!(fmt(0x69A1FFF0), "ldl $1, -0x10($13)");
    assert_eq!(fmt(0xD9450020), "lqc2 $vf5, 0x20($10)");
    assert_eq!(fmt(0xF945FFF0), "sqc2 $vf5, -0x10($10)");
    assert_eq!(fmt(0xBC500010), "cache 0x10, 0x10($2)");
    assert_eq!(fmt(0xCC410000), "pref 0x01, 0x0($2)");
}

#[test]
fn branches_and_jumps() {
    // beq $0,$0 -> b; beq rs,$0 -> beqz; bne rs,$0 -> bnez.
    assert_eq!(fmt_at(0x10000004, 0x00200000), "b .L00200014");
    assert_eq!(fmt_at(0x10E00010, 0x00200000), "beqz $7, .L00200044");
    assert_eq!(fmt_at(0x14E0FFFF, 0x00200000), "bnez $7, .L00200000");
    // Likely branches keep both registers (no beqzl/bnezl pseudo).
    assert_eq!(fmt_at(0x50400001, 0x00100000), "beql $2, $0, .L00100008");
    assert_eq!(fmt_at(0x04010001, 0x00100000), "bgez $0, .L00100008");
    assert_eq!(fmt_at(0x04110001, 0x00100000), "bal .L00100008");
    assert_eq!(fmt_at(0x0C0402E2, 0x00100000), "jal .L00100B88");
    assert_eq!(fmt(0x03E00008), "jr $31");
    assert_eq!(fmt(0x0060F809), "jalr $3");
    assert_eq!(fmt(0x00602009), "jalr $4, $3");
}

#[test]
fn hilo_and_traps() {
    assert_eq!(fmt(0x02051018), "mult $2, $16, $5");
    assert_eq!(fmt(0x02050018), "mult $0, $16, $5");
    assert_eq!(fmt(0x00480019), "multu $2, $8");
    assert_eq!(fmt(0x014C001A), "div $0, $10, $12");
    assert_eq!(fmt(0x70850018), "mult1 $0, $4, $5");
    assert_eq!(fmt(0x7222001A), "div1 $0, $17, $2");
    assert_eq!(fmt(0x70821001), "maddu $2, $4, $2");
    assert_eq!(fmt(0x0000010C), "syscall 4");
    assert_eq!(fmt(0x0000000D), "break 0");
    assert_eq!(fmt(0x03FFFFCD), "break 1023, 1023");
    assert_eq!(fmt(0x000001CD), "break 0, 7");
    assert_eq!(fmt(0x00A4E834), "teq $5, $4, 928");
    assert_eq!(fmt(0x040CFFFF), "teqi $0, -0x1");
    assert_eq!(fmt(0x04D80010), "mtsab $6, 0x10");
    assert_eq!(fmt(0x04D90010), "mtsah $6, 0x10");
    assert_eq!(fmt(0x00001028), "mfsa $2");
    assert_eq!(fmt(0x00400029), "mtsa $2");
    assert_eq!(fmt(0x0000000F), "sync");
    assert_eq!(fmt(0x0000040F), "sync.p");
}

#[test]
fn cop0() {
    assert_eq!(fmt(0x40026000), "mfc0 $2, $12");
    assert_eq!(fmt(0x40826000), "mtc0 $2, $12");
    assert_eq!(fmt(0x42000018), "eret");
    assert_eq!(fmt(0x42000038), "ei");
    assert_eq!(fmt(0x42000039), "di");
    assert_eq!(fmt(0x42000002), "tlbwi");
    assert_eq!(fmt(0x42000008), "tlbp");
}

#[test]
fn cop1() {
    assert_eq!(fmt(0x46000864), "cvt.w.s $f1, $f1");
    assert_eq!(fmt(0x46800860), "cvt.s.w $f1, $f1");
    assert_eq!(fmt(0x46020030), "c.f.s $f0, $f2");
    assert_eq!(fmt(0x46001034), "c.lt.s $f2, $f0");
    assert_eq!(fmt(0x44816000), "mtc1 $1, $f12");
    assert_eq!(fmt(0x44026000), "mfc1 $2, $f12");
    assert_eq!(fmt(0x4442F800), "cfc1 $2, $31");
    assert_eq!(fmt(0x46000004), "sqrt.s $f0, $f0");
    assert_eq!(fmt(0x46040084), "sqrt.s $f2, $f4");
    // adda.s writes ACC: fd field must be zero.
    assert_eq!(fmt(0x46020018), "adda.s $f0, $f2");
    // fmt = D is not implemented on the EE.
    assert_eq!(fmt(0x46200000), ".word 0x46200000");
}

#[test]
fn mmi_tables() {
    assert_eq!(fmt(0x712856E8), "qfsrv $10, $9, $8");
    assert_eq!(fmt(0x70081EE9), "pcpyh $3, $8");
    assert_eq!(fmt(0x70084136), "psrlh $8, $8, 4");
    assert_eq!(fmt(0x7003217C), "psllw $4, $3, 5");
    assert_eq!(fmt(0x70001070), "pmfhl.uw $2");
    assert_eq!(fmt(0x70000130), "pmfhl.sh $0");
    assert_eq!(fmt(0x70000031), "pmthl.lw $0");
    assert_eq!(fmt(0x70A81A89), "pinth $3, $5, $8");
    assert_eq!(fmt(0x70081EC9), "prevh $3, $8");
    assert_eq!(fmt(0x70A81889), "psllvw $3, $8, $5");
    assert_eq!(fmt(0x70A81809), "pmaddw $3, $5, $8");
    assert_eq!(fmt(0x70081868), "pabsw $3, $8");
    assert_eq!(fmt(0x70081F88), "pext5 $3, $8");
    // psrlh with a nonzero rs field is not a valid encoding.
    assert_eq!(fmt(0x70420436), ".word 0x70420436");
    // pdivbw writes LO/HI: rd must be zero.
    assert_eq!(fmt(0x70A81F49), ".word 0x70A81F49");
    assert_eq!(fmt(0x70A80749), "pdivbw $5, $8");
}

#[test]
fn cop2_transfers() {
    assert_eq!(fmt(0x4829A000), "qmfc2.ni $9, $vf20");
    assert_eq!(fmt(0x4829A001), "qmfc2.i $9, $vf20");
    assert_eq!(fmt(0x48A83000), "qmtc2.ni $8, $vf6");
    assert_eq!(fmt(0x48491000), "cfc2.ni $9, $vi2");
    assert_eq!(fmt(0x48C20801), "ctc2.i $2, $vi1");
}

#[test]
fn cop2_broadcast_and_dest() {
    assert_eq!(fmt(c2(0xF, 9, 4, 6, 0x2C)), "vsub.xyzw $vf6, $vf4, $vf9");
    assert_eq!(fmt(c2(0xE, 8, 7, 6, 0x2A)), "vmul.xyz $vf6, $vf7, $vf8");
    assert_eq!(fmt(c2(0x8, 3, 2, 1, 0x01)), "vaddy.x $vf1, $vf2, $vf3y");
    assert_eq!(fmt(c2(0x8, 3, 2, 1, 0x02)), "vaddz.x $vf1, $vf2, $vf3z");
    assert_eq!(fmt(c2(0xF, 2, 1, 0x00, 0x3F)), "vaddaw.xyzw ACC, $vf1, $vf2w");
    assert_eq!(fmt(c2(0xF, 0, 1, 0x08, 0x3F)), "vmaddai.xyzw ACC, $vf1, I");
    assert_eq!(fmt(c2(0xF, 0, 1, 0x07, 0x3C)), "vmulaq.xyzw ACC, $vf1, Q");
    assert_eq!(fmt(c2(0xE, 2, 1, 3, 0x2E)), "vopmsub.xyz $vf3, $vf1, $vf2");
    assert_eq!(fmt(c2(0xE, 2, 1, 0x0B, 0x3E)), "vopmula.xyz ACC, $vf1, $vf2");
    assert_eq!(fmt(c2(0xF, 16, 4, 0x07, 0x3D)), "vabs.xyzw $vf16, $vf4");
    assert_eq!(fmt(c2(0xF, 10, 4, 0x04, 0x3C)), "vitof0.xyzw $vf10, $vf4");
    assert_eq!(fmt(c2(0xF, 10, 4, 0x05, 0x3D)), "vftoi4.xyzw $vf10, $vf4");
    assert_eq!(fmt(c2(0xF, 10, 4, 0x05, 0x3F)), "vftoi15.xyzw $vf10, $vf4");
    assert_eq!(fmt(c2(0x3, 5, 4, 0x02, 0x3E)), "vmaddaz.zw ACC, $vf4, $vf5z");
}

#[test]
fn cop2_special2_shapes() {
    // fsf lives in bits 22..21, ftf in bits 24..23 (they overlap the
    // dest field, which is dest = (ftf << 2) | fsf here).
    assert_eq!(fmt(c2(0x9, 4, 3, 0x0E, 0x3C)), "vdiv Q, $vf3y, $vf4z");
    assert_eq!(fmt(c2(0xC, 7, 0, 0x0E, 0x3D)), "vsqrt Q, $vf7w");
    assert_eq!(fmt(c2(0x9, 4, 3, 0x0E, 0x3E)), "vrsqrt Q, $vf3y, $vf4z");
    assert_eq!(fmt(c2(0x0, 0, 0, 0x0E, 0x3F)), "vwaitq");
    assert_eq!(fmt(c2(0xE, 5, 5, 0x07, 0x3F)), "vclipw.xyz $vf5, $vf5w");
    assert_eq!(fmt(c2(0xF, 5, 8, 0x0D, 0x3C)), "vlqi.xyzw $vf5, ($vi8++)");
    assert_eq!(fmt(c2(0xF, 8, 5, 0x0D, 0x3D)), "vsqi.xyzw $vf5, ($vi8++)");
    assert_eq!(fmt(c2(0xF, 5, 8, 0x0D, 0x3E)), "vlqd.xyzw $vf5, (--$vi8)");
    assert_eq!(fmt(c2(0xF, 8, 5, 0x0D, 0x3F)), "vsqd.xyzw $vf5, (--$vi8)");
    assert_eq!(fmt(c2(0x8, 8, 5, 0x0F, 0x3E)), "vilwr.x $vi8, ($vi5)");
    assert_eq!(fmt(c2(0x8, 8, 5, 0x0F, 0x3F)), "viswr.x $vi8, ($vi5)");
    assert_eq!(fmt(c2(0x0, 8, 5, 0x0F, 0x3C)), "vmtir $vi8, $vf5x");
    assert_eq!(fmt(c2(0xC, 8, 5, 0x0F, 0x3D)), "vmfir.xy $vf8, $vi5");
    assert_eq!(fmt(c2(0x8, 3, 0, 0x10, 0x3C)), "vrnext.x $vf3, R");
    assert_eq!(fmt(c2(0x9, 8, 0, 0x10, 0x3D)), "vrget.xw $vf8, R");
    assert_eq!(fmt(c2(0x2, 0, 6, 0x10, 0x3E)), "vrinit R, $vf6z");
    assert_eq!(fmt(c2(0x1, 0, 6, 0x10, 0x3F)), "vrxor R, $vf6y");
    assert_eq!(fmt(c2(0x0, 0, 0, 0x0B, 0x3F)), "vnop");
    assert_eq!(fmt(0x4A031074), "viand $vi1, $vi2, $vi3");
    assert_eq!(fmt(0x4A0117F2), "viaddi $vi1, $vi2, -0x1");
    assert_eq!(fmt(0x4A000838), "vcallms 0x100");
    assert_eq!(fmt(0x4A00D839), "vcallmsr $vi27");
}

#[test]
fn invalid_encodings() {
    // dsra32 with a nonzero rs field is not a valid encoding; handwritten
    // data words in .text depend on this class of check.
    assert_eq!(fmt(0x00FF00FF), ".word 0x00FF00FF");
    // EE has no ll/sc, ldc1/sdc1, lwc2/swc2, dmult, or COP3.
    assert_eq!(fmt(0xC0410000), ".word 0xC0410000");
    assert_eq!(fmt(0xD4410000), ".word 0xD4410000");
    assert_eq!(fmt(0xC8410000), ".word 0xC8410000");
    assert_eq!(fmt(0x0044001C), ".word 0x0044001C");
    assert_eq!(fmt(0x4C000000), ".word 0x4C000000");
}

#[test]
fn total_no_panic() {
    // Sweep a sparse sample of the whole 32-bit space.
    let mut w: u32 = 0;
    loop {
        let _ = decode(w, 0x0010_0000);
        let (next, carry) = w.overflowing_add(0x0001_0007);
        if carry {
            break;
        }
        w = next;
    }
}
