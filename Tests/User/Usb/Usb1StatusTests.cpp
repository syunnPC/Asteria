// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/OhciStatus.hpp"
#include "User/Usb/UhciStatus.hpp"

#include "Support/TestContext.hpp"

namespace
{
namespace U = Asteria::User::Usb;
using Asteria::User::Input::TransferStatus;
} // namespace
int main()
{
    Asteria::TestSupport::TestContext context{"Usb1Status"};
    constexpr TransferStatus codes[]{
        TransferStatus::Success, TransferStatus::Retryable, TransferStatus::Retryable, TransferStatus::Retryable,
        TransferStatus::Stall,   TransferStatus::Retryable, TransferStatus::Error,     TransferStatus::Error,
        TransferStatus::Error,   TransferStatus::Success,   TransferStatus::Error,     TransferStatus::Error,
        TransferStatus::Error,   TransferStatus::Error,     TransferStatus::Retryable, TransferStatus::Retryable,
    };
    for (U::UInt32 code{0U}; code < 16U; ++code)
    {
        U::OhciTdResult result{};
        ASTERIA_EXPECT(context, "all OHCI CC values and residual",
                       U::DecodeOhciTd((code << 28U) | (1U << 18U), 0x10004U, 0x10000U, 8U, result),
                       result.Active == (code >= 14U), result.Status == codes[code], result.ActualLength == 4U);
    }
    U::OhciTdResult ohci{};
    ASTERIA_EXPECT(context, "OHCI final DMA byte may be at 4GiB minus one",
                   U::DecodeOhciTd(0U, 0U, 0xFFFFFFF8U, 8U, ohci), ohci.ActualLength == 8U);
    ASTERIA_EXPECT(context, "unrounded underrun is an error", U::DecodeOhciTd(9U << 28U, 0x10004U, 0x10000U, 8U, ohci),
                   ohci.Status == TransferStatus::Error);
    constexpr U::UInt32 flags[]{0U,
                                1U << 17U,
                                1U << 18U,
                                1U << 19U,
                                1U << 20U,
                                1U << 21U,
                                1U << 22U,
                                (1U << 20U) | (1U << 22U),
                                (1U << 21U) | (1U << 22U)};
    constexpr TransferStatus statuses[]{TransferStatus::Success,   TransferStatus::Retryable, TransferStatus::Retryable,
                                        TransferStatus::Retryable, TransferStatus::Error,     TransferStatus::Error,
                                        TransferStatus::Stall,     TransferStatus::Error,     TransferStatus::Error};
    for (U::Size i{0U}; i < 9U; ++i)
    {
        for (U::UInt32 errors{0U}; errors < 4U; ++errors)
        {
            U::UhciTdResult result{};
            ASTERIA_EXPECT(context, "UHCI completed status and C_ERR",
                           U::DecodeUhciTd(flags[i] | (errors << 27U) | 3U, 8U, result), !result.Active,
                           result.Status == statuses[i], result.ActualLength == 4U);
            ASTERIA_EXPECT(context, "active hardware retry remains pending",
                           U::DecodeUhciTd(flags[i] | (errors << 27U) | (1U << 23U) | 0x7FFU, 8U, result),
                           result.Active, result.Status == TransferStatus::Retryable, result.ActualLength == 0U);
        }
    }
    U::UhciTdResult uhci{};
    ASTERIA_EXPECT(context, "UHCI zero length encoding", U::DecodeUhciTd(0x7FFU, 0U, uhci), uhci.ActualLength == 0U);
    ASTERIA_EXPECT(context, "UHCI corrupt residual rejected", !U::DecodeUhciTd(8U, 8U, uhci));
    return context.Finish();
}
