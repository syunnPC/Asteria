# SPDX-License-Identifier: Apache-2.0
"""AMD64 native-preemption probe image (presets amd64-preemption-probe and
amd64-preemption-probe-relocated).

The probe build runs a one-shot kernel preemption proof on the BSP before the
user launch (Amd64NativePreemptionProbe.cpp) and then continues exactly like
the default image. These cases boot the production firmware of their preset
(the production user image, which reports nothing), so the flow is the
default driver's without the guest's TAP stream, and the lifecycle, host and
evidence checks are the same. The preset's `amd64_native_preemption_probe`
switch makes the driver expect the stage `AMD64-NATIVE-PREEMPTION` before
the user launch and judge the proof's evidence
(`host.amd64.evidence.native-preemption`). The preset must enable the probe.
"""
from __future__ import annotations

from AsteriaVm.Drivers import Amd64Default


def run(context) -> None:
    configured = context.target.option('amd64_native_preemption_probe')
    context.require('lifecycle.preset-native-preemption-probe', configured == '1',
                    'preset %s was built with amd64_native_preemption_probe=%r, expected "1"'
                    % (context.case.preset, configured))
    Amd64Default.run_image(context)
