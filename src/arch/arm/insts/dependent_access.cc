#include "arch/arm/insts/dependent_access.hh"

#include <memory>

#include "base/logging.hh"

namespace gem5
{

namespace ArmISA
{

ARMDependentAccessGen::ARMDependentAccessGen(
    AccessIntent intent, size_t index_size,
    Addr index_addr, Addr base_addr, size_t data_size)
    : DependentAccessGen(index_size)
{
    addDescriptor(intent, index_addr, base_addr, data_size);
}

std::unique_ptr<ExtensionBase>
ARMDependentAccessGen::clone() const
{
    return std::make_unique<ARMDependentAccessGen>(*this);
}

RequestPtr
ARMDependentAccessGen::genNextRequest(uint64_t index_value)
{
    assert(!isMerged() && "genNextRequest on merged DAG — unwrap first");
    const auto& desc = _descriptors.front();

    panic_if(!desc.translateFn,
        "ARMDependentAccessGen::genNextRequest called without a translator. "
        "The translator should be set by MMU::translateComplete during "
        "Phase 1 address translation.");

    // Compute the virtual address of the gather target element.
    Addr value_vaddr = desc.baseAddr + (index_value * desc.dataSize);

    // Translate VA -> PA via the callback set at instruction execute time.
    Addr value_paddr = desc.translateFn(value_vaddr);

    // Build a physical-address Request.
    return std::make_shared<Request>(value_paddr, desc.dataSize, 0,
                                    _requestorId);
}

Addr
ARMDependentAccessGen::genAddress(uint64_t index_value) const
{
    assert(!isMerged() && "genAddress on merged DAG — unwrap first");
    const auto& desc = _descriptors.front();
    panic_if(!desc.translateFn,
        "ARMDependentAccessGen::genAddress called without a translator.");
    Addr vaddr = desc.baseAddr + (index_value * desc.dataSize);
    return desc.translateFn(vaddr);
}

std::vector<std::unique_ptr<DependentAccessGen>>
ARMDependentAccessGen::unwrap() const
{
    std::vector<std::unique_ptr<DependentAccessGen>> result;
    for (const auto& desc : _descriptors) {
        auto single = std::make_unique<ARMDependentAccessGen>(
            desc.intent, _indexSize,
            desc.indexAddr, desc.baseAddr, desc.dataSize);
        if (desc.translateFn)
            single->setTranslator(desc.translateFn);
        single->setRequestorId(_requestorId);
        result.push_back(std::move(single));
    }
    return result;
}

} // namespace ArmISA
} // namespace gem5
