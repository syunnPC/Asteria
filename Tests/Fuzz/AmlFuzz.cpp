// SPDX-License-Identifier: Apache-2.0
#include "Acpi/Aml/Facts.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace
{
namespace Aml = Asteria::Acpi::Aml;
// The hosted harness owns reusable scratch. Library::Reset clears all live
// indices before each parse; the determinism replay below verifies reuse.
constinit Aml::Arena g_arena{};
constinit Aml::Resources g_resources{};
void Require(const bool good) noexcept
{
    if (!good)
    {
        std::abort();
    }
}
void Checksum(std::vector<Aml::UInt8>& bytes) noexcept
{
    for (Aml::Size i{0U}; i < 4U; ++i)
    {
        bytes[4U + i] = static_cast<Aml::UInt8>((bytes.size() >> (i * 8U)) & 0xFFU);
    }
    bytes[9U] = 0U;
    Aml::UInt32 checksum{0U};
    for (const Aml::UInt8 byte : bytes)
    {
        checksum = (checksum + byte) & 0xFFU;
    }
    bytes[9U] = static_cast<Aml::UInt8>((256U - checksum) & 0xFFU);
}
void Run(Aml::Library& library, const Aml::Bytes bytes) noexcept
{
    library.Reset();
    static_cast<void>(library.Load(bytes));
    static_cast<void>(library.FinishLoading());
    const auto nodes{library.Used().Nodes};
    Require(nodes <= Aml::MaximumNodes);
    Require(library.Used().SyntaxObjects <= Aml::MaximumSyntax);
    Require(library.Used().LoadOperations <= Aml::MaximumLoadOperations);
    for (Aml::Id id{1U}; id < nodes && id < 64U; ++id)
    {
        const auto* const node{library.GetNode(id)};
        Require(node != nullptr && node->Parent < nodes);
        if (node->Kind == Aml::NodeKind::Name || (node->Kind == Aml::NodeKind::Method && (node->Flags & 7U) == 0U))
        {
            const auto result{library.Evaluate(id, nullptr, 0U, 1000U)};
            Require(result.Operations <= 1000U);
            Require(library.Used().Objects <= Aml::MaximumObjects);
            Require(library.Used().BytesUsed <= Aml::MaximumByteStorage);
            Require(library.Used().Elements <= Aml::MaximumElements);
        }
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    if (size > Aml::MaximumTableBytes - 36U)
    {
        return 0;
    }
    Aml::Library library{g_arena.Get()};
    Run(library, {data, size});
    // Treat mutations as AML bodies too, so checksums never hide the parser.
    std::vector<Aml::UInt8> table(36U, 0U);
    table[0U] = 'D';
    table[1U] = 'S';
    table[2U] = 'D';
    table[3U] = 'T';
    table[8U] = 2U;
    if (size != 0U)
    {
        table.insert(table.end(), data, data + size);
    }
    Checksum(table);
    Run(library, {table.data(), table.size()});
    const auto count{library.Used().Nodes};
    const auto syntax{library.Used().SyntaxObjects};
    const auto diagnostics{library.DiagnosticCount()};
    Run(library, {table.data(), table.size()});
    Require(library.Used().Nodes == count && library.Used().SyntaxObjects == syntax &&
            library.DiagnosticCount() == diagnostics);
    static_cast<void>(Aml::ResourceDecoder::Decode({data, size}, g_resources));
    Require(g_resources.Count <= Aml::MaximumResources);
    for (Aml::Size i{0U}; i < g_resources.Count; ++i)
    {
        const auto& resource{g_resources.Items[i]};
        Require(resource.Raw.Length <= size);
        Require(resource.Raw.Data >= data && resource.Raw.Data + resource.Raw.Length <= data + size);
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    // Project-authored AML bodies and resource bytes from the specification.
    constexpr std::uint8_t name[]{0x08U, 'T', 'E', 'S', 'T', 0x0CU, 1U, 2U, 3U, 4U};
    constexpr std::uint8_t method[]{0x14U, 0x09U, 'T', 'E', 'S', 'T', 0U, 0xA4U, 0x0AU, 42U};
    constexpr std::uint8_t cycle[]{0x08U, 'L', 'O', 'O', 'P', 'L', 'O', 'O', 'P'};
    constexpr std::uint8_t buffer[]{0x08U, 'B', 'U', 'F', 'F', 0x11U, 0x06U, 0x0AU, 3U, 1U, 2U, 3U};
    constexpr std::uint8_t resource[]{0x47U, 1U, 0x60U, 0U, 0x60U, 0U, 1U, 1U, 0x22U, 2U, 0U, 0x79U, 0U};
    emit(name, sizeof(name));
    emit(method, sizeof(method));
    emit(cycle, sizeof(cycle));
    emit(buffer, sizeof(buffer));
    emit(resource, sizeof(resource));
    emit(name, 0U);
}
