
#ifndef __ARCH_ARM_INSTS_DEPENDENT_ACCESS_HH__
#define __ARCH_ARM_INSTS_DEPENDENT_ACCESS_HH__

#include <cstdint>
#include <memory>
#include <vector>

#include "base/types.hh"
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
  public:
    ARMDependentAccessGen(AccessIntent intent, size_t index_size,
                          Addr index_addr, Addr base_addr, size_t data_size,
                          Addr alias);
    virtual std::unique_ptr<ExtensionBase> clone() const override;
    virtual RequestPtr genNextRequest(uint64_t index_value) override;
    virtual std::vector<std::unique_ptr<DependentAccessGen>>
        unwrap() const override;

    // Convenience accessor (asserts single descriptor)
    Addr getBaseAddr() const {
        assert(!isMerged());
        return _descriptors.front().baseAddr;
    }

  protected:
    virtual Addr dataAddr(const DepDescriptor &desc,
                          uint64_t index_value) const override;
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_INSTS_DEPENDENT_ACCESS_HH__
