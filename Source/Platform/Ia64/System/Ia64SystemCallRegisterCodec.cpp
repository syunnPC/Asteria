// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/System/Ia64SystemCallRegisterCodec.hpp"

namespace Asteria::Platform::Ia64::System
{
Ia64SystemCallRegisterDecodeError
Ia64SystemCallRegisterCodec::DecodeRequest(const Ia64SystemCallRegisterImage& image,
                                           Kernel::System::SystemCallRequest& request) noexcept
{
    if ((image.InputNatBits & ~Ia64SystemCallKnownInputNatBitsMask) != 0U)
    {
        return Ia64SystemCallRegisterDecodeError::InvalidNatMetadata;
    }
    if ((image.InputNatBits & Ia64SystemCallKnownInputNatBitsMask) != 0U)
    {
        return Ia64SystemCallRegisterDecodeError::InputContainsNat;
    }

    request = Kernel::System::SystemCallRequest{
        .Number = image.R15,
        .Arguments =
            {
                image.R32,
                image.R33,
                image.R34,
                image.R35,
                image.R36,
                image.R37,
            },
    };
    return Ia64SystemCallRegisterDecodeError::None;
}

void Ia64SystemCallRegisterCodec::ApplyResponse(const Kernel::System::SystemCallResponse& response,
                                                Ia64SystemCallRegisterImage& image) noexcept
{
    image.R8 = static_cast<Foundation::UInt64>(response.Error);
    image.R9 = response.Value0;
    image.R10 = response.Value1;
}
} // namespace Asteria::Platform::Ia64::System
