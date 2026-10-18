// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Executive/ObjectDirectory/ObjectDirectoryServiceCore.hpp"
#include "Kernel/Ipc/Endpoint.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Executive::ObjectDirectory::BindIfAbsentRequest;
using Executive::ObjectDirectory::BindingVersion;
using Executive::ObjectDirectory::DirectoryCoreError;
using Executive::ObjectDirectory::DirectoryCoreState;
using Executive::ObjectDirectory::DirectoryMessageContext;
using Executive::ObjectDirectory::DirectoryName;
using Executive::ObjectDirectory::DirectoryNameView;
using Executive::ObjectDirectory::DirectoryStatus;
using Executive::ObjectDirectory::InterfaceVersionRange;
using Executive::ObjectDirectory::MutationReply;
using Executive::ObjectDirectory::ObjectDirectoryProtocol;
using Executive::ObjectDirectory::ObjectDirectoryProtocolError;
using Executive::ObjectDirectory::ObjectDirectoryServiceConfiguration;
using Executive::ObjectDirectory::ObjectDirectoryServiceCore;
using Executive::ObjectDirectory::ObjectDirectoryServiceDispatchOutcome;
using Executive::ObjectDirectory::ObjectDirectoryServiceError;
using Executive::ObjectDirectory::ObjectDirectoryServiceFailure;
using Executive::ObjectDirectory::RemoveIfVersionRequest;
using Executive::ObjectDirectory::ReplaceIfVersionRequest;
using Executive::ObjectDirectory::ResolveReply;
using Executive::ObjectDirectory::ResolveRequest;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Kernel::Ipc::EndpointReceiveMetadata;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ResourceDomainId;

constexpr UInt64 ExpectedSelector{0xD1EC'7000'0000'0001ULL};
constexpr UInt64 DirectoryGeneration{0xD1EC'7000'0000'0002ULL};

[[nodiscard]] bool HandlesAreEqual(const CapabilityHandle left, const CapabilityHandle right) noexcept
{
    return (left.GetSelector() == right.GetSelector()) && (left.GetAuthenticator() == right.GetAuthenticator());
}

[[nodiscard]] bool CleanupContains(const ObjectDirectoryServiceDispatchOutcome& outcome,
                                   const CapabilityHandle capability) noexcept
{
    for (Size index{0U}; index < outcome.CapabilityCloseCount; ++index)
    {
        if (HandlesAreEqual(outcome.CapabilitiesToClose[index], capability))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] DirectoryName MakeName(const char* const text, const Size sizeBytes) noexcept
{
    UInt8 bytes[DirectoryName::MaximumSizeBytes]{};
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        bytes[index] = static_cast<UInt8>(text[index]);
    }
    const auto result = DirectoryName::Create(DirectoryNameView{
        .Data = bytes,
        .SizeBytes = sizeBytes,
    });
    return result.GetValue();
}

[[nodiscard]] CapabilityRights InvokeRights() noexcept
{
    return CapabilityRights{CapabilityRight::Invoke};
}

[[nodiscard]] CapabilityRights InvokeManageRights() noexcept
{
    return InvokeRights().With(CapabilityRight::Manage);
}

[[nodiscard]] CapabilityRights PublishedRights() noexcept
{
    return InvokeRights().With(CapabilityRight::Read);
}

[[nodiscard]] DirectoryMessageContext MakeContext(const UInt64 transactionIdentifier) noexcept
{
    return DirectoryMessageContext{
        .Trace =
            {
                .TraceIdentifierHigh = 0x1111'2222'3333'4444ULL,
                .TraceIdentifierLow = transactionIdentifier,
                .SpanIdentifier = 0x5555'6666'7777'8888ULL,
            },
        .Deadline = MonotonicTime{8'000U},
        .TransactionIdentifier = transactionIdentifier,
    };
}

[[nodiscard]] InterfaceVersionRange PublishedVersion() noexcept
{
    return InterfaceVersionRange{
        .Major = 2U,
        .MinimumMinor = 3U,
        .MaximumMinor = 8U,
        .Reserved = 0U,
    };
}

[[nodiscard]] InterfaceIdentifier ServiceInterface() noexcept
{
    return InterfaceIdentifier{0xA001'A002'A003'A004ULL, 0xB001'B002'B003'B004ULL};
}

[[nodiscard]] BindIfAbsentRequest MakeBindRequest(const DirectoryName& name, const CapabilityHandle target,
                                                  const CapabilityHandle reply,
                                                  const UInt64 transactionIdentifier = 1U) noexcept
{
    return BindIfAbsentRequest{
        .Name = name,
        .ServiceInterface = ServiceInterface(),
        .ServiceGeneration = 7U,
        .Version = PublishedVersion(),
        .PublishedRights = PublishedRights(),
        .TargetEndpoint = target,
        .TargetDeadline = MonotonicTime{5'000U},
        .ReplyEndpoint = reply,
        .Context = MakeContext(transactionIdentifier),
    };
}

[[nodiscard]] ReplaceIfVersionRequest MakeReplaceRequest(const BindIfAbsentRequest& bind,
                                                         const BindingVersion expectedVersion,
                                                         const CapabilityHandle target, const CapabilityHandle reply,
                                                         const UInt64 transactionIdentifier) noexcept
{
    return ReplaceIfVersionRequest{
        .Name = bind.Name,
        .ExpectedVersion = expectedVersion,
        .ServiceInterface = bind.ServiceInterface,
        .ServiceGeneration = bind.ServiceGeneration + 1U,
        .Version = bind.Version,
        .PublishedRights = bind.PublishedRights,
        .TargetEndpoint = target,
        .TargetDeadline = bind.TargetDeadline,
        .ReplyEndpoint = reply,
        .Context = MakeContext(transactionIdentifier),
    };
}

[[nodiscard]] ResolveRequest MakeResolveRequest(const DirectoryName& name, const CapabilityHandle reply,
                                                const UInt64 transactionIdentifier) noexcept
{
    return ResolveRequest{
        .Name = name,
        .ServiceInterface = ServiceInterface(),
        .RequestedDeadline = MonotonicTime{4'000U},
        .RequiredVersion =
            InterfaceVersionRange{
                .Major = 2U,
                .MinimumMinor = 4U,
                .MaximumMinor = 9U,
                .Reserved = 0U,
            },
        .RequiredRights = InvokeRights(),
        .ReplyEndpoint = reply,
        .Context = MakeContext(transactionIdentifier),
    };
}

[[nodiscard]] RemoveIfVersionRequest MakeRemoveRequest(const DirectoryName& name, const BindingVersion expectedVersion,
                                                       const CapabilityHandle reply,
                                                       const UInt64 transactionIdentifier) noexcept
{
    return RemoveIfVersionRequest{
        .Name = name,
        .ExpectedVersion = expectedVersion,
        .ReplyEndpoint = reply,
        .Context = MakeContext(transactionIdentifier),
    };
}

[[nodiscard]] EndpointReceiveMetadata MakeMetadata(const CapabilityRights rights,
                                                   const UInt64 sealedSelector = ExpectedSelector,
                                                   const UInt64 sender = 1U) noexcept
{
    return EndpointReceiveMetadata{
        .Sequence = 1U,
        .Sender = ResourceDomainId{sender},
        .SenderEndpointSealedObjectSelector = sealedSelector,
        .SenderEndpointRights = rights,
        .FrameSizeBytes = 0U,
        .AttachmentCount = 0U,
        .SendClass = EndpointSendClass::Normal,
    };
}

[[nodiscard]] ObjectDirectoryServiceConfiguration
MakeConfiguration(const UInt64 revision = 0U, const UInt64 selector = ExpectedSelector,
                  const UInt64 generation = DirectoryGeneration) noexcept
{
    return ObjectDirectoryServiceConfiguration{
        .Core =
            {
                .DirectoryGeneration = generation,
                .LastIssuedRevision = revision,
            },
        .ExpectedSealedObjectSelector = selector,
    };
}

[[nodiscard]] MessageEnvelope BuildBind(const BindIfAbsentRequest& request) noexcept
{
    MessageEnvelope message{};
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(request, message));
    return message;
}

[[nodiscard]] MessageEnvelope BuildReplace(const ReplaceIfVersionRequest& request) noexcept
{
    MessageEnvelope message{};
    static_cast<void>(ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(request, message));
    return message;
}

[[nodiscard]] MessageEnvelope BuildResolve(const ResolveRequest& request) noexcept
{
    MessageEnvelope message{};
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveRequest(request, message));
    return message;
}

[[nodiscard]] MessageEnvelope BuildRemove(const RemoveIfVersionRequest& request) noexcept
{
    MessageEnvelope message{};
    static_cast<void>(ObjectDirectoryProtocol::BuildRemoveIfVersionRequest(request, message));
    return message;
}

[[nodiscard]] MutationReply ParseMutationReply(const Foundation::UInt32 method, const MessageEnvelope& message,
                                               ObjectDirectoryProtocolError& error) noexcept
{
    MutationReply reply{};
    switch (method)
    {
    case ObjectDirectoryProtocol::BindIfAbsentMethodIdentifier:
        error = ObjectDirectoryProtocol::ParseBindIfAbsentReply(message, reply);
        break;

    case ObjectDirectoryProtocol::ReplaceIfVersionMethodIdentifier:
        error = ObjectDirectoryProtocol::ParseReplaceIfVersionReply(message, reply);
        break;

    case ObjectDirectoryProtocol::RemoveIfVersionMethodIdentifier:
        error = ObjectDirectoryProtocol::ParseRemoveIfVersionReply(message, reply);
        break;

    default:
        error = ObjectDirectoryProtocolError::InvalidMethod;
        break;
    }
    return reply;
}

[[nodiscard]] MutationReply DispatchMutation(ObjectDirectoryServiceCore& service, const MessageEnvelope& message,
                                             const CapabilityRights invocationRights,
                                             ObjectDirectoryServiceDispatchOutcome& outcome,
                                             ObjectDirectoryServiceFailure& failure,
                                             ObjectDirectoryProtocolError& parseError,
                                             const UInt64 sender = 1U) noexcept
{
    failure = service.Dispatch(message, MakeMetadata(invocationRights, ExpectedSelector, sender), outcome);
    return ParseMutationReply(message.MethodIdentifier, outcome.PreparedReply, parseError);
}

void TestAuthorityAndCleanup(TestContext& runner) noexcept
{
    ObjectDirectoryServiceCore service{MakeConfiguration()};
    const DirectoryName name = MakeName("authority", 9U);
    const CapabilityHandle target{0x1001U, 0xA001U};
    const CapabilityHandle reply{0x1002U, 0xA002U};
    const MessageEnvelope request = BuildBind(MakeBindRequest(name, target, reply));

    ObjectDirectoryServiceDispatchOutcome wrongSelector{};
    const ObjectDirectoryServiceFailure selectorFailure =
        service.Dispatch(request, MakeMetadata(InvokeManageRights(), ExpectedSelector + 1U), wrongSelector);
    ASTERIA_EXPECT(runner, "wrong sealed selector drops and returns all received attachments",
                   selectorFailure.Code == ObjectDirectoryServiceError::InvocationRejected,
                   !wrongSelector.HasPreparedReply, wrongSelector.CapabilityCloseCount == 2U,
                   CleanupContains(wrongSelector, target), CleanupContains(wrongSelector, reply),
                   service.GetSnapshot().BindingCount == 0U);

    ObjectDirectoryServiceDispatchOutcome missingInvoke{};
    const ObjectDirectoryServiceFailure invokeFailure =
        service.Dispatch(request, MakeMetadata(CapabilityRights{CapabilityRight::Manage}), missingInvoke);
    ASTERIA_EXPECT(runner, "missing trusted Invoke drops without parsing payload authority",
                   invokeFailure.Code == ObjectDirectoryServiceError::InvocationRejected,
                   !missingInvoke.HasPreparedReply, missingInvoke.CapabilityCloseCount == 2U,
                   service.GetSnapshot().BindingCount == 0U);

    ObjectDirectoryServiceDispatchOutcome denied{};
    ObjectDirectoryServiceFailure deniedFailure{};
    ObjectDirectoryProtocolError deniedParse{};
    const MutationReply deniedReply =
        DispatchMutation(service, request, InvokeRights(), denied, deniedFailure, deniedParse);
    ASTERIA_EXPECT(
        runner, "valid mutation without Manage replies PermissionDenied and cleans target",
        deniedFailure.Code == ObjectDirectoryServiceError::None, deniedParse == ObjectDirectoryProtocolError::None,
        deniedReply.Status == DirectoryStatus::PermissionDenied, deniedReply.Version.DirectoryGeneration == 0U,
        deniedReply.Version.Revision == 0U, denied.CapabilityCloseCount == 2U, CleanupContains(denied, target),
        CleanupContains(denied, reply), service.GetSnapshot().BindingCount == 0U);

    ObjectDirectoryServiceDispatchOutcome accepted;
    ObjectDirectoryServiceFailure acceptedFailure{};
    ObjectDirectoryProtocolError acceptedParse{};
    const MutationReply acceptedReply =
        DispatchMutation(service, request, InvokeManageRights(), accepted, acceptedFailure, acceptedParse, 0U);
    ASTERIA_EXPECT(runner, "Sender identity is ignored while trusted rights authorize binding",
                   acceptedFailure.Code == ObjectDirectoryServiceError::None,
                   acceptedParse == ObjectDirectoryProtocolError::None,
                   acceptedReply.Status == DirectoryStatus::Success, accepted.CapabilityCloseCount == 1U,
                   CleanupContains(accepted, reply), !CleanupContains(accepted, target),
                   HandlesAreEqual(accepted.ReplyEndpoint, reply), service.GetSnapshot().BindingCount == 1U);
}

void TestFourSuccessPathsAndCorrelation(TestContext& runner) noexcept
{
    ObjectDirectoryServiceCore service{MakeConfiguration()};
    const DirectoryName name = MakeName("service.main", 12U);
    const CapabilityHandle firstTarget{0x2001U, 0xB001U};
    const CapabilityHandle bindEndpoint{0x2002U, 0xB002U};
    const BindIfAbsentRequest bind = MakeBindRequest(name, firstTarget, bindEndpoint, 0x101U);

    ObjectDirectoryServiceDispatchOutcome bindOutcome{};
    ObjectDirectoryServiceFailure bindFailure{};
    ObjectDirectoryProtocolError bindParse{};
    const MutationReply bindReply =
        DispatchMutation(service, BuildBind(bind), InvokeManageRights(), bindOutcome, bindFailure, bindParse);
    ASTERIA_EXPECT(runner, "BindIfAbsent adopts target and preserves reply correlation",
                   bindFailure.Code == ObjectDirectoryServiceError::None,
                   bindParse == ObjectDirectoryProtocolError::None, bindReply.Status == DirectoryStatus::Success,
                   bindReply.Context.TransactionIdentifier == bind.Context.TransactionIdentifier,
                   bindReply.Context.Deadline.GetTicks() == bind.Context.Deadline.GetTicks(),
                   bindOutcome.CapabilityCloseCount == 1U, CleanupContains(bindOutcome, bindEndpoint),
                   !CleanupContains(bindOutcome, firstTarget));

    const CapabilityHandle resolveEndpoint{0x2003U, 0xB003U};
    const ResolveRequest resolve = MakeResolveRequest(name, resolveEndpoint, 0x102U);
    ObjectDirectoryServiceDispatchOutcome resolveOutcome{};
    const ObjectDirectoryServiceFailure resolveFailure = service.Dispatch(
        BuildResolve(resolve), MakeMetadata(InvokeRights(), ExpectedSelector, 0xFFFFU), resolveOutcome);
    ResolveReply resolved{};
    const ObjectDirectoryProtocolError resolveParse =
        ObjectDirectoryProtocol::ParseResolveReply(resolveOutcome.PreparedReply, resolved);
    ASTERIA_EXPECT(
        runner, "Resolve prepares borrowed target reply without scheduling it for close",
        resolveFailure.Code == ObjectDirectoryServiceError::None, resolveFailure.Core == DirectoryCoreError::None,
        resolveParse == ObjectDirectoryProtocolError::None, resolved.Status == DirectoryStatus::Success,
        resolved.SelectedMinor == 8U, HandlesAreEqual(resolved.TargetEndpoint, firstTarget),
        resolved.Context.TransactionIdentifier == resolve.Context.TransactionIdentifier,
        HandlesAreEqual(resolveOutcome.ReplyEndpoint, resolveEndpoint), resolveOutcome.CapabilityCloseCount == 1U,
        CleanupContains(resolveOutcome, resolveEndpoint), !CleanupContains(resolveOutcome, firstTarget));

    const CapabilityHandle secondTarget{0x2004U, 0xB004U};
    const CapabilityHandle replaceEndpoint{0x2005U, 0xB005U};
    const ReplaceIfVersionRequest replace =
        MakeReplaceRequest(bind, bindReply.Version, secondTarget, replaceEndpoint, 0x103U);
    ObjectDirectoryServiceDispatchOutcome replaceOutcome{};
    ObjectDirectoryServiceFailure replaceFailure{};
    ObjectDirectoryProtocolError replaceParse{};
    const MutationReply replaceReply = DispatchMutation(service, BuildReplace(replace), InvokeManageRights(),
                                                        replaceOutcome, replaceFailure, replaceParse);
    ASTERIA_EXPECT(runner, "ReplaceIfVersion adopts replacement and returns displaced custody",
                   replaceFailure.Code == ObjectDirectoryServiceError::None,
                   replaceParse == ObjectDirectoryProtocolError::None, replaceReply.Status == DirectoryStatus::Success,
                   replaceReply.Version.Revision > bindReply.Version.Revision,
                   replaceOutcome.CapabilityCloseCount == 2U, CleanupContains(replaceOutcome, replaceEndpoint),
                   CleanupContains(replaceOutcome, firstTarget), !CleanupContains(replaceOutcome, secondTarget));

    const CapabilityHandle removeEndpoint{0x2006U, 0xB006U};
    const RemoveIfVersionRequest remove = MakeRemoveRequest(name, replaceReply.Version, removeEndpoint, 0x104U);
    ObjectDirectoryServiceDispatchOutcome removeOutcome{};
    ObjectDirectoryServiceFailure removeFailure{};
    ObjectDirectoryProtocolError removeParse{};
    const MutationReply removeReply =
        DispatchMutation(service, BuildRemove(remove), InvokeManageRights(), removeOutcome, removeFailure, removeParse);
    ASTERIA_EXPECT(runner, "RemoveIfVersion returns removed custody after commit",
                   removeFailure.Code == ObjectDirectoryServiceError::None,
                   removeParse == ObjectDirectoryProtocolError::None, removeReply.Status == DirectoryStatus::Success,
                   removeOutcome.CapabilityCloseCount == 2U, CleanupContains(removeOutcome, removeEndpoint),
                   CleanupContains(removeOutcome, secondTarget), service.GetSnapshot().BindingCount == 0U);
}

void TestSemanticStatusMappings(TestContext& runner) noexcept
{
    ObjectDirectoryServiceCore service{MakeConfiguration()};
    const DirectoryName name = MakeName("mapping", 7U);
    const CapabilityHandle firstTarget{0x3001U, 0xC001U};
    const CapabilityHandle firstReplyEndpoint{0x3002U, 0xC002U};
    const BindIfAbsentRequest first = MakeBindRequest(name, firstTarget, firstReplyEndpoint, 0x201U);
    ObjectDirectoryServiceDispatchOutcome firstOutcome{};
    ObjectDirectoryServiceFailure firstFailure{};
    ObjectDirectoryProtocolError firstParse{};
    const MutationReply firstReply =
        DispatchMutation(service, BuildBind(first), InvokeManageRights(), firstOutcome, firstFailure, firstParse);

    const CapabilityHandle contender{0x3003U, 0xC003U};
    const CapabilityHandle contenderReply{0x3004U, 0xC004U};
    ObjectDirectoryServiceDispatchOutcome alreadyOutcome{};
    ObjectDirectoryServiceFailure alreadyFailure{};
    ObjectDirectoryProtocolError alreadyParse{};
    const MutationReply alreadyReply =
        DispatchMutation(service, BuildBind(MakeBindRequest(name, contender, contenderReply, 0x202U)),
                         InvokeManageRights(), alreadyOutcome, alreadyFailure, alreadyParse);
    ASTERIA_EXPECT(runner, "AlreadyBound maps with zero mutation version and target cleanup",
                   alreadyReply.Status == DirectoryStatus::AlreadyBound,
                   alreadyFailure.Core == DirectoryCoreError::AlreadyBound,
                   alreadyReply.Version.DirectoryGeneration == 0U, alreadyReply.Version.Revision == 0U,
                   CleanupContains(alreadyOutcome, contender), CleanupContains(alreadyOutcome, contenderReply));

    const BindingVersion stale{
        .DirectoryGeneration = firstReply.Version.DirectoryGeneration,
        .Revision = firstReply.Version.Revision + 1U,
    };
    const CapabilityHandle staleTarget{0x3005U, 0xC005U};
    const CapabilityHandle staleEndpoint{0x3006U, 0xC006U};
    ObjectDirectoryServiceDispatchOutcome staleOutcome{};
    ObjectDirectoryServiceFailure staleFailure{};
    ObjectDirectoryProtocolError staleParse{};
    const MutationReply staleReply =
        DispatchMutation(service, BuildReplace(MakeReplaceRequest(first, stale, staleTarget, staleEndpoint, 0x203U)),
                         InvokeManageRights(), staleOutcome, staleFailure, staleParse);
    ASTERIA_EXPECT(runner, "stale replacement maps BindingChanged without mutation",
                   staleReply.Status == DirectoryStatus::BindingChanged,
                   staleFailure.Core == DirectoryCoreError::BindingChanged, CleanupContains(staleOutcome, staleTarget),
                   service.GetSnapshot().LastIssuedRevision == firstReply.Version.Revision);

    ResolveRequest incompatible = MakeResolveRequest(name, CapabilityHandle{0x3007U, 0xC007U}, 0x204U);
    incompatible.ServiceInterface = InterfaceIdentifier{9U, 10U};
    ObjectDirectoryServiceDispatchOutcome incompatibleOutcome{};
    const ObjectDirectoryServiceFailure incompatibleFailure =
        service.Dispatch(BuildResolve(incompatible), MakeMetadata(InvokeRights()), incompatibleOutcome);
    ResolveReply incompatibleReply{};
    const ObjectDirectoryProtocolError incompatibleParse =
        ObjectDirectoryProtocol::ParseResolveReply(incompatibleOutcome.PreparedReply, incompatibleReply);
    ASTERIA_EXPECT(runner, "resolve interface mismatch maps to fully zero NoCompatibleVersion",
                   incompatibleParse == ObjectDirectoryProtocolError::None,
                   incompatibleReply.Status == DirectoryStatus::NoCompatibleVersion,
                   incompatibleFailure.Core == DirectoryCoreError::InvalidInterface,
                   !incompatibleReply.TargetEndpoint.IsValid());

    ResolveRequest excessiveRights = MakeResolveRequest(name, CapabilityHandle{0x3008U, 0xC008U}, 0x205U);
    excessiveRights.RequiredRights = PublishedRights().With(CapabilityRight::Write);
    ObjectDirectoryServiceDispatchOutcome rightsOutcome{};
    const ObjectDirectoryServiceFailure rightsFailure =
        service.Dispatch(BuildResolve(excessiveRights), MakeMetadata(InvokeRights()), rightsOutcome);
    ResolveReply rightsReply{};
    const ObjectDirectoryProtocolError rightsParse =
        ObjectDirectoryProtocol::ParseResolveReply(rightsOutcome.PreparedReply, rightsReply);
    ASTERIA_EXPECT(runner, "resolve attenuation failure maps RightsUnavailable",
                   rightsParse == ObjectDirectoryProtocolError::None,
                   rightsReply.Status == DirectoryStatus::RightsUnavailable,
                   rightsFailure.Core == DirectoryCoreError::InvalidRights);

    ResolveRequest excessiveLease = MakeResolveRequest(name, CapabilityHandle{0x3009U, 0xC009U}, 0x206U);
    excessiveLease.RequestedDeadline = MonotonicTime{5'001U};
    ObjectDirectoryServiceDispatchOutcome leaseOutcome{};
    const ObjectDirectoryServiceFailure leaseFailure =
        service.Dispatch(BuildResolve(excessiveLease), MakeMetadata(InvokeRights()), leaseOutcome);
    ResolveReply leaseReply{};
    const ObjectDirectoryProtocolError leaseParse =
        ObjectDirectoryProtocol::ParseResolveReply(leaseOutcome.PreparedReply, leaseReply);
    ASTERIA_EXPECT(runner, "resolve lease failure maps LeaseUnavailable",
                   leaseParse == ObjectDirectoryProtocolError::None,
                   leaseReply.Status == DirectoryStatus::LeaseUnavailable,
                   leaseFailure.Core == DirectoryCoreError::LeaseUnavailable);

    const DirectoryName missingName = MakeName("missing", 7U);
    ObjectDirectoryServiceDispatchOutcome missingOutcome{};
    const ObjectDirectoryServiceFailure missingFailure =
        service.Dispatch(BuildResolve(MakeResolveRequest(missingName, CapabilityHandle{0x3010U, 0xC010U}, 0x207U)),
                         MakeMetadata(InvokeRights()), missingOutcome);
    ResolveReply missingReply{};
    const ObjectDirectoryProtocolError missingParse =
        ObjectDirectoryProtocol::ParseResolveReply(missingOutcome.PreparedReply, missingReply);
    ASTERIA_EXPECT(runner, "missing resolve maps NotFound", missingParse == ObjectDirectoryProtocolError::None,
                   missingReply.Status == DirectoryStatus::NotFound,
                   missingFailure.Core == DirectoryCoreError::NotFound);
}

void TestResourceAndDuplicateCustodyMappings(TestContext& runner) noexcept
{
    constexpr UInt64 maximumRevision{~UInt64{0U}};
    ObjectDirectoryServiceCore exhausted{MakeConfiguration(maximumRevision)};
    const DirectoryName name = MakeName("exhausted", 9U);
    const CapabilityHandle target{0x4001U, 0xD001U};
    const CapabilityHandle replyEndpoint{0x4002U, 0xD002U};
    ObjectDirectoryServiceDispatchOutcome exhaustedOutcome{};
    ObjectDirectoryServiceFailure exhaustedFailure{};
    ObjectDirectoryProtocolError exhaustedParse{};
    const MutationReply exhaustedReply =
        DispatchMutation(exhausted, BuildBind(MakeBindRequest(name, target, replyEndpoint, 0x301U)),
                         InvokeManageRights(), exhaustedOutcome, exhaustedFailure, exhaustedParse);
    ASTERIA_EXPECT(runner, "revision exhaustion maps ResourceExhausted with target cleanup",
                   exhaustedParse == ObjectDirectoryProtocolError::None,
                   exhaustedReply.Status == DirectoryStatus::ResourceExhausted,
                   exhaustedFailure.Core == DirectoryCoreError::RevisionExhausted,
                   CleanupContains(exhaustedOutcome, target));

    ObjectDirectoryServiceCore duplicate{MakeConfiguration()};
    const DirectoryName firstName = MakeName("first", 5U);
    const CapabilityHandle sharedTarget{0x4003U, 0xD003U};
    ObjectDirectoryServiceDispatchOutcome firstOutcome{};
    ObjectDirectoryServiceFailure firstFailure{};
    ObjectDirectoryProtocolError firstParse{};
    static_cast<void>(DispatchMutation(
        duplicate, BuildBind(MakeBindRequest(firstName, sharedTarget, CapabilityHandle{0x4004U, 0xD004U}, 0x302U)),
        InvokeManageRights(), firstOutcome, firstFailure, firstParse));

    const DirectoryName secondName = MakeName("second", 6U);
    ObjectDirectoryServiceDispatchOutcome duplicateOutcome{};
    ObjectDirectoryServiceFailure duplicateFailure{};
    ObjectDirectoryProtocolError duplicateParse{};
    const MutationReply duplicateReply = DispatchMutation(
        duplicate, BuildBind(MakeBindRequest(secondName, sharedTarget, CapabilityHandle{0x4005U, 0xD005U}, 0x303U)),
        InvokeManageRights(), duplicateOutcome, duplicateFailure, duplicateParse);
    ASTERIA_EXPECT(runner, "duplicate candidate custody maps InvalidRequest without core mutation",
                   duplicateParse == ObjectDirectoryProtocolError::None,
                   duplicateReply.Status == DirectoryStatus::InvalidRequest,
                   duplicateFailure.Core == DirectoryCoreError::CapabilityAlreadyOwned,
                   CleanupContains(duplicateOutcome, sharedTarget), duplicate.GetSnapshot().BindingCount == 1U);
}

void TestMalformedDropsAndOutputAtomicity(TestContext& runner) noexcept
{
    ObjectDirectoryServiceCore service{MakeConfiguration()};
    const DirectoryName name = MakeName("malformed", 9U);
    const CapabilityHandle target{0x5001U, 0xE001U};
    const CapabilityHandle replyEndpoint{0x5002U, 0xE002U};
    MessageEnvelope malformed = BuildBind(MakeBindRequest(name, target, replyEndpoint, 0x401U));
    malformed.FixedPayloadSizeBytes = 0U;
    ObjectDirectoryServiceDispatchOutcome malformedOutcome{};
    const ObjectDirectoryServiceFailure malformedFailure =
        service.Dispatch(malformed, MakeMetadata(InvokeManageRights()), malformedOutcome);
    ASTERIA_EXPECT(runner, "malformed known method drops and returns all attachments",
                   malformedFailure.Code == ObjectDirectoryServiceError::ProtocolFailure,
                   malformedFailure.Protocol == ObjectDirectoryProtocolError::InvalidMessageContract,
                   !malformedOutcome.HasPreparedReply, malformedOutcome.CapabilityCloseCount == 2U,
                   CleanupContains(malformedOutcome, target), CleanupContains(malformedOutcome, replyEndpoint));

    MessageEnvelope unknown{};
    unknown.MethodIdentifier = 0xFFFFU;
    unknown.AttachmentCount = 4U;
    const CapabilityHandle duplicate{0x5003U, 0xE003U};
    unknown.Attachments[0U].Handle = duplicate;
    unknown.Attachments[1U].Handle = duplicate;
    unknown.Attachments[2U].Handle = CapabilityHandle{};
    unknown.Attachments[3U].Handle = duplicate;
    ObjectDirectoryServiceDispatchOutcome unknownOutcome{};
    const ObjectDirectoryServiceFailure unknownFailure =
        service.Dispatch(unknown, MakeMetadata(InvokeManageRights()), unknownOutcome);
    ASTERIA_EXPECT(runner, "unknown method cleanup skips invalid and deduplicates received handles",
                   unknownFailure.Code == ObjectDirectoryServiceError::ProtocolFailure,
                   unknownFailure.Protocol == ObjectDirectoryProtocolError::InvalidMethod,
                   !unknownOutcome.HasPreparedReply, unknownOutcome.CapabilityCloseCount == 1U,
                   CleanupContains(unknownOutcome, duplicate));

    const MessageEnvelope valid = BuildBind(MakeBindRequest(name, target, replyEndpoint, 0x402U));
    ObjectDirectoryServiceDispatchOutcome occupied{};
    occupied.PreparedReply.MethodIdentifier = 77U;
    const ObjectDirectoryServiceDispatchOutcome before = occupied;
    const ObjectDirectoryServiceFailure occupiedFailure =
        service.Dispatch(valid, MakeMetadata(InvokeManageRights()), occupied);
    ASTERIA_EXPECT(runner, "occupied output rejects atomically before cleanup or mutation",
                   occupiedFailure.Code == ObjectDirectoryServiceError::OutputOccupied,
                   occupied.PreparedReply.MethodIdentifier == before.PreparedReply.MethodIdentifier,
                   occupied.CapabilityCloseCount == before.CapabilityCloseCount, !occupied.HasPreparedReply,
                   service.GetSnapshot().BindingCount == 0U);

    ObjectDirectoryServiceCore invalid{MakeConfiguration(0U, 0U)};
    ObjectDirectoryServiceDispatchOutcome invalidOutcome{};
    const ObjectDirectoryServiceFailure invalidFailure =
        invalid.Dispatch(valid, MakeMetadata(InvokeManageRights()), invalidOutcome);
    ASTERIA_EXPECT(runner, "zero configured selector is rejected after received custody collection",
                   invalidFailure.Code == ObjectDirectoryServiceError::InvalidConfiguration,
                   !invalidOutcome.HasPreparedReply, invalidOutcome.CapabilityCloseCount == 2U);
}

void TestQuiescedCoreAndDrain(TestContext& runner) noexcept
{
    ObjectDirectoryServiceCore empty{MakeConfiguration()};
    ASTERIA_EXPECT(runner, "empty service core begins quiesce", empty.BeginQuiesce() == DirectoryCoreError::None);
    const DirectoryName name = MakeName("quiet", 5U);
    const CapabilityHandle replyEndpoint{0x6001U, 0xF001U};
    ObjectDirectoryServiceDispatchOutcome quiescedOutcome{};
    const ObjectDirectoryServiceFailure quiescedFailure = empty.Dispatch(
        BuildResolve(MakeResolveRequest(name, replyEndpoint, 0x501U)), MakeMetadata(InvokeRights()), quiescedOutcome);
    ResolveReply quiescedReply{};
    const ObjectDirectoryProtocolError parseError =
        ObjectDirectoryProtocol::ParseResolveReply(quiescedOutcome.PreparedReply, quiescedReply);
    ASTERIA_EXPECT(runner, "quiesced core maps invalid state to InternalFailure reply",
                   parseError == ObjectDirectoryProtocolError::None,
                   quiescedReply.Status == DirectoryStatus::InternalFailure,
                   quiescedFailure.Core == DirectoryCoreError::NotReady, quiescedOutcome.CapabilityCloseCount == 1U,
                   CleanupContains(quiescedOutcome, replyEndpoint));

    ObjectDirectoryServiceCore populated{MakeConfiguration()};
    const CapabilityHandle target{0x6002U, 0xF002U};
    ObjectDirectoryServiceDispatchOutcome bindOutcome{};
    ObjectDirectoryServiceFailure bindFailure{};
    ObjectDirectoryProtocolError bindParse{};
    static_cast<void>(DispatchMutation(
        populated, BuildBind(MakeBindRequest(name, target, CapabilityHandle{0x6003U, 0xF003U}, 0x502U)),
        InvokeManageRights(), bindOutcome, bindFailure, bindParse));
    const DirectoryCoreError beginError = populated.BeginQuiesce();
    CapabilityHandle drained{};
    const DirectoryCoreError drainError = populated.TakeNextOwnedEndpoint(drained);
    ASTERIA_EXPECT(runner, "service exposes exact quiesce drain custody without closing it",
                   beginError == DirectoryCoreError::None, drainError == DirectoryCoreError::None,
                   HandlesAreEqual(drained, target), populated.GetSnapshot().State == DirectoryCoreState::Quiesced,
                   populated.GetSnapshot().BindingCount == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ObjectDirectoryServiceCoreTests"};
    Asteria::Test::TestAuthorityAndCleanup(runner);
    Asteria::Test::TestFourSuccessPathsAndCorrelation(runner);
    Asteria::Test::TestSemanticStatusMappings(runner);
    Asteria::Test::TestResourceAndDuplicateCustodyMappings(runner);
    Asteria::Test::TestMalformedDropsAndOutputAtomicity(runner);
    Asteria::Test::TestQuiescedCoreAndDrain(runner);
    return runner.Finish();
}
