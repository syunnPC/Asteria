# SPDX-License-Identifier: Apache-2.0
"""Verification helpers for Asteria build artifacts.

The modules read ELF and PE images directly, extract and normalize
disassembly, and compare it against golden files. Checks written on
top of them run as CTest tests of the target build trees.
"""
