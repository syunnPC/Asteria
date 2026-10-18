# SPDX-License-Identifier: Apache-2.0
"""Execute the production IA-64 AHCI boot decision with a hosted PCI model."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class Ia64AhciBootTests(unittest.TestCase):
    def test_absent_controller_skips_diagnostic_but_present_failure_is_fatal(self):
        source = (ROOT / 'Source/Kernel/Boot/KernelEntry.cpp').read_text()
        begin = source.index('[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool RunIa64AhciIdentifyAndRead(')
        brace = source.index('{', begin)
        depth = 1
        end = brace + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        function = source[begin:end]
        # Compile the actual boot function, retaining its full control flow.
        # The hosted model has a non-AHCI PCI function and a failed DMA-span
        # acquisition for a present controller. It never supplies DMA memory.
        harness = r'''
#include <cstdint>
#define ASTERIA_NOINLINE_NOIPA
namespace Foundation { using UInt8 = std::uint8_t; using UInt64 = std::uint64_t; }
namespace Platform::Device {
enum class AhciPciAvailability { Ready, Absent, EcamUnavailable, Failed };
struct PciEcamRuntime {
    AhciPciAvailability Availability;
    unsigned PublishedFunctionCount{1};
    AhciPciAvailability GetAhciAvailability() const { return Availability; }
};
}
unsigned Diagnostics = 0;
namespace Platform::Firmware { void WriteDiagnosticAscii(const char*) { ++Diagnostics; } }
namespace Kernel::Device {
struct AhciDmaPageWritableView {};
struct AhciReadDmaExtCommandPageWritableView {};
struct AhciReadDmaExtDataBufferSpan {};
}
enum class Ia64AhciReadDiagnosticStage { Prerequisite, Storage, Deadline };
void WriteIa64AhciReadFailureDiagnostic(Ia64AhciReadDiagnosticStage, Foundation::UInt64) { ++Diagnostics; }
unsigned DmaAttempts = 0;
bool TryGetIa64AhciDmaSpans(Kernel::Device::AhciDmaPageWritableView&,
    Kernel::Device::AhciReadDmaExtCommandPageWritableView&,
    Kernel::Device::AhciReadDmaExtDataBufferSpan&) { ++DmaAttempts; return false; }
bool TryGetIa64AhciDeadline(Foundation::UInt64&) { return false; }
enum class Ia64AhciIdentifyBootResult { NoDisk, Succeeded, Failed };
Ia64AhciIdentifyBootResult ExecuteIa64AhciIdentify(Platform::Device::PciEcamRuntime&,
    Kernel::Device::AhciDmaPageWritableView, Foundation::UInt64) { return Ia64AhciIdentifyBootResult::Failed; }
bool FinishIa64AhciIdentifyAndRunRead(Platform::Device::PciEcamRuntime&,
    Kernel::Device::AhciReadDmaExtCommandPageWritableView,
    Kernel::Device::AhciReadDmaExtDataBufferSpan) { return false; }
'''
        harness += function + r'''
int main() {
    using Platform::Device::AhciPciAvailability;
    Platform::Device::PciEcamRuntime absent{AhciPciAvailability::Absent};
    if (!RunIa64AhciIdentifyAndRead(absent) || DmaAttempts != 0 || Diagnostics != 1) { return 1; }
    Platform::Device::PciEcamRuntime failed{AhciPciAvailability::Failed};
    if (RunIa64AhciIdentifyAndRead(failed) || DmaAttempts != 0) { return 2; }
    Platform::Device::PciEcamRuntime present{AhciPciAvailability::Ready};
    if (RunIa64AhciIdentifyAndRead(present) || DmaAttempts != 1) { return 3; }
}
'''
        scratch = ROOT / 'Build/Tmp'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            cpp = pathlib.Path(directory) / 'boot.cpp'
            executable = pathlib.Path(directory) / 'boot'
            cpp.write_text(harness)
            build = subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++23', str(cpp), '-o', str(executable)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, 'AHCI absence must skip DMA; present failures must refuse boot')
