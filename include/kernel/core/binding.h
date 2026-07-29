#pragma once

#include "processing_rate.h"
#include "processing_rate_probability_function.h"
#include "relationship.h"
#include "attributes.h"
#include <llvm/ADT/STLExtras.h>

#include <llvm/Support/raw_ostream.h>

namespace llvm { class Type; }
namespace llvm { class raw_ostream; }

namespace kernel {

namespace detail {

struct AnnotatedProcessingRate : public AttributeSet {

    AnnotatedProcessingRate() = default;

    AnnotatedProcessingRate(ProcessingRate && rate)
    : mRate(std::move(rate)) {

    }

    AnnotatedProcessingRate(ProcessingRate && rate, AttributeSet && attributeSet)
    : AttributeSet(std::move(attributeSet))
    , mRate(std::move(rate)) {

    }

    AnnotatedProcessingRate(ProcessingRate && rate, const AttributeSet & attributeSet)
    : AttributeSet(attributeSet)
    , mRate(std::move(rate)) {

    }

    template <typename ParamType>
    static void __set_apr_param(AnnotatedProcessingRate &, ParamType && value);

    template <typename... ParamTypes>
    friend AnnotatedProcessingRate annotateProcessingRate(ParamTypes &&... params);

public:
    ProcessingRate mRate;
};

template <>
BOOST_FORCEINLINE void AnnotatedProcessingRate::__set_apr_param<ProcessingRate>(AnnotatedProcessingRate & apr, ProcessingRate && rate) {
    apr.mRate = rate;
}

template <>
BOOST_FORCEINLINE void AnnotatedProcessingRate::__set_apr_param<Attribute>(AnnotatedProcessingRate & apr, Attribute && attr) {
    apr.addAttribute(attr);
}

template<unsigned I, typename... ParamTypes>
BOOST_FORCEINLINE static typename std::enable_if<I < sizeof...(ParamTypes), void>::type
__set_apr_params(AnnotatedProcessingRate & apr, std::tuple<ParamTypes...> && params) {
    using HeadType = typename std::tuple_element<I, std::tuple<ParamTypes...>>::type;
    AnnotatedProcessingRate::__set_apr_param<HeadType>(apr, std::move(std::get<I>(params)));
    __set_apr_params<I + 1U, ParamTypes...>(apr, std::move(params));
}

template<unsigned I, typename... ParamTypes>
BOOST_FORCEINLINE typename std::enable_if<I == sizeof...(ParamTypes), void>::type
__set_apr_params(AnnotatedProcessingRate &, std::tuple<ParamTypes...> &&) { }

template <typename... ParamTypes>
inline AnnotatedProcessingRate annotateProcessingRate(ParamTypes &&... params) {
    AnnotatedProcessingRate binding;
    // Because the ParamTypes will likely be struct objects, I'm worried here we'll
    // end up recursively calling their copy constructors each time. By passing a
    // tuple, this should lessen the chance of that happening.
    __set_apr_params<0U, ParamTypes...>(binding, std::make_tuple(params...));
    return binding;
}

} /* end of detail */

struct Binding : public detail::AnnotatedProcessingRate {

    friend class Kernel;
    friend class PipelineBuilder;
    friend class PipelineCompiler;
    friend class PipelineKernel;

    template <typename ParamType>
    static void __set_binding_param(Binding &, ParamType value);

    template <typename... ParamTypes>
    friend Binding Bind(std::string name, StreamSet * streamSet, ParamTypes &&... params);

    template <typename... ParamTypes>
    friend Binding Bind(llvm::Type * const scalarType, std::string name, ParamTypes &&... params);

    // TODO: use templatized var-args to simplify the constructors? would need to default in the processing rate and verify only one was added.

    Binding(std::string name, Relationship * const value, ProcessingRate r = FixedRate(1));
    Binding(std::string name, Relationship * const value, ProcessingRate r, Attribute && attribute);
    Binding(std::string name, Relationship * const value, ProcessingRate r, std::initializer_list<Attribute> attributes);
    Binding(std::string && name, Relationship * const value, detail::AnnotatedProcessingRate && bindingRate);

    Binding(llvm::Type * const scalarType, std::string name, ProcessingRate r = FixedRate(1));
    Binding(llvm::Type * const scalarType, std::string name, ProcessingRate r, Attribute && attribute);
    Binding(llvm::Type * const scalarType, std::string name, ProcessingRate r, std::initializer_list<Attribute> attribute);
    Binding(llvm::Type * const scalarType, std::string && name, detail::AnnotatedProcessingRate && bindingRate);

    Binding(llvm::Type * const scalarType, std::string name, Relationship * const value, ProcessingRate r = FixedRate(1));
    Binding(llvm::Type * const scalarType, std::string name, Relationship * const value, ProcessingRate r, Attribute && attribute);
    Binding(llvm::Type * const scalarType, std::string name, Relationship * const value, ProcessingRate r, std::initializer_list<Attribute> attributes);

    Binding(llvm::Type * const scalarType, std::string && name, Relationship * const value, detail::AnnotatedProcessingRate && bindingRate);

    Binding(const Binding & original, ProcessingRate r);

    const std::string & getName() const LLVM_READNONE {
        return mName;
    }

    const ProcessingRate & getRate() const LLVM_READNONE {
        return mRate;
    }

    bool isPrincipal() const LLVM_READNONE {
        return hasAttribute(AttributeId::Principal);
    }

    bool hasLookahead() const LLVM_READNONE {
        return hasAttribute(AttributeId::LookAhead);
    }

    unsigned getLookahead() const LLVM_READNONE {
        return findAttribute(AttributeId::LookAhead).amount();
    }

    bool isDeferred() const LLVM_READNONE {
        return hasAttribute(AttributeId::Deferred);
    }

    llvm::Type * getType() const {
        return mType;
    }

    void setType(llvm::Type * type)  {
        mType = type;
    }

    Relationship * getRelationship() const {
        return mRelationship;
    }

    const ProcessingRateProbabilityDistribution & getDistribution() const {
        return mDistribution;
    }

    void setRelationship(Relationship * const value);

    LLVM_READNONE unsigned getNumElements() const;

    LLVM_READNONE unsigned getFieldWidth() const;

    void print(const Kernel * const kernel, llvm::raw_ostream & out) const noexcept;

private:
    const std::string                     mName;
    llvm::Type *                          mType;
    Relationship *                        mRelationship;
    ProcessingRateProbabilityDistribution mDistribution;
};

using Bindings = std::vector<Binding>;

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief isCountable
 ** ------------------------------------------------------------------------------------------------------------- */
LLVM_READNONE inline bool isCountable(const Binding & binding) {
    const ProcessingRate & rate = binding.getRate();
    switch (rate.getKind()) {
        case ProcessingRate::KindId::Fixed:
        case ProcessingRate::KindId::PopCount:
        case ProcessingRate::KindId::NegatedPopCount:
        case ProcessingRate::KindId::PartialSum:
        case ProcessingRate::KindId::Greedy:
            return true;
        default:
            return false;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief isNonFixedCountable
 ** ------------------------------------------------------------------------------------------------------------- */
LLVM_READNONE inline bool isNonFixedCountable(const Binding & binding) {
    const ProcessingRate & rate = binding.getRate();
    switch (rate.getKind()) {
        case ProcessingRate::KindId::PopCount:
        case ProcessingRate::KindId::NegatedPopCount:
        case ProcessingRate::KindId::PartialSum:
        case ProcessingRate::KindId::Greedy:
            return true;
        default:
            return false;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief isAddressable
 ** ------------------------------------------------------------------------------------------------------------- */
LLVM_READNONE inline bool isAddressable(const Binding & binding) {    
    for (const auto & attr : binding.getAttributes()) {
        switch (attr.getKind()) {
            case Binding::AttributeId::Deferred:
            case Binding::AttributeId::ReturnedBuffer:
            return true;
        default:
            break;
        }
    }
    const ProcessingRate & rate = binding.getRate();
    switch (rate.getKind()) {
        case ProcessingRate::KindId::Bounded:
        case ProcessingRate::KindId::Unknown:
            return true;
        default:
            return false;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief requiresItemCount
 ** ------------------------------------------------------------------------------------------------------------- */
LLVM_READNONE inline bool requiresItemCount(const Binding & binding) {
    return isAddressable(binding) || isNonFixedCountable(binding);
}

template <>
BOOST_FORCEINLINE void Binding::__set_binding_param<Scalar *>(Binding & binding, Scalar * value) {
    binding.setRelationship(value);
}

template <>
BOOST_FORCEINLINE void Binding::__set_binding_param<ScalarConstant *>(Binding & binding, ScalarConstant * value) {
    binding.setRelationship(value);
}

template <>
BOOST_FORCEINLINE void Binding::__set_binding_param<ProcessingRate>(Binding & binding, ProcessingRate rate) {
    binding.mRate = rate;
}

template <>
BOOST_FORCEINLINE void Binding::__set_binding_param<Attribute>(Binding & binding, Attribute attr) {
    binding.addAttribute(attr);
}

template <>
BOOST_FORCEINLINE void Binding::__set_binding_param<ProcessingRateProbabilityDistribution>(Binding & binding, ProcessingRateProbabilityDistribution df) {
    binding.mDistribution = df;
}

template<unsigned I, typename... ParamTypes>
BOOST_FORCEINLINE static typename std::enable_if<I < sizeof...(ParamTypes), void>::type
__set_binding_params(Binding & binding, std::tuple<ParamTypes...> && params) {
    using HeadType = typename std::tuple_element<I, std::tuple<ParamTypes...>>::type;
    Binding::__set_binding_param<HeadType>(binding, std::move(std::get<I>(params)));
    __set_binding_params<I + 1U, ParamTypes...>(binding, std::move(params));
}

template<unsigned I, typename... ParamTypes>
BOOST_FORCEINLINE typename std::enable_if<I == sizeof...(ParamTypes), void>::type
__set_binding_params(Binding &, std::tuple<ParamTypes...> &&) { }

template <typename... ParamTypes>
inline Binding Bind(std::string name, StreamSet * streamSet, ParamTypes... params) {
    Binding binding(std::move(name), streamSet);
    // Because the ParamTypes will likely be struct objects, I'm worried here we'll
    // end up recursively calling their copy constructors each time. By passing a
    // tuple, this should lessen the chance of that happening.
    __set_binding_params<0U, ParamTypes...>(binding, std::make_tuple(params...));
    assert (binding.getRelationship());
    assert (binding.getType());
    return binding;
}

template <typename... ParamTypes>
inline Binding Bind(llvm::Type * const scalarType, std::string name, ParamTypes... params) {
    Binding binding(scalarType, std::move(name));
    __set_binding_params<0U, ParamTypes...>(binding, std::make_tuple(params...));
    assert (binding.getType());
    return binding;
}

}

