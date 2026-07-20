#include "arch/arm/insts/dependent_access.hh"

#include <memory>

#include "base/logging.hh"

namespace gem5
{

namespace ArmISA
{

ARMDependentAccessGen::ARMDependentAccessGen(Addr base_addr, Addr size)
    : DependentAccessGen(), _baseAddr(base_addr), _size(size)
{}





std::unique_ptr<ExtensionBase>
ARMDependentAccessGen::clone() const
{
    auto copy = std::make_unique<ARMDependentAccessGen>(_baseAddr, _size);
    if (hasData()) {
        copy->setStoreData(getStoreData(), getStoreDataSize());
    }
    // Preserve the translator callback so the clone can also call
    // genNextRequest without needing setTranslator() again.
    if (hasTranslator())
        copy->setTranslator(_translateFn);
    return copy;
}

RequestPtr
ARMDependentAccessGen::genNextRequest(RequestPtr og_req, uint64_t index_value)
{
    panic_if(!hasTranslator(),
        "ARMDependentAccessGen::genNextRequest called without a translator. "
        "The translator should be set by MMU::translateComplete during "
        "Phase 1 address translation.");

    // Compute the virtual address of the gather target element.
    Addr value_vaddr = _baseAddr + (index_value * _size);

    // Translate VA -> PA via the callback set at instruction execute time.
    Addr value_paddr = _translateFn(value_vaddr);

    // Build a physical-address Request so Ruby can use it directly.
    return std::make_shared<Request>(
        value_paddr, _size, og_req->getFlags(), og_req->requestorId());
}

ARMIndependentAccessResp::ARMIndependentAccessResp(RegIndex dest_reg)
    : IndependentAccessResp(), _destReg(dest_reg), _seqNum(-1)
{}

std::unique_ptr<ExtensionBase>
ARMIndependentAccessResp::clone() const
{
    auto ret = std::make_unique<ARMIndependentAccessResp>(_destReg);
    ret->setSeqNum(_seqNum);
    ret->setIndexValue(getIndexValue());
    return ret;
}

RegIndex
ARMIndependentAccessResp::destReg() const
{
    return _destReg;
}

InstSeqNum
ARMIndependentAccessResp::seqNum() const
{
    return _seqNum;
}

void
ARMIndependentAccessResp::setSeqNum(InstSeqNum seq_num)
{
    _seqNum = seq_num;
}

} // namespace ArmISA
} // namespace gem5
