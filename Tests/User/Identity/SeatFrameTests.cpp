// SPDX-License-Identifier: Apache-2.0
#include "User/Identity/SeatFrame.hpp"
#include "User/Identity/Support.hpp"
using Asteria::User::Identity::MaximumPasswordBytes;
using Asteria::User::Identity::ReadSeatFrame;
using Asteria::User::Identity::ScreenEnrollment;
using Asteria::User::Identity::SeatDeliveryBytes;
using Asteria::User::Identity::SeatFace;
using Asteria::User::Identity::SeatFrameBytes;
using Asteria::User::Identity::SeatMaximumWords;
using Asteria::User::Identity::SeatMessage;
using Asteria::User::Identity::SeatMethod;
using Asteria::User::Identity::Size;
using Asteria::User::Identity::UInt64;
using Asteria::User::Identity::UInt8;
using IdentityTest::View;
using Asteria::User::Identity::WriteSeatFrame;
namespace Comp = Asteria::User::Component;
static UInt8 Frame[SeatFrameBytes]{};
static UInt8 Delivery[SeatDeliveryBytes]{};
static UInt64 Words[SeatMaximumWords]{};
static SeatMessage Output{};
static SeatMessage Before{};
static Size DeliveryOf(Size n) noexcept
{
    std::memset(Delivery, 0, sizeof(Delivery));
    std::memcpy(Delivery + Comp::EchoDeliveryHeaderBytes, Frame, n);
    return n + Comp::EchoDeliveryHeaderBytes;
}
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"SeatFrame"};
    Words[0U] = 1U;
    Words[1U] = 2U;
    Words[2U] = 1U;
    Words[3U] = 13U;
    Words[4U] = 20U;
    const auto name = View("administrator");
    for (Size i{0U}; i < name.Length; ++i)
    {
        Words[5U + i / 8U] |= static_cast<UInt64>(name.Bytes[i]) << (8U * (i % 8U));
    }
    const Asteria::User::System::CapabilityHandle handle{77U, 88U};
    const auto bytes = WriteSeatFrame(Frame, SeatFace::AuthSubmit, SeatMethod::Submit, 7U, Words, handle);
    auto delivered = DeliveryOf(bytes);
    ASTERIA_EXPECT(t, "counted metadata with Map-only Frame", bytes == SeatFrameBytes,
                   ReadSeatFrame(Delivery, delivered, Output), Output.Face == SeatFace::AuthSubmit,
                   Output.Attachment.Selector == 77U, Output.Words[4U] == 20U);
    Before = Output;
    bool atomic = true;
    for (Size i = 0U; i < delivered; ++i)
    {
        const auto original = Delivery[i];
        Delivery[i] ^= 0x80U;
        if (!ReadSeatFrame(Delivery, delivered, Output))
        {
            atomic &= std::memcmp(&Before, &Output, sizeof(Output)) == 0;
        }
        Delivery[i] = original;
        Output = Before;
    }
    ASTERIA_EXPECT(t, "every decode refusal preserves output", atomic, !ReadSeatFrame(Delivery, delivered - 1U, Output),
                   std::memcmp(&Before, &Output, sizeof(Output)) == 0);
    Words[4U] = MaximumPasswordBytes + 1U;
    ASTERIA_EXPECT(t, "oversize refused before a secret read",
                   WriteSeatFrame(Frame, SeatFace::AuthSubmit, SeatMethod::Submit, 7U, Words, handle) == 0U);
    Words[4U] = 20U;
    ASTERIA_EXPECT(t, "no inline credential or forwarding endpoint",
                   WriteSeatFrame(Frame, SeatFace::AuthSubmit, SeatMethod::Submit, 7U, Words) == 0U,
                   WriteSeatFrame(Frame, SeatFace::AuthControl, SeatMethod::Submit, 7U, Words, handle) == 0U);
    Delivery[Comp::EchoDeliveryHeaderBytes + Comp::EchoHeaderBytes + 24U] = 0x18U;
    ASTERIA_EXPECT(t, "writable attachment refused", !ReadSeatFrame(Delivery, delivered, Output),
                   std::memcmp(&Before, &Output, sizeof(Output)) == 0);
    ScreenEnrollment enrolled;
    ASTERIA_EXPECT(t, "drawing bound to current incarnation and epoch", enrolled.Enroll(9U, 1U),
                   enrolled.Accepts(9U, 1U), !enrolled.Accepts(8U, 1U), !enrolled.Accepts(9U, 0U),
                   !enrolled.Enroll(10U, 1U), enrolled.Enroll(10U, 2U), !enrolled.Accepts(9U, 1U));
    enrolled.Retire();
    ASTERIA_EXPECT(t, "retirement never reuses an epoch", !enrolled.Accepts(10U, 2U), !enrolled.Enroll(10U, 2U),
                   enrolled.Enroll(11U, 3U));
    return t.Finish();
}
