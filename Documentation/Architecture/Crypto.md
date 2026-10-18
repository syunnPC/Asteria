<!-- SPDX-License-Identifier: Apache-2.0 -->

# Crypto library

Specification-derived freestanding user primitives and password hashing
support [identity](Identity.md). The library defines no service, kernel policy,
persisted password format or TLS integration.

## Purpose and API

Source/User/Crypto/Crypto.hpp exposes Result, Bytes/MutableBytes, HashAlgorithm,
AeadAlgorithm, a caller-owned Workspace, Argon2Parameters/Argon2Block and
HmacDrbg. Every buffer has an explicit byte length; nullptr is valid only for
zero length. Output sizes are exact except DRBG/HKDF's requested output length.
Primitive functions are noexcept and return explicit errors; Wipe and Equal
are utility operations with a valid-buffer precondition. Workspace is exclusive
to a call, must not overlap buffers and must be held off the target stack.
There is no allocation or mutable static state. Contexts own their DRBG state;
callers serialize use and provide trusted entropy through Instantiate/Reseed.
No entropy-credit estimation is performed by this library.

Cryptographic inputs are bytes, including passwords. A Users caller encodes
native text in UTF-8; the library performs no normalization or interpretation.
Hashes, tags and keys use the specification's raw octet representation. There
is no parser for PEM/DER, password records or textual key formats.

## Algorithms and formats

- FIPS 180-4 (August 2015), §§4–6: SHA-256 and SHA-512; rolling schedules.
  Existing boot-trust SHA-256 is reused through SharedSha256.cmake's generated
  namespace projection (no maintained copy, kernel edits or kernel imports).
  The projection adds volatile erasure and fails if its source shape changes.
  HMAC uses the shared hash through the common two-hash implementation.
  CRC-32C remains solely Foundation/Crc32c.hpp; it is not a cryptographic authenticator.
- RFC 2104 (February 1997), §2, FIPS 198-1 (July 2008), §4: HMAC for both hashes.
  RFC 4231 (December 2005), §4 supplies HMAC vectors.
- RFC 5869 (May 2010), §§2–3, Appendix A: extract and expand for both hashes.
- FIPS 197-upd1 (May 2023), §§4–5: AES-128/256 encryption; arithmetic GF(256)
  S-box, without lookup tables or vector intrinsics (native checks use the
  general-register user ABI). SP 800-38D (November
  2007), §§6–7: GCM, fixed 96-bit nonce and full 128-bit tag only.
- RFC 8439 (June 2018), §§2.3–2.8, Appendix A: IETF ChaCha20, Poly1305 and AEAD,
  256-bit key, 96-bit nonce, full 128-bit tag.
- RFC 7748 (January 2016), §§5–6: X25519, 32-byte little-endian values;
  mask input high bit and accept noncanonical field coordinates as required.
  Reject an all-zero shared secret.
- RFC 8032 (January 2017), §§5.1, 7.1: pure Ed25519, 32-byte seed/public key,
  64-byte signature. Decode canonical encodings, reject noncanonical S and
  small-order public keys/R, use the uncofactored verification equation.
- RFC 9106 (September 2021), §§3–5: Argon2id v1.3, Blake2b from RFC 7693
  (November 2015), §§2–3. Caller supplies explicitly counted 1024-byte blocks.
  Standard Argon2id addressing follows §3.4.1.3: data-independent in slices
  0–1 of pass 0, then data-dependent, with direct reference-block reads.
- SP 800-90A Rev. 1 (June 2015), §10.1.2: HMAC-DRBG-SHA256, 256-bit strength;
  32-byte minimum entropy, 16-byte minimum instantiation nonce, maximum
  65536-byte generate request, 2^48 generate calls before mandatory reseed.
  Prediction resistance is explicit caller reseeding before generate.

## Bounds and failure behavior

Hash/HMAC input is limited to 2^61-1 bytes per call. HKDF output is at most
255 digest blocks. AEAD payloads are capped at 16 MiB per operation, AAD at
16 MiB, and ChaCha counter exhaustion is rejected before writing. Read-only
inputs may overlap each other. Hash, HMAC/HKDF extract and Poly1305 finish
reading inputs before writing their result, so their output may alias inputs.
Other mutable outputs must be disjoint, except AES/ChaCha/AEAD payload may be
exactly in-place. Keys, nonce, AAD and tag may not overlap payload output.
AEAD open authenticates before releasing plaintext and preserves output on
failure. Workspace is disjoint from every input and output for every operation.

Argon2id accepts 1–4 lanes, 1–32 passes, 8*lanes–65536 KiB memory, salt
8–1024 bytes, password/secret/associated data at most 1024 bytes each, tag
4–1024 bytes. Memory is rounded down to a multiple of 4*lanes, per RFC 9106.
Insufficient caller memory fails before any secret processing. Scratch and
used memory are wiped on success. All secret scratch is cleared after every
operation that accepts disjoint workspace buffers; overlap refusal occurs
before any read/write so it cannot erase a caller buffer aliased to scratch.
DRBG persistent state survives until Clear/destruction. Volatile
byte stores prevent dead-store elimination. Output keys remain caller-owned.

## Policy requirements

Argon2id uses the standard RFC 9106 reference-block addressing.
Its data-dependent reference-block access is the one documented exception to
this library's no-secret-indexed-access rule. The residual risk is a co-resident
cache-timing observer during password hashing. RFC 9106 §§1, 3.4.1.3 and 7.4
describe Argon2id's hybrid addressing for the recommended password-hashing options.
The exception does not extend to branches, arithmetic or other primitives. The structural inventory retains
the exact phase boundary and the one reviewed reference-block read.

LoginArgon2Defaults follows RFC 9106 §4 item 2 / §7.4's second recommended
option: 65536 KiB (64 MiB), 3 passes and 4 lanes. LoginArgon2SaltBytes is
16 (128 bits), and LoginArgon2TagBytes is 32 (256 bits). The [authentication service](Identity.md) supplies the arena through the existing caller-provided scratch
interface; there is no hidden allocation or service wiring. Lanes are scheduled
serially by this freestanding library and produce the RFC's four-lane result.
The RFC 9106 §5.3 vector supplies an independent known-answer test.

## Timing-independence review checklist

Host known-answer tests are supplemented by this structural checklist and
Tools/Verify/CheckCryptoStructure.py. CryptoReview.json inventories all source
branch/loop conditions, subscripts and conditional selections, including the
shared SHA source. CTest CryptoStructure fails if that reviewed inventory
changes; CryptoStructureMutations verifies rejection of five representative
mutations. The additional checks retain volatile wiping, enforce the Argon2id
phase boundary/reference-access exception, and reject named AES substitution
tables. This is not an assertion
that empirical timings or a regex prove constant-time machine code.

1. Branches and loop bounds depend only on lengths, public algorithm selections,
   round indices and public parameters. Tag/key validation yields one final
   result after full fixed-length comparison. Secret bit selection uses masks
   with erased volatile round trips to inhibit compiler recognition of 0/-1
   selects as secret-conditioned memory operations.
2. SHA schedules, AES arithmetic, GHASH and Poly1305 have fixed public indices;
   modular correction is mask-based, never repeated subtraction until reduced.
3. Curve ladders/scalar multiplication use fixed bit counts and masked swaps;
   all field exponentiation schedules are public. Scalar reduction is fixed.
4. Argon2id directly reads the reference block computed by RFC 9106 §3.4;
   only pass 0 / slices 0–1 use password-independent addresses. Later reference
   addresses are the documented exception above. Lane/index reduction
   still uses fixed bitwise remainder; secret-conditioned branches and
   variable-time integer division of secrets remain prohibited.
5. Audit both ISA optimized assembly before security-sensitive deployment;
   compilers may transform mask idioms. Target stack gates cover O0 and O2.
6. Every error precedes secret processing or passes through wipe cleanup.
   AEAD failure output is unchanged; no early-exit comparison of secret bytes.
7. Workspace/memory are caller-owned, exclusive and disjoint; no hidden shared
   secret state. Lengths and success/failure are explicitly public.

## Unsupported features

No AES decryption/ECB API, shortened GCM tags, arbitrary GCM nonces, X448,
Ed448/Ed25519ctx/ph, ASN.1, PEM, password text format, RNG entropy acquisition,
TLS, disk encryption, service wiring or VM scenarios.

## Consumer requirements

- The entropy owner must define entropy-source credit and reseed cadence.
- Each AEAD consumer must define nonce allocation and persistence. Nonces must
  be unique per key; this stateless library cannot enforce that across calls.

Argon2id optionally calls an
injected service poll at the public pass/slice/lane segment boundary
(RFC 9106, September 2021, v1.3, §3.4). The callback inspects no password/tag
bytes and cannot change the profile or return early. Authentication checks
cancellation before and after hashing, accounts the terminal attempt and
wipes the entire arena/workspace. The KAT covers 48 callback invocations; the structural inventory classifies only this public callback
and its context. Root never hashes in its seat event loop.
