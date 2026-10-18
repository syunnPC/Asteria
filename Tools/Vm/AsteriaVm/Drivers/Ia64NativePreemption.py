# SPDX-License-Identifier: Apache-2.0
"""IA-64 one-shot native preemption proof: Ia64.NativePreemption.

The probe build preempts one fixed BSP kernel loop through the native timer
path, resumes it, records the proof in the kernel's evidence record, reports
`ASTERIA:STAGE:NATIVE-PREEMPTION-RECORDED` and halts (no user image runs).
The driver waits for that stage, stops the guest, reads the record and
judges the proof's boot stages once and in order, one capture and
one restore of the preempted worker, which exited with its own exit code
(`Observers.Ia64Evidence`), and the evidence every IA-64 image shares
(processors, PCI, no disk, AP runtime, shared-root shootdown), plus each
CPU's consolidated kernel translation plan. The register dumps supply that
check; the IVT, stable user loop and screenshot are not captured.
"""
from __future__ import annotations

from AsteriaVm.Drivers import Ia64Default
from AsteriaVm.Observers import Ia64Capture, Ia64Evidence, Ia64Image


def evaluate(observation: Ia64Capture.Observation, expected: Ia64Default.Expectation) -> list[Ia64Default.Check]:
    checks = Ia64Default.stage_checks(observation, expected, Ia64Default.PREEMPTION_STAGES)
    checks += Ia64Default.native_kernel_translation_checks(observation, expected)
    # The proof runs after the synthetic probe's first local-timer interrupt
    # and never dispatches the probe, so its wait-timer record stays empty.
    checks += Ia64Default.evidence_checks(observation, expected, ('UserProbe.WaitTimerPublished',
                                                                 'UserPreemption.Published',
                                                                 'ObjectManagement.Published',
                                                                 'Threads.Published'))
    evidence, problems = Ia64Default.evidence_record(observation, expected)
    if evidence is None:
        return checks + [('host.ia64.evidence.native-preemption', problems)]
    return checks + [('host.ia64.evidence.native-preemption',
                      Ia64Evidence.native_preemption_problems(evidence.NativePreemption, expected.constants),
                      Ia64Evidence.describe_native_preemption(evidence.NativePreemption))]


def run(context) -> None:
    facts = Ia64Image.ImageFacts.from_target(context.target, context.case.firmware)
    expected = Ia64Default.expectation(context.case, context.target, facts, context.artifact('Disk.raw'))
    Ia64Default.gate(context, Ia64Default.PREEMPTION_STAGE, Ia64Default.BOOT_TIMEOUT)
    context.stop()
    observation, errors = Ia64Default.capture(context, {}, processors=expected.cpus)
    errors.update(Ia64Default.capture_evidence(context, observation, expected))
    Ia64Capture.save(observation, context.run_directory)
    Ia64Default.record(context, evaluate(observation, expected), errors)
