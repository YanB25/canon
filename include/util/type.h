#pragma once
namespace util::type
{
enum class TypeTag
{
    tag_char,
    tag_signed_char,
    tag_unsigned_char,
    tag_char16_t,
    tag_char32_t,
    tag_wchar_t,
    tag_short_int,
    tag_unsigned_short_int,
    tag_int,
    tag_unsigned_int,
    tag_long_int,
    tag_unsigned_long_int,
    tag_long_long_int,
    tag_unsigned_long_long_int,
    tag_float,
    tag_double,
    tag_long_double,
    tag_void,
    tag_nullptr_t,
    tag_bool,
    tag_others,
};
template <typename T>
inline TypeTag to_type_tag(T &&)
{
    if (std::is_same_v<T, char>)
    {
        return TypeTag::tag_char;
    }
    if (std::is_same_v<T, signed char>)
    {
        return TypeTag::tag_signed_char;
    }
    if (std::is_same_v<T, unsigned char>)
    {
        return TypeTag::tag_unsigned_char;
    }
    if (std::is_same_v<T, char16_t>)
    {
        return TypeTag::tag_char16_t;
    }
    if (std::is_same_v<T, char32_t>)
    {
        return TypeTag::tag_char32_t;
    }
    if (std::is_same_v<T, wchar_t>)
    {
        return TypeTag::tag_wchar_t;
    }
    // short
    if (std::is_same_v<T, short int>)
    {
        return TypeTag::tag_short_int;
    }
    if (std::is_same_v<T, unsigned short int>)
    {
        return TypeTag::tag_unsigned_short_int;
    }
    if (std::is_same_v<T, char32_t>)
    {
        return TypeTag::tag_char32_t;
    }
    if (std::is_same_v<T, wchar_t>)
    {
        return TypeTag::tag_wchar_t;
    }
}
}  // namespace util::type