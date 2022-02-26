//===-- M68kMCCodeEmitter.cpp - Convert M68k code emitter -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains defintions for M68k code emitter.
///
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/M68kMCCodeEmitter.h"
#include "MCTargetDesc/M68kBaseInfo.h"
#include "MCTargetDesc/M68kFixupKinds.h"
#include "MCTargetDesc/M68kMCTargetDesc.h"

#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/raw_ostream.h"
#include <type_traits>

using namespace llvm;

#define DEBUG_TYPE "m68k-mccodeemitter"

namespace {
class M68kMCCodeEmitter : public MCCodeEmitter {
  M68kMCCodeEmitter(const M68kMCCodeEmitter &) = delete;
  void operator=(const M68kMCCodeEmitter &) = delete;
  const MCInstrInfo &MCII;
  MCContext &Ctx;

  void getBinaryCodeForInstr(const MCInst &MI, SmallVectorImpl<MCFixup> &Fixups,
                             APInt &Inst, APInt &Scratch,
                             const MCSubtargetInfo &STI) const;

  void getMachineOpValue(const MCInst &MI, const MCOperand &Op,
                         unsigned InsertPos, APInt &Value,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const;

  template <unsigned Size>
  void encodeRelocImm(const MCInst &MI, unsigned OpIdx, unsigned InsertPos,
                      APInt &Value, SmallVectorImpl<MCFixup> &Fixups,
                      const MCSubtargetInfo &STI) const;

  template <unsigned Size>
  void encodePCRelImm(const MCInst &MI, unsigned OpIdx, unsigned InsertPos,
                      APInt &Value, SmallVectorImpl<MCFixup> &Fixups,
                      const MCSubtargetInfo &STI) const;

public:
  M68kMCCodeEmitter(const MCInstrInfo &mcii, MCContext &ctx)
      : MCII(mcii), Ctx(ctx) {}

  ~M68kMCCodeEmitter() override {}

  // TableGen'erated function
  const uint8_t *getGenInstrBeads(const MCInst &MI) const {
    return M68k::getMCInstrBeads(MI.getOpcode());
  }

  unsigned encodeBits(unsigned ThisByte, uint8_t Bead, const MCInst &MI,
                      const MCInstrDesc &Desc, uint64_t &Buffer,
                      unsigned Offset, SmallVectorImpl<MCFixup> &Fixups,
                      const MCSubtargetInfo &STI) const;

  unsigned encodeReg(unsigned ThisByte, uint8_t Bead, const MCInst &MI,
                     const MCInstrDesc &Desc, uint64_t &Buffer, unsigned Offset,
                     SmallVectorImpl<MCFixup> &Fixups,
                     const MCSubtargetInfo &STI) const;

  unsigned encodeImm(unsigned ThisByte, uint8_t Bead, const MCInst &MI,
                     const MCInstrDesc &Desc, uint64_t &Buffer, unsigned Offset,
                     SmallVectorImpl<MCFixup> &Fixups,
                     const MCSubtargetInfo &STI) const;

  void encodeInstruction(const MCInst &MI, raw_ostream &OS,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;
};

} // end anonymous namespace

#include "M68kGenMCCodeEmitter.inc"

// Select the proper unsigned integer type from a bit size.
template <unsigned Size> struct select_uint_t {
  using type = typename std::conditional<
      Size == 8, uint8_t,
      typename std::conditional<
          Size == 16, uint16_t,
          typename std::conditional<Size == 32, uint32_t,
                                    uint64_t>::type>::type>::type;
};

// On a LE host:
// MSB                   LSB    MSB                   LSB
// | 0x12 0x34 | 0xAB 0xCD | -> | 0xAB 0xCD | 0x12 0x34 |
// (On a BE host nothing changes)
template <typename value_t> static value_t swapWord(value_t Val) {
  const unsigned NumWords = sizeof(Val) / 2;
  if (NumWords <= 1)
    return Val;
  Val = support::endian::byte_swap(Val, support::big);
  value_t NewVal = 0;
  for (unsigned i = 0U; i != NumWords; ++i) {
    uint16_t Part = (Val >> (i * 16)) & 0xFFFF;
    Part = support::endian::byte_swap(Part, support::big);
    NewVal |= (Part << (i * 16));
  }
  return NewVal;
}

// Figure out which byte we're at in big endian mode.
template <unsigned Size> static unsigned getBytePosition(unsigned BitPos) {
  if (Size % 16) {
    return static_cast<unsigned>(BitPos / 8 + ((BitPos & 0b1111) < 8 ? 1 : -1));
  } else {
    assert(!(BitPos & 0b1111) && "Not aligned to word boundary?");
    return BitPos / 8;
  }
}

// We need special handlings for relocatable & pc-relative operands that are
// larger than a word.
// A M68k instruction is aligned by word (16 bits). That means, 32-bit
// (& 64-bit) immediate values are separated into hi & lo words and placed
// at lower & higher addresses, respectively. For immediate values that can
// be easily expressed in TG, we explicitly rotate the word ordering like
// this:
// ```
// (ascend (slice "$imm", 31, 16), (slice "$imm", 15, 0))
// ```
// For operands that call into encoder functions, we need to use the `swapWord`
// function to assure the correct word ordering on LE host. Note that
// M68kMCCodeEmitter does massage _byte_ ordering of the final encoded
// instruction but it assumes everything aligns on word boundaries. So things
// will go wrong if we don't take care of the _word_ ordering here.
template <unsigned Size>
void M68kMCCodeEmitter::encodeRelocImm(const MCInst &MI, unsigned OpIdx,
                                       unsigned InsertPos, APInt &Value,
                                       SmallVectorImpl<MCFixup> &Fixups,
                                       const MCSubtargetInfo &STI) const {
  using value_t = typename select_uint_t<Size>::type;
  const MCOperand &MCO = MI.getOperand(OpIdx);
  if (MCO.isImm()) {
    Value |= swapWord<value_t>(static_cast<value_t>(MCO.getImm()));
  } else if (MCO.isExpr()) {
    const MCExpr *Expr = MCO.getExpr();

    // Absolute address
    int64_t Addr;
    if (Expr->evaluateAsAbsolute(Addr)) {
      Value |= swapWord<value_t>(static_cast<value_t>(Addr));
      return;
    }

    // Relocatable address
    unsigned InsertByte = getBytePosition<Size>(InsertPos);
    Fixups.push_back(MCFixup::create(InsertByte, Expr,
                                     getFixupForSize(Size, /*IsPCRel=*/false),
                                     MI.getLoc()));
  }
}

template <unsigned Size>
void M68kMCCodeEmitter::encodePCRelImm(const MCInst &MI, unsigned OpIdx,
                                       unsigned InsertPos, APInt &Value,
                                       SmallVectorImpl<MCFixup> &Fixups,
                                       const MCSubtargetInfo &STI) const {
  const MCOperand &MCO = MI.getOperand(OpIdx);
  if (MCO.isImm()) {
    using value_t = typename select_uint_t<Size>::type;
    Value |= swapWord<value_t>(static_cast<value_t>(MCO.getImm()));
  } else if (MCO.isExpr()) {
    const MCExpr *Expr = MCO.getExpr();
    unsigned InsertByte = getBytePosition<Size>(InsertPos);

    // Special handlings for sizes smaller than a word.
    if (Size < 16) {
      int LabelOffset = 0;
      if (InsertPos < 16)
        // If the patch point is at the first word, PC is pointing at the
        // next word.
        LabelOffset = InsertByte - 2;
      else if (InsertByte % 2)
        // Otherwise the PC is pointing at the first byte of this word.
        // So we need to consider the offset between PC and the fixup byte.
        LabelOffset = 1;

      if (LabelOffset)
        Expr = MCBinaryExpr::createAdd(
            Expr, MCConstantExpr::create(LabelOffset, Ctx), Ctx);
    }

    Fixups.push_back(MCFixup::create(InsertByte, Expr,
                                     getFixupForSize(Size, /*IsPCRel=*/true),
                                     MI.getLoc()));
  }
}

void M68kMCCodeEmitter::getMachineOpValue(const MCInst &MI, const MCOperand &Op,
                                          unsigned InsertPos, APInt &Value,
                                          SmallVectorImpl<MCFixup> &Fixups,
                                          const MCSubtargetInfo &STI) const {
  // Register
  if (Op.isReg()) {
    unsigned RegNum = Op.getReg();
    const auto *RI = Ctx.getRegisterInfo();
    Value |= RI->getEncodingValue(RegNum);
    // Setup the D/A bit
    if (M68kII::isAddressRegister(RegNum))
      Value |= 0b1000;
  } else if (Op.isImm()) {
    // Immediate
    Value |= static_cast<uint64_t>(Op.getImm());
  } else if (Op.isExpr()) {
    // Absolute address
    int64_t Addr;
    if (!Op.getExpr()->evaluateAsAbsolute(Addr))
      report_fatal_error("Unsupported asm expression. Only absolute address "
                         "can be placed here.");
    Value |= static_cast<uint64_t>(Addr);
  } else {
    llvm_unreachable("Unsupported operand type");
  }
}

unsigned M68kMCCodeEmitter::encodeBits(unsigned ThisByte, uint8_t Bead,
                                       const MCInst &MI,
                                       const MCInstrDesc &Desc,
                                       uint64_t &Buffer, unsigned Offset,
                                       SmallVectorImpl<MCFixup> &Fixups,
                                       const MCSubtargetInfo &STI) const {
  unsigned Num = 0;
  switch (Bead & 0xF) {
  case M68kBeads::Bits1:
    Num = 1;
    break;
  case M68kBeads::Bits2:
    Num = 2;
    break;
  case M68kBeads::Bits3:
    Num = 3;
    break;
  case M68kBeads::Bits4:
    Num = 4;
    break;
  }
  unsigned char Val = (Bead & 0xF0) >> 4;

  LLVM_DEBUG(dbgs() << "\tEncodeBits"
                    << " Num: " << Num << " Val: 0x");
  LLVM_DEBUG(dbgs().write_hex(Val) << "\n");

  Buffer |= (Val << Offset);

  return Num;
}

unsigned M68kMCCodeEmitter::encodeReg(unsigned ThisByte, uint8_t Bead,
                                      const MCInst &MI, const MCInstrDesc &Desc,
                                      uint64_t &Buffer, unsigned Offset,
                                      SmallVectorImpl<MCFixup> &Fixups,
                                      const MCSubtargetInfo &STI) const {
  bool DA, Reg;
  switch (Bead & 0xF) {
  default:
    llvm_unreachable("Unrecognized Bead code for register type");
  case M68kBeads::DAReg:
    Reg = true;
    DA = true;
    break;
  case M68kBeads::DA:
    Reg = false;
    DA = true;
    break;
  case M68kBeads::DReg:
  case M68kBeads::Reg:
    Reg = true;
    DA = false;
    break;
  }

  unsigned Op = (Bead & 0x70) >> 4;
  bool Alt = (Bead & 0x80);
  LLVM_DEBUG(dbgs() << "\tEncodeReg"
                    << " Op: " << Op << ", DA: " << DA << ", Reg: " << Reg
                    << ", Alt: " << Alt << "\n");

  auto MIOpIdx = M68k::getLogicalOperandIdx(MI.getOpcode(), Op);
  bool IsPCRel = Desc.OpInfo[MIOpIdx].OperandType == MCOI::OPERAND_PCREL;

  MCOperand MCO;
  if (M68kII::hasMultiMIOperands(MI.getOpcode(), Op)) {
    if (IsPCRel) {
      assert(Alt &&
             "PCRel addresses use Alt bead register encoding by default");
      MCO = MI.getOperand(MIOpIdx + M68k::PCRelIndex);
    } else {
      MCO = MI.getOperand(MIOpIdx + (Alt ? M68k::MemIndex : M68k::MemBase));
    }
  } else {
    assert(!Alt && "You cannot use Alt register with a simple operand");
    MCO = MI.getOperand(MIOpIdx);
  }

  unsigned RegNum = MCO.getReg();
  auto RI = Ctx.getRegisterInfo();

  unsigned Written = 0;
  if (Reg) {
    uint32_t Val = RI->getEncodingValue(RegNum);
    Buffer |= (Val & 7) << Offset;
    Offset += 3;
    Written += 3;
  }

  if (DA) {
    Buffer |= (uint64_t)M68kII::isAddressRegister(RegNum) << Offset;
    Written++;
  }

  return Written;
}

static unsigned EmitConstant(uint64_t Val, unsigned Size, unsigned Pad,
                             uint64_t &Buffer, unsigned Offset) {
  assert(Size + Offset <= 64 && isUIntN(Size, Val) && "Value does not fit");

  // Writing Value in host's endianness
  Buffer |= (Val & ((1ULL << Size) - 1)) << Offset;
  return Size + Pad;
}

unsigned M68kMCCodeEmitter::encodeImm(unsigned ThisByte, uint8_t Bead,
                                      const MCInst &MI, const MCInstrDesc &Desc,
                                      uint64_t &Buffer, unsigned Offset,
                                      SmallVectorImpl<MCFixup> &Fixups,
                                      const MCSubtargetInfo &STI) const {
  unsigned ThisWord = ThisByte / 2;
  unsigned Size = 0;
  unsigned Pad = 0;
  unsigned FixOffset = 0;
  int64_t Addendum = 0;
  bool NoExpr = false;

  unsigned Type = Bead & 0xF;
  unsigned Op = (Bead & 0x70) >> 4;
  bool Alt = (Bead & 0x80);

  auto MIOpIdx = M68k::getLogicalOperandIdx(MI.getOpcode(), Op);
  bool IsPCRel = Desc.OpInfo[MIOpIdx].OperandType == MCOI::OPERAND_PCREL;

  // The PC value upon instruction reading of a short jump will point to the
  // next instruction, thus we need to compensate 2 bytes, which is the diff
  // between the patch point and the PC.
  if (IsPCRel && ThisWord == 0)
    Addendum -= 2;

  switch (Type) {
  // ??? what happens if it is not byte aligned
  // ??? is it even possible
  case M68kBeads::Disp8:
    Size = 8;
    Pad = 0;
    FixOffset = ThisByte + 1;
    Addendum += 1;
    break;
  case M68kBeads::Imm8:
    Size = 8;
    Pad = 8;
    FixOffset = ThisByte;
    break;
  case M68kBeads::Imm16:
    Size = 16;
    Pad = 0;
    FixOffset = ThisByte;
    break;
  case M68kBeads::Imm32:
    Size = 32;
    Pad = 0;
    FixOffset = ThisByte;
    break;
  case M68kBeads::Imm3:
    Size = 3;
    Pad = 0;
    NoExpr = true;
    break;
  }

  LLVM_DEBUG(dbgs() << "\tEncodeImm"
                    << " Op: " << Op << ", Size: " << Size << ", Alt: " << Alt
                    << "\n");

  MCOperand MCO;
  if (M68kII::hasMultiMIOperands(MI.getOpcode(), Op)) {

    if (IsPCRel) {
      assert(!Alt && "You cannot use ALT operand with PCRel");
      MCO = MI.getOperand(MIOpIdx + M68k::PCRelDisp);
    } else {
      MCO = MI.getOperand(MIOpIdx + (Alt ? M68k::MemOuter : M68k::MemDisp));
    }

    if (MCO.isExpr()) {
      assert(!NoExpr && "Cannot use expression here");
      const MCExpr *Expr = MCO.getExpr();

      // This only makes sense for PCRel instructions since PC points to the
      // extension word and Disp8 for example is right justified and requires
      // correction. E.g. R_68K_PC32 is calculated as S + A - P, P for Disp8
      // will be EXTENSION_WORD + 1 thus we need to have A equal to 1 to
      // compensate.
      // TODO count extension words
      if (IsPCRel && Addendum != 0) {
        Expr = MCBinaryExpr::createAdd(
            Expr, MCConstantExpr::create(Addendum, Ctx), Ctx);
      }

      Fixups.push_back(MCFixup::create(
          FixOffset, Expr, getFixupForSize(Size, IsPCRel), MI.getLoc()));
      // Write zeros
      return EmitConstant(0, Size, Pad, Buffer, Offset);
    }

  } else {
    MCO = MI.getOperand(MIOpIdx);
    if (MCO.isExpr()) {
      assert(!NoExpr && "Cannot use expression here");
      const MCExpr *Expr = MCO.getExpr();

      if (Addendum != 0) {
        Expr = MCBinaryExpr::createAdd(
            Expr, MCConstantExpr::create(Addendum, Ctx), Ctx);
      }

      Fixups.push_back(MCFixup::create(
          FixOffset, Expr, getFixupForSize(Size, IsPCRel), MI.getLoc()));
      // Write zeros
      return EmitConstant(0, Size, Pad, Buffer, Offset);
    }
  }

  int64_t I = MCO.getImm();

  // Store 8 as 0, thus making range 1-8
  if (Type == M68kBeads::Imm3 && Alt) {
    assert(I && "Cannot encode Alt Imm3 zero value");
    I %= 8;
  } else {
    assert(isIntN(Size, I));
  }

  uint64_t Imm = I;

  // 32 bit Imm requires HI16 first then LO16
  if (Size == 32) {
    Offset += EmitConstant((Imm >> 16) & 0xFFFF, 16, Pad, Buffer, Offset);
    EmitConstant(Imm & 0xFFFF, 16, Pad, Buffer, Offset);
    return Size;
  }

  return EmitConstant(Imm & ((1ULL << Size) - 1), Size, Pad, Buffer, Offset);
}

#include "M68kGenMCCodeBeads.inc"

void M68kMCCodeEmitter::encodeInstruction(const MCInst &MI, raw_ostream &OS,
                                          SmallVectorImpl<MCFixup> &Fixups,
                                          const MCSubtargetInfo &STI) const {
  unsigned Opcode = MI.getOpcode();
  const MCInstrDesc &Desc = MCII.get(Opcode);

  LLVM_DEBUG(dbgs() << "EncodeInstruction: " << MCII.getName(Opcode) << "("
                    << Opcode << ")\n");

  // Try using the new method first.
  APInt EncodedInst(16, 0U);
  APInt Scratch(16, 0U);
  getBinaryCodeForInstr(MI, Fixups, EncodedInst, Scratch, STI);
  if (EncodedInst.getBitWidth()) {
    LLVM_DEBUG(dbgs() << "Instruction " << MCII.getName(Opcode) << "(" << Opcode
                      << ") is using the new code emitter\n");
    ArrayRef<uint64_t> Data(EncodedInst.getRawData(),
                            EncodedInst.getNumWords());
    int64_t InstSize = EncodedInst.getBitWidth();
    for (uint64_t Word : Data) {
      for (int i = 0; i < 4 && InstSize > 0; ++i, InstSize -= 16) {
        support::endian::write<uint16_t>(OS, static_cast<uint16_t>(Word),
                                         support::big);
        Word >>= 16;
      }
    }
    return;
  }

  const uint8_t *Beads = getGenInstrBeads(MI);
  if (!Beads || !*Beads) {
    llvm_unreachable("*** Instruction does not have Beads defined");
  }

  uint64_t Buffer = 0;
  unsigned Offset = 0;
  unsigned ThisByte = 0;

  for (uint8_t Bead = *Beads; Bead; Bead = *++Beads) {
    // Check for control beads
    if (!(Bead & 0xF)) {
      switch (Bead >> 4) {
      case M68kBeads::Ignore:
        continue;
      }
    }

    switch (Bead & 0xF) {
    default:
      llvm_unreachable("Unknown Bead code");
      break;
    case M68kBeads::Bits1:
    case M68kBeads::Bits2:
    case M68kBeads::Bits3:
    case M68kBeads::Bits4:
      Offset +=
          encodeBits(ThisByte, Bead, MI, Desc, Buffer, Offset, Fixups, STI);
      break;
    case M68kBeads::DAReg:
    case M68kBeads::DA:
    case M68kBeads::DReg:
    case M68kBeads::Reg:
      Offset +=
          encodeReg(ThisByte, Bead, MI, Desc, Buffer, Offset, Fixups, STI);
      break;
    case M68kBeads::Disp8:
    case M68kBeads::Imm8:
    case M68kBeads::Imm16:
    case M68kBeads::Imm32:
    case M68kBeads::Imm3:
      Offset +=
          encodeImm(ThisByte, Bead, MI, Desc, Buffer, Offset, Fixups, STI);
      break;
    }

    // Since M68k is Big Endian we need to rotate each instruction word
    while (Offset / 16) {
      support::endian::write<uint16_t>(OS, Buffer, support::big);
      Buffer >>= 16;
      Offset -= 16;
      ThisByte += 2;
    }
  }

  assert(Offset == 0 && "M68k Instructions are % 2 bytes");
  assert((ThisByte && !(ThisByte % 2)) && "M68k Instructions are % 2 bytes");
}

MCCodeEmitter *llvm::createM68kMCCodeEmitter(const MCInstrInfo &MCII,
                                             MCContext &Ctx) {
  return new M68kMCCodeEmitter(MCII, Ctx);
}
