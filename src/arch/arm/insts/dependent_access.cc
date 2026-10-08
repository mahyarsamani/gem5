#include "arch/arm/insts/dependent_access.hh"

#include <memory>

#include "base/logging.hh"

namespace gem5
{

namespace ArmISA
{

ARMDependentAccessGen::ARMDependentAccessGen(
    AccessIntent intent, size_t index_size,
    Addr index_addr, Addr base_addr, size_t data_size, Addr alias)
    : DependentAccessGen(index_size)
{
    addDescriptor(intent, index_addr, base_addr, data_size, alias);
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

    // Build a physical-address Request.
    return std::make_shared<Request>(genAddress(index_value),
                                     _descriptors.front().dataSize, 0,
                                     _requestorId);
}

Addr
ARMDependentAccessGen::dataAddr(const DepDescriptor &desc,
                                uint64_t index_value) const
{
    panic_if(!desc.translateFn,
        "ARMDependentAccessGen::dataAddr called without a translator. "
        "The translator should be set by MMU::translateComplete during "
        "Phase 1 address translation.");

    // baseAddr is the data array's VA; the translator is its only
    // VA -> PA translation.
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
            desc.indexAddr, desc.baseAddr, desc.dataSize, desc.alias);
        if (desc.translateFn)
            single->setTranslator(desc.translateFn);
        single->setRequestorId(_requestorId);
        result.push_back(std::move(single));
    }
    return result;
}

} // namespace ArmISA
} // namespace gem5
