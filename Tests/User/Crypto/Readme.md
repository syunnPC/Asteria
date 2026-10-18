<!-- SPDX-License-Identifier: Apache-2.0 -->

# Crypto test vectors

Tests use the following published vectors:

| Primitive | Source and scope |
| --- | --- |
| SHA-256/512 | FIPS 180-4 §6 and NIST SHS examples: padding boundaries and million-a |
| HMAC | RFC 4231 §§4.2–4.8, all SHA-256/512 cases |
| HKDF | RFC 5869 Appendix A.1–A.3; SHA-512 also checks the 255-block bound |
| AES | FIPS 197 Appendix C.1/C.3 |
| GCM | [NIST GCMVS vectors](https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Algorithm-Validation-Program/documents/mac/gcmtestvectors.zip), Count 0 per supported length group, 96-bit IV / 128-bit tag |
| ChaCha20 / Poly1305 | RFC 8439 §§2.3.2, 2.5.2, 2.8.2 and all Appendix A.3 vectors |
| X25519 | RFC 7748 §§5.2/6.1, including 1/1000 iterations |
| Ed25519 | RFC 8032 §7.1, all five pure Ed25519 cases |
| Argon2id | RFC 9106 §5.3, secret and associated data; login-shape regression also covers scaled memory |
| HMAC_DRBG | [NIST DRBGVS vectors](https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Algorithm-Validation-Program/documents/drbg/drbgtestvectors.zip), SHA-256 Count 0 groups across reseed modes |

RFC tables keep their original terms. Post-2008 sets use BSD-3-Clause; RFC
4231 uses the RFC 3978 grant. Mixed test files carry both SPDX identifiers;
[NOTICE](../../../NOTICE) and [license texts](../../../LICENSES/) retain the
conditions and attribution. NIST publications remain identified above.

API and fuzz tests cover invalid inputs, overlap, counter exhaustion, tag
mutations, failure atomicity and workspace erasure. Structural checks retain
secrecy classifications and must reject their mutants. These checks
do not establish machine-code timing; see [Crypto](../../../Documentation/Architecture/Crypto.md).
