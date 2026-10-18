// SPDX-License-Identifier: Apache-2.0
#include "User/Pager/UsbProgress.hpp"

#include <cstdio>
#include <string>

#include "Support/TestContext.hpp"
#include "User/Component/UsbProgressFrame.hpp"
#include "User/Input/UsbDevice.hpp"

namespace
{
namespace C = Asteria::User::Component;
namespace In = Asteria::User::Input;
namespace P = Asteria::User::Pager;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::TestSupport::TestContext;
using K = In::UsbDiagnosticKind;

struct Golden
{
    In::UsbDiagnostic Record;
    const char* Line;
    UInt8 Controller{0U};
};

// Synthetic diagnostic records cover each USB progress and failure format.
const Golden Goldens[]{
    {{K::Connect, 3U, 2U, 0U, 0x603U, 0U, 0U}, "# usb: xhci0 port 3 connect usb2 portsc=0x00000603\n"},
    {{K::Connect, 2U, 3U, 0U, 0x1203U, 0U, 0U}, "# usb: xhci0 port 2 connect usb3 portsc=0x00001203\n"},
    {{K::Disconnect, 3U, 2U, 0U, 0x20000U, 0U, 0U}, "# usb: xhci0 port 3 disconnect portsc=0x00020000\n"},
    {{K::ResetDone, 3U, 1U, 2U, 0x603U, 0U, 0U},
     "# usb: xhci0 port 3 reset done speed=full psiv=1 portsc=0x00000603\n"},
    {{K::ResetDone, 3U, 0U, 0U, 0x203U, 0U, 0U},
     "# usb: xhci0 port 3 reset done speed=unknown psiv=0 portsc=0x00000203\n"},
    {{K::ResetTimeout, 3U, 0U, 0U, 0x211U, 0U, 0U}, "# usb: xhci0 port 3 reset timed out portsc=0x00000211\n"},
    {{K::SlotEnabled, 3U, 1U, 0U, 0U, 0U, 0U}, "# usb: xhci0 port 3 slot 1 enabled\n"},
    {{K::Addressed, 3U, 1U, 1U, 0U, 0U, 0U}, "# usb: xhci0 port 3 slot 1 default address (bsr=1) usb address 0\n"},
    {{K::Addressed, 3U, 1U, 0U, 1U, 0U, 0U}, "# usb: xhci0 port 3 slot 1 addressed, usb address 1\n"},
    {{K::Command, 3U, 11U, 4U, 0U, 0U, 0U},
     "# usb: xhci0 port 3 command address device failed code=4 (transaction error)\n"},
    {{K::Command, 3U, 9U, 0U, 0U, 0U, 0U}, "# usb: xhci0 port 3 command enable slot failed code=0 (timed out)\n"},
    {{K::Command, 3U, 40U, 200U, 0U, 0U, 0U}, "# usb: xhci0 port 3 command type 40 failed code=200\n"},
    {{K::EndpointConfigured, 3U, 0x81U, 3U, 3U, 8U | (8U << 16U), 0U},
     "# usb: xhci0 port 3 endpoint 0x81 configured dci 3 interval 3 mps 8 esit 8\n"},
    {{K::Transfer, 3U, 0U, 6U, 10U, 0U, 0U},
     "# usb: xhci0 port 3 ep0 request 0x0a transfer code=6 (stall) residual 0\n"},
    {{K::Transfer, 3U, 0x81U, 3U, 0U, 8U, 0U},
     "# usb: xhci0 port 3 endpoint 0x81 transfer code=3 (babble) residual 8\n"},
    {{K::Controller, 0U, 0U, 0U, 0x1005U, 0U, 0U}, "# usb: xhci0 controller error usbsts=0x00001005\n"},
    {{K::EndpointZero, 3U, 0U, 0U, 8U, 0U, 0U}, "# usb: xhci0 port 3 ep0 max packet 8\n"},
    {{K::Device, 3U, 8U, 0U, 0x5678'1234U, 0x0200U | (1U << 16U), 0U},
     "# usb: xhci0 port 3 device 1234:5678 usb 2.00 class 00 mps0 8 configurations 1\n"},
    {{K::Configuration, 3U, 1U, 0U, 84U, 3U | (2U << 8U), 0U},
     "# usb: xhci0 port 3 configuration 1 length 84 interfaces 3 bound 2\n"},
    {{K::Configuration, 3U, 1U, 12U, 84U, 3U, 0U},
     "# usb: xhci0 port 3 configuration 1 length 84 interfaces 3 rejected: hid\n"},
    {{K::Interface, 3U, 0U, 1U, 3U | (1U << 8U) | (2U << 16U), 0x81U | (8U << 8U) | (1U << 24U), 0U},
     "# usb: xhci0 port 3 interface 0 class 03/01/02 endpoint 0x81 mps 8 interval 1 bound\n"},
    {{K::Interface, 3U, 2U, 0U, 3U, 0x83U | (32U << 8U) | (1U << 24U), 0U},
     "# usb: xhci0 port 3 interface 2 class 03/00/00 endpoint 0x83 mps 32 interval 1 skipped\n"},
    {{K::Request, 3U, 9U, 0U, 1U, 0U, 0U}, "# usb: xhci0 port 3 set configuration 1: ok\n"},
    {{K::Request, 3U, 11U, 0U, 0U, 0U, 0U}, "# usb: xhci0 port 3 interface 0 set protocol boot: ok\n"},
    {{K::Request, 3U, 11U, 2U, 1U, 0U, 0U},
     "# usb: xhci0 port 3 interface 1 set protocol boot: stall (interface dropped)\n"},
    {{K::Request, 3U, 10U, 2U, 0U, 0U, 0U}, "# usb: xhci0 port 3 interface 0 set idle: stall (optional, continuing)\n"},
    {{K::Request, 3U, 10U, 6U, 1U, 0U, 0U}, "# usb: xhci0 port 3 interface 1 set idle: timed out\n"},
    {{K::Polling, 3U, 2U, 0U, 0U, 0U, 0U}, "# usb: xhci0 port 3 polling 2 interfaces\n"},
    {{K::Polling, 3U, 1U, 0U, 0U, 0U, 0U}, "# usb: xhci0 port 3 polling 1 interface\n"},
    {{K::Report, 3U, 0U, 0U, 8U, 0U, 0x4342'4140'00FD'0501ULL},
     "# usb: xhci0 port 3 interface 0 report length 8: 01 05 fd 00 40 41 42 43 accepted\n"},
    {{K::Report, 3U, 1U, 3U, 2U, 0U, 0x0102U}, "# usb: xhci0 port 3 interface 1 report length 2: 02 01 rejected\n"},
    {{K::ReportError, 3U, 0U, 1U, 0U, 0U, 0U}, "# usb: xhci0 port 3 interface 0 report retryable\n"},
    {{K::Failed, 3U, 10U, 3U, 0U, 0U, 0U}, "# usb: xhci0 port 3 enumeration failed in set-idle: error length 0\n"},
    {{K::Failed, 3U, 1U, In::UsbDiagnosticRefused, 0U, 0U, 0U},
     "# usb: xhci0 port 3 enumeration failed in reset: refused length 0\n"},
    {{K::Failed, 3U, 7U, 0U, 44U, 0U, 0U}, "# usb: xhci0 port 3 no boot keyboard or mouse interface, left idle\n"},
    {{K::Failed, 3U, 5U, 0U, 18U, 0U, 0U}, "# usb: xhci0 port 3 hub or ep0 size mismatch, left idle\n"},
    {{K::Suppressed, 3U, 0U, 0U, 0U, 0U, 0U}, "# usb: xhci0 port 3 further records for this device suppressed\n"},
    {{K::Suppressed, 0U, 1U, 0U, 0U, 0U, 0U}, "# usb: xhci0 further records suppressed\n"},
    {{K::Suppressed, 0U, 2U, 0U, 17U, 0U, 0U}, "# usb: xhci0 17 records lost, driver queue full\n"},
    // The controller byte names the kind and index of a USB 1.1/2.0 driver's
    // controller (UsbProgressFrame.hpp).
    {{K::Connect, 4U, 2U, 0U, 0x603U, 0U, 0U},
     "# usb: xhci15 port 4 connect usb2 portsc=0x00000603\n",
     C::UsbControllerByte(C::UsbController::Xhci, 15U)},
    {{K::Connect, 1U, 1U, 0U, 0x1803U, 0U, 0U},
     "# usb: uhci3 port 1 connect usb1 portsc=0x00001803\n",
     C::UsbControllerByte(C::UsbController::Uhci, 3U)},
    {{K::Disconnect, 2U, 1U, 0U, 0x0002'0000U, 0U, 0U},
     "# usb: ohci1 port 2 disconnect portsc=0x00020000\n",
     C::UsbControllerByte(C::UsbController::Ohci, 1U)},
    {{K::Companion, 2U, 0U, 0U, 0x1403U, 0U, 0U},
     "# usb: ehci0 port 2 full/low speed, released to companion portsc=0x00001403\n",
     C::UsbControllerByte(C::UsbController::Ehci, 0U)},
    {{K::Started, 0U, 0U, 0U, 6U, 0U, 0U},
     "# usb: ehci2 started, 6 ports, polled\n",
     C::UsbControllerByte(C::UsbController::Ehci, 2U)},
    {{K::Started, 0U, 1U, 0U, 2U, 0U, 0U},
     "# usb: uhci0 started, 2 ports, interrupt\n",
     C::UsbControllerByte(C::UsbController::Uhci, 0U)},
};

void Formats(TestContext& context) noexcept
{
    for (const Golden& golden : Goldens)
    {
        char line[P::UsbProgressLineBytes]{};
        const Size bytes = P::FormatUsbProgress(golden.Controller, golden.Record, line);
        const std::string text{line, bytes};
        if (text != golden.Line)
        {
            static_cast<void>(std::fprintf(stderr, "got:      %swanted:   %s", text.c_str(), golden.Line));
            ASTERIA_FAIL(context, "progress line matches its golden");
        }
    }
}

// Every field at its extreme still fits the line with its newline, and the
// frame carries each field unchanged.
void Bounds(TestContext& context) noexcept
{
    for (UInt8 kind{1U}; kind <= In::LastUsbDiagnosticKind; ++kind)
    {
        const In::UsbDiagnostic record{.Kind = static_cast<K>(kind),
                                       .Port = 255U,
                                       .Detail = 255U,
                                       .Code = 255U,
                                       .Value0 = 0xFFFF'FFFFU,
                                       .Value1 = 0xFFFF'FFFFU,
                                       .Data = ~0ULL};
        char line[P::UsbProgressLineBytes]{};
        const Size bytes = P::FormatUsbProgress(255U, record, line);
        bool printable{true};
        for (Size index{0U}; index + 1U < bytes; ++index)
        {
            printable = printable && (line[index] >= ' ') && (line[index] <= '~');
        }
        ASTERIA_EXPECT(context, "extreme fields fit one printable line", bytes <= sizeof(line),
                       line[bytes - 1U] == '\n', printable);
        UInt8 delivery[C::EchoDeliveryHeaderBytes + C::XhciProgressFrameBytes]{};
        ASTERIA_EXPECT(context, "fixed frame size",
                       C::WriteXhciProgress(delivery + C::EchoDeliveryHeaderBytes, 7U, 254U, record) ==
                           C::XhciProgressFrameBytes);
        UInt8 controller{0U};
        In::UsbDiagnostic decoded{};
        ASTERIA_EXPECT(context, "frame round trip",
                       C::ReadXhciProgress(delivery, sizeof(delivery), controller, decoded), controller == 254U,
                       decoded.Kind == record.Kind, decoded.Port == 255U, decoded.Detail == 255U, decoded.Code == 255U,
                       decoded.Value0 == record.Value0, decoded.Value1 == record.Value1, decoded.Data == record.Data);
        delivery[C::EchoDeliveryHeaderBytes + 32U] ^= 1U;
        ASTERIA_EXPECT(context, "other methods are not progress",
                       !C::ReadXhciProgress(delivery, sizeof(delivery), controller, decoded));
    }
}
} // namespace

int main()
{
    TestContext context{"USB progress lines"};
    Formats(context);
    Bounds(context);
    return context.Finish();
}
