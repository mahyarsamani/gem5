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
#include "arch/arm/tlb.hh"
#include "arch/generic/memhelpers.hh"
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

std::string
MemoryInd64::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    // ldindx_w x4, x0, x1, x2
    //           ^dest ^base_data ^base_index ^i
    // 'base' here is base_index; base_data is tracked in the subclass.
    std::stringstream ss;
    printMnemonic(ss, "", false);
    printIntReg(ss, dest);
    ccprintf(ss, ", [");
    printIntReg(ss, base);
    ccprintf(ss, "]");
    return ss.str();
}

// =========================================================================
// MicroIndIdx — uop0: fetch index[i] from the index array
// =========================================================================

MicroIndIdx::MicroIndIdx(const char *mnem, ExtMachInst machInst,
                         RegIndex _destIdx, RegIndex _baseIndex,
                         RegIndex _indexReg, RegIndex _aliasReg,
                         RegIndex _baseData,
                         unsigned _sizeIndex, unsigned _sizeData,
                         bool isLoad)
    : MightBeMicro64(mnem, machInst, MemReadOp),
      destIdx(_destIdx), baseIndex(_baseIndex),
      indexReg(_indexReg), aliasReg(_aliasReg),
      baseData(_baseData),
      sizeIndex(_sizeIndex), sizeData(_sizeData),
      shiftAmtIndex(ceilLog2(_sizeIndex)),
      memAccessFlags(MMU::AllowUnaligned),
      _isLoad(isLoad)
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

    // Sources: base_index, index_reg, alias_reg (for IAA value), base_data (for DAG)
    setSrcRegIdx(_numSrcRegs++, intRegClass[_baseIndex]);
    setSrcRegIdx(_numSrcRegs++, intRegClass[_indexReg]);
    setSrcRegIdx(_numSrcRegs++, intRegClass[_aliasReg]);
    setSrcRegIdx(_numSrcRegs++, intRegClass[_baseData]);

    // Dest: destIdx (Rd+1) — receives index[i]
    setDestRegIdx(_numDestRegs++, intRegClass[_destIdx]);
    _numTypedDestRegs[IntRegClass]++;
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
    uint64_t base_index_val = xc->getRegOperand(this, 0); // baseIndex
    uint64_t index_val      = xc->getRegOperand(this, 1); // indexReg
    uint64_t alias_val      = xc->getRegOperand(this, 2); // aliasReg
    uint64_t base_data_val  = xc->getRegOperand(this, 3); // baseData

    Addr EA = base_index_val + (index_val << shiftAmtIndex);

    DPRINTF(IndirectAccess, "[%s initiateAcc] alias=0x%llx "
            "base_data=0x%llx base_index=0x%llx index=%llu EA=0x%llx "
            "sizeIndex=%u sizeData=%u\n",
            mnemonic,
            (unsigned long long)alias_val, (unsigned long long)base_data_val,
            (unsigned long long)base_index_val, (unsigned long long)index_val,
            (unsigned long long)EA, sizeIndex, sizeData);

    // Attach IndirectAccessAlias so CHI protocol recognizes this as indirect
    auto iaa = std::make_shared<IndirectAccessAlias>(alias_val);
    // Attach DependentAccessGen so CHI can generate Phase 2 (ReadValue)
    auto intent = _isLoad ? DependentAccessGen::AccessIntent::Read
                          : DependentAccessGen::AccessIntent::Write;
    auto dag = std::make_shared<ARMDependentAccessGen>(intent, sizeIndex, EA, base_data_val, sizeData);
    xc->setIAAExt(iaa);
    xc->setDAGExt(dag);

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

    DPRINTF(IndirectAccess, "[%s completeAcc] index[i]=%llu\n",
            mnemonic, (unsigned long long)index_value);

    // Write index[i] to Rd+1
    xc->setRegOperand(this, 0, index_value);
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
    ss << ", [";
    printIntReg(ss, baseIndex);
    ss << ", ";
    printIntReg(ss, indexReg);
    ss << "]";
    return ss.str();
}

// =========================================================================
// MicroLdIndVal — uop1 for ldind: load data element from the data array
// =========================================================================

MicroLdIndVal::MicroLdIndVal(const char *mnem, ExtMachInst machInst,
                             RegIndex _destData, RegIndex _srcIdx,
                             RegIndex _baseData, RegIndex _aliasReg,
                             unsigned _sizeData)
    : MightBeMicro64(mnem, machInst, MemReadOp),
      destData(_destData), srcIdx(_srcIdx),
      baseData(_baseData), aliasReg(_aliasReg),
      sizeData(_sizeData),
      shiftAmtData(ceilLog2(_sizeData)),
      memAccessFlags(MMU::AllowUnaligned)
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

    // Sources: srcIdx (Rd+1, index value from uop0), baseData, aliasReg
    setSrcRegIdx(_numSrcRegs++, intRegClass[_srcIdx]);
    setSrcRegIdx(_numSrcRegs++, intRegClass[_baseData]);
    setSrcRegIdx(_numSrcRegs++, intRegClass[_aliasReg]);

    // Dest: destData (Rd)
    setDestRegIdx(_numDestRegs++, intRegClass[_destData]);
    _numTypedDestRegs[IntRegClass]++;
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
    uint64_t index_value   = xc->getRegOperand(this, 0); // srcIdx (Rd+1)
    uint64_t base_data_val = xc->getRegOperand(this, 1); // baseData
    uint64_t alias_val     = xc->getRegOperand(this, 2); // aliasReg

    Addr EA = base_data_val + (index_value << shiftAmtData);

    DPRINTF(IndirectAccess, "[%s initiateAcc] index_value=%llu "
            "base_data=0x%llx EA=0x%llx sizeData=%u alias=0x%llx\n",
            mnemonic,
            (unsigned long long)index_value,
            (unsigned long long)base_data_val,
            (unsigned long long)EA, sizeData,
            (unsigned long long)alias_val);

    // Attach IAA extension so the Sequencer can identify this as an
    // indirect-value access and route through m_IndirectRequestTable.
    auto iaa = std::make_shared<IndirectAccessAlias>(alias_val);
    xc->setIAAExt(iaa);

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

    // Write data element to Rd
    xc->setRegOperand(this, 0, data);
    return NoFault;
}

std::string
MicroLdIndVal::generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    printMnemonic(ss, "", false);
    ss << " ";
    printIntReg(ss, destData);
    ss << ", [";
    printIntReg(ss, baseData);
    ss << ", ";
    printIntReg(ss, srcIdx);
    ss << "]";
    return ss.str();
}

// =========================================================================
// MicroStIndVal — uop1 for stind: store data element to the data array
// =========================================================================

MicroStIndVal::MicroStIndVal(const char *mnem, ExtMachInst machInst,
                             RegIndex _srcData, RegIndex _srcIdx,
                             RegIndex _baseData, RegIndex _aliasReg,
                             unsigned _sizeData)
    : MightBeMicro64(mnem, machInst, MemWriteOp),
      srcData(_srcData), srcIdx(_srcIdx),
      baseData(_baseData), aliasReg(_aliasReg),
      sizeData(_sizeData),
      shiftAmtData(ceilLog2(_sizeData)),
      memAccessFlags(MMU::AllowUnaligned)
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

    // Sources: srcIdx (Rd+1, index value), baseData, srcData (Rd), aliasReg
    setSrcRegIdx(_numSrcRegs++, intRegClass[_srcIdx]);
    setSrcRegIdx(_numSrcRegs++, intRegClass[_baseData]);
    setSrcRegIdx(_numSrcRegs++, intRegClass[_srcData]);
    setSrcRegIdx(_numSrcRegs++, intRegClass[_aliasReg]);
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
    uint64_t index_value   = xc->getRegOperand(this, 0); // srcIdx (Rd+1)
    uint64_t base_data_val = xc->getRegOperand(this, 1); // baseData
    uint64_t data_val      = xc->getRegOperand(this, 2); // srcData (Rd)
    uint64_t alias_val     = xc->getRegOperand(this, 3); // aliasReg

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

    // Attach IAA extension so the Sequencer can identify this as an
    // indirect-value access and route through m_IndirectRequestTable.
    auto iaa = std::make_shared<IndirectAccessAlias>(alias_val);
    xc->setIAAExt(iaa);

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
    printIntReg(ss, srcData);
    ss << ", [";
    printIntReg(ss, baseData);
    ss << ", ";
    printIntReg(ss, srcIdx);
    ss << "]";
    return ss.str();
}

} // namespace ArmISA
} // namespace gem5
