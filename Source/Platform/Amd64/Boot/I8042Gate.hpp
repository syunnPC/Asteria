// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_BOOT_I8042_GATE_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_BOOT_I8042_GATE_HPP_INCLUDED

#include "Kernel/Boot/I8042Publication.hpp"

// The publication gate is ISA-neutral (Kernel/Boot/I8042Publication.hpp).
namespace Asteria::Platform::Amd64::Bootstrap
{
using ::Asteria::Boot::Bootstrap::GateI8042;
using ::Asteria::Boot::Bootstrap::I8042Publication;
using ::Asteria::Boot::Bootstrap::I8042PublicationReason;
using ::Asteria::Boot::Bootstrap::I8042ReasonText;
}
#endif
