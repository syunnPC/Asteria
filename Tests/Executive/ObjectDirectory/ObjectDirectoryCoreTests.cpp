// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Executive/ObjectDirectory/ObjectDirectoryCore.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Executive::ObjectDirectory::BindingVersion;
using Executive::ObjectDirectory::DirectoryBindingCandidate;
using Executive::ObjectDirectory::DirectoryCoreConfiguration;
using Executive::ObjectDirectory::DirectoryCoreError;
using Executive::ObjectDirectory::DirectoryCoreSnapshot;
using Executive::ObjectDirectory::DirectoryCoreState;
using Executive::ObjectDirectory::DirectoryMutationReceipt;
using Executive::ObjectDirectory::DirectoryName;
using Executive::ObjectDirectory::DirectoryNameError;
using Executive::ObjectDirectory::DirectoryNameView;
using Executive::ObjectDirectory::DirectoryResolution;
using Executive::ObjectDirectory::DirectoryResolveQuery;
using Executive::ObjectDirectory::InterfaceVersionRange;
using Executive::ObjectDirectory::ObjectDirectoryCore;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt64;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectType;

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

[[nodiscard]] DirectoryName MakeIndexedName(const Size index) noexcept
{
    const UInt8 bytes[]{
        static_cast<UInt8>('n'),
        static_cast<UInt8>('0' + ((index / 10U) % 10U)),
        static_cast<UInt8>('0' + (index % 10U)),
    };
    const auto result = DirectoryName::Create(DirectoryNameView{
        .Data = bytes,
        .SizeBytes = sizeof(bytes),
    });
    return result.GetValue();
}

[[nodiscard]] CapabilityRights MakePublishedRights(const bool includeRead = true,
                                                   const bool includeWrite = false) noexcept
{
    CapabilityRights rights{CapabilityRight::Invoke};
    if (includeRead)
    {
        rights = rights.With(CapabilityRight::Read);
    }
    if (includeWrite)
    {
        rights = rights.With(CapabilityRight::Write);
    }
    return rights;
}

[[nodiscard]] DirectoryBindingCandidate
MakeCandidate(const UInt64 selector, const UInt64 authenticator = 0xA500U, const UInt64 interfaceLow = 0x1111U,
              const UInt64 interfaceHigh = 0x2222U, const UInt64 serviceGeneration = 1U, const UInt16 major = 1U,
              const UInt16 minimumMinor = 2U, const UInt16 maximumMinor = 7U, const UInt64 targetDeadline = 1'000U,
              const CapabilityRights publishedRights = MakePublishedRights()) noexcept
{
    return DirectoryBindingCandidate{
        .Custody = CapabilityHandle{selector, authenticator},
        .ServiceInterface = InterfaceIdentifier{interfaceLow, interfaceHigh},
        .ServiceGeneration = serviceGeneration,
        .TargetDeadline = MonotonicTime{targetDeadline},
        .PublishedRights = publishedRights,
        .CustodyRights = publishedRights.With(CapabilityRight::Duplicate),
        .Type = ObjectType::Endpoint,
        .Reserved = {},
        .Version =
            InterfaceVersionRange{
                .Major = major,
                .MinimumMinor = minimumMinor,
                .MaximumMinor = maximumMinor,
                .Reserved = 0U,
            },
    };
}

[[nodiscard]] DirectoryResolveQuery
MakeQuery(const UInt64 interfaceLow = 0x1111U, const UInt64 interfaceHigh = 0x2222U, const UInt16 major = 1U,
          const UInt16 minimumMinor = 4U, const UInt16 maximumMinor = 9U, const UInt64 requestedDeadline = 900U,
          const CapabilityRights requiredRights = CapabilityRights{CapabilityRight::Invoke}) noexcept
{
    return DirectoryResolveQuery{
        .ServiceInterface = InterfaceIdentifier{interfaceLow, interfaceHigh},
        .RequestedDeadline = MonotonicTime{requestedDeadline},
        .RequiredVersion =
            InterfaceVersionRange{
                .Major = major,
                .MinimumMinor = minimumMinor,
                .MaximumMinor = maximumMinor,
                .Reserved = 0U,
            },
        .RequiredRights = requiredRights,
        .Reserved = {},
    };
}

[[nodiscard]] bool HandlesAreEqual(const CapabilityHandle left, const CapabilityHandle right) noexcept
{
    return (left.GetSelector() == right.GetSelector()) && (left.GetAuthenticator() == right.GetAuthenticator());
}

[[nodiscard]] bool ReceiptsAreEqual(const DirectoryMutationReceipt& left,
                                    const DirectoryMutationReceipt& right) noexcept
{
    return (left.Version.DirectoryGeneration == right.Version.DirectoryGeneration) &&
           (left.Version.Revision == right.Version.Revision) &&
           HandlesAreEqual(left.DisplacedEndpoint, right.DisplacedEndpoint);
}

[[nodiscard]] bool CandidatesAreEqual(const DirectoryBindingCandidate& left,
                                      const DirectoryBindingCandidate& right) noexcept
{
    bool reservedEqual{true};
    for (Size index{0U}; index < sizeof(left.Reserved); ++index)
    {
        reservedEqual = reservedEqual && (left.Reserved[index] == right.Reserved[index]);
    }
    return HandlesAreEqual(left.Custody, right.Custody) &&
           (left.ServiceInterface.GetLow() == right.ServiceInterface.GetLow()) &&
           (left.ServiceInterface.GetHigh() == right.ServiceInterface.GetHigh()) &&
           (left.ServiceGeneration == right.ServiceGeneration) &&
           (left.TargetDeadline.GetTicks() == right.TargetDeadline.GetTicks()) &&
           left.PublishedRights.IsEqualTo(right.PublishedRights) && left.CustodyRights.IsEqualTo(right.CustodyRights) &&
           (left.Type == right.Type) && reservedEqual && (left.Version.Major == right.Version.Major) &&
           (left.Version.MinimumMinor == right.Version.MinimumMinor) &&
           (left.Version.MaximumMinor == right.Version.MaximumMinor) &&
           (left.Version.Reserved == right.Version.Reserved);
}

[[nodiscard]] bool ResolutionsAreEqual(const DirectoryResolution& left, const DirectoryResolution& right) noexcept
{
    return (left.Version.DirectoryGeneration == right.Version.DirectoryGeneration) &&
           (left.Version.Revision == right.Version.Revision) &&
           (left.ServiceInterface.GetLow() == right.ServiceInterface.GetLow()) &&
           (left.ServiceInterface.GetHigh() == right.ServiceInterface.GetHigh()) &&
           (left.ServiceGeneration == right.ServiceGeneration) &&
           (left.GrantedDeadline.GetTicks() == right.GrantedDeadline.GetTicks()) &&
           left.GrantedRights.IsEqualTo(right.GrantedRights) && (left.Major == right.Major) &&
           (left.SelectedMinor == right.SelectedMinor) && (left.Reserved == right.Reserved) &&
           HandlesAreEqual(left.TargetEndpoint, right.TargetEndpoint);
}

[[nodiscard]] bool SnapshotsAreEqual(const DirectoryCoreSnapshot& left, const DirectoryCoreSnapshot& right) noexcept
{
    bool reservedEqual{true};
    for (Size index{0U}; index < sizeof(left.Reserved); ++index)
    {
        reservedEqual = reservedEqual && (left.Reserved[index] == right.Reserved[index]);
    }
    return (left.BindingCount == right.BindingCount) && (left.FreeSlotCount == right.FreeSlotCount) &&
           (left.DirectoryGeneration == right.DirectoryGeneration) &&
           (left.LastIssuedRevision == right.LastIssuedRevision) && (left.State == right.State) && reservedEqual;
}

void CheckInvalidCandidate(TestContext& runner, const DirectoryBindingCandidate candidateValue,
                           const DirectoryCoreError expectedError, const char* const description) noexcept
{
    ObjectDirectoryCore core{DirectoryCoreConfiguration{
        .DirectoryGeneration = 3U,
        .LastIssuedRevision = 0U,
    }};
    DirectoryBindingCandidate candidate = candidateValue;
    const DirectoryBindingCandidate before = candidate;
    DirectoryMutationReceipt output{};
    const DirectoryMutationReceipt outputBefore = output;
    const DirectoryCoreError error = core.BindIfAbsent(MakeName("service", 7U), candidate, output);
    const auto snapshot = core.GetSnapshot();
    ASTERIA_EXPECT(runner, description, error == expectedError, CandidatesAreEqual(candidate, before),
                   ReceiptsAreEqual(output, outputBefore), snapshot.BindingCount == 0U,
                   snapshot.LastIssuedRevision == 0U);
}

void TestNameValidation(TestContext& runner) noexcept
{
    const UInt8 oneByte[]{static_cast<UInt8>('a')};
    const auto nullData = DirectoryName::Create(DirectoryNameView{
        .Data = nullptr,
        .SizeBytes = 1U,
    });
    const auto empty = DirectoryName::Create(DirectoryNameView{
        .Data = oneByte,
        .SizeBytes = 0U,
    });

    UInt8 maximumBytes[DirectoryName::MaximumSizeBytes]{};
    for (UInt8& value : maximumBytes)
    {
        value = static_cast<UInt8>('a');
    }
    const auto maximum = DirectoryName::Create(DirectoryNameView{
        .Data = maximumBytes,
        .SizeBytes = sizeof(maximumBytes),
    });

    UInt8 oversizedBytes[DirectoryName::MaximumSizeBytes + 1U]{};
    for (UInt8& value : oversizedBytes)
    {
        value = static_cast<UInt8>('a');
    }
    const auto oversized = DirectoryName::Create(DirectoryNameView{
        .Data = oversizedBytes,
        .SizeBytes = sizeof(oversizedBytes),
    });

    const UInt8 punctuationBytes[]{
        static_cast<UInt8>('a'), static_cast<UInt8>('.'), static_cast<UInt8>('_'),
        static_cast<UInt8>('-'), static_cast<UInt8>('z'),
    };
    const auto punctuation = DirectoryName::Create(DirectoryNameView{
        .Data = punctuationBytes,
        .SizeBytes = sizeof(punctuationBytes),
    });
    const UInt8 leadingPunctuationBytes[]{
        static_cast<UInt8>('_'),
        static_cast<UInt8>('a'),
    };
    const auto leadingPunctuation = DirectoryName::Create(DirectoryNameView{
        .Data = leadingPunctuationBytes,
        .SizeBytes = sizeof(leadingPunctuationBytes),
    });
    const UInt8 trailingPunctuationBytes[]{
        static_cast<UInt8>('a'),
        static_cast<UInt8>('-'),
    };
    const auto trailingPunctuation = DirectoryName::Create(DirectoryNameView{
        .Data = trailingPunctuationBytes,
        .SizeBytes = sizeof(trailingPunctuationBytes),
    });

    ASTERIA_EXPECT(runner, "directory name storage rejects null empty and oversized input", !nullData.HasValue(),
                   nullData.GetError() == DirectoryNameError::NullData, !empty.HasValue(),
                   empty.GetError() == DirectoryNameError::InvalidSize, !oversized.HasValue(),
                   oversized.GetError() == DirectoryNameError::InvalidSize);
    ASTERIA_EXPECT(runner, "directory names accept the maximum length and interior punctuation", maximum.HasValue(),
                   maximum.GetValue().GetSizeBytes() == DirectoryName::MaximumSizeBytes, punctuation.HasValue(),
                   punctuation.GetValue().GetSizeBytes() == sizeof(punctuationBytes));
    ASTERIA_EXPECT(runner, "directory names reject punctuation at either boundary", !leadingPunctuation.HasValue(),
                   leadingPunctuation.GetError() == DirectoryNameError::InvalidCharacter,
                   !trailingPunctuation.HasValue(),
                   trailingPunctuation.GetError() == DirectoryNameError::InvalidCharacter);
}

void TestConfigurationAndCandidateValidation(TestContext& runner) noexcept
{
    const DirectoryName name = MakeName("service", 7U);
    DirectoryBindingCandidate validCandidate = MakeCandidate(1U);
    DirectoryMutationReceipt output{};
    ObjectDirectoryCore invalid{DirectoryCoreConfiguration{
        .DirectoryGeneration = 0U,
        .LastIssuedRevision = 41U,
    }};
    const DirectoryCoreError invalidError = invalid.BindIfAbsent(name, validCandidate, output);
    const auto invalidSnapshot = invalid.GetSnapshot();
    ASTERIA_EXPECT(runner, "zero directory generation produces a permanently invalid core",
                   invalidError == DirectoryCoreError::InvalidConfiguration, validCandidate.Custody.IsValid(),
                   invalidSnapshot.State == DirectoryCoreState::InvalidConfiguration,
                   invalidSnapshot.BindingCount == 0U, invalidSnapshot.DirectoryGeneration == 0U,
                   invalidSnapshot.LastIssuedRevision == 41U);

    ObjectDirectoryCore ready{DirectoryCoreConfiguration{
        .DirectoryGeneration = 9U,
        .LastIssuedRevision = 17U,
    }};
    const auto readySnapshot = ready.GetSnapshot();
    ASTERIA_EXPECT(runner, "nonzero generation and arbitrary revision seed are accepted",
                   readySnapshot.State == DirectoryCoreState::Ready, readySnapshot.BindingCount == 0U,
                   readySnapshot.FreeSlotCount == ObjectDirectoryCore::MaximumBindings,
                   readySnapshot.DirectoryGeneration == 9U, readySnapshot.LastIssuedRevision == 17U);

    DirectoryBindingCandidate invalidCandidate = MakeCandidate(0U);
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidCandidate,
                          "candidate rejects invalid custody");

    invalidCandidate = MakeCandidate(2U);
    invalidCandidate.Type = ObjectType::Notification;
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidCandidate,
                          "candidate requires Endpoint type");

    invalidCandidate = MakeCandidate(3U);
    invalidCandidate.ServiceInterface = {};
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidInterface,
                          "candidate requires a valid interface");

    invalidCandidate = MakeCandidate(4U);
    invalidCandidate.ServiceGeneration = 0U;
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidCandidate,
                          "candidate requires a service generation");

    invalidCandidate = MakeCandidate(5U);
    invalidCandidate.Version.Major = 0U;
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidVersion,
                          "candidate requires a nonzero major");

    invalidCandidate = MakeCandidate(6U);
    invalidCandidate.Version.MinimumMinor = 8U;
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidVersion,
                          "candidate rejects reversed minor range");

    invalidCandidate = MakeCandidate(7U);
    invalidCandidate.Version.Reserved = 1U;
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidVersion,
                          "candidate rejects version reserved data");

    invalidCandidate = MakeCandidate(8U);
    invalidCandidate.Reserved[1] = 1U;
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidCandidate,
                          "candidate rejects reserved data");

    invalidCandidate = MakeCandidate(9U);
    invalidCandidate.PublishedRights = {};
    invalidCandidate.CustodyRights = CapabilityRights{CapabilityRight::Duplicate};
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidRights,
                          "candidate rejects empty published rights");

    invalidCandidate = MakeCandidate(10U);
    invalidCandidate.PublishedRights = CapabilityRights{CapabilityRight::Read};
    invalidCandidate.CustodyRights = invalidCandidate.PublishedRights.With(CapabilityRight::Duplicate);
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidRights,
                          "candidate published rights require Invoke");

    invalidCandidate = MakeCandidate(11U);
    invalidCandidate.CustodyRights = invalidCandidate.PublishedRights;
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidRights,
                          "candidate custody requires Duplicate");

    invalidCandidate = MakeCandidate(12U);
    invalidCandidate.CustodyRights = invalidCandidate.CustodyRights.With(CapabilityRight::Manage);
    CheckInvalidCandidate(runner, invalidCandidate, DirectoryCoreError::InvalidRights,
                          "candidate custody rejects extra rights");
}

void TestNamesBindAndResolve(TestContext& runner) noexcept
{
    const UInt8 uppercaseBytes[]{
        static_cast<UInt8>('S'), static_cast<UInt8>('e'), static_cast<UInt8>('r'), static_cast<UInt8>('v'),
        static_cast<UInt8>('i'), static_cast<UInt8>('c'), static_cast<UInt8>('e'),
    };
    const auto uppercase = DirectoryName::Create(DirectoryNameView{
        .Data = uppercaseBytes,
        .SizeBytes = sizeof(uppercaseBytes),
    });
    ASTERIA_EXPECT(runner, "directory names reject uppercase instead of folding case", !uppercase.HasValue(),
                   uppercase.GetError() == DirectoryNameError::InvalidCharacter);

    ObjectDirectoryCore core{DirectoryCoreConfiguration{
        .DirectoryGeneration = 55U,
        .LastIssuedRevision = 0U,
    }};
    const DirectoryName name = MakeName("service", 7U);
    DirectoryBindingCandidate candidate = MakeCandidate(21U, 0xBEEFU);
    const DirectoryBindingCandidate candidateBefore = candidate;
    DirectoryMutationReceipt receipt;
    const DirectoryCoreError bindError = core.BindIfAbsent(name, candidate, receipt);
    ASTERIA_EXPECT(runner, "bind adopts custody and publishes the first global revision",
                   bindError == DirectoryCoreError::None, !candidate.Custody.IsValid(),
                   candidate.Custody.GetAuthenticator() == 0U, receipt.Version.DirectoryGeneration == 55U,
                   receipt.Version.Revision == 1U, !receipt.DisplacedEndpoint.IsValid(),
                   core.GetSnapshot().BindingCount == 1U);

    DirectoryBindingCandidate duplicateName = MakeCandidate(22U);
    const DirectoryBindingCandidate duplicateNameBefore = duplicateName;
    DirectoryMutationReceipt duplicateOutput{};
    ASTERIA_EXPECT(runner, "bind is create-only and duplicate name failure is atomic",
                   core.BindIfAbsent(name, duplicateName, duplicateOutput) == DirectoryCoreError::AlreadyBound,
                   CandidatesAreEqual(duplicateName, duplicateNameBefore), core.GetSnapshot().LastIssuedRevision == 1U);

    DirectoryResolveQuery query = MakeQuery();
    DirectoryResolution resolution;
    const DirectoryCoreError resolveError = core.Resolve(name, query, resolution);
    ASTERIA_EXPECT(runner, "resolve selects the highest intersecting minor and exactly attenuates authority",
                   resolveError == DirectoryCoreError::None,
                   HandlesAreEqual(resolution.TargetEndpoint, candidateBefore.Custody),
                   resolution.Version.DirectoryGeneration == 55U, resolution.Version.Revision == 1U,
                   resolution.ServiceGeneration == 1U, resolution.Major == 1U, resolution.SelectedMinor == 7U,
                   resolution.GrantedRights.IsEqualTo(query.RequiredRights),
                   resolution.GrantedDeadline.GetTicks() == 900U, core.GetSnapshot().LastIssuedRevision == 1U);

    DirectoryResolution missing{};
    ASTERIA_EXPECT(runner, "name lookup is exact within one shared node",
                   core.Resolve(MakeName("service2", 8U), query, missing) == DirectoryCoreError::NotFound);

    DirectoryResolution sentinel{
        .Version = BindingVersion{91U, 92U},
        .ServiceInterface = InterfaceIdentifier{93U, 94U},
        .ServiceGeneration = 95U,
        .GrantedDeadline = MonotonicTime{96U},
        .GrantedRights = CapabilityRights{CapabilityRight::Invoke},
        .Major = 97U,
        .SelectedMinor = 98U,
        .Reserved = 0U,
        .TargetEndpoint = CapabilityHandle{99U, 100U},
    };
    const DirectoryResolution sentinelBefore = sentinel;
    ASTERIA_EXPECT(runner, "resolve rejects an occupied output without mutation",
                   core.Resolve(name, query, sentinel) == DirectoryCoreError::OutputOccupied,
                   ResolutionsAreEqual(sentinel, sentinelBefore));

    DirectoryResolution incompatibleOutput{};
    DirectoryResolveQuery incompatible = query;
    incompatible.ServiceInterface = InterfaceIdentifier{0x3333U, 0x4444U};
    ASTERIA_EXPECT(runner, "resolve rejects an incompatible interface",
                   core.Resolve(name, incompatible, incompatibleOutput) == DirectoryCoreError::InvalidInterface);

    incompatible = query;
    incompatible.RequiredVersion.Major = 2U;
    ASTERIA_EXPECT(runner, "resolve rejects an incompatible major",
                   core.Resolve(name, incompatible, incompatibleOutput) == DirectoryCoreError::InvalidVersion);

    incompatible = query;
    incompatible.RequiredVersion.MinimumMinor = 8U;
    incompatible.RequiredVersion.MaximumMinor = 9U;
    ASTERIA_EXPECT(runner, "resolve rejects a disjoint minor range",
                   core.Resolve(name, incompatible, incompatibleOutput) == DirectoryCoreError::InvalidVersion);

    incompatible = query;
    incompatible.RequiredRights = MakePublishedRights(true, true);
    ASTERIA_EXPECT(runner, "resolve rejects rights outside the published set",
                   core.Resolve(name, incompatible, incompatibleOutput) == DirectoryCoreError::InvalidRights);

    incompatible = query;
    incompatible.RequestedDeadline = MonotonicTime{1'001U};
    ASTERIA_EXPECT(runner, "resolve rejects a lease beyond a finite target ceiling",
                   core.Resolve(name, incompatible, incompatibleOutput) == DirectoryCoreError::LeaseUnavailable);

    incompatible = query;
    incompatible.RequestedDeadline = MonotonicTime::Infinite();
    ASTERIA_EXPECT(runner, "resolve rejects an infinite lease against a finite target ceiling",
                   core.Resolve(name, incompatible, incompatibleOutput) == DirectoryCoreError::LeaseUnavailable);

    incompatible = query;
    incompatible.RequiredRights = {};
    ASTERIA_EXPECT(runner, "resolve rejects empty rights",
                   core.Resolve(name, incompatible, incompatibleOutput) == DirectoryCoreError::InvalidRights);

    incompatible = query;
    incompatible.RequiredVersion.Reserved = 1U;
    ASTERIA_EXPECT(runner, "resolve rejects nonzero reserved version data",
                   core.Resolve(name, incompatible, incompatibleOutput) == DirectoryCoreError::InvalidVersion);

    incompatible = query;
    incompatible.ServiceInterface = {};
    ASTERIA_EXPECT(runner, "resolve rejects an invalid required interface",
                   core.Resolve(name, incompatible, incompatibleOutput) == DirectoryCoreError::InvalidInterface);
}

void TestPublicFailureBranches(TestContext& runner) noexcept
{
    const DirectoryName name = MakeName("service", 7U);
    const DirectoryName secondName = MakeName("secondary", 9U);
    const DirectoryName missingName = MakeName("missing", 7U);
    const DirectoryName invalidName{};
    ObjectDirectoryCore core{DirectoryCoreConfiguration{60U, 0U}};

    DirectoryBindingCandidate initial = MakeCandidate(200U, 300U);
    DirectoryMutationReceipt initialReceipt{};
    static_cast<void>(core.BindIfAbsent(name, initial, initialReceipt));
    DirectoryBindingCandidate secondary = MakeCandidate(201U, 301U);
    const DirectoryBindingCandidate secondaryBefore = secondary;
    DirectoryMutationReceipt secondaryReceipt{};
    static_cast<void>(core.BindIfAbsent(secondName, secondary, secondaryReceipt));
    const DirectoryCoreSnapshot stableSnapshot = core.GetSnapshot();

    DirectoryBindingCandidate invalidNameCandidate = MakeCandidate(202U, 302U);
    const DirectoryBindingCandidate invalidNameCandidateBefore = invalidNameCandidate;
    DirectoryMutationReceipt invalidNameOutput{};
    ASTERIA_EXPECT(runner, "bind rejects an invalid canonical name before custody adoption",
                   core.BindIfAbsent(invalidName, invalidNameCandidate, invalidNameOutput) ==
                       DirectoryCoreError::InvalidName,
                   CandidatesAreEqual(invalidNameCandidate, invalidNameCandidateBefore),
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    DirectoryBindingCandidate replaceCandidate = MakeCandidate(203U, 303U);
    const DirectoryBindingCandidate replaceCandidateBefore = replaceCandidate;
    DirectoryMutationReceipt replaceOutput{};
    ASTERIA_EXPECT(runner, "replace rejects an invalid canonical name atomically",
                   core.ReplaceIfVersion(invalidName, initialReceipt.Version, replaceCandidate, replaceOutput) ==
                       DirectoryCoreError::InvalidName,
                   CandidatesAreEqual(replaceCandidate, replaceCandidateBefore),
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    ASTERIA_EXPECT(runner, "replace rejects a zero compare-and-swap version atomically",
                   core.ReplaceIfVersion(name, BindingVersion{}, replaceCandidate, replaceOutput) ==
                       DirectoryCoreError::InvalidVersion,
                   CandidatesAreEqual(replaceCandidate, replaceCandidateBefore),
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    DirectoryBindingCandidate malformedCandidate = MakeCandidate(204U, 304U);
    malformedCandidate.Type = ObjectType::Notification;
    const DirectoryBindingCandidate malformedCandidateBefore = malformedCandidate;
    ASTERIA_EXPECT(runner, "replace validates a candidate before lookup or custody adoption",
                   core.ReplaceIfVersion(name, initialReceipt.Version, malformedCandidate, replaceOutput) ==
                       DirectoryCoreError::InvalidCandidate,
                   CandidatesAreEqual(malformedCandidate, malformedCandidateBefore),
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    DirectoryMutationReceipt occupiedReplaceOutput{
        .DisplacedEndpoint = CapabilityHandle{0U, 900U},
    };
    const DirectoryMutationReceipt occupiedReplaceBefore = occupiedReplaceOutput;
    ASTERIA_EXPECT(runner, "replace detects an authenticator-only occupied output atomically",
                   core.ReplaceIfVersion(name, initialReceipt.Version, replaceCandidate, occupiedReplaceOutput) ==
                       DirectoryCoreError::OutputOccupied,
                   CandidatesAreEqual(replaceCandidate, replaceCandidateBefore),
                   ReceiptsAreEqual(occupiedReplaceOutput, occupiedReplaceBefore),
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    ASTERIA_EXPECT(runner, "replace reports an absent name without consuming custody",
                   core.ReplaceIfVersion(missingName, initialReceipt.Version, replaceCandidate, replaceOutput) ==
                       DirectoryCoreError::NotFound,
                   CandidatesAreEqual(replaceCandidate, replaceCandidateBefore),
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    DirectoryBindingCandidate alreadyOwned =
        MakeCandidate(secondaryBefore.Custody.GetSelector(), secondaryBefore.Custody.GetAuthenticator());
    const DirectoryBindingCandidate alreadyOwnedBefore = alreadyOwned;
    ASTERIA_EXPECT(runner, "replace cannot alias custody already held by another binding",
                   core.ReplaceIfVersion(name, initialReceipt.Version, alreadyOwned, replaceOutput) ==
                       DirectoryCoreError::CapabilityAlreadyOwned,
                   CandidatesAreEqual(alreadyOwned, alreadyOwnedBefore),
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    DirectoryMutationReceipt removeOutput{};
    ASTERIA_EXPECT(runner, "remove rejects an invalid canonical name atomically",
                   core.RemoveIfVersion(invalidName, initialReceipt.Version, removeOutput) ==
                       DirectoryCoreError::InvalidName,
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));
    ASTERIA_EXPECT(runner, "remove rejects a zero compare-and-swap version atomically",
                   core.RemoveIfVersion(name, BindingVersion{}, removeOutput) == DirectoryCoreError::InvalidVersion,
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    DirectoryMutationReceipt occupiedRemoveOutput{
        .Version = BindingVersion{1U, 0U},
    };
    const DirectoryMutationReceipt occupiedRemoveBefore = occupiedRemoveOutput;
    ASTERIA_EXPECT(runner, "remove rejects an occupied output atomically",
                   core.RemoveIfVersion(name, initialReceipt.Version, occupiedRemoveOutput) ==
                       DirectoryCoreError::OutputOccupied,
                   ReceiptsAreEqual(occupiedRemoveOutput, occupiedRemoveBefore),
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));
    ASTERIA_EXPECT(runner, "remove reports an absent name without issuing a revision",
                   core.RemoveIfVersion(missingName, initialReceipt.Version, removeOutput) ==
                       DirectoryCoreError::NotFound,
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    DirectoryResolveQuery query = MakeQuery();
    DirectoryResolution resolution{};
    ASTERIA_EXPECT(runner, "resolve rejects an invalid canonical name",
                   core.Resolve(invalidName, query, resolution) == DirectoryCoreError::InvalidName,
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    query.RequiredVersion.Major = 0U;
    ASTERIA_EXPECT(runner, "resolve rejects a zero required major before lookup",
                   core.Resolve(name, query, resolution) == DirectoryCoreError::InvalidVersion);
    query = MakeQuery();
    query.RequiredVersion.MinimumMinor = 10U;
    query.RequiredVersion.MaximumMinor = 9U;
    ASTERIA_EXPECT(runner, "resolve rejects a reversed required minor range before lookup",
                   core.Resolve(name, query, resolution) == DirectoryCoreError::InvalidVersion);

    bool reservedRejected{true};
    for (Size index{0U}; index < sizeof(query.Reserved); ++index)
    {
        query = MakeQuery();
        query.Reserved[index] = 1U;
        DirectoryResolution reservedOutput{};
        reservedRejected =
            reservedRejected && (core.Resolve(name, query, reservedOutput) == DirectoryCoreError::InvalidVersion);
    }
    ASTERIA_EXPECT(runner, "resolve rejects every nonzero reserved query byte", reservedRejected);

    DirectoryMutationReceipt occupiedMutationCases[]{
        {.Version = BindingVersion{1U, 0U}},
        {.Version = BindingVersion{0U, 1U}},
        {.DisplacedEndpoint = CapabilityHandle{1U, 0U}},
        {.DisplacedEndpoint = CapabilityHandle{0U, 1U}},
    };
    bool mutationFieldsDetected{true};
    for (DirectoryMutationReceipt& occupied : occupiedMutationCases)
    {
        DirectoryBindingCandidate candidate = MakeCandidate(205U, 305U);
        const DirectoryBindingCandidate candidateBefore = candidate;
        const DirectoryMutationReceipt outputBefore = occupied;
        mutationFieldsDetected =
            mutationFieldsDetected &&
            (core.BindIfAbsent(missingName, candidate, occupied) == DirectoryCoreError::OutputOccupied) &&
            CandidatesAreEqual(candidate, candidateBefore) && ReceiptsAreEqual(occupied, outputBefore);
    }
    ASTERIA_EXPECT(runner, "every mutation output field participates in pristine checking", mutationFieldsDetected,
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    DirectoryResolution occupiedResolutionCases[]{
        {.Version = BindingVersion{1U, 0U}},
        {.Version = BindingVersion{0U, 1U}},
        {.ServiceInterface = InterfaceIdentifier{1U, 0U}},
        {.ServiceInterface = InterfaceIdentifier{0U, 1U}},
        {.ServiceGeneration = 1U},
        {.GrantedDeadline = MonotonicTime{1U}},
        {.GrantedRights = CapabilityRights{CapabilityRight::Invoke}},
        {.Major = 1U},
        {.SelectedMinor = 1U},
        {.Reserved = 1U},
        {.TargetEndpoint = CapabilityHandle{1U, 0U}},
        {.TargetEndpoint = CapabilityHandle{0U, 1U}},
    };
    bool resolutionFieldsDetected{true};
    for (DirectoryResolution& occupied : occupiedResolutionCases)
    {
        const DirectoryResolution outputBefore = occupied;
        resolutionFieldsDetected = resolutionFieldsDetected &&
                                   (core.Resolve(name, MakeQuery(), occupied) == DirectoryCoreError::OutputOccupied) &&
                                   ResolutionsAreEqual(occupied, outputBefore);
    }
    ASTERIA_EXPECT(runner, "every resolution output field participates in pristine checking", resolutionFieldsDetected,
                   SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    CapabilityHandle prematureDrain{};
    ASTERIA_EXPECT(runner, "ready directory ownership cannot be drained before quiesce",
                   core.TakeNextOwnedEndpoint(prematureDrain) == DirectoryCoreError::NotReady,
                   !prematureDrain.IsValid(), SnapshotsAreEqual(core.GetSnapshot(), stableSnapshot));

    ObjectDirectoryCore invalid{DirectoryCoreConfiguration{0U, 99U}};
    DirectoryBindingCandidate invalidReplace = MakeCandidate(206U, 306U);
    DirectoryMutationReceipt invalidMutationOutput{};
    DirectoryResolution invalidResolutionOutput{};
    CapabilityHandle invalidDrainOutput{};
    ASTERIA_EXPECT(runner, "all remaining public operations reject an invalid configuration",
                   invalid.ReplaceIfVersion(name, BindingVersion{1U, 1U}, invalidReplace, invalidMutationOutput) ==
                       DirectoryCoreError::InvalidConfiguration,
                   invalid.RemoveIfVersion(name, BindingVersion{1U, 1U}, invalidMutationOutput) ==
                       DirectoryCoreError::InvalidConfiguration,
                   invalid.Resolve(name, MakeQuery(), invalidResolutionOutput) ==
                       DirectoryCoreError::InvalidConfiguration,
                   invalid.BeginQuiesce() == DirectoryCoreError::InvalidConfiguration,
                   invalid.TakeNextOwnedEndpoint(invalidDrainOutput) == DirectoryCoreError::InvalidConfiguration);
}

void TestCompareAndSwapReplacement(TestContext& runner) noexcept
{
    ObjectDirectoryCore core{DirectoryCoreConfiguration{70U, 0U}};
    const DirectoryName name = MakeName("renderer", 8U);
    DirectoryBindingCandidate initial = MakeCandidate(30U);
    DirectoryMutationReceipt bindReceipt{};
    static_cast<void>(core.BindIfAbsent(name, initial, bindReceipt));

    DirectoryBindingCandidate firstContender =
        MakeCandidate(31U, 0xB501U, 0x9001U, 0x9002U, 44U, 3U, 10U, 20U, 800U, MakePublishedRights(true, true));
    const DirectoryBindingCandidate contenderBefore = firstContender;
    DirectoryMutationReceipt staleOutput{};
    ASTERIA_EXPECT(runner, "replacement rejects a stale revision without adopting the contender",
                   core.ReplaceIfVersion(name, BindingVersion{70U, 999U}, firstContender, staleOutput) ==
                       DirectoryCoreError::BindingChanged,
                   CandidatesAreEqual(firstContender, contenderBefore), core.GetSnapshot().LastIssuedRevision == 1U);

    DirectoryMutationReceipt replaceReceipt{};
    const DirectoryCoreError replaceError =
        core.ReplaceIfVersion(name, bindReceipt.Version, firstContender, replaceReceipt);
    ASTERIA_EXPECT(runner, "replacement atomically swaps complete metadata and returns old custody",
                   replaceError == DirectoryCoreError::None, !firstContender.Custody.IsValid(),
                   replaceReceipt.Version.DirectoryGeneration == 70U, replaceReceipt.Version.Revision == 2U,
                   HandlesAreEqual(replaceReceipt.DisplacedEndpoint, CapabilityHandle{30U, 0xA500U}));

    DirectoryBindingCandidate secondContender = MakeCandidate(32U);
    const DirectoryBindingCandidate secondBefore = secondContender;
    DirectoryMutationReceipt lostRaceOutput{};
    ASTERIA_EXPECT(runner, "only one of two contenders can consume the same CAS token",
                   core.ReplaceIfVersion(name, bindReceipt.Version, secondContender, lostRaceOutput) ==
                       DirectoryCoreError::BindingChanged,
                   CandidatesAreEqual(secondContender, secondBefore), core.GetSnapshot().LastIssuedRevision == 2U);

    DirectoryResolveQuery changedQuery =
        MakeQuery(0x9001U, 0x9002U, 3U, 15U, 30U, 700U, MakePublishedRights(true, true));
    DirectoryResolution changed{};
    ASTERIA_EXPECT(runner, "replacement may change interface version generation rights and deadline metadata",
                   core.Resolve(name, changedQuery, changed) == DirectoryCoreError::None,
                   changed.ServiceGeneration == 44U, changed.Major == 3U, changed.SelectedMinor == 20U,
                   changed.Version.Revision == 2U, HandlesAreEqual(changed.TargetEndpoint, contenderBefore.Custody));

    DirectoryBindingCandidate wrongGeneration = MakeCandidate(33U);
    DirectoryMutationReceipt wrongGenerationOutput{};
    ASTERIA_EXPECT(runner, "replacement CAS includes the configured directory generation",
                   core.ReplaceIfVersion(name, BindingVersion{71U, 2U}, wrongGeneration, wrongGenerationOutput) ==
                       DirectoryCoreError::BindingChanged);

    DirectoryBindingCandidate sameCustody =
        MakeCandidate(contenderBefore.Custody.GetSelector(), contenderBefore.Custody.GetAuthenticator(), 0x9001U,
                      0x9002U, 45U, 3U, 10U, 20U, 700U, MakePublishedRights(true, true));
    const DirectoryBindingCandidate sameCustodyBefore = sameCustody;
    DirectoryMutationReceipt sameCustodyOutput{};
    ASTERIA_EXPECT(runner, "replacement cannot return and retain the exact same custody handle",
                   core.ReplaceIfVersion(name, replaceReceipt.Version, sameCustody, sameCustodyOutput) ==
                       DirectoryCoreError::CapabilityAlreadyOwned,
                   CandidatesAreEqual(sameCustody, sameCustodyBefore), !sameCustodyOutput.DisplacedEndpoint.IsValid(),
                   core.GetSnapshot().LastIssuedRevision == 2U);
}

void TestRemovalRebindAndSlotReuse(TestContext& runner) noexcept
{
    ObjectDirectoryCore core{DirectoryCoreConfiguration{80U, 0U}};
    const DirectoryName firstName = MakeName("alpha", 5U);
    const DirectoryName secondName = MakeName("beta", 4U);
    DirectoryBindingCandidate first = MakeCandidate(40U);
    DirectoryBindingCandidate second = MakeCandidate(41U);
    DirectoryMutationReceipt firstReceipt{};
    DirectoryMutationReceipt secondReceipt{};
    static_cast<void>(core.BindIfAbsent(firstName, first, firstReceipt));
    static_cast<void>(core.BindIfAbsent(secondName, second, secondReceipt));

    DirectoryMutationReceipt staleRemove{};
    ASTERIA_EXPECT(runner, "remove rejects a stale binding revision",
                   core.RemoveIfVersion(firstName, BindingVersion{80U, 2U}, staleRemove) ==
                       DirectoryCoreError::BindingChanged);

    DirectoryMutationReceipt removeReceipt{};
    const DirectoryCoreError removeError = core.RemoveIfVersion(firstName, firstReceipt.Version, removeReceipt);
    ASTERIA_EXPECT(runner, "remove issues a revision and returns the exact endpoint for cleanup",
                   removeError == DirectoryCoreError::None, removeReceipt.Version.Revision == 3U,
                   HandlesAreEqual(removeReceipt.DisplacedEndpoint, CapabilityHandle{40U, 0xA500U}),
                   core.GetSnapshot().BindingCount == 1U);

    DirectoryBindingCandidate rebound = MakeCandidate(42U);
    DirectoryMutationReceipt reboundReceipt{};
    ASTERIA_EXPECT(runner, "removed name can be rebound with a fresh revision",
                   core.BindIfAbsent(firstName, rebound, reboundReceipt) == DirectoryCoreError::None,
                   reboundReceipt.Version.Revision == 4U, core.GetSnapshot().BindingCount == 2U);

    static_cast<void>(core.BeginQuiesce());
    CapabilityHandle drained{};
    const DirectoryCoreError firstDrain = core.TakeNextOwnedEndpoint(drained);
    const bool reusedLowestSlot =
        (firstDrain == DirectoryCoreError::None) && HandlesAreEqual(drained, CapabilityHandle{42U, 0xA500U});
    drained = {};
    const DirectoryCoreError secondDrain = core.TakeNextOwnedEndpoint(drained);
    ASTERIA_EXPECT(runner, "bind deterministically reuses the lowest free slot", reusedLowestSlot,
                   secondDrain == DirectoryCoreError::None, HandlesAreEqual(drained, CapabilityHandle{41U, 0xA500U}));
}

void TestCapacityDuplicateAndFullReplacement(TestContext& runner) noexcept
{
    ObjectDirectoryCore core{DirectoryCoreConfiguration{90U, 0U}};
    for (Size index{0U}; index < ObjectDirectoryCore::MaximumBindings; ++index)
    {
        DirectoryBindingCandidate candidate =
            MakeCandidate(1'000U + static_cast<UInt64>(index), 2'000U + static_cast<UInt64>(index));
        DirectoryMutationReceipt output{};
        const DirectoryCoreError error = core.BindIfAbsent(MakeIndexedName(index), candidate, output);
        ASTERIA_EXPECT(runner, "each of 64 fixed directory slots accepts one binding",
                       error == DirectoryCoreError::None, !candidate.Custody.IsValid(),
                       output.Version.Revision == static_cast<UInt64>(index + 1U));
    }
    const auto fullSnapshot = core.GetSnapshot();
    ASTERIA_EXPECT(runner, "snapshot reports the fixed directory at capacity",
                   fullSnapshot.BindingCount == ObjectDirectoryCore::MaximumBindings, fullSnapshot.FreeSlotCount == 0U,
                   fullSnapshot.LastIssuedRevision == 64U);

    DirectoryBindingCandidate overflow = MakeCandidate(9'999U, 8'888U);
    const DirectoryBindingCandidate overflowBefore = overflow;
    DirectoryMutationReceipt overflowOutput{};
    ASTERIA_EXPECT(runner, "65th bind fails atomically without consuming custody or revision",
                   core.BindIfAbsent(MakeName("overflow", 8U), overflow, overflowOutput) ==
                       DirectoryCoreError::CapacityExceeded,
                   CandidatesAreEqual(overflow, overflowBefore), core.GetSnapshot().LastIssuedRevision == 64U);

    DirectoryBindingCandidate duplicate = MakeCandidate(1'001U, 2'001U);
    const DirectoryBindingCandidate duplicateBefore = duplicate;
    DirectoryMutationReceipt duplicateOutput{};
    ASTERIA_EXPECT(runner, "identical selector and authenticator cannot be owned by two entries",
                   core.BindIfAbsent(MakeName("duplicate", 9U), duplicate, duplicateOutput) ==
                       DirectoryCoreError::CapabilityAlreadyOwned,
                   CandidatesAreEqual(duplicate, duplicateBefore));

    DirectoryBindingCandidate replacement = MakeCandidate(7'777U, 6'666U);
    DirectoryMutationReceipt replacementOutput{};
    ASTERIA_EXPECT(runner, "replacement works at capacity without requiring a free slot",
                   core.ReplaceIfVersion(MakeIndexedName(0U), BindingVersion{90U, 1U}, replacement,
                                         replacementOutput) == DirectoryCoreError::None,
                   core.GetSnapshot().BindingCount == ObjectDirectoryCore::MaximumBindings,
                   replacementOutput.Version.Revision == 65U,
                   HandlesAreEqual(replacementOutput.DisplacedEndpoint, CapabilityHandle{1'000U, 2'000U}));
}

void TestFailureAtomicityAndRevisionExhaustion(TestContext& runner) noexcept
{
    ObjectDirectoryCore occupiedCore{DirectoryCoreConfiguration{100U, 0U}};
    DirectoryBindingCandidate occupiedCandidate = MakeCandidate(50U);
    const DirectoryBindingCandidate occupiedBefore = occupiedCandidate;
    DirectoryMutationReceipt occupiedOutput{
        .Version = BindingVersion{700U, 701U},
        .DisplacedEndpoint = CapabilityHandle{702U, 703U},
    };
    const DirectoryMutationReceipt outputBefore = occupiedOutput;
    ASTERIA_EXPECT(runner, "occupied mutation output leaves candidate output and store unchanged",
                   occupiedCore.BindIfAbsent(MakeName("occupied", 8U), occupiedCandidate, occupiedOutput) ==
                       DirectoryCoreError::OutputOccupied,
                   CandidatesAreEqual(occupiedCandidate, occupiedBefore),
                   ReceiptsAreEqual(occupiedOutput, outputBefore), occupiedCore.GetSnapshot().BindingCount == 0U);

    const UInt64 maximumRevision = ~UInt64{0U};
    ObjectDirectoryCore nearExhausted{DirectoryCoreConfiguration{
        .DirectoryGeneration = 101U,
        .LastIssuedRevision = maximumRevision - 1U,
    }};
    DirectoryBindingCandidate lastCandidate = MakeCandidate(51U);
    DirectoryMutationReceipt lastReceipt{};
    ASTERIA_EXPECT(runner, "revision seed near UInt64 maximum makes final revision issuable",
                   nearExhausted.BindIfAbsent(MakeName("last", 4U), lastCandidate, lastReceipt) ==
                       DirectoryCoreError::None,
                   lastReceipt.Version.Revision == maximumRevision);

    DirectoryBindingCandidate exhaustedCandidate = MakeCandidate(52U);
    const DirectoryBindingCandidate exhaustedBefore = exhaustedCandidate;
    DirectoryMutationReceipt exhaustedOutput{};
    ASTERIA_EXPECT(runner, "revision issuance never wraps and exhausted bind is atomic",
                   nearExhausted.BindIfAbsent(MakeName("next", 4U), exhaustedCandidate, exhaustedOutput) ==
                       DirectoryCoreError::RevisionExhausted,
                   CandidatesAreEqual(exhaustedCandidate, exhaustedBefore),
                   nearExhausted.GetSnapshot().BindingCount == 1U,
                   nearExhausted.GetSnapshot().LastIssuedRevision == maximumRevision);

    DirectoryBindingCandidate exhaustedReplacement = MakeCandidate(53U);
    DirectoryMutationReceipt exhaustedReplacementOutput{};
    ASTERIA_EXPECT(runner, "replacement also refuses to wrap the global revision",
                   nearExhausted.ReplaceIfVersion(MakeName("last", 4U), lastReceipt.Version, exhaustedReplacement,
                                                  exhaustedReplacementOutput) == DirectoryCoreError::RevisionExhausted);

    DirectoryMutationReceipt exhaustedRemove{};
    ASTERIA_EXPECT(runner, "removal also refuses to wrap the global revision",
                   nearExhausted.RemoveIfVersion(MakeName("last", 4U), lastReceipt.Version, exhaustedRemove) ==
                       DirectoryCoreError::RevisionExhausted);

    ObjectDirectoryCore fullyExhausted{DirectoryCoreConfiguration{
        .DirectoryGeneration = 102U,
        .LastIssuedRevision = maximumRevision,
    }};
    DirectoryBindingCandidate noRevision = MakeCandidate(54U);
    DirectoryMutationReceipt noRevisionOutput{};
    ASTERIA_EXPECT(runner, "configuration may seed an already exhausted revision counter for testing",
                   fullyExhausted.BindIfAbsent(MakeName("none", 4U), noRevision, noRevisionOutput) ==
                       DirectoryCoreError::RevisionExhausted);
}

void TestQuiesceAndDrain(TestContext& runner) noexcept
{
    ObjectDirectoryCore core{DirectoryCoreConfiguration{110U, 0U}};
    for (Size index{0U}; index < 3U; ++index)
    {
        DirectoryBindingCandidate candidate =
            MakeCandidate(60U + static_cast<UInt64>(index), 70U + static_cast<UInt64>(index));
        DirectoryMutationReceipt output{};
        static_cast<void>(core.BindIfAbsent(MakeIndexedName(index), candidate, output));
    }

    ASTERIA_EXPECT(runner, "begin quiesce closes the core to new operations",
                   core.BeginQuiesce() == DirectoryCoreError::None,
                   core.GetSnapshot().State == DirectoryCoreState::Quiescing);

    DirectoryBindingCandidate rejected = MakeCandidate(80U);
    DirectoryBindingCandidate rejectedReplacement = MakeCandidate(81U);
    DirectoryMutationReceipt rejectedOutput{};
    DirectoryMutationReceipt rejectedReplaceOutput{};
    DirectoryResolution rejectedResolution{};
    ASTERIA_EXPECT(runner, "quiescing prevents bind replace remove and resolve",
                   core.BindIfAbsent(MakeName("closed", 6U), rejected, rejectedOutput) == DirectoryCoreError::NotReady,
                   core.Resolve(MakeIndexedName(0U), MakeQuery(), rejectedResolution) == DirectoryCoreError::NotReady,
                   core.ReplaceIfVersion(MakeIndexedName(0U), BindingVersion{110U, 1U}, rejectedReplacement,
                                         rejectedReplaceOutput) == DirectoryCoreError::NotReady,
                   core.RemoveIfVersion(MakeIndexedName(0U), BindingVersion{110U, 1U}, rejectedOutput) ==
                       DirectoryCoreError::NotReady);

    CapabilityHandle occupied{999U, 1'000U};
    const auto beforeOccupiedDrain = core.GetSnapshot();
    ASTERIA_EXPECT(runner, "drain rejects occupied output without consuming an endpoint",
                   core.TakeNextOwnedEndpoint(occupied) == DirectoryCoreError::OutputOccupied,
                   HandlesAreEqual(occupied, CapabilityHandle{999U, 1'000U}),
                   core.GetSnapshot().BindingCount == beforeOccupiedDrain.BindingCount);

    bool exactOrder{true};
    for (Size index{0U}; index < 3U; ++index)
    {
        CapabilityHandle endpoint{};
        const DirectoryCoreError error = core.TakeNextOwnedEndpoint(endpoint);
        exactOrder = exactOrder && (error == DirectoryCoreError::None) &&
                     HandlesAreEqual(endpoint, CapabilityHandle{60U + static_cast<UInt64>(index),
                                                                70U + static_cast<UInt64>(index)});
    }
    ASTERIA_EXPECT(runner, "drain returns each stored endpoint exactly once then quiesces", exactOrder,
                   core.GetSnapshot().BindingCount == 0U, core.GetSnapshot().State == DirectoryCoreState::Quiesced);

    CapabilityHandle none{};
    ASTERIA_EXPECT(runner, "quiesced core has no endpoint to return and cannot restart",
                   core.TakeNextOwnedEndpoint(none) == DirectoryCoreError::NoOwnedCapability, !none.IsValid(),
                   core.BeginQuiesce() == DirectoryCoreError::NotReady);

    ObjectDirectoryCore empty{DirectoryCoreConfiguration{111U, 0U}};
    ASTERIA_EXPECT(runner, "empty core transitions directly to Quiesced",
                   empty.BeginQuiesce() == DirectoryCoreError::None,
                   empty.GetSnapshot().State == DirectoryCoreState::Quiesced);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ObjectDirectoryCoreTests"};
    Asteria::Test::TestNameValidation(runner);
    Asteria::Test::TestConfigurationAndCandidateValidation(runner);
    Asteria::Test::TestNamesBindAndResolve(runner);
    Asteria::Test::TestPublicFailureBranches(runner);
    Asteria::Test::TestCompareAndSwapReplacement(runner);
    Asteria::Test::TestRemovalRebindAndSlotReuse(runner);
    Asteria::Test::TestCapacityDuplicateAndFullReplacement(runner);
    Asteria::Test::TestFailureAtomicityAndRevisionExhaustion(runner);
    Asteria::Test::TestQuiesceAndDrain(runner);
    return runner.Finish();
}
