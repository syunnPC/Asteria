# SPDX-License-Identifier: Apache-2.0
"""Exercise the two PS/2 publication links with the real syscall client."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class Ps2PublicationTests(unittest.TestCase):
    def test_product_and_notice_links_preserve_nonblocking_publication(self):
        harness = r'''
#include "User/Component/Ps2Publication.hpp"
#include "User/Component/DisplayFrame.hpp"
#include <cstring>
using namespace Asteria;
namespace C = User::Component;
namespace S = User::System;
struct Capture {
    unsigned Calls{0};
    Abi::SystemCallError Error{Abi::SystemCallError::WouldBlock};
    Foundation::UInt8 First[C::DisplayMaximumFrameBytes]{};
    Foundation::Size Bytes{0};
    bool Changed{false}, Pointer{false}, Key{false};
};
void Send(void* context, const Abi::SystemCallRequest* request, Abi::SystemCallResponse* response) noexcept {
    auto& c = *static_cast<Capture*>(context);
    const auto* frame = reinterpret_cast<const Foundation::UInt8*>(request->Arguments[2]);
    const auto bytes = request->Arguments[3];
    if (request->Number != static_cast<Foundation::UInt64>(Abi::SystemCallNumber::IpcSend)
        || request->Arguments[0] != 7 || request->Arguments[1] != 9
        || bytes > sizeof(c.First)) { c.Changed = true; return; }
    if (c.Calls == 0) { c.Bytes = bytes; std::memcpy(c.First, frame, bytes); }
    if (c.Calls == 1 && (c.Bytes != bytes || std::memcmp(c.First, frame, bytes))) { c.Changed = true; }
    for (Foundation::Size i{0}; i < bytes; ++i) {
        constexpr char pointer[]{"source pointer-down"}, key[]{"source key-b"};
        if (i + sizeof(pointer)-1 <= bytes && !std::memcmp(frame+i, pointer, sizeof(pointer)-1)) { c.Pointer = true; }
        if (i + sizeof(key)-1 <= bytes && !std::memcmp(frame+i, key, sizeof(key)-1)) { c.Key = true; }
    }
    ++c.Calls;
    response->Error = c.Error;
}
int main() {
    static Capture capture;
    const S::SystemCallClient client{S::SystemCallTransport{Send, &capture}};
    constexpr S::CapabilityHandle root{7, 9};
    User::Input::InputEvent pointer{}, key{};
    pointer.Kind = User::Input::EventKind::Buttons; pointer.Buttons = 1;
    key.Kind = User::Input::EventKind::KeyDown; key.Usage = 5;
    if (C::Ps2PublicationPending() || !C::SendPs2Publication(client, root) || capture.Calls) { return 1; }
    C::ObservePs2Publication(&pointer, 1);
#if NOTICE
    if (!C::Ps2PublicationPending() || !C::SendPs2Publication(client, root)
        || !C::Ps2PublicationPending() || !capture.Pointer || capture.Key) { return 2; }
    C::ObservePs2Publication(&key, 1);
    capture.Error = Abi::SystemCallError::None;
    if (!C::SendPs2Publication(client, root) || capture.Changed || !C::Ps2PublicationPending()) { return 3; }
    if (!C::SendPs2Publication(client, root) || !capture.Key || C::Ps2PublicationPending()) { return 4; }
    C::ObservePs2Publication(&pointer, 1);
    capture.Error = Abi::SystemCallError::InvalidHandle;
    if (C::SendPs2Publication(client, root) || !C::Ps2PublicationPending()) { return 5; }
#else
    C::ObservePs2Publication(&key, 1);
    if (C::Ps2PublicationPending() || !C::SendPs2Publication(client, root) || capture.Calls) { return 6; }
#endif
}
'''
        with tempfile.TemporaryDirectory() as directory:
            scratch = pathlib.Path(directory)
            cpp = scratch / 'publication.cpp'
            cpp.write_text(harness)
            for notice, source in ((False, 'Source/User/Component/Ps2Publication.cpp'),
                                   (True, 'Tests/Guest/Session/Ps2PublicationNotice.cpp')):
                with self.subTest(notice=notice):
                    executable = scratch / ('notice' if notice else 'product')
                    build = subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++23',
                                            '-DNOTICE=%d' % notice, '-I', str(ROOT / 'Source'),
                                            str(cpp), str(ROOT / source),
                                            str(ROOT / 'Source/User/System/SystemCallClient.cpp'),
                                            '-o', str(executable)], capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stderr)
                    run = subprocess.run([str(executable)], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stderr)
