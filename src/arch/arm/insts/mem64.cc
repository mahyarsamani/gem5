/*
 * Copyright (c) 2011-2013,2018, 2021-2022 Arm Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "arch/arm/insts/mem64.hh"

#include "arch/arm/insts/dependent_access.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/tlb.hh"
#include "arch/generic/memhelpers.hh"
#include "base/cprintf.hh"
#include "base/loader/symtab.hh"
#include "debug/IndirectAccess.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"

namespace gem5
{

namespace ArmISA
{

std::string
SysDC64::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    ccprintf(ss, ", ");
    printIntReg(ss, base);
    return ss.str();
}


uint32_t
SysDC64::iss() const
{
    const auto misc_reg = encodeAArch64SysReg(dest);
    return _iss(misc_reg.value(), base);
}

void
Memory64::startDisassembly(std::ostream &os) const
{
    printMnemonic(os, "", false);
    if (isDataPrefetch()||isInstPrefetch()){
        printPFflags(os, dest);
    }else{
        printIntReg(os, dest);
    }
    ccprintf(os, ", [");
    printIntReg(os, base);
}

void
Memory64::setExcAcRel(bool exclusive, bool acrel)
{
    if (exclusive)
        memAccessFlags |= Request::LLSC;
    else
        memAccessFlags |= ArmISA::MMU::AllowUnaligned;
    if (acrel) {
        flags[IsWriteBarrier] = true;
        flags[IsReadBarrier] = true;
    }
}

std::string
MemoryImm64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    startDisassembly(ss);
    if (imm)
        ccprintf(ss, ", #%d", imm);
    ccprintf(ss, "]");
    return ss.str();
}

std::string
MemoryDImm64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    printIntReg(ss, dest);
    ccprintf(ss, ", ");
    printIntReg(ss, dest2);
    ccprintf(ss, ", [");
    printIntReg(ss, base);
    if (imm)
        ccprintf(ss, ", #%d", imm);
    ccprintf(ss, "]");
    return ss.str();
}

std::string
MemoryDImmEx64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    printIntReg(ss, result);
    ccprintf(ss, ", ");
    printIntReg(ss, dest);
    ccprintf(ss, ", ");
    printIntReg(ss, dest2);
    ccprintf(ss, ", [");
    printIntReg(ss, base);
    if (imm)
        ccprintf(ss, ", #%d", imm);
    ccprintf(ss, "]");
    return ss.str();
}

std::string
MemoryPreIndex64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    startDisassembly(ss);
    ccprintf(ss, ", #%d]!", imm);
    return ss.str();
}

std::string
MemoryPostIndex64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    startDisassembly(ss);
    if (imm)
        ccprintf(ss, "], #%d", imm);
    ccprintf(ss, "]");
    return ss.str();
}

std::string
MemoryReg64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    startDisassembly(ss);
    printExtendOperand(false, ss, offset, type, shiftAmt);
    ccprintf(ss, "]");
    return ss.str();
}

std::string
MemoryRaw64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    startDisassembly(ss);
    ccprintf(ss, "]");
    return ss.str();
}

std::string
MemoryEx64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    printIntReg(ss, dest);
    ccprintf(ss, ", ");
    printIntReg(ss, result);
    ccprintf(ss, ", [");
    printIntReg(ss, base);
    ccprintf(ss, "]");
    return ss.str();
}

std::string
MemoryLiteral64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    printIntReg(ss, dest);
    ccprintf(ss, ", #%d", pc + imm);
    return ss.str();
}

std::string
MemoryAtomicPair64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    printIntReg(ss, result);
    ccprintf(ss, ", ");
    printIntReg(ss, result2);
    ccprintf(ss, ", ");
    printIntReg(ss, dest);
    ccprintf(ss, ", ");
    printIntReg(ss, dest2);
    ccprintf(ss, ", [");
    printIntReg(ss, base);
    ccprintf(ss, "]");
    return ss.str();
}

} // namespace ArmISA
} // namespace gem5

namespace gem5
{
namespace ArmISA
{

MemoryInd64::MemoryInd64(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass,
                         RegIndex _dest, RegIndex _base,
                         unsigned _sizeIndex, unsigned _sizeData)
    : MightBeMicro64(mnem, _machInst, __opClass),
      dest(_dest), base(_base),
      sizeIndex(_sizeIndex), sizeData(_sizeData),
      shiftAmtIndex(ceilLog2(_sizeIndex)),
      shiftAmtData(ceilLog2(_sizeData)),
      memAccessFlags(ArmISA::MMU::AllowUnaligned)
{}

/** Name of the FP scalar register an FP ldind/stind uses: S or D. */
static void
printIndFpReg(std::ostream &os, RegIndex reg, unsigned size_data)
{
    ccprintf(os, "%c%d", size_data == 8 ? 'd' : 's', reg);
}

std::string
MemoryInd64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    // ldindx_w d1, #0: value register and side-car pair id ('base').
    std::stringstream ss;
    printMnemonic(ss, "", false);
    if (isFp)
        printIndFpReg(ss, dest, sizeData);
    else
        printIntReg(ss, dest);
    ccprintf(ss, ", #%d", base);
    return ss.str();
}

// =========================================================================
// MicroIndIdx — uop0: fetch index[i] from the index array
// =========================================================================

MicroIndIdx::MicroIndIdx(const char *mnem, ExtMachInst machInst,
                         RegIndex _destIdx, RegIndex _indexReg,
                         unsigned _pair,
                         unsigned _sizeIndex, unsigned _sizeData,
                         bool isLoad, bool index_in_reg)
    : MightBeMicro64(mnem, machInst, MemReadOp),
      destIdx(_destIdx), indexReg(_indexReg), pair(_pair),
      sizeIndex(_sizeIndex), sizeData(_sizeData),
      shiftAmtIndex(ceilLog2(_sizeIndex)),
      shiftAmtData(ceilLog2(_sizeData)),
      memAccessFlags(MMU::AllowUnaligned),
      _isLoad(isLoad),
      carriers(_destIdx, _indexReg, index_in_reg)
{
    flags[IsLoad] = true;
    flags[IsMicroop] = true;
    // MYSTUFF
    flags[IsHovIndirectMem] = true;
    // FFUTSYM

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::_srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::_destRegIdxArr));

    _numSrcRegs = 0;
    _numDestRegs = 0;

    // Source: i. The pair's bases come from the side-car entry (misc
    // registers, read at execute; a bind's msr serialises before us).
    setSrcRegIdx(_numSrcRegs++, intRegClass[_indexReg]);

    if (carriers.indexInReg) {
        // The register form reads index[i] from Rt2 and writes nothing:
        // uop1 reads Rt2 and Ra itself.
        setSrcRegIdx(_numSrcRegs++, carriers.index);
        return;
    }

    // Dests: index[i]'s one carrier (Rt2, or Ureg1 when Rt2 is XZR), and
    // the alias in Ureg0 only when this uop overwrites Ra (IndCarriers).
    setDestRegIdx(_numDestRegs++, carriers.index);
    _numTypedDestRegs[IntRegClass]++;
    if (carriers.aliasInUreg0) {
        setDestRegIdx(_numDestRegs++, int_reg::Ureg0);
        _numTypedDestRegs[IntRegClass]++;
    }
}

Addr
MicroIndIdx::computeAlias(ExecContext *xc) const
{
    uint64_t index_val      = xc->getRegOperand(this, 0); // indexReg
    uint64_t base_alias_val =
        xc->readMiscReg(hovPairReg(pair, HOV_PAIR_ALIAS));
    return base_alias_val + (index_val << shiftAmtData);
}

Fault
MicroIndIdx::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    panic("MicroIndIdx: atomic (non-split) execution not supported; "
          "this instruction requires the Ruby CHI memory system.");
}

Fault
MicroIndIdx::initiateAcc(ExecContext *xc, trace::InstRecord *traceData) const
{
    uint64_t index_val      = xc->getRegOperand(this, 0); // indexReg
    uint64_t base_index_val =
        xc->readMiscReg(hovPairReg(pair, HOV_PAIR_INDEX));
    uint64_t base_data_val =
        xc->readMiscReg(hovPairReg(pair, HOV_PAIR_DATA));
    uint64_t count = xc->readMiscReg(hovPairReg(pair, HOV_PAIR_COUNT));
    uint64_t alias_val      = computeAlias(xc);

    Addr EA = base_index_val + (index_val << shiftAmtIndex);

    DPRINTF(IndirectAccess, "[%s initiateAcc] pair=%u alias=0x%llx "
            "base_data=0x%llx base_index=0x%llx index=%llu count=%llu "
            "EA=0x%llx sizeIndex=%u sizeData=%u\n",
            mnemonic, pair,
            (unsigned long long)alias_val, (unsigned long long)base_data_val,
            (unsigned long long)base_index_val, (unsigned long long)index_val,
            (unsigned long long)count,
            (unsigned long long)EA, sizeIndex, sizeData);

    // Attach IndirectAccessAlias so CHI protocol recognizes this as indirect
    auto iaa = std::make_shared<IndirectAccessAlias>(alias_val);
    // Attach DependentAccessGen so the Gather Unit can generate the value
    // access (ReadValue/WriteValue)
    auto intent = _isLoad ? DependentAccessGen::AccessIntent::Read
                          : DependentAccessGen::AccessIntent::Write;
    // The access names of the entry's arrays (registered with the core,
    // else hov<p>_index and hov<p>_data): this read's, and the data
    // array's for the IDR the Gather Unit builds from the DAG.
    auto dag = std::make_shared<ARMDependentAccessGen>(
        intent, sizeIndex, EA, base_data_val, sizeData, alias_val, pair,
        xc->hovAccessName(pair, false));
    if (carriers.indexInReg) {
        // The register form: index[i] is in Rt2 (zero-extended by the
        // program; masked to the index size here). The DAG carries it,
        // and the LSQ sends the descriptor without reading index[i].
        uint64_t known = xc->getRegOperand(this, 1);
        if (sizeIndex < 8)
            known &= (1ULL << (8 * sizeIndex)) - 1;
        dag->setIndexValue(known);
    }
    xc->setIAAExt(iaa);
    xc->setDAGExt(dag);
    xc->setAccessName(xc->hovAccessName(pair, true));

    std::vector<bool> byte_enable(sizeIndex, true);
    return initiateMemRead(xc, EA, sizeIndex, memAccessFlags, byte_enable);
}

Fault
MicroIndIdx::completeAcc(PacketPtr pkt, ExecContext *xc,
                         trace::InstRecord *traceData) const
{
    // Extract index[i] from the returned cache line data.
    // getMemLE handles the offset within the line based on the packet's
    // address and size.
    uint64_t index_value = 0;
    switch (sizeIndex) {
        case 4: {
            uint32_t v = 0;
            getMemLE(pkt, v, traceData);
            index_value = v;
            break;
        }
        case 8:
            getMemLE(pkt, index_value, traceData);
            break;
        default:
            panic("MicroIndIdx: unsupported sizeIndex=%u", sizeIndex);
    }

    Addr alias_val = computeAlias(xc);

    DPRINTF(IndirectAccess, "[%s completeAcc] index[i]=%llu alias=0x%llx\n",
            mnemonic, (unsigned long long)index_value,
            (unsigned long long)alias_val);

    // The register form writes nothing (the packet carried the value
    // the program supplied). Otherwise write index[i] to its carrier, and
    // the alias to Ureg0 when uop1 cannot form it from Ra.
    if (carriers.indexInReg)
        return NoFault;
    xc->setRegOperand(this, 0, index_value);
    if (carriers.aliasInUreg0)
        xc->setRegOperand(this, 1, alias_val);
    return NoFault;
}

std::string
MicroIndIdx::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    ss << " ";
    printIntReg(ss, destIdx);
    ccprintf(ss, ", #%u, ", pair);
    printIntReg(ss, indexReg);
    if (carriers.indexInReg)
        ss << " (index in reg)";
    return ss.str();
}

// =========================================================================
// MicroLdIndVal — uop1 for ldind: load data element from the data array
// =========================================================================

/**
 * The alias in uop1: read from Ureg0, or formed from i and the entry's
 * base_alias exactly as uop0 forms it (both read the same Ra mapping, and
 * a bind's msr serialises, so both read the same entry).
 */
static Addr
uop1Alias(ExecContext *xc, const StaticInst *si, const IndCarriers &c,
          unsigned pair, uint64_t shift_amt_data)
{
    uint64_t v = xc->getRegOperand(si, 1); // Ureg0 or Ra
    if (c.aliasInUreg0)
        return v;
    return xc->readMiscReg(hovPairReg(pair, HOV_PAIR_ALIAS)) +
           (v << shift_amt_data);
}

MicroLdIndVal::MicroLdIndVal(const char *mnem, ExtMachInst machInst,
                             RegIndex _destData, unsigned _pair,
                             unsigned _sizeData, bool _isFp,
                             RegIndex _destIdx, RegIndex _indexReg,
                             bool index_in_reg)
    : MightBeMicro64(mnem, machInst, MemReadOp),
      destData(_destData), pair(_pair),
      sizeData(_sizeData),
      shiftAmtData(ceilLog2(_sizeData)),
      memAccessFlags(MMU::AllowUnaligned),
      isFp(_isFp),
      carriers(_destIdx, _indexReg, index_in_reg)
{
    flags[IsLoad] = true;
    flags[IsMicroop] = true;
    // MYSTUFF
    flags[IsHovIndirectMem] = true;
    // FFUTSYM

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::_srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::_destRegIdxArr));

    _numSrcRegs = 0;
    _numDestRegs = 0;

    // Sources: index[i]'s carrier, written by uop0, then Ureg0 (the alias)
    // or Ra (i, to form the alias); base_data and base_alias come from
    // the side-car entry.
    setSrcRegIdx(_numSrcRegs++, carriers.index);
    setSrcRegIdx(_numSrcRegs++, carriers.aliasSource());

    // Dest: destData (Rt), an X register or the FP/SIMD register
    if (isFp) {
        setDestRegIdx(_numDestRegs++, vecRegClass[_destData]);
        _numTypedDestRegs[VecRegClass]++;
    } else {
        setDestRegIdx(_numDestRegs++, intRegClass[_destData]);
        _numTypedDestRegs[IntRegClass]++;
    }
}

Fault
MicroLdIndVal::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    panic("MicroLdIndVal: atomic (non-split) execution not supported.");
}

Fault
MicroLdIndVal::initiateAcc(ExecContext *xc,
                           trace::InstRecord *traceData) const
{
    uint64_t index_value   = xc->getRegOperand(this, 0); // carrier
    uint64_t alias_val     =
        uop1Alias(xc, this, carriers, pair, shiftAmtData);
    uint64_t base_data_val = xc->readMiscReg(hovPairReg(pair, HOV_PAIR_DATA));

    Addr EA = base_data_val + (index_value << shiftAmtData);

    DPRINTF(IndirectAccess, "[%s initiateAcc] index_value=%llu "
            "base_data=0x%llx EA=0x%llx sizeData=%u alias=0x%llx\n",
            mnemonic,
            (unsigned long long)index_value,
            (unsigned long long)base_data_val,
            (unsigned long long)EA, sizeData,
            (unsigned long long)alias_val);

    // Attach the alias so the Sequencer can identify this as the value
    // access (uop1) of an ldind; it is a normal load (no DAG).
    auto iaa = std::make_shared<IndirectAccessAlias>(alias_val);
    xc->setIAAExt(iaa);
    xc->setAccessName(xc->hovAccessName(pair, false));

    // Standard ReadReq — no DAG extension (ReadValue from IDR handles that)
    std::vector<bool> byte_enable(sizeData, true);
    return initiateMemRead(xc, EA, sizeData, memAccessFlags, byte_enable);
}

Fault
MicroLdIndVal::completeAcc(PacketPtr pkt, ExecContext *xc,
                           trace::InstRecord *traceData) const
{
    uint64_t data = 0;
    switch (sizeData) {
        case 1: {
            uint8_t v = 0;
            getMemLE(pkt, v, traceData);
            data = v;
            break;
        }
        case 2: {
            uint16_t v = 0;
            getMemLE(pkt, v, traceData);
            data = v;
            break;
        }
        case 4: {
            uint32_t v = 0;
            getMemLE(pkt, v, traceData);
            data = v;
            break;
        }
        case 8:
            getMemLE(pkt, data, traceData);
            break;
        default:
            panic("MicroLdIndVal: unsupported sizeData=%u", sizeData);
    }

    DPRINTF(IndirectAccess, "[%s completeAcc] data=0x%llx\n",
            mnemonic, (unsigned long long)data);

    // Write data element to Rd. An FP destination (S or D) is the low
    // element of the vector register, the rest zeroed, as LDR (SIMD&FP).
    if (isFp) {
        VecRegContainer vc;
        vc.zero();
        vc.as<uint64_t>()[0] = data;
        xc->setRegOperand(this, 0, &vc);
    } else {
        xc->setRegOperand(this, 0, data);
    }
    return NoFault;
}

std::string
MicroLdIndVal::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    ss << " ";
    if (isFp)
        printIndFpReg(ss, destData, sizeData);
    else
        printIntReg(ss, destData);
    ccprintf(ss, ", #%u", pair);
    return ss.str();
}

// =========================================================================
// MicroStIndVal — uop1 for stind: store data element to the data array
// =========================================================================

MicroStIndVal::MicroStIndVal(const char *mnem, ExtMachInst machInst,
                             RegIndex _srcData, unsigned _pair,
                             unsigned _sizeData, bool _isFp,
                             RegIndex _destIdx, RegIndex _indexReg,
                             bool index_in_reg)
    : MightBeMicro64(mnem, machInst, MemWriteOp),
      srcData(_srcData), pair(_pair),
      sizeData(_sizeData),
      shiftAmtData(ceilLog2(_sizeData)),
      memAccessFlags(MMU::AllowUnaligned),
      isFp(_isFp),
      carriers(_destIdx, _indexReg, index_in_reg)
{
    flags[IsStore] = true;
    flags[IsMicroop] = true;
    // MYSTUFF
    flags[IsHovIndirectMem] = true;
    // FFUTSYM

    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::_srcRegIdxArr),
        nullptr);

    _numSrcRegs = 0;
    _numDestRegs = 0;

    // Sources: index[i]'s carrier from uop0, Ureg0 (the alias) or Ra (i,
    // to form the alias), then srcData (Rt: an X register or the FP/SIMD
    // register); base_data and base_alias come from the side-car entry.
    setSrcRegIdx(_numSrcRegs++, carriers.index);
    setSrcRegIdx(_numSrcRegs++, carriers.aliasSource());
    if (isFp)
        setSrcRegIdx(_numSrcRegs++, vecRegClass[_srcData]);
    else
        setSrcRegIdx(_numSrcRegs++, intRegClass[_srcData]);
}

Fault
MicroStIndVal::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    panic("MicroStIndVal: atomic (non-split) execution not supported.");
}

Fault
MicroStIndVal::initiateAcc(ExecContext *xc,
                           trace::InstRecord *traceData) const
{
    uint64_t index_value   = xc->getRegOperand(this, 0); // carrier
    uint64_t alias_val     =
        uop1Alias(xc, this, carriers, pair, shiftAmtData);
    uint64_t base_data_val = xc->readMiscReg(hovPairReg(pair, HOV_PAIR_DATA));
    // srcData (Rt): an FP source (S or D) is the vector register's low
    // element; writeMem stores its low sizeData bytes.
    uint64_t data_val;
    if (isFp) {
        VecRegContainer vc;
        xc->getRegOperand(this, 2, &vc);
        data_val = vc.as<uint64_t>()[0];
    } else {
        data_val = xc->getRegOperand(this, 2);
    }

    Addr EA = base_data_val + (index_value << shiftAmtData);

    DPRINTF(IndirectAccess, "[%s initiateAcc] index_value=%llu "
            "base_data=0x%llx data_to_store=0x%llx EA=0x%llx sizeData=%u "
            "alias=0x%llx\n",
            mnemonic,
            (unsigned long long)index_value,
            (unsigned long long)base_data_val,
            (unsigned long long)data_val,
            (unsigned long long)EA, sizeData,
            (unsigned long long)alias_val);

    // Attach the alias so the value access (uop1) of an stind can be
    // identified; it is a normal store (no DAG).
    auto iaa = std::make_shared<IndirectAccessAlias>(alias_val);
    xc->setIAAExt(iaa);
    xc->setAccessName(xc->hovAccessName(pair, false));

    // Standard WriteReq — no DAG extension
    std::vector<bool> byte_enable(sizeData, true);
    return xc->writeMem((uint8_t *)&data_val, sizeData, EA,
                        memAccessFlags, NULL, byte_enable);
}

Fault
MicroStIndVal::completeAcc(PacketPtr pkt, ExecContext *xc,
                           trace::InstRecord *traceData) const
{
    DPRINTF(IndirectAccess, "[%s completeAcc] store complete\n", mnemonic);
    return NoFault;
}

std::string
MicroStIndVal::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    ss << " ";
    if (isFp)
        printIndFpReg(ss, srcData, sizeData);
    else
        printIntReg(ss, srcData);
    ccprintf(ss, ", #%u", pair);
    return ss.str();
}

} // namespace ArmISA
} // namespace gem5
