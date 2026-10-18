// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_USBDIAGNOSTIC_HPP_INCLUDED
#define ASTERIA_USER_INPUT_USBDIAGNOSTIC_HPP_INCLUDED

#include "User/Input/InputEvent.hpp"

namespace Asteria::User::Input
{
/// One enumeration or transfer fact, reported for screen diagnostics on
/// machines without a serial port. Field meanings per kind:
///
/// | Kind | Detail | Code | Value0 | Value1 | Data |
/// | --- | --- | --- | --- | --- | --- |
/// | Connect | protocol major (2, 3) | | PORTSC | | |
/// | Disconnect | | | PORTSC | | |
/// | ResetDone | Port Speed ID | UsbSpeed + 1, 0 unknown | PORTSC | | |
/// | ResetTimeout | | | PORTSC | | |
/// | SlotEnabled | slot ID | | | | |
/// | Addressed | slot ID | BSR (1 default, 0 address) | USB address | | |
/// | Command | command TRB type | completion code | | | |
/// | EndpointConfigured | endpoint address | DCI | interval field | max packet \| ESIT payload << 16 | |
/// | Transfer | endpoint address | completion code | bRequest (control) or 0 | residual | |
/// | Controller | | | USBSTS | | |
/// | EndpointZero | | | bMaxPacketSize0 | | |
/// | Device | bMaxPacketSize0 | bDeviceClass | VID \| PID << 16 | bcdUSB \| configurations << 16 | |
/// | Configuration | bConfigurationValue | DescriptorError | wTotalLength | interfaces \| bound << 8 | |
/// | Interface | bInterfaceNumber | bound (1) | class \| subclass << 8 \| protocol << 16 | endpoint \| max packet << 8 \| bInterval << 24 | |
/// | Request | bRequest | TransferStatus | interface or value | | |
/// | Polling | interface count | | | | |
/// | Report | bInterfaceNumber | ReportResult | length | | first eight bytes, little-endian |
/// | ReportError | bInterfaceNumber | TransferStatus | | | |
/// | Failed | UsbState | TransferStatus or UsbDiagnosticRefused | actual length | | |
/// | Suppressed | 1 for the whole controller, 0 for one port | | records dropped | | |
/// | Companion | | | PORTSC | | |
/// | Started | 1 interrupt bound, 0 polled | | root ports | | |
///
/// Port is the root port number for controller records; the USB core reports
/// its assigned address, which root-port drivers set to the port number.
enum class UsbDiagnosticKind : UInt8
{
    Connect = 1U,
    Disconnect,
    ResetDone,
    ResetTimeout,
    SlotEnabled,
    Addressed,
    Command,
    EndpointConfigured,
    Transfer,
    Controller,
    EndpointZero,
    Device,
    Configuration,
    Interface,
    Request,
    Polling,
    Report,
    ReportError,
    Failed,
    Suppressed, // produced by a driver that bounds the records it forwards
    Companion,  // an EHCI root port released a full- or low-speed device to its companion
    Started,    // a USB 1.1/2.0 host controller runs, with or without its interrupt
};
inline constexpr UInt8 LastUsbDiagnosticKind{static_cast<UInt8>(UsbDiagnosticKind::Started)};
/// Failed's Code when the controller refused the request three times.
inline constexpr UInt8 UsbDiagnosticRefused{0xFFU};

struct UsbDiagnostic
{
    UsbDiagnosticKind Kind;
    UInt8 Port;
    UInt8 Detail;
    UInt8 Code;
    UInt32 Value0;
    UInt32 Value1;
    Foundation::UInt64 Data;
};

/// Receives records synchronously from the controller and USB core that
/// produce them. Implementations copy the record into bounded storage and
/// return: they must not block, allocate, or call back into the producer.
/// Producers emit a bounded number of records per operation.
class IUsbDiagnosticSink
{
protected:
    ~IUsbDiagnosticSink() = default;

public:
    virtual void Record(const UsbDiagnostic& record) noexcept = 0;
};
} // namespace Asteria::User::Input

#endif
