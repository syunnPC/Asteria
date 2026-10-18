// SPDX-License-Identifier: Apache-2.0

#include "Platform/AsmLayout.hpp"
#include "Platform/Amd64/Memory/Amd64ControlRegister4.hpp"
#include "Kernel/Memory/PhysicalPageAllocator.hpp"
#include "Platform/Amd64/Memory/Amd64FrameScrubber.hpp"

extern "C" void AsteriaAmd64FrameScrubberAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_DISABLED_TRANSLATION_FEATURES_CLEAR,
                       ~Asteria::Platform::Amd64::Memory::DisabledTranslationFeatures);
    ASTERIA_ASM_LAYOUT(ASTERIA_SCRUB_WINDOW, Asteria::Platform::Amd64::Bootstrap::ScrubSlotVirtualAddress);
    ASTERIA_ASM_LAYOUT(ASTERIA_SCRUB_PROCESSORS, Asteria::Platform::Amd64::Memory::Amd64FrameScrubberProcessors);
    ASTERIA_ASM_LAYOUT(ASTERIA_SCRUB_CHUNK_PAGES, Asteria::Kernel::Memory::PhysicalPageAllocator::MaximumScrubPages);
    ASTERIA_ASM_LAYOUT(ASTERIA_SCRUB_WATCHES, Asteria::Platform::Amd64::Memory::Amd64FrameScrubberWatches);
    ASTERIA_ASM_LAYOUT(ASTERIA_SCRUB_SAMPLE_BYTES, sizeof(Asteria::Platform::Amd64::Memory::Amd64FrameScrubSample));
    ASTERIA_ASM_LAYOUT(ASTERIA_SCRUB_SAMPLE_WORDS, Asteria::Platform::Amd64::Memory::Amd64FrameScrubberSampleWords);
}
