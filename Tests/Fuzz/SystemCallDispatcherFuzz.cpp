// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the capability system-call boundary (SystemCallDispatcher).
//
// An input is a sequence of 16-byte records, each one system call of seven
// words against one CapabilitySpace that starts with three minted handles.
// Record layout: number, handle index (bit 7 forges the authenticator),
// argument mode, rights (two bytes), deadline (eight bytes), argument 4,
// argument 5, clock advance. Handles a call creates join the pool.
//
// Properties checked for every call:
// - An error response carries zero in both value words.
// - A number outside the capability surface is UnknownSystemCall.
// - A nonzero reserved argument fails with InvalidArguments and changes no
//   slot.
// - Only a successful Close removes a slot and only a successful Derive or
//   Duplicate adds one.
// - A created handle has no right its source lacks, no later lease, and
//   Revoke only when Derive asked for a child revocation domain; Query shows
//   exactly the requested rights and deadline when the handle may inspect.
// - A handle that revoked its domain is revoked, and a closed one is unknown.

#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"

namespace
{
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
using Asteria::Kernel::Object::CapabilityHandle;
using Asteria::Kernel::Object::CapabilityRight;
using Asteria::Kernel::Object::CapabilityRights;
using Asteria::Kernel::Object::CapabilitySpace;
using Asteria::Kernel::Object::IKernelObject;
using Asteria::Kernel::Object::MonotonicTime;
using Asteria::Kernel::Object::ObjectRegistry;
using Asteria::Kernel::Object::ObjectType;
using Asteria::Kernel::Object::ResourceDomainId;
using Asteria::Kernel::Object::RevocationRegistry;
using Asteria::Kernel::Object::RootCapabilityIssuer;
using Asteria::Kernel::System::SystemCallDeriveChildRevocationDomain;
using Asteria::Kernel::System::SystemCallDispatcher;
using Asteria::Kernel::System::SystemCallError;
using Asteria::Kernel::System::SystemCallNumber;
using Asteria::Kernel::System::SystemCallQueryRightsShift;
using Asteria::Kernel::System::SystemCallRequest;
using Asteria::Kernel::System::SystemCallResponse;
using Asteria::Platform::Synchronization::SpinLock;
using Asteria::TestSupport::ManualClock;

constexpr std::size_t RecordBytes{16U};
constexpr std::size_t PoolCapacity{64U};
constexpr UInt64 InfiniteTicks{~UInt64{0U}};
constexpr UInt16 RevokeBit{static_cast<UInt16>(CapabilityRight::Revoke)};
constexpr UInt16 InspectBit{static_cast<UInt16>(CapabilityRight::Inspect)};

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

class FuzzObject final : public IKernelObject
{
public:
    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return ObjectType::MemoryObject;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{77U};
    }
};

// What the harness knows about a pool handle: its creation-time rights and
// lease, which never change, and what it has observed about its lifetime.
struct PoolEntry
{
    CapabilityHandle Handle;
    UInt16 Rights;
    UInt64 Deadline;
    bool Closed;
    bool Revoked;
};

class Harness final
{
public:
    Harness() noexcept
        : m_registry{m_synchronization}, m_revocations{m_synchronization}, m_issuer{m_registry},
          m_space{m_registry, m_revocations, m_clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        const auto object = m_issuer.CreateObject(m_object);
        const auto first = m_revocations.CreateRoot();
        const auto second = m_revocations.CreateRoot();
        if (!object.HasValue() || !first.HasValue() || !second.HasValue())
        {
            return false;
        }
        const CapabilityRights full = CapabilityRights{CapabilityRight::Inspect}
                                          .With(CapabilityRight::Duplicate)
                                          .With(CapabilityRight::Derive)
                                          .With(CapabilityRight::Revoke)
                                          .With(CapabilityRight::Transfer);
        const CapabilityRights leased =
            CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Derive).With(CapabilityRight::Duplicate);
        const CapabilityRights inspect{CapabilityRight::Inspect};
        return Mint(object.GetValue(), full, first.GetValue(), InfiniteTicks) &&
               Mint(object.GetValue(), leased, second.GetValue(), 1000U) &&
               Mint(object.GetValue(), inspect, first.GetValue(), InfiniteTicks);
    }

    void Run(const UInt8* const record) noexcept
    {
        const UInt8 rawNumber = static_cast<UInt8>(record[0U] % 19U);
        const std::size_t index = (record[1U] & 0x7FU) % m_count;
        const bool forged = (record[1U] & 0x80U) != 0U;
        const UInt8 mode = record[2U];
        const UInt16 rights = static_cast<UInt16>(record[3U] | (UInt16{record[4U]} << 8U));
        UInt64 deadline{0U};
        for (std::size_t byte{0U}; byte < 8U; ++byte)
        {
            deadline |= UInt64{record[5U + byte]} << (8U * byte);
        }
        // Mode bit 0 keeps the deadline small enough to be meaningful against
        // the clock; bit 1 makes it infinite.
        deadline = ((mode & 2U) != 0U) ? InfiniteTicks : (((mode & 1U) != 0U) ? (deadline % 2000U) : deadline);
        const UInt64 argument4 = record[13U];
        const UInt64 argument5 = ((mode & 4U) != 0U) ? record[14U] : 0U;
        m_clock.Set(m_clock.GetTicks() + (record[15U] % 4U));

        PoolEntry& source = m_pool[index];
        CapabilityHandle handle = source.Handle;
        if (forged)
        {
            handle =
                CapabilityHandle{handle.GetSelector(), handle.GetAuthenticator() ^ (UInt64{1U} << (record[14U] % 64U))};
        }
        const bool creation = (rawNumber == 3U) || (rawNumber == 4U);
        const SystemCallRequest request{
            .Number = rawNumber,
            .Arguments =
                {
                    handle.GetSelector(),
                    handle.GetAuthenticator(),
                    creation ? rights : (((mode & 8U) != 0U) ? rights : 0U),
                    creation ? deadline : (((mode & 16U) != 0U) ? deadline : 0U),
                    ((mode & 32U) != 0U) ? argument4 : 0U,
                    argument5,
                },
        };

        const Asteria::Foundation::Size before = m_space.GetActiveCount();
        const SystemCallResponse response = SystemCallDispatcher::Dispatch(m_space, request);
        const Asteria::Foundation::Size after = m_space.GetActiveCount();
        if ((response.Error != SystemCallError::None) && ((response.Value0 != 0U) || (response.Value1 != 0U)))
        {
            Violation("an error response has zero value words");
        }
        // CapabilityGrant (16) needs a CapabilitySpace object, which this
        // space never holds: it can only fail, and changes nothing.
        if (rawNumber == static_cast<UInt8>(SystemCallNumber::CapabilityGrant))
        {
            if ((response.Error == SystemCallError::None) || (after != before))
            {
                Violation("a grant without a destination space fails without mutation");
            }
            return;
        }
        if ((rawNumber == 0U) || (rawNumber > 5U))
        {
            if ((response.Error != SystemCallError::UnknownSystemCall) || (after != before))
            {
                Violation("a number outside the capability surface is unknown and changes nothing");
            }
            return;
        }
        if (HasReservedBits(rawNumber, request))
        {
            if ((response.Error != SystemCallError::InvalidArguments) || (after != before))
            {
                Violation("a nonzero reserved argument fails without mutation");
            }
            return;
        }
        const bool succeeded = response.Error == SystemCallError::None;
        const bool closed = succeeded && (rawNumber == 1U);
        const bool created = succeeded && creation;
        if (after != (before - (closed ? 1U : 0U) + (created ? 1U : 0U)))
        {
            Violation("only Close removes and only Derive or Duplicate adds a slot");
        }
        if (closed)
        {
            source.Closed = true;
            if (SystemCallDispatcher::Dispatch(m_space, Request(SystemCallNumber::CapabilityClose, source.Handle))
                    .Error != SystemCallError::InvalidHandle)
            {
                Violation("a closed handle is unknown");
            }
        }
        if (succeeded && (rawNumber == 5U))
        {
            source.Revoked = true;
            if (SystemCallDispatcher::Dispatch(m_space, Request(SystemCallNumber::CapabilityRevoke, source.Handle))
                    .Error != SystemCallError::Revoked)
            {
                Violation("a handle that revoked its domain is revoked");
            }
        }
        if (created)
        {
            CheckCreated(source, rawNumber == 3U, rights, deadline, argument4 != 0U, response);
        }
    }

private:
    [[nodiscard]] static SystemCallRequest Request(const SystemCallNumber number,
                                                   const CapabilityHandle handle) noexcept
    {
        return SystemCallRequest{
            .Number = static_cast<UInt64>(number),
            .Arguments = {handle.GetSelector(), handle.GetAuthenticator(), 0U, 0U, 0U, 0U},
        };
    }

    [[nodiscard]] static bool HasReservedBits(const UInt8 number, const SystemCallRequest& request) noexcept
    {
        if ((number == 3U) || (number == 4U))
        {
            const UInt64 permitted = (number == 3U) ? SystemCallDeriveChildRevocationDomain : 0U;
            return ((request.Arguments[4U] & ~permitted) != 0U) || (request.Arguments[5U] != 0U);
        }
        return (request.Arguments[2U] | request.Arguments[3U] | request.Arguments[4U] | request.Arguments[5U]) != 0U;
    }

    void CheckCreated(const PoolEntry& source, const bool derive, const UInt16 rights, const UInt64 deadline,
                      const bool child, const SystemCallResponse& response) noexcept
    {
        if ((rights & ~source.Rights) != 0U)
        {
            Violation("a created handle has no right its source lacks");
        }
        if ((source.Deadline != InfiniteTicks) && ((deadline == InfiniteTicks) || (deadline > source.Deadline)))
        {
            Violation("a created handle has no later lease than its source");
        }
        if (((rights & RevokeBit) != 0U) && !(derive && child))
        {
            Violation("only a child-domain Derive carries Revoke");
        }
        const CapabilityHandle handle{response.Value0, response.Value1};
        if ((rights & InspectBit) != 0U)
        {
            const SystemCallResponse queried =
                SystemCallDispatcher::Dispatch(m_space, Request(SystemCallNumber::CapabilityQuery, handle));
            if ((queried.Error != SystemCallError::None) ||
                ((queried.Value0 >> SystemCallQueryRightsShift) != rights) || (queried.Value1 != deadline))
            {
                Violation("Query shows exactly the requested rights and deadline");
            }
        }
        if (m_count < PoolCapacity)
        {
            m_pool[m_count++] = PoolEntry{handle, rights, deadline, false, false};
        }
    }

    [[nodiscard]] bool Mint(const Asteria::Kernel::Object::ObjectId object, const CapabilityRights rights,
                            const Asteria::Kernel::Object::RevocationDomainId domain, const UInt64 deadline) noexcept
    {
        const auto minted = m_issuer.Mint(m_space, object, rights, domain, MonotonicTime{deadline}, 0x1234U);
        if (!minted.HasValue())
        {
            return false;
        }
        m_pool[m_count++] = PoolEntry{minted.GetValue(), rights.GetRawValue(), deadline, false, false};
        return true;
    }

    SpinLock m_synchronization;
    ManualClock m_clock;
    Asteria::TestSupport::TestObjectRegistry m_registry;
    RevocationRegistry m_revocations;
    RootCapabilityIssuer m_issuer;
    CapabilitySpace m_space;
    FuzzObject m_object;
    PoolEntry m_pool[PoolCapacity]{};
    std::size_t m_count{0U};
};

void EmitRecords(void (*const emit)(const std::uint8_t* data, std::size_t size),
                 const UInt8 (*const records)[RecordBytes], const std::size_t count) noexcept
{
    UInt8 bytes[8U * RecordBytes]{};
    for (std::size_t record{0U}; record < count; ++record)
    {
        for (std::size_t byte{0U}; byte < RecordBytes; ++byte)
        {
            bytes[(record * RecordBytes) + byte] = records[record][byte];
        }
    }
    emit(bytes, count * RecordBytes);
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    // Rights bytes: Inspect 0x0001, Duplicate 0x0100, Derive 0x0200,
    // Revoke 0x0400; mode 2 asks for an infinite deadline, mode 34 also
    // passes argument 4.
    constexpr UInt8 Branches[][RecordBytes]{
        {3U, 0U, 34U, 0x01U, 0x07U, 0, 0, 0, 0, 0, 0, 0, 0, 1U, 0U, 0U},
        {5U, 3U, 0U, 0U, 0U, 0, 0, 0, 0, 0, 0, 0, 0, 0U, 0U, 1U},
        {2U, 0U, 0U, 0U, 0U, 0, 0, 0, 0, 0, 0, 0, 0, 0U, 0U, 0U},
        {5U, 0U, 0U, 0U, 0U, 0, 0, 0, 0, 0, 0, 0, 0, 0U, 0U, 0U},
    };
    constexpr UInt8 Leases[][RecordBytes]{
        {4U, 1U, 1U, 0x01U, 0x01U, 0xF4U, 0x01U, 0, 0, 0, 0, 0, 0, 0U, 0U, 0U},
        {3U, 1U, 1U, 0x01U, 0x00U, 0x90U, 0x01U, 0, 0, 0, 0, 0, 0, 0U, 0U, 3U},
        {3U, 1U, 1U, 0x01U, 0x03U, 0xE8U, 0x03U, 0, 0, 0, 0, 0, 0, 0U, 0U, 0U},
        {1U, 3U, 0U, 0U, 0U, 0, 0, 0, 0, 0, 0, 0, 0, 0U, 0U, 0U},
    };
    constexpr UInt8 Reserved[][RecordBytes]{
        {2U, 0U, 32U, 0U, 0U, 0, 0, 0, 0, 0, 0, 0, 0, 2U, 0U, 0U},
        {4U, 0U, 34U, 0x01U, 0x00U, 0, 0, 0, 0, 0, 0, 0, 0, 1U, 0U, 0U},
        {3U, 0U, 6U, 0x01U, 0x00U, 0, 0, 0, 0, 0, 0, 0, 0, 0U, 9U, 0U},
        {9U, 0x82U, 0U, 0U, 0U, 0, 0, 0, 0, 0, 0, 0, 0, 0U, 5U, 0U},
    };
    EmitRecords(emit, Branches, 4U);
    EmitRecords(emit, Leases, 4U);
    EmitRecords(emit, Reserved, 4U);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    Harness harness;
    if (!harness.Initialize())
    {
        Violation("the harness capability space initializes");
    }
    for (std::size_t offset{0U}; (offset + RecordBytes) <= size; offset += RecordBytes)
    {
        harness.Run(data + offset);
    }
    return 0;
}
