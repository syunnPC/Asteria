// SPDX-License-Identifier: Apache-2.0

#include "Acpi/Aml/Report.hpp"

#include "Abi/I8042.hpp"
#include "Acpi/Aml/Ps2Evidence.hpp"

namespace Asteria::Acpi::Aml
{
Report::Report(Library& library, Facts& facts, ReportStorage& storage, IReportSink& sink) noexcept
    : m_library{library}, m_facts{facts}, m_storage{storage}, m_sink{sink}
{
}
void Report::Text(const char* const text) noexcept
{
    Size length{0U};
    while (text[length] != '\0')
    {
        ++length;
    }
    if (m_good)
    {
        m_good = m_sink.Write(text, length);
    }
}
void Report::Number(UInt64 number, const bool hex) noexcept
{
    char text[32U]{};
    Size cursor{sizeof(text) - 1U};
    const UInt64 base{hex ? 16U : 10U};
    constexpr char digits[]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    do
    {
        text[--cursor] = digits[number % base];
        number /= base;
    } while (number != 0U);
    if (hex)
    {
        text[--cursor] = 'x';
        text[--cursor] = '0';
    }
    Text(text + cursor);
}
void Report::Quoted(const Bytes bytes) noexcept
{
    Text("\"");
    constexpr char digits[]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    for (Size i{0U}; i < bytes.Length && m_good; ++i)
    {
        const UInt8 byte{bytes.Data[i]};
        char text[5U]{};
        if (byte >= 0x20U && byte <= 0x7EU && byte != '\\' && byte != '"')
        {
            text[0U] = static_cast<char>(byte);
        }
        else
        {
            text[0U] = '\\';
            text[1U] = 'x';
            text[2U] = digits[byte >> 4U];
            text[3U] = digits[byte & 15U];
        }
        Text(text);
    }
    Text("\"");
}
void Report::Path(const Id node) noexcept
{
    if (m_library.Path(node, m_storage.Path, sizeof(m_storage.Path)))
    {
        Text(m_storage.Path);
    }
    else
    {
        Text("<path-capacity>");
    }
}
void Report::Unknown(const Reason reason) noexcept
{
    Text("Unknown(");
    Text(ReasonText(reason));
    Text(")");
}
void Report::Value(const EvaluationResult value) noexcept
{
    if (!value.Known())
    {
        Unknown(value.Error);
        return;
    }
    const Object& object{m_library.GetObject(value.Value)};
    switch (object.Kind)
    {
    case ObjectKind::Integer:
        Number(object.Integer, true);
        break;
    case ObjectKind::String:
        Quoted(m_library.Data(value.Value));
        break;
    case ObjectKind::Buffer:
        Text("Buffer(");
        Number(object.Count);
        Text(")");
        break;
    case ObjectKind::Package:
        Text("Package(");
        Number(object.Count);
        Text(")");
        break;
    case ObjectKind::Reference:
        if (object.Reference == ReferenceKind::Node)
        {
            Path(object.First);
        }
        else
        {
            Text("Reference");
        }
        break;
    case ObjectKind::Unknown:
        Unknown(object.Error);
        break;
    }
}
void Report::Ids(const Identifiers& ids) noexcept
{
    if (ids.Error != Reason::None)
    {
        Unknown(ids.Error);
        return;
    }
    Text("[");
    for (Size i{0U}; i < ids.Count; ++i)
    {
        if (i != 0U)
        {
            Text(", ");
        }
        if (ids.Items[i].Eisa[0U] != '\0')
        {
            Text(ids.Items[i].Eisa);
        }
        else
        {
            Quoted(m_library.Data(ids.Items[i].Value));
        }
    }
    Text("]");
}
void Report::ResourceLine(const Resource& resource) noexcept
{
    Text("  resource ");
    Text(ResourceText(resource.Kind));
    switch (resource.Kind)
    {
    case ResourceKind::Irq:
    case ResourceKind::Dma:
        Text(" mask=");
        Number(resource.Interrupt, true);
        break;
    case ResourceKind::ExtendedIrq:
        Text(" interrupts=[");
        for (Size i{0U}; i < resource.Length; ++i)
        {
            if (i != 0U)
            {
                Text(",");
            }
            UInt32 interrupt{0U};
            for (Size byte{0U}; byte < 4U; ++byte)
            {
                interrupt |= UInt32{resource.Raw.Data[5U + i * 4U + byte]} << (byte * 8U);
            }
            Number(interrupt);
        }
        Text("]");
        break;
    case ResourceKind::Io:
    case ResourceKind::FixedIo:
    case ResourceKind::Memory24:
    case ResourceKind::Memory32:
    case ResourceKind::FixedMemory32:
    case ResourceKind::WordAddress:
    case ResourceKind::DWordAddress:
    case ResourceKind::QWordAddress:
    case ResourceKind::ExtendedAddress:
        Text(" min=");
        Number(resource.Minimum, true);
        Text(" max=");
        Number(resource.Maximum, true);
        Text(" length=");
        Number(resource.Length, true);
        Text(" alignment=");
        Number(resource.Alignment, true);
        Text(" translation=");
        Number(resource.Translation, true);
        Text(" space=");
        Number(resource.Space);
        Text(" attributes=");
        Number(resource.Attributes, true);
        break;
    case ResourceKind::Vendor:
        Text(" bytes=");
        Number(resource.Raw.Length);
        break;
    case ResourceKind::End:
    case ResourceKind::Gpio:
    case ResourceKind::I2c:
    case ResourceKind::Spi:
    case ResourceKind::Uart:
    case ResourceKind::Pin:
    case ResourceKind::Dependent:
    case ResourceKind::Skipped:
        break;
    }
    if (resource.Kind == ResourceKind::Irq || resource.Kind == ResourceKind::ExtendedIrq)
    {
        const bool extended = resource.Kind == ResourceKind::ExtendedIrq;
        const bool edge = (resource.Flags & (extended ? 2U : 1U)) != 0U;
        const bool low = (resource.Flags & (extended ? 4U : 8U)) != 0U;
        Text(edge ? " mode=edge" : " mode=level");
        Text(low ? " polarity=low" : " polarity=high");
    }
    Text(" flags=");
    Number(resource.Flags, true);
    if (resource.Source.Data != nullptr)
    {
        Text(" source=");
        Quoted(resource.Source);
        Text(" source-index=");
        Number(resource.SourceIndex);
    }
    Text("\n");
}
void Report::Device(const DeviceFacts& device) noexcept
{
    Text("device ");
    Path(device.Node);
    Text("\n  _HID=");
    Ids(device.Hid);
    Text(" _CID=");
    Ids(device.Cid);
    Text(" _UID=");
    Value(device.Uid);
    Text(" _ADR=");
    Value(device.Adr);
    Text("\n  _STA=");
    switch (device.Sta)
    {
    case Presence::Present:
        Text("present");
        break;
    case Presence::Absent:
        Text("absent");
        break;
    case Presence::Unknown:
        Unknown(device.StaError);
        break;
    }
    Text(" keyboard=");
    Text(device.Keyboard ? "yes" : "no");
    Text(" mouse=");
    Text(device.Mouse ? "yes" : "no");
    Text(" pci-root=");
    Text(device.PciRoot ? "yes" : "no");
    Text("\n  _CRS=");
    if (device.Crs.Error != Reason::None)
    {
        Unknown(device.Crs.Error);
        Text("\n");
    }
    else
    {
        Number(device.Crs.Count);
        Text(" descriptors\n");
        for (Size i{0U}; i < device.Crs.Count; ++i)
        {
            ResourceLine(device.Crs.Items[i]);
        }
    }
    if (device.PciRoot)
    {
        Text("  _PRT=");
        if (device.PrtError != Reason::None)
        {
            Unknown(device.PrtError);
            Text("\n");
            return;
        }
        Number(device.PrtCount);
        Text(" entries\n");
        for (Size i{0U}; i < device.PrtCount; ++i)
        {
            const PrtEntry& entry{device.Prt[i]};
            Text("  route address=");
            Number(entry.Address, true);
            Text(" pin=");
            Number(entry.Pin);
            Text(" source=");
            if (entry.Direct)
            {
                Text("GSI");
            }
            else if (entry.SourceNode != NoId)
            {
                Path(entry.SourceNode);
            }
            else
            {
                Quoted(m_library.Data(entry.Source));
            }
            Text(" index=");
            Number(entry.Index);
            Text("\n");
        }
    }
}

namespace
{
// The first qualified keyboard's IRQ 1 and mouse's IRQ 12 signaling, as the
// identifier is the first keyboard's (Abi/I8042.hpp interrupt evidence bits).
void RecordInterrupt(ReportSummary& summary, const Ps2Declaration& declaration) noexcept
{
    const UInt32 declared =
        static_cast<UInt32>(declaration.Mouse ? Abi::I8042MouseIrqDeclared : Abi::I8042KeyboardIrqDeclared);
    if ((summary.I8042Flags & declared) != 0U)
    {
        return;
    }
    summary.I8042Flags |= declared;
    if (declaration.InterruptLevel)
    {
        summary.I8042Flags |=
            static_cast<UInt32>(declaration.Mouse ? Abi::I8042MouseIrqLevel : Abi::I8042KeyboardIrqLevel);
    }
    if (declaration.InterruptActiveLow)
    {
        summary.I8042Flags |=
            static_cast<UInt32>(declaration.Mouse ? Abi::I8042MouseIrqActiveLow : Abi::I8042KeyboardIrqActiveLow);
    }
}
} // namespace

bool Report::Print(Library& library, Facts& facts, ReportStorage& storage, IReportSink& sink) noexcept
{
    storage.Summary = {};
    Report report{library, facts, storage, sink};
    report.Text("Asteria AML facts v1\n_PIC(1)=");
    const Reason pic{facts.Prepare()};
    if (pic == Reason::None)
    {
        report.Text("known");
    }
    else
    {
        report.Unknown(pic);
    }
    report.Text("\n");
    for (Id node{facts.NextDevice()}; node != NoId; node = facts.NextDevice(node))
    {
        facts.ReadDevice(node, storage.Device);
        report.Device(storage.Device);
        constexpr Ps2DeviceKind kinds[]{Ps2DeviceKind::Keyboard, Ps2DeviceKind::Mouse};
        for (const Ps2DeviceKind kind : kinds)
        {
            const Ps2Declaration declaration = Ps2Evidence::Derive(library, storage.Device, kind);
            if (declaration.Candidate)
            {
                report.Text("  i8042 ");
                report.Text(kind == Ps2DeviceKind::Mouse ? "mouse " : "keyboard ");
                report.Text(Ps2Evidence::ConditionText(declaration.Condition));
                if (declaration.StaUnknown)
                {
                    report.Text(" STA-UNKNOWN");
                }
                report.Text("\n");
            }
            if (declaration.Condition == Ps2Condition::Qualified)
            {
                storage.Summary.I8042Flags |= declaration.Mouse ? 4U : 2U;
                storage.Summary.I8042Flags |= declaration.StaUnknown ? 8U : 0U;
                if (!declaration.Mouse && storage.Summary.I8042KeyboardId == 0U)
                {
                    storage.Summary.I8042KeyboardId = declaration.Identifier;
                }
                RecordInterrupt(storage.Summary, declaration);
            }
        }
        ++storage.Summary.Devices;
        if (storage.Device.Keyboard || storage.Device.Mouse)
        {
            ++storage.Summary.Ps2Candidates;
        }
        storage.Summary.PrtEntries += storage.Device.PrtCount;
    }
    const SleepFacts sleep{facts.Sleep()};
    storage.Summary.Sleep = sleep;
    report.Text("_S5=");
    if (sleep.Error == Reason::None)
    {
        report.Number(sleep.TypeA);
        report.Text(",");
        report.Number(sleep.TypeB);
    }
    else
    {
        report.Unknown(sleep.Error);
    }
    report.Text("\n");
    for (Size i{0U}; i < library.OsiQueryCount(); ++i)
    {
        report.Text("_OSI query=");
        report.Quoted(library.OsiQuery(i));
        report.Text("\n");
    }
    for (Size i{0U}; i < library.EffectCount(); ++i)
    {
        const Effect& effect{*library.GetEffect(i)};
        report.Text("effect opcode=");
        report.Number(effect.Opcode, true);
        report.Text(" subject=");
        report.Value({effect.Subject, Reason::None, 0U});
        report.Text(" value=");
        report.Value({effect.Value, Reason::None, 0U});
        report.Text("\n");
    }
    for (Size i{0U}; i < library.DiagnosticCount(); ++i)
    {
        const Diagnostic& diagnostic{*library.GetDiagnostic(i)};
        report.Text("diagnostic ");
        report.Text(ReasonText(diagnostic.Code));
        report.Text(" table=");
        report.Number(diagnostic.Table);
        report.Text(" offset=");
        report.Number(diagnostic.Offset, true);
        report.Text(" opcode=");
        report.Number(diagnostic.Opcode, true);
        report.Text(" node=");
        report.Number(diagnostic.Node);
        report.Text("\n");
    }
    if (library.Limits().NodeCapacity < MaximumNodes)
    {
        report.Text("arena reduced node-budget=");
        report.Number(library.Limits().NodeLimit);
        report.Text("\n");
    }
    const Budgets& used{library.Used()};
    report.Text("budgets nodes=");
    report.Number(used.Nodes);
    report.Text("/");
    report.Number(library.Limits().NodeLimit);
    report.Text(" syntax=");
    report.Number(used.SyntaxObjects);
    report.Text("/");
    report.Number(library.Limits().SyntaxLimit);
    report.Text(" objects=");
    report.Number(used.Objects);
    report.Text("/");
    report.Number(library.Limits().ObjectLimit);
    report.Text(" elements=");
    report.Number(used.Elements);
    report.Text("/");
    report.Number(library.Limits().ElementLimit);
    report.Text(" bytes=");
    report.Number(used.BytesUsed);
    report.Text("/");
    report.Number(library.Limits().ByteLimit);
    report.Text("\n  load-operations=");
    report.Number(used.LoadOperations);
    report.Text("/");
    report.Number(MaximumLoadOperations);
    report.Text(" evaluation-operations=");
    report.Number(used.EvaluationOperations);
    report.Text(" per-call-limit=");
    report.Number(MaximumEvaluationOperations);
    report.Text(" facts-operations=");
    report.Number(facts.Operations());
    report.Text("/");
    report.Number(MaximumFactOperations);
    report.Text("\n  parse-depth=");
    report.Number(used.ParseDepth);
    report.Text(" evaluation-depth=");
    report.Number(used.EvaluationDepth);
    report.Text(" call-depth=");
    report.Number(used.CallDepth);
    report.Text(" dropped-diagnostics=");
    report.Number(used.DroppedDiagnostics);
    report.Text("\n");
    return report.m_good;
}
} // namespace Asteria::Acpi::Aml
