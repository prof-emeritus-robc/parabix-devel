#ifndef PROGRAM_BUILDER_H
#define PROGRAM_BUILDER_H

#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/pipeline/driver/driver.h>
#include <kernel/core/streamsetptr.h>
#include <type_traits>

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief ProgramBuilder
 ** ------------------------------------------------------------------------------------------------------------- */
class ProgramBuilder : public PipelineBuilder {
    friend class ::BaseDriver;
public:

    void * compile();

    Kernel * makeKernel() override;

    ProgramBuilder(BaseDriver & driver, PipelineKernel * const kernel);

private:

    void * compileKernel(Kernel * const kernel);
};

struct streamset_t {

    template<typename... Attrs>
    explicit constexpr streamset_t(int elementCount, int fieldWidth, Attrs &&... attrs)
    : ElementCount(elementCount)
    , FieldWidth(fieldWidth)
    , StreamSet(nullptr)
    , AnnotatedProcessingRate(kernel::detail::annotateProcessingRate<Attrs...>(std::forward<Attrs>(attrs)...)) {

    }

    template<typename... Attrs>
    explicit constexpr streamset_t(kernel::StreamSet * streamSet, Attrs &&... attrs)
    : ElementCount(0)
    , FieldWidth(0)
    , StreamSet(streamSet)
    , AnnotatedProcessingRate(kernel::detail::annotateProcessingRate<Attrs...>(std::forward<Attrs>(attrs)...)) {

    }

    const int ElementCount;
    const int FieldWidth;
    kernel::StreamSet * const StreamSet;
    kernel::detail::AnnotatedProcessingRate AnnotatedProcessingRate;
};

template <typename T>
struct Input {

    Input(std::string && name, kernel::Scalar * scalar = nullptr)
    : Name(std::move(name))
    , Scalar(scalar) {

    }

    std::string Name;
    kernel::Scalar * const Scalar;
};

template <>
struct Input<streamset_t> : public streamset_t {

    template<typename ... ValueArgs>
    Input(std::string && name, ValueArgs &&... args)
    : streamset_t(std::forward<ValueArgs>(args)...)
    , Name(std::move(name)) {

    }

    std::string Name;
};

template <typename T>
struct Output {

    explicit Output(std::string name, kernel::Scalar * scalar = nullptr)
    : Name(std::move(name))
    , Scalar(scalar) {

    }

    std::string Name;
    kernel::Scalar * const Scalar;
};

template <>
struct Output<streamset_t> : public streamset_t {

    template<typename ... ValueArgs>
    explicit Output(std::string name, ValueArgs &&... args)
    : streamset_t(std::forward<ValueArgs>(args)...)
    , Name(std::move(name)) {

    }

    std::string Name;
};

struct Signature {

    explicit Signature(std::string && signature)
    : Value(std::move(signature)) {

    }

    Signature(llvm::StringRef signature)
    : Value(signature.str()) {

    }

    std::string Value;
};


namespace { /* anonymous */

template <typename T, typename U>
struct tuple_cons {
    using type = typename std::tuple<T, U>;
};

template <typename T, typename... Us>
struct tuple_cons<T, std::tuple<Us...>> {
    using type = typename std::tuple<T, Us...>;
};

// void types represent non-existant type values; filter them whenever possible
// so the tuple reduces to a single void type if and only if no types exist
template <typename T>
struct tuple_cons<T, void> {
    using type = T;
};

template <typename T>
struct tuple_cons<void, T> {
    using type = T;
};

template <typename T>
struct tuple_cons<T, std::tuple<>> {
    using type = T;
};

template<typename R, typename T>
struct make_function_decl {
    using type = R(T);
};

template<typename R>
struct make_function_decl<R, void> {
    using type = R();
};

template<typename R, typename ...Ts>
struct make_function_decl<R, std::tuple<Ts...>> {
    using type = R(Ts...);
};

template<typename ... Rest>
struct extract_args {
    using ReturnType = void;
    using ParamsType = void;
    static constexpr auto InputScalarCount = 0;
    static constexpr auto OutputScalarCount = 0;
    static constexpr auto InputStreamSetCount = 0;
    static constexpr auto OutputStreamSetCount = 0;
};

template<typename InputType>
struct extract_args<Input<InputType>> {
    using ReturnType = void;
    using ParamsType = const InputType;
    static constexpr auto InputScalarCount = 1;
    static constexpr auto OutputScalarCount = 0;
    static constexpr auto InputStreamSetCount = 0;
    static constexpr auto OutputStreamSetCount = 0;
};

template<typename OutputType>
struct extract_args<Output<OutputType>> {
    using Type = OutputType;
    using ReturnType = OutputType;
    using ParamsType = void;
    static constexpr auto InputScalarCount = 0;
    static constexpr auto OutputScalarCount = 1;
    static constexpr auto InputStreamSetCount = 0;
    static constexpr auto OutputStreamSetCount = 0;
};

template<>
struct extract_args<Input<streamset_t>> {
    using ReturnType = void;
    using ParamsType = const StreamSetPtr &;
    static constexpr auto InputScalarCount = 0;
    static constexpr auto OutputScalarCount = 0;
    static constexpr auto InputStreamSetCount = 1;
    static constexpr auto OutputStreamSetCount = 0;
};

template<>
struct extract_args<Output<streamset_t>> {
    using ReturnType = void;
    using ParamsType = StreamSetPtr &;
    static constexpr auto InputScalarCount = 0;
    static constexpr auto OutputScalarCount = 0;
    static constexpr auto InputStreamSetCount = 0;
    static constexpr auto OutputStreamSetCount = 1;
};

template<typename InputType, typename ... Rest>
struct extract_args<Input<InputType>, Rest...> {
    using Type = InputType;
    using ReturnType = typename extract_args<Rest...>::ReturnType;
    using ParamsType = typename tuple_cons<Type, typename extract_args<Rest...>::ParamsType>::type;
    static constexpr auto InputScalarCount = extract_args<Rest...>::InputScalarCount + 1;
    static constexpr auto OutputScalarCount = extract_args<Rest...>::OutputScalarCount;
    static constexpr auto InputStreamSetCount = extract_args<Rest...>::InputStreamSetCount;
    static constexpr auto OutputStreamSetCount = extract_args<Rest...>::OutputStreamSetCount;
};

template<typename OutputType, typename ... Rest>
struct extract_args<Output<OutputType>, Rest...> {
    using Type = OutputType;
    using ReturnType = typename tuple_cons<Type, typename extract_args<Rest...>::ReturnType>::type;
    using ParamsType = typename extract_args<Rest...>::ParamsType;
    static constexpr auto InputScalarCount = extract_args<Rest...>::InputScalarCount;
    static constexpr auto OutputScalarCount = extract_args<Rest...>::OutputScalarCount + 1;
    static constexpr auto InputStreamSetCount = extract_args<Rest...>::InputStreamSetCount;
    static constexpr auto OutputStreamSetCount = extract_args<Rest...>::OutputStreamSetCount;
};

template<typename ... Rest>
struct extract_args<Input<streamset_t>, Rest...> {
    using Type = const StreamSetPtr &;
    using ReturnType = typename extract_args<Rest...>::ReturnType;
    using ParamsType = typename tuple_cons<Type, typename extract_args<Rest...>::ParamsType>::type;
    static constexpr auto InputScalarCount = extract_args<Rest...>::InputScalarCount;
    static constexpr auto OutputScalarCount = extract_args<Rest...>::OutputScalarCount;
    static constexpr auto InputStreamSetCount = extract_args<Rest...>::InputStreamSetCount + 1;
    static constexpr auto OutputStreamSetCount = extract_args<Rest...>::OutputStreamSetCount;
};

template<typename ... Rest>
struct extract_args<Output<streamset_t>, Rest...> {
    using Type = StreamSetPtr &;
    // despite being an output argument, references to the output streamset
    // values are passed as input arguments
    using ReturnType = typename extract_args<Rest...>::ReturnType;
    using ParamsType = typename tuple_cons<Type, typename extract_args<Rest...>::ParamsType>::type;
    static constexpr auto InputScalarCount = extract_args<Rest...>::InputScalarCount;
    static constexpr auto OutputScalarCount = extract_args<Rest...>::OutputScalarCount;
    static constexpr auto InputStreamSetCount = extract_args<Rest...>::InputStreamSetCount;
    static constexpr auto OutputStreamSetCount = extract_args<Rest...>::OutputStreamSetCount + 1;
};

struct PipelineConfig {
    std::string Signature;
    Bindings InputScalars;
    Bindings OutputScalars;
    Bindings InputStreamSets;
    Bindings OutputStreamSets;
    AttributeSet Attributes;
};

} /* end of anonymous namespace */

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief ProgramBuilder
 ** ------------------------------------------------------------------------------------------------------------- */
template<typename ... Args>
class TypedProgramBuilder final : public ProgramBuilder {
    friend class ::BaseDriver;

    using ParamsType = typename extract_args<Args...>::ParamsType;


public:
    using ReturnType = typename extract_args<Args...>::ReturnType;
    using FunctionDeclType =  typename make_function_decl<ReturnType, ParamsType>::type *;

    FunctionDeclType compile() {
        return reinterpret_cast<FunctionDeclType>(ProgramBuilder::compile());
    }

    inline TypedProgramBuilder(BaseDriver & driver, Args &&... args)
    : ProgramBuilder(driver, constructKernel(driver, std::forward<Args>(args)...)) {

    }

    TypedProgramBuilder(TypedProgramBuilder &&) = default;

private:


    static inline void append_arg(BaseDriver & driver, PipelineConfig & config, Signature && S) {
        config.Signature = S.Value;
    }

    static inline void append_arg(BaseDriver & driver, PipelineConfig & config, Attribute && A) {
        config.Attributes.addAttribute(A);
    }

    template<typename InputType>
    static inline void append_arg(BaseDriver & driver, PipelineConfig & config, Input<InputType> && I) {
        if constexpr (std::is_base_of<streamset_t, InputType>::value) {
            StreamSet * streamSet = nullptr;
            if (I.StreamSet) {
                streamSet = I.StreamSet;
            } else {
                streamSet = driver.CreateStreamSet(I.ElementCount, I.FieldWidth);
            }
            config.InputStreamSets.emplace_back(std::move(I.Name), streamSet,  std::move(I.AnnotatedProcessingRate));
        } else {
            llvm::Type * const ty = llvm::TypeBuilder<InputType, false>::get(driver.getContext());
            Scalar * scalar = nullptr;
            if (I.Scalar) {
                scalar = I.Scalar;
            } else {
                scalar = driver.CreateScalar(ty);
            }
            config.InputScalars.emplace_back(ty, I.Name, scalar);
        }

    }

    template<typename OutputType>
    static inline void append_arg(BaseDriver & driver, PipelineConfig & config, Output<OutputType> && O) {
        if constexpr (std::is_base_of<streamset_t, OutputType>::value) {
            StreamSet * streamSet = nullptr;
            if (O.StreamSet) {
                streamSet = O.StreamSet;
            } else {
                streamSet = driver.CreateStreamSet(O.ElementCount, O.FieldWidth);
            }
            config.OutputStreamSets.emplace_back(std::move(O.Name), streamSet,  std::move(O.AnnotatedProcessingRate));
        } else {
            llvm::Type * const ty = llvm::TypeBuilder<OutputType, false>::get(driver.getContext());
            Scalar * scalar = nullptr;
            if (O.Scalar) {
                scalar = O.Scalar;
            } else {
                scalar = driver.CreateScalar(ty);
            }
            config.OutputScalars.emplace_back(ty, O.Name, scalar);
        }
    }

    template<typename... Fs>
    static inline void append_args(BaseDriver & driver, PipelineConfig & config, Fs &&... fs) {
        static_assert(sizeof...(Fs) == 0, "compilation error");
        /* do nothing */
    }

    template<typename F, typename... Fs>
    static inline void append_args(BaseDriver & driver, PipelineConfig & config, F f, Fs &&... fs) {
        append_arg(driver, config, std::forward<F>(f));
        append_args(driver, config, std::forward<Fs>(fs)...);
    }

    static PipelineKernel * constructKernel(BaseDriver & driver, Args &&... args) {

        PipelineConfig config;
        constexpr auto inputScalarCount = extract_args<Args...>::InputScalarCount;
        if constexpr (inputScalarCount > 0) config.InputScalars.reserve(inputScalarCount);
        constexpr auto outputScalarCount = extract_args<Args...>::OutputScalarCount;
        if constexpr (outputScalarCount > 0) config.OutputScalars.reserve(outputScalarCount);
        constexpr auto inputStreamSetCount = extract_args<Args...>::InputStreamSetCount;
        if constexpr (inputStreamSetCount > 0) config.InputStreamSets.reserve(inputStreamSetCount);
        constexpr auto outputStreamSetCount = extract_args<Args...>::OutputStreamSetCount;
        if constexpr (outputStreamSetCount > 0) config.OutputStreamSets.reserve(outputStreamSetCount);

        append_args(driver, config, std::forward<Args>(args)...);

        assert (config.InputScalars.size() == inputScalarCount);
        assert (config.OutputScalars.size() == outputScalarCount);
        assert (config.InputStreamSets.size() == inputStreamSetCount);
        assert (config.OutputStreamSets.size() == outputStreamSetCount);

        PipelineKernel * const pipeline =
            new PipelineKernel(*driver.getMainBuilder(),
                               std::move(config.Signature),
                               std::move(config.Attributes),
                               std::move(config.InputStreamSets), std::move(config.OutputStreamSets),
                               std::move(config.InputScalars), std::move(config.OutputScalars));
        return pipeline;
    }

};

namespace {

// For compilation simplicity, we still require that the generated "main" function provided by
// the TypedProgramBuilder compile method matches the standard binding ordering, the following
// constraints report a simple compile error if ordering is invalid. This is eliminated at C++
// compile time.

template<typename T>
struct ordering_rank {
    constexpr static unsigned value = std::is_base_of_v<Attribute, T> ? 6 : 0; // attribute or unknown type id
};

template<>
struct ordering_rank<Signature> {
    constexpr static unsigned value = 1;
};

template<typename U>
struct ordering_rank<Input<U>> {
    constexpr static unsigned value = std::is_same_v<streamset_t, U> ? 2 : 4;
};

template<typename U>
struct ordering_rank<Output<U>> {
    constexpr static unsigned value = std::is_same_v<streamset_t, U> ? 3 : 5;
};

template<size_t i, typename ...Args>
constexpr bool ordering_constraints() {
    if constexpr ((i + 1) >= sizeof...(Args)) {
        return true;
    } else {
        using U = typename std::tuple_element<i, std::tuple<Args...>>::type;
        using V = typename std::tuple_element<i + 1, std::tuple<Args...>>::type;
        static_assert(ordering_rank<U>::value > 0 && ordering_rank<V>::value > 0,
        "CreatePipeline and CreateNestedPipeline can accept only Signature, Input<X> and Output<Y> arguments");
        if constexpr (ordering_rank<U>::value > ordering_rank<V>::value) {
            return false;
        }
        return ordering_constraints<i + 1, Args...>();
    }
}

} /* end of anonymous namespace */

template<typename ... Args>
TypedProgramBuilder<Args...> CreatePipeline(BaseDriver & driver, Args... args) {
    static_assert(ordering_constraints<0, Args...>(),
    "Program I/O orderings must be ordered in <Signature??, <Input StreamSet>*, <Output StreamSet>*, <Input Scalar>*, <Output Scalar>*, <Pipeline Attribute>*.");
    return TypedProgramBuilder<Args...>{driver, std::forward<Args>(args)...};
}

}

#endif // PROGRAM_BUILDER_H
