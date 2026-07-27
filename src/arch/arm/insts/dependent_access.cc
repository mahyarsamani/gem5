#include "arch/arm/insts/dependent_access.hh"

#include <memory>

#include "base/logging.hh"

namespace gem5
{

namespace ArmISA
{

ARMDependentAccessGen::ARMDependentAccessGen(size_t index_size, size_t data_size, Addr base_addr):
    DependentAccessGen(index_size, data_size), _baseAddr(base_addr)
{}

std::unique_ptr<ExtensionBase>
ARMDependentAccessGen::clone() const
{
    auto copy = std::make_unique<ARMDependentAccessGen>(_indexSize, _dataSize, _baseAddr);
    if (hasData()) {
        copy->setStoreData(getStoreData(), _dataSize);
    }
    // Preserve the translator callback so the clone can also call
    // genNextRequest without needing setTranslator() again.
    if (hasTranslator())
        copy->setTranslator(_translateFn);
    // Preserve requestorId.
    copy->setRequestorId(_requestorId);
    return copy;
}

RequestPtr
ARMDependentAccessGen::genNextRequest(uint64_t index_value)
{
    panic_if(!hasTranslator(),
        "ARMDependentAccessGen::genNextRequest called without a translator. "
        "The translator should be set by MMU::translateComplete during "
        "Phase 1 address translation.");

    // Compute the virtual address of the gather target element.
    Addr value_vaddr = _baseAddr + (index_value * _dataSize);

    // Translate VA -> PA via the callback set at instruction execute time.
    Addr value_paddr = _translateFn(value_vaddr);

    // Build a physical-address Request.
    return std::make_shared<Request>(value_paddr, _dataSize, 0, _requestorId);
}

} // namespace ArmISA
} // namespace gem5
