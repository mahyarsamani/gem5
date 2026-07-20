
#ifndef __ARCH_ARM_INSTS_DEPENDENT_ACCESS_HH__
#define __ARCH_ARM_INSTS_DEPENDENT_ACCESS_HH__

#include <cstdint>
#include <memory>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "mem/request.hh"

namespace gem5
{

namespace ArmISA
{

/**
 * ARM-specific DependentAccessGen for LDINDX_W scatter-gather offload.
 *
 * Constructed at ISA execute time with the gather parameters (base address
 * and element size). The VA->PA translation callback is set by
 * MMU::translateComplete once the Phase 1 address translation completes.
 * This is the single canonical location for translator setup.
 */
class ARMDependentAccessGen: public DependentAccessGen
{
  private:
    Addr _baseAddr;
    Addr _size;

  public:
    ARMDependentAccessGen(Addr base_addr, Addr size);
    virtual std::unique_ptr<ExtensionBase> clone() const override;
    virtual RequestPtr genNextRequest(RequestPtr og_req, uint64_t index_value) override;

    Addr getBaseAddr() const { return _baseAddr; }
};

class ARMIndependentAccessResp: public IndependentAccessResp
{
  private:
    RegIndex _destReg;
    InstSeqNum _seqNum;

  public:
    ARMIndependentAccessResp(RegIndex dest_reg);

    std::unique_ptr<ExtensionBase> clone() const override;

    RegIndex destReg() const;

    InstSeqNum seqNum() const;
    void setSeqNum(InstSeqNum seq_num);
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_INSTS_DEPENDENT_ACCESS_HH__
