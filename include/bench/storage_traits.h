#pragma once
#include <type_traits>

namespace bench
{
template <typename, typename = void>
struct spec_gls
{
    using type = Void;
};

template <typename T>
struct spec_gls<T, std::void_t<typename T::GLS>>
{
    using type = typename T::GLS;
};

template <typename T>
using spec_gls_t = typename spec_gls<T>::type;

template <typename, typename = void>
struct spec_tls
{
    using type = Void;
};

template <typename T>
struct spec_tls<T, std::void_t<typename T::TLS>>
{
    using type = typename T::TLS;
};

template <typename T>
using spec_tls_t = typename spec_tls<T>::type;

template <typename, typename = void>
struct spec_ptls
{
    using type = Void;
};

template <typename T>
struct spec_ptls<T, std::void_t<typename T::PTLS>>
{
    using type = typename T::PTLS;
};

template <typename T>
using spec_ptls_t = typename spec_ptls<T>::type;

template <typename, typename = void>
struct spec_cls
{
    using type = Void;
};

template <typename T>
struct spec_cls<T, std::void_t<typename T::CLS>>
{
    using type = typename T::CLS;
};

template <typename T>
using spec_cls_t = typename spec_cls<T>::type;

template <typename, typename = void>
struct spec_pcls
{
    using type = Void;
};

template <typename T>
struct spec_pcls<T, std::void_t<typename T::PCLS>>
{
    using type = typename T::PCLS;
};

template <typename T>
using spec_pcls_t = typename spec_pcls<T>::type;

}  // namespace bench
