// SPDX-License-Identifier: Apache-2.0
#include "User/Component/XhciProgress.hpp"

#include <cstdio>
#include <memory>
#include <vector>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Component/UsbProgressFrame.hpp"

namespace
{
namespace C = Asteria::User::Component;
namespace In = Asteria::User::Input;
namespace S = Asteria::User::System;
namespace Abi = Asteria::Abi;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::TestSupport::TestContext;

// The report Endpoint: each Send answers from a script, and a delivered
// frame is decoded as the root service decodes it.
struct Endpoint
{
    std::vector<Abi::SystemCallError> Answers{};
    Size Answered{0U};
    std::vector<In::UsbDiagnostic> Delivered{};
    UInt64 LastTransaction{0U};
    bool Ordered{true};
    bool Decoded{true};
};

void Invoke(void* const state, const Abi::SystemCallRequest* const request,
            Abi::SystemCallResponse* const response) noexcept
{
    auto& endpoint = *static_cast<Endpoint*>(state);
    *response = {.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
    if (static_cast<Abi::SystemCallNumber>(request->Number) != Abi::SystemCallNumber::IpcSend)
    {
        response->Error = Abi::SystemCallError::InvalidConfiguration;
        return;
    }
    if (endpoint.Answered < endpoint.Answers.size())
    {
        response->Error = endpoint.Answers[endpoint.Answered++];
    }
    if (response->Error != Abi::SystemCallError::None)
    {
        return;
    }
    // The delivery the root receives: a zeroed header before the frame.
    UInt8 delivery[C::EchoDeliveryHeaderBytes + C::XhciProgressFrameBytes]{};
    const auto* const frame = reinterpret_cast<const UInt8*>(request->Arguments[2U]);
    for (Size index{0U}; (index < request->Arguments[3U]) && (index < C::XhciProgressFrameBytes); ++index)
    {
        delivery[C::EchoDeliveryHeaderBytes + index] = frame[index];
    }
    UInt8 controller{0U};
    In::UsbDiagnostic record{};
    endpoint.Decoded = endpoint.Decoded && (request->Arguments[3U] == C::XhciProgressFrameBytes) &&
                       C::ReadXhciProgress(delivery, sizeof(delivery), controller, record) && (controller == 3U);
    const UInt64 transaction = S::LoadWord(frame + 40U);
    endpoint.Ordered = endpoint.Ordered && (transaction == endpoint.LastTransaction + 1U);
    endpoint.LastTransaction = transaction;
    endpoint.Delivered.push_back(record);
}

// The contract: per port at most PortRecords records between connects, then
// one port Suppressed; at most TotalRecords in all, then one controller
// Suppressed; at most Capacity queued, losses announced once drained; sends
// stop at a full queue and keep the record; another error disables.
struct Model
{
    void Push(const In::UsbDiagnostic& record)
    {
        if (Queue.size() == C::XhciProgress::Capacity)
        {
            ++Lost;
            return;
        }
        Queue.push_back(record);
    }
    void Record(const In::UsbDiagnostic& record)
    {
        if (!Enabled)
        {
            return;
        }
        if (record.Kind == In::UsbDiagnosticKind::Connect)
        {
            Ports[record.Port] = 0U;
        }
        if (Total >= C::XhciProgress::TotalRecords)
        {
            if (!TotalAnnounced)
            {
                TotalAnnounced = true;
                Push({.Kind = In::UsbDiagnosticKind::Suppressed,
                      .Port = 0U,
                      .Detail = 1U,
                      .Code = 0U,
                      .Value0 = 0U,
                      .Value1 = 0U,
                      .Data = 0U});
            }
            return;
        }
        if (record.Port != 0U)
        {
            if (Ports[record.Port] == C::XhciProgress::PortRecords)
            {
                ++Ports[record.Port];
                ++Total;
                Push({.Kind = In::UsbDiagnosticKind::Suppressed,
                      .Port = record.Port,
                      .Detail = 0U,
                      .Code = 0U,
                      .Value0 = 0U,
                      .Value1 = 0U,
                      .Data = 0U});
                return;
            }
            if (Ports[record.Port] > C::XhciProgress::PortRecords)
            {
                return;
            }
            ++Ports[record.Port];
        }
        ++Total;
        Push(record);
    }
    void Flush(const std::vector<Abi::SystemCallError>& answers)
    {
        Size answered{0U};
        for (Size sent{0U}; Enabled && (sent < C::XhciProgress::FlushLimit); ++sent)
        {
            if (Queue.empty() && (Lost != 0U))
            {
                Push({.Kind = In::UsbDiagnosticKind::Suppressed,
                      .Port = 0U,
                      .Detail = 2U,
                      .Code = 0U,
                      .Value0 = Lost,
                      .Value1 = 0U,
                      .Data = 0U});
                Lost = 0U;
            }
            if (Queue.empty())
            {
                return;
            }
            const Abi::SystemCallError answer =
                answered < answers.size() ? answers[answered] : Abi::SystemCallError::None;
            ++answered;
            if ((answer == Abi::SystemCallError::WouldBlock) || (answer == Abi::SystemCallError::ResourceExhausted))
            {
                return;
            }
            if (answer != Abi::SystemCallError::None)
            {
                Enabled = false;
                Queue.clear();
                return;
            }
            Delivered.push_back(Queue.front());
            Queue.erase(Queue.begin());
        }
    }
    std::vector<In::UsbDiagnostic> Queue{};
    std::vector<In::UsbDiagnostic> Delivered{};
    UInt16 Ports[256U]{};
    UInt32 Total{0U};
    UInt32 Lost{0U};
    bool TotalAnnounced{false};
    bool Enabled{true};
};

[[nodiscard]] bool Same(const In::UsbDiagnostic& left, const In::UsbDiagnostic& right) noexcept
{
    return (left.Kind == right.Kind) && (left.Port == right.Port) && (left.Detail == right.Detail) &&
           (left.Code == right.Code) && (left.Value0 == right.Value0) && (left.Value1 == right.Value1) &&
           (left.Data == right.Data);
}

void Seeded(TestContext& context) noexcept
{
    const auto seeds = Asteria::TestSupport::GetSeedRange(0x55534250U, 24U);
    for (UInt64 seed{seeds.First}; seed - seeds.First < seeds.Count; ++seed)
    {
        Asteria::TestSupport::Random random{seed};
        auto endpoint = std::make_unique<Endpoint>();
        const S::SystemCallClient client{{.Function = Invoke, .Context = endpoint.get()}};
        // Constructed in place in the driver; a heap object here.
        auto progress = std::make_unique<C::XhciProgress>();
        auto model = std::make_unique<Model>();
        progress->Configure(3U, true);
        UInt8 frame[C::XhciProgressFrameBytes]{};
        const bool failing = random.Chance(1U, 3U);
        // Seeds vary the port count, connect rate and flush rate, so port
        // budgets, the controller budget and a full queue are each reached.
        const UInt64 ports = 1U + random.Below(4U);
        const UInt64 connects = 20U + random.Below(400U);
        const UInt64 flushes = 1U + random.Below(6U);
        bool good{true};
        Size step{0U};
        const char* operation{"none"};
        for (; good && (step < 3000U); ++step)
        {
            const UInt64 choice = random.Below(16U);
            if (choice >= flushes)
            {
                operation = "record";
                const In::UsbDiagnostic record{
                    .Kind = random.Chance(1U, connects)
                                ? In::UsbDiagnosticKind::Connect
                                : static_cast<In::UsbDiagnosticKind>(
                                      2U + random.Below(static_cast<UInt8>(In::UsbDiagnosticKind::Suppressed) - 2U)),
                    .Port = static_cast<UInt8>(random.Chance(1U, 8U) ? 0U : 1U + random.Below(ports)),
                    .Detail = static_cast<UInt8>(random.Below(256U)),
                    .Code = static_cast<UInt8>(random.Below(256U)),
                    .Value0 = static_cast<UInt32>(random.Next()),
                    .Value1 = static_cast<UInt32>(random.Next()),
                    .Data = random.Next()};
                progress->Record(record);
                model->Record(record);
            }
            else
            {
                operation = "flush";
                std::vector<Abi::SystemCallError> answers{};
                for (Size index{0U}; index < C::XhciProgress::FlushLimit; ++index)
                {
                    // A full queue often, another error only on failing seeds.
                    const UInt64 answer = random.Below(400U);
                    answers.push_back(answer < 100U                   ? Abi::SystemCallError::WouldBlock
                                      : answer < 150U                 ? Abi::SystemCallError::ResourceExhausted
                                      : (failing && (answer == 399U)) ? Abi::SystemCallError::InvalidArguments
                                                                      : Abi::SystemCallError::None);
                }
                endpoint->Answers = answers;
                endpoint->Answered = 0U;
                progress->Flush(client, S::CapabilityHandle{5U, 0U}, frame);
                model->Flush(answers);
                endpoint->Answers.clear();
            }
            good = (progress->Pending() == model->Queue.size()) && (progress->Enabled() == model->Enabled) &&
                   (endpoint->Delivered.size() == model->Delivered.size()) && endpoint->Ordered && endpoint->Decoded;
            for (Size index{0U}; good && (index < model->Delivered.size()); ++index)
            {
                good = Same(endpoint->Delivered[index], model->Delivered[index]);
            }
        }
        if (!good)
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu operation=%s\n",
                                           static_cast<unsigned long long>(seed), step - 1U, operation));
        }
        ASTERIA_EXPECT(context, "progress queue matches its bounded-output model", good);
    }
}

void Disabled(TestContext& context) noexcept
{
    auto endpoint = std::make_unique<Endpoint>();
    const S::SystemCallClient client{{.Function = Invoke, .Context = endpoint.get()}};
    auto progress = std::make_unique<C::XhciProgress>();
    UInt8 frame[C::XhciProgressFrameBytes]{};
    progress->Record({.Kind = In::UsbDiagnosticKind::Connect,
                      .Port = 1U,
                      .Detail = 2U,
                      .Code = 0U,
                      .Value0 = 0x603U,
                      .Value1 = 0U,
                      .Data = 0U});
    progress->Flush(client, S::CapabilityHandle{5U, 0U}, frame);
    ASTERIA_EXPECT(context, "a driver started without the flag sends nothing", progress->Pending() == 0U,
                   endpoint->Delivered.empty(), !progress->Enabled());
}
} // namespace

int main()
{
    TestContext context{"xHCI progress queue"};
    Seeded(context);
    Disabled(context);
    return context.Finish();
}
