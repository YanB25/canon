#ifndef NOSTD_SOURCE_LOCATION_HPP
#define NOSTD_SOURCE_LOCATION_HPP

#pragma once

#include <cstdint>
#include <iostream>
#include <tuple>

// Clang
#if defined(__clang__) && !defined(__apple_build_version__) && \
    (__clang_major__ >= 9)
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FILE
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FUNCTION
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_LINE
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_COLUMN
// AppleClang https://en.wikipedia.org/wiki/Xcode#Toolchain_versions
#elif defined(__apple_build_version__) && defined(__clang__) && \
    (__clang_major__ * 10000 + __clang_minor__ * 100 +          \
     __clang_patchlevel__ % 100) >= 110003
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FILE
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FUNCTION
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_LINE
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_COLUMN
// GCC
#elif defined(__GNUC__) && \
    (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FILE
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FUNCTION
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_LINE
#define NOSTD_SOURCE_LOCATION_NO_BUILTIN_COLUMN
// MSVC https://github.com/microsoft/STL/issues/54#issuecomment-616904069
// https://learn.microsoft.com/en-us/cpp/overview/compiler-versions?view=msvc-170
#elif defined(_MSC_VER) && !defined(__clang__) && \
    !defined(__INTEL_COMPILER) && (_MSC_VER >= 1926)
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FILE
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FUNCTION
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_LINE
#define NOSTD_SOURCE_LOCATION_HAS_BUILTIN_COLUMN
#endif

namespace nostd
{
struct source_location
{
public:
#if defined(NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FILE) &&     \
    defined(NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FUNCTION) && \
    defined(NOSTD_SOURCE_LOCATION_HAS_BUILTIN_LINE) &&     \
    defined(NOSTD_SOURCE_LOCATION_HAS_BUILTIN_COLUMN)
    static constexpr source_location current(
        const char *fileName = __builtin_FILE(),
        const char *functionName = __builtin_FUNCTION(),
        const uint_least32_t lineNumber = __builtin_LINE(),
        const uint_least32_t columnOffset = __builtin_COLUMN()) noexcept
#elif defined(NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FILE) &&   \
    defined(NOSTD_SOURCE_LOCATION_HAS_BUILTIN_FUNCTION) && \
    defined(NOSTD_SOURCE_LOCATION_HAS_BUILTIN_LINE) &&     \
    defined(NOSTD_SOURCE_LOCATION_NO_BUILTIN_COLUMN)
    static constexpr source_location current(
        const char *fileName = __builtin_FILE(),
        const char *functionName = __builtin_FUNCTION(),
        const uint_least32_t lineNumber = __builtin_LINE(),
        const uint_least32_t columnOffset = 0) noexcept
#else
    static constexpr source_location current(
        const char *fileName = "unsupported",
        const char *functionName = "unsupported",
        const uint_least32_t lineNumber = 0,
        const uint_least32_t columnOffset = 0) noexcept
#endif
    {
        return source_location(
            fileName, functionName, lineNumber, columnOffset);
    }

    constexpr source_location() noexcept = default;

    constexpr const char *file_name() const noexcept
    {
        return fileName;
    }

    constexpr const char *function_name() const noexcept
    {
        return functionName;
    }

    constexpr uint_least32_t line() const noexcept
    {
        return lineNumber;
    }

    constexpr std::uint_least32_t column() const noexcept
    {
        return columnOffset;
    }

    auto operator<=>(const source_location &rhs) const
    {
        return std::tie(fileName, functionName, lineNumber, columnOffset) <=>
               std::tie(rhs.fileName,
                        rhs.functionName,
                        rhs.lineNumber,
                        rhs.columnOffset);
    }

private:
    constexpr source_location(const char *fileName,
                              const char *functionName,
                              uint_least32_t lineNumber,
                              uint_least32_t columnOffset) noexcept
        : fileName(fileName),
          functionName(functionName),
          lineNumber(lineNumber),
          columnOffset(columnOffset)
    {
    }

    const char *fileName = "";
    const char *functionName = "";
    std::uint_least32_t lineNumber{};
    std::uint_least32_t columnOffset{};
};

inline std::ostream &operator<<(std::ostream &os, const source_location &loc)
{
    os << loc.function_name() << " " << loc.file_name() << ":" << loc.line()
       << ":" << loc.column();
    return os;
}

}  // namespace nostd

namespace std
{
template <>
struct hash<nostd::source_location>
{
    std::size_t operator()(const nostd::source_location &loc) const
    {
        using std::hash;
        using std::size_t;
        using std::string;

        return hash<string>()(loc.file_name()) ^ hash<uint32_t>()(loc.line());
    }
};
}  // namespace std

#endif