#pragma once

#include <bit>
#include <cstring>
#include <type_traits>

namespace compat {

template<typename To, typename From>
To bit_cast(const From& source) noexcept {
    static_assert(sizeof(To) == sizeof(From));
    static_assert(std::is_trivially_copyable_v<To>);
    static_assert(std::is_trivially_copyable_v<From>);

#if defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L
    return std::bit_cast<To>(source);
#else
    To result;
    std::memcpy(&result, &source, sizeof(result));
    return result;
#endif
}

}
