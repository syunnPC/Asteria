// SPDX-License-Identifier: Apache-2.0
#include "User/Component/UsbProgressFrame.hpp"
#include "User/Pager/UsbProgress.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// The root service decodes an xHCI driver's diagnostic deliveries and prints
// them: decoding is failure-atomic, an accepted frame round-trips through the
// canonical writer, and every line is one bounded printable line.
namespace
{
namespace C = Asteria::User::Component;
namespace In = Asteria::User::Input;
namespace P = Asteria::User::Pager;
constexpr std::size_t DeliveryBytes{C::EchoDeliveryHeaderBytes + C::XhciProgressFrameBytes};
void Require(const bool good) noexcept
{
    if (!good)
    {
        std::abort();
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    std::uint8_t delivery[DeliveryBytes]{};
    if (size != 0U)
    {
        std::memcpy(delivery, data, size < DeliveryBytes ? size : DeliveryBytes);
    }
    // The first input byte past the delivery chooses a shorter length.
    const std::size_t bytes = size > DeliveryBytes ? DeliveryBytes - (data[DeliveryBytes] & 3U) : size;
    In::UsbDiagnostic record{.Kind = In::UsbDiagnosticKind::Failed,
                             .Port = 0xA5U,
                             .Detail = 0xA5U,
                             .Code = 0xA5U,
                             .Value0 = 0xA5A5U,
                             .Value1 = 0xA5A5U,
                             .Data = 0xA5A5U};
    std::uint8_t controller{0xA5U};
    const In::UsbDiagnostic before = record;
    if (!C::ReadXhciProgress(delivery, bytes, controller, record))
    {
        Require((controller == 0xA5U) && (std::memcmp(&record, &before, sizeof(record)) == 0));
        return 0;
    }
    Require(bytes == DeliveryBytes);
    std::uint8_t again[DeliveryBytes]{};
    Require(C::WriteXhciProgress(again + C::EchoDeliveryHeaderBytes, 1U, controller, record) ==
            C::XhciProgressFrameBytes);
    std::uint8_t controllerAgain{0U};
    In::UsbDiagnostic decoded{};
    Require(C::ReadXhciProgress(again, sizeof(again), controllerAgain, decoded) && (controllerAgain == controller) &&
            (decoded.Kind == record.Kind) && (decoded.Port == record.Port) && (decoded.Detail == record.Detail) &&
            (decoded.Code == record.Code) && (decoded.Value0 == record.Value0) && (decoded.Value1 == record.Value1) &&
            (decoded.Data == record.Data));
    char line[P::UsbProgressLineBytes]{};
    const std::size_t length = P::FormatUsbProgress(controller, record, line);
    Require((length >= 2U) && (length <= sizeof(line)) && (line[length - 1U] == '\n'));
    for (std::size_t index{0U}; index + 1U < length; ++index)
    {
        Require((line[index] >= ' ') && (line[index] <= '~'));
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr In::UsbDiagnostic records[]{
        {.Kind = In::UsbDiagnosticKind::Connect,
         .Port = 3U,
         .Detail = 2U,
         .Code = 0U,
         .Value0 = 0x603U,
         .Value1 = 0U,
         .Data = 0U},
        {.Kind = In::UsbDiagnosticKind::Report,
         .Port = 3U,
         .Detail = 0U,
         .Code = 0U,
         .Value0 = 8U,
         .Value1 = 0U,
         .Data = 0x0000'0000'00FD'0501ULL},
        {.Kind = In::UsbDiagnosticKind::Suppressed,
         .Port = 0U,
         .Detail = 2U,
         .Code = 0U,
         .Value0 = 9U,
         .Value1 = 0U,
         .Data = 0U},
    };
    for (const auto& record : records)
    {
        std::uint8_t delivery[DeliveryBytes]{};
        static_cast<void>(C::WriteXhciProgress(delivery + C::EchoDeliveryHeaderBytes, 1U, 0U, record));
        emit(delivery, sizeof(delivery));
    }
}
