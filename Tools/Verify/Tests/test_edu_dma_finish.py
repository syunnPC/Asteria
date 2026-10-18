# SPDX-License-Identifier: Apache-2.0
"""Execute the production edu DMA finish path with syscall responses."""
import os
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class EduDmaFinishTests(unittest.TestCase):
    def test_deferred_detach_uses_owner_drain_but_failed_stop_remains_fatal(self):
        source = (ROOT / 'Source/User/Component/ComponentProgram.cpp').read_text()
        declaration = re.search(r'\[\[nodiscard,\s*gnu::noinline\]\]\s+Foundation::UInt64\s+ConfineDevice\s*\(', source)
        self.assertIsNotNone(declaration)
        begin = declaration.start()
        brace = source.index('{', begin)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        harness = r'''
#include "User/Component/DriverSetup.hpp"
namespace Asteria::User::Component {
using ComponentClock = Foundation::UInt64 (*)() noexcept;
struct DeviceAccess {
    volatile Foundation::UInt32* Registers;
    System::CapabilityHandle WaitSet;
    System::CapabilityHandle Interrupt;
};
struct DmaSetup {
    System::CapabilityHandle Iommu;
    System::CapabilityHandle Target;
    volatile Foundation::UInt64* SourcePage;
    const volatile Foundation::UInt64* TargetPage;
};
Foundation::UInt64 PrepareDma(const System::SystemCallClient&,
    const Foundation::UInt64 (&)[Abi::ComponentBlockWords], DmaSetup&) noexcept { return 0; }
Foundation::UInt64 ConnectInterrupt(const System::SystemCallClient&, System::CapabilityHandle,
    System::CapabilityHandle, DeviceAccess&) noexcept { return 0; }
Foundation::UInt64 CopyThroughDevice(const System::SystemCallClient&, System::CapabilityHandle,
    System::CapabilityHandle, const DeviceAccess&, volatile Foundation::UInt64*,
    const volatile Foundation::UInt64*) noexcept { return 0; }
'''
        harness += source[begin:end] + r'''
}
using namespace Asteria;
struct Kernel {
    Abi::SystemCallError Stop, Detach;
    unsigned Stops{0}, Detaches{0};
    static void Transport(void* context, const Abi::SystemCallRequest* request,
        Abi::SystemCallResponse* response) noexcept {
        auto& kernel = *static_cast<Kernel*>(context);
        *response = {.Error = Abi::SystemCallError::InvalidHandle, .Value0 = 0, .Value1 = 0};
        if (request->Number == static_cast<Foundation::UInt64>(Abi::SystemCallNumber::DeviceControl)
            && request->Arguments[2] == Abi::DeviceDisableBusMaster) {
            ++kernel.Stops; response->Error = kernel.Stop;
        } else if (request->Number == static_cast<Foundation::UInt64>(Abi::SystemCallNumber::IommuControl)
            && request->Arguments[2] == Abi::IommuDetach) {
            ++kernel.Detaches; response->Error = kernel.Detach;
        }
    }
};
int main() {
    Foundation::UInt64 block[Abi::ComponentBlockWords]{};
    const Abi::SystemCallError answers[]{Abi::SystemCallError::None, Abi::SystemCallError::WouldBlock,
        Abi::SystemCallError::RightsDenied, Abi::SystemCallError::InvalidHandle};
    for (unsigned index = 0; index < 4; ++index) {
        Kernel kernel{Abi::SystemCallError::None, answers[index]};
        const User::System::SystemCallClient client{{Kernel::Transport, &kernel}};
        const auto result = User::Component::ConfineDevice(client, block, nullptr);
        if ((result == 0) != (index < 2) || kernel.Stops != 1 || kernel.Detaches != 1) { return 1; }
    }
    Kernel failed{Abi::SystemCallError::WouldBlock, Abi::SystemCallError::None};
    const User::System::SystemCallClient client{{Kernel::Transport, &failed}};
    if (User::Component::ConfineDevice(client, block, nullptr) == 0 || failed.Detaches != 0) { return 2; }
}
'''
        scratch = ROOT / 'Build/Tmp'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            cpp = pathlib.Path(directory) / 'finish.cpp'
            executable = pathlib.Path(directory) / 'finish'
            cpp.write_text(harness)
            build = subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++23', '-I', str(ROOT / 'Source'),
                                    str(cpp), str(ROOT / 'Source/User/System/SystemCallClient.cpp'),
                                    '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, 'deferred Detach must use the owner drain after a successful stop')
