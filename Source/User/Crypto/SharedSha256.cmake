# SPDX-License-Identifier: Apache-2.0

# Share boot trust's specification-derived SHA-256 through a user namespace
# projection, maintaining one implementation and the kernel/user dependency boundary.
# The generated projection only changes namespace/include guard, removes the
# unused legacy HMAC and adds volatile scratch erasure. Changes to the shared
# source trigger configuration again; unexpected source shape fails closed.
set(crypto_shared "${CMAKE_CURRENT_BINARY_DIR}/CryptoShared")
file(MAKE_DIRECTORY "${crypto_shared}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Kernel/Security/Sha256.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Kernel/Security/Sha256.cpp")
foreach(extension IN ITEMS hpp cpp)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Kernel/Security/Sha256.${extension}" shared)
    string(REPLACE "Asteria::Kernel::Security" "Asteria::User::Crypto::Detail" shared "${shared}")
    string(REPLACE "ASTERIA_KERNEL_SECURITY_SHA256" "ASTERIA_CRYPTO_SHARED_SHA256" shared "${shared}")
    string(REPLACE "Kernel/Security/Sha256.hpp" "User/Crypto/Sha256.hpp" shared "${shared}")
    if(extension STREQUAL "hpp")
        string(FIND "${shared}" "/// HMAC-SHA256" start)
        string(FIND "${shared}" "static_assert(sizeof(Sha256)" finish)
        if(start LESS 0 OR finish LESS start)
            message(FATAL_ERROR "Crypto shared SHA-256 header shape changed; review projection")
        endif()
        string(SUBSTRING "${shared}" 0 ${start} prefix)
        string(SUBSTRING "${shared}" ${finish} -1 suffix)
        string(REPLACE "static_assert(sizeof(HmacSha256) <= 192U);\n" "" suffix "${suffix}")
        set(shared "${prefix}${suffix}")
        file(CONFIGURE OUTPUT "${crypto_shared}/Sha256.hpp" CONTENT "@shared@" @ONLY)
    else()
        string(FIND "${shared}" "HmacSha256::HmacSha256" start)
        if(start LESS 0)
            message(FATAL_ERROR "Crypto shared SHA-256 source shape changed; review projection")
        endif()
        string(SUBSTRING "${shared}" 0 ${start} shared)
        foreach(marker IN ITEMS "    m_state[7U] += h;" "    *this = Sha256{};")
            string(FIND "${shared}" "${marker}" found)
            if(found LESS 0)
                message(FATAL_ERROR "Crypto shared SHA-256 wipe location changed")
            endif()
        endforeach()
        string(REPLACE "constexpr UInt8 InnerPadByte{0x36U};\n" "" shared "${shared}")
        string(REPLACE "constexpr UInt8 OuterPadByte{0x5CU};\n" "" shared "${shared}")
        string(REPLACE "    m_state[7U] += h;" "    m_state[7U] += h;\n    volatile UInt32* clear = schedule;\n    for (Size i = 0U; i < 16U; ++i) clear[i] = 0U;" shared "${shared}")
        string(REPLACE "    *this = Sha256{};" "    volatile UInt8* clear = reinterpret_cast<volatile UInt8*>(this);\n    for (Size i = 0U; i < sizeof(*this); ++i) clear[i] = 0U;\n    *this = Sha256{};" shared "${shared}")
        string(APPEND shared "} // namespace Asteria::User::Crypto::Detail\n")
        file(CONFIGURE OUTPUT "${crypto_shared}/Sha256.inc" CONTENT "@shared@" @ONLY)
    endif()
endforeach()
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
