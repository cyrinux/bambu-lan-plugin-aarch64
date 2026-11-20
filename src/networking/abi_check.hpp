// Compile-time check that each exported function has exactly the type the
// slicer casts the dlsym() result to.
#pragma once

#include "network_agent_abi.hpp"

#include "../common/log.hpp"

#include <exception>
#include <type_traits>

namespace bbl_lan {
// Exceptions must never cross the extern "C" boundary into the slicer.
template <typename R, typename Fn> R guarded(const char* name, R fail_value, Fn&& fn)
{
    try {
        return fn();
    } catch (const std::exception& e) {
        LOG_E(name, " failed: ", e.what());
    } catch (...) {
        LOG_E(name, " failed with an unknown exception");
    }
    return fail_value;
}
} // namespace bbl_lan

#define BBL_EXPORT __attribute__((visibility("default")))

#define BBL_CHECK(name)                                                                   \
    static_assert(std::is_same_v<decltype(&bambu_network_##name), bbl_abi::func_##name>, \
                  "ABI mismatch for bambu_network_" #name);
