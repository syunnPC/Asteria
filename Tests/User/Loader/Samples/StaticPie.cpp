// SPDX-License-Identifier: Apache-2.0
// A freestanding link probe, never executed on the host. The pointer table
// forces data relocations and the callback forces an IA-64.opd initializer.
namespace Asteria::TestSample
{
unsigned long long Value{7U};
unsigned long long* Table[]{&Value, &Value};

[[nodiscard]] unsigned long long Function() noexcept
{
    return *Table[0U];
}

unsigned long long (*Pointer)() noexcept {Function};
} // Namespace Asteria::TestSample

extern "C" void Entry() noexcept
{
    Asteria::TestSample::Value = Asteria::TestSample::Pointer();
}
