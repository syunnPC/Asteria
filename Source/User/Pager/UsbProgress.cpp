// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/UsbProgress.hpp"

#include "Inventory/ReportText.hpp"
#include "User/Component/UsbProgressFrame.hpp"
#include "User/Input/UsbDevice.hpp"

namespace Asteria::User::Pager
{
namespace
{
using Inventory::ReportText;
using Input::UsbDiagnosticKind;

// TransferStatus, in its enumerator order.
constexpr const char* StatusNames[]{"ok", "retryable", "stall", "error", "disconnected", "cancelled", "timed out"};
// UsbState, in its enumerator order.
constexpr const char* StateNames[]{"detached",
                                   "reset",
                                   "first-descriptor",
                                   "ep0-update",
                                   "set-address",
                                   "device-descriptor",
                                   "configuration-header",
                                   "configuration",
                                   "set-configuration",
                                   "set-protocol",
                                   "set-idle",
                                   "configure-endpoint",
                                   "polling",
                                   "failed",
                                   "hub-depth"};
// DescriptorError, in its enumerator order.
constexpr const char* DescriptorNames[]{"none",
                                        "null",
                                        "header",
                                        "truncated",
                                        "length",
                                        "interface-count",
                                        "duplicate-interface",
                                        "missing-default",
                                        "endpoint-count",
                                        "orphan",
                                        "endpoint",
                                        "duplicate-endpoint",
                                        "hid",
                                        "companion",
                                        "association"};
// ReportResult, in its enumerator order.
constexpr const char* ReportNames[]{"accepted", "short", "rollover", "rejected"};
// UsbSpeed + 1; zero is an undecodable Port Speed ID.
constexpr const char* SpeedNames[]{"unknown", "low", "full", "high", "super"};

template <Foundation::Size Count>
void Name(ReportText& text, const char* const (&names)[Count], const Foundation::UInt64 value) noexcept
{
    if (value < Count)
    {
        text.Text(names[value]);
        return;
    }
    text.Character('#');
    text.Decimal(value);
}

void Status(ReportText& text, const Foundation::UInt8 code) noexcept
{
    if (code == Input::UsbDiagnosticRefused)
    {
        text.Text("refused");
        return;
    }
    Name(text, StatusNames, code);
}

// xHCI 1.2 section 6.4.5, table 6-90: the completion codes a root device sees.
const char* CompletionName(const Foundation::UInt8 code) noexcept
{
    switch (code)
    {
    case 0U:
        return "timed out";
    case 1U:
        return "success";
    case 2U:
        return "data buffer error";
    case 3U:
        return "babble";
    case 4U:
        return "transaction error";
    case 5U:
        return "trb error";
    case 6U:
        return "stall";
    case 7U:
        return "resource error";
    case 8U:
        return "bandwidth error";
    case 9U:
        return "no slots";
    case 11U:
        return "slot not enabled";
    case 12U:
        return "endpoint not enabled";
    case 13U:
        return "short packet";
    case 17U:
        return "parameter error";
    case 19U:
        return "context state error";
    case 21U:
        return "event ring full";
    case 22U:
        return "incompatible device";
    case 26U:
        return "stopped";
    case 36U:
        return "split transaction error";
    default:
        return nullptr;
    }
}

void Completion(ReportText& text, const Foundation::UInt8 code) noexcept
{
    text.Text(" code=");
    text.Decimal(code);
    const char* const name = CompletionName(code);
    if (name != nullptr)
    {
        text.Text(" (");
        text.Text(name);
        text.Character(')');
    }
}

// Section 6.4.3: the command TRB types this driver issues.
const char* CommandName(const Foundation::UInt8 type) noexcept
{
    switch (type)
    {
    case 9U:
        return "enable slot";
    case 10U:
        return "disable slot";
    case 11U:
        return "address device";
    case 12U:
        return "configure endpoint";
    case 13U:
        return "evaluate context";
    case 14U:
        return "reset endpoint";
    case 15U:
        return "stop endpoint";
    case 16U:
        return "set dequeue";
    default:
        return nullptr;
    }
}

void Hex32(ReportText& text, const char* const label, const Foundation::UInt32 value) noexcept
{
    text.Text(label);
    text.Text("0x");
    text.Hex(value, 8U);
}

void Endpoint(ReportText& text, const Foundation::UInt8 address) noexcept
{
    text.Text("endpoint 0x");
    text.Hex(address, 2U);
}

void Request(ReportText& text, const Input::UsbDiagnostic& record) noexcept
{
    switch (record.Detail)
    {
    case 9U:
        text.Text("set configuration ");
        text.Decimal(record.Value0);
        break;
    case 10U:
        text.Text("interface ");
        text.Decimal(record.Value0);
        text.Text(" set idle");
        break;
    case 11U:
        text.Text("interface ");
        text.Decimal(record.Value0);
        text.Text(" set protocol boot");
        break;
    default:
        text.Text("request 0x");
        text.Hex(record.Detail, 2U);
        text.Character(' ');
        text.Decimal(record.Value0);
        break;
    }
    text.Text(": ");
    Status(text, record.Code);
    if ((record.Detail == 10U) && (record.Code == static_cast<Foundation::UInt8>(Input::TransferStatus::Stall)))
    {
        text.Text(" (optional, continuing)");
    }
    else if ((record.Detail == 11U) && (record.Code == static_cast<Foundation::UInt8>(Input::TransferStatus::Stall)))
    {
        text.Text(" (interface dropped)");
    }
}

void Device(ReportText& text, const Input::UsbDiagnostic& record) noexcept
{
    text.Text("device ");
    text.Hex(record.Value0 & 0xFFFFU, 4U);
    text.Character(':');
    text.Hex(record.Value0 >> 16U, 4U);
    text.Text(" usb ");
    text.Hex((record.Value1 >> 8U) & 0xFFU, 1U);
    text.Character('.');
    text.Hex(record.Value1 & 0xFFU, 2U);
    text.Text(" class ");
    text.Hex(record.Code, 2U);
    text.Text(" mps0 ");
    text.Decimal(record.Detail);
    text.Text(" configurations ");
    text.Decimal(record.Value1 >> 16U);
}

void Configuration(ReportText& text, const Input::UsbDiagnostic& record) noexcept
{
    text.Text("configuration ");
    text.Decimal(record.Detail);
    text.Text(" length ");
    text.Decimal(record.Value0);
    text.Text(" interfaces ");
    text.Decimal(record.Value1 & 0xFFU);
    if (record.Code != 0U)
    {
        text.Text(" rejected: ");
        Name(text, DescriptorNames, record.Code);
        return;
    }
    text.Text(" bound ");
    text.Decimal(record.Value1 >> 8U);
}

void Interface(ReportText& text, const Input::UsbDiagnostic& record) noexcept
{
    text.Text("interface ");
    text.Decimal(record.Detail);
    text.Text(" class ");
    text.Hex(record.Value0 & 0xFFU, 2U);
    text.Character('/');
    text.Hex((record.Value0 >> 8U) & 0xFFU, 2U);
    text.Character('/');
    text.Hex((record.Value0 >> 16U) & 0xFFU, 2U);
    text.Character(' ');
    Endpoint(text, static_cast<Foundation::UInt8>(record.Value1 & 0xFFU));
    text.Text(" mps ");
    text.Decimal((record.Value1 >> 8U) & 0xFFFFU);
    text.Text(" interval ");
    text.Decimal(record.Value1 >> 24U);
    text.Text(record.Code != 0U ? " bound" : " skipped");
}

void Report(ReportText& text, const Input::UsbDiagnostic& record) noexcept
{
    text.Text("interface ");
    text.Decimal(record.Detail);
    text.Text(" report length ");
    text.Decimal(record.Value0);
    text.Character(':');
    for (Foundation::Size byte{0U}; (byte < 8U) && (byte < record.Value0); ++byte)
    {
        text.Character(' ');
        text.Hex((record.Data >> (byte * 8U)) & 0xFFU, 2U);
    }
    text.Character(' ');
    Name(text, ReportNames, record.Code);
}

void Body(ReportText& text, const Input::UsbDiagnostic& record) noexcept
{
    switch (record.Kind)
    {
    case UsbDiagnosticKind::Connect:
        text.Text(record.Detail == 3U ? "connect usb3 " : (record.Detail == 1U ? "connect usb1 " : "connect usb2 "));
        Hex32(text, "portsc=", record.Value0);
        return;
    case UsbDiagnosticKind::Disconnect:
        Hex32(text, "disconnect portsc=", record.Value0);
        return;
    case UsbDiagnosticKind::ResetDone:
        text.Text("reset done speed=");
        Name(text, SpeedNames, record.Code);
        text.Text(" psiv=");
        text.Decimal(record.Detail);
        Hex32(text, " portsc=", record.Value0);
        return;
    case UsbDiagnosticKind::ResetTimeout:
        Hex32(text, "reset timed out portsc=", record.Value0);
        return;
    case UsbDiagnosticKind::SlotEnabled:
        text.Text("slot ");
        text.Decimal(record.Detail);
        text.Text(" enabled");
        return;
    case UsbDiagnosticKind::Addressed:
        text.Text("slot ");
        text.Decimal(record.Detail);
        text.Text(record.Code != 0U ? " default address (bsr=1) usb address " : " addressed, usb address ");
        text.Decimal(record.Value0);
        return;
    case UsbDiagnosticKind::Command:
    {
        text.Text("command ");
        const char* const name = CommandName(record.Detail);
        if (name != nullptr)
        {
            text.Text(name);
        }
        else
        {
            text.Text("type ");
            text.Decimal(record.Detail);
        }
        text.Text(" failed");
        Completion(text, record.Code);
        return;
    }
    case UsbDiagnosticKind::EndpointConfigured:
        Endpoint(text, record.Detail);
        text.Text(" configured dci ");
        text.Decimal(record.Code);
        text.Text(" interval ");
        text.Decimal(record.Value0);
        text.Text(" mps ");
        text.Decimal(record.Value1 & 0xFFFFU);
        text.Text(" esit ");
        text.Decimal(record.Value1 >> 16U);
        return;
    case UsbDiagnosticKind::Transfer:
        if (record.Detail == 0U)
        {
            text.Text("ep0 request 0x");
            text.Hex(record.Value0 & 0xFFU, 2U);
        }
        else
        {
            Endpoint(text, record.Detail);
        }
        text.Text(" transfer");
        Completion(text, record.Code);
        text.Text(" residual ");
        text.Decimal(record.Value1);
        return;
    case UsbDiagnosticKind::Controller:
        Hex32(text, "controller error usbsts=", record.Value0);
        return;
    case UsbDiagnosticKind::EndpointZero:
        text.Text("ep0 max packet ");
        text.Decimal(record.Value0);
        return;
    case UsbDiagnosticKind::Device:
        Device(text, record);
        return;
    case UsbDiagnosticKind::Configuration:
        Configuration(text, record);
        return;
    case UsbDiagnosticKind::Interface:
        Interface(text, record);
        return;
    case UsbDiagnosticKind::Request:
        Request(text, record);
        return;
    case UsbDiagnosticKind::Polling:
        text.Text("polling ");
        text.Decimal(record.Detail);
        text.Text(record.Detail == 1U ? " interface" : " interfaces");
        return;
    case UsbDiagnosticKind::Report:
        Report(text, record);
        return;
    case UsbDiagnosticKind::ReportError:
        text.Text("interface ");
        text.Decimal(record.Detail);
        text.Text(" report ");
        Status(text, record.Code);
        return;
    case UsbDiagnosticKind::Failed:
        if ((record.Code == static_cast<Foundation::UInt8>(Input::TransferStatus::Success)) &&
            ((record.Detail == static_cast<Foundation::UInt8>(Input::UsbState::DeviceDescriptor)) ||
             (record.Detail == static_cast<Foundation::UInt8>(Input::UsbState::Configuration))))
        {
            // Hubs (or an inconsistent bMaxPacketSize0), and devices without
            // a boot keyboard or mouse interface, stay unconfigured.
            text.Text(record.Detail == static_cast<Foundation::UInt8>(Input::UsbState::Configuration)
                          ? "no boot keyboard or mouse interface, left idle"
                          : "hub or ep0 size mismatch, left idle");
            return;
        }
        text.Text("enumeration failed in ");
        Name(text, StateNames, record.Detail);
        text.Text(": ");
        Status(text, record.Code);
        text.Text(" length ");
        text.Decimal(record.Value0);
        return;
    case UsbDiagnosticKind::Suppressed:
        if (record.Detail == 2U)
        {
            text.Decimal(record.Value0);
            text.Text(" records lost, driver queue full");
            return;
        }
        text.Text(record.Detail != 0U ? "further records suppressed" : "further records for this device suppressed");
        return;
    case UsbDiagnosticKind::Companion:
        Hex32(text, "full/low speed, released to companion portsc=", record.Value0);
        return;
    case UsbDiagnosticKind::Started:
        text.Text("started, ");
        text.Decimal(record.Value0);
        text.Text(record.Detail != 0U ? " ports, interrupt" : " ports, polled");
        return;
    }
    text.Text("record ");
    text.Decimal(static_cast<Foundation::UInt8>(record.Kind));
}
} // namespace

Foundation::Size FormatUsbProgress(const Foundation::UInt8 controller, const Input::UsbDiagnostic& record,
                                   char (&line)[UsbProgressLineBytes]) noexcept
{
    ReportText text{line, sizeof(line) - 1U};
    constexpr const char* Names[]{"# usb: xhci", "# usb: ehci", "# usb: ohci", "# usb: uhci"};
    text.Text(Names[(controller >> 4U) & 3U]);
    text.Decimal(controller & 15U);
    if (record.Port != 0U)
    {
        text.Text(" port ");
        text.Decimal(record.Port);
    }
    text.Character(' ');
    Body(text, record);
    // The capacity leaves room for the newline even after truncation.
    Foundation::Size bytes = text.Bytes();
    line[bytes++] = '\n';
    return bytes;
}

bool PrintUsbProgress(ScenarioContext& context, const Foundation::UInt64 bytes) noexcept
{
    Foundation::UInt8 controller{0U};
    Input::UsbDiagnostic record{};
    if (!Component::ReadXhciProgress(context.Delivery, static_cast<Foundation::Size>(bytes), controller, record))
    {
        return false;
    }
    char line[UsbProgressLineBytes]{};
    WriteBetweenLines(context, line, FormatUsbProgress(controller, record, line));
    return true;
}
} // namespace Asteria::User::Pager
