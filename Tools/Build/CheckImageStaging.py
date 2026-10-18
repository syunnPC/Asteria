#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that a user image fits the boot staging the kernel stages it in.

The AMD64 boot copies each boot user image's page-rounded PT_LOAD segments
into a fixed staging slot of `InitialUserStoragePages` pages, the last
`InitialUserStackPages` of which hold its initial stack
(Source/Platform/Amd64/Executable/Amd64InitialUserImage.hpp). An image that
does not fit fails only when the boot prepares it, so the static gate checks
it at build time.

The arguments are the ELF image and the header that defines both constants.

With `--segment-limit NAME`, it checks instead that every loadable segment
of each image given is at most the header's constant NAME pages: a component
image's segment is staged through one Frame and the pager's mapping window,
so one that is larger fails only when the root service loads it
(Source/User/Pager/ComponentImage.hpp; Source/User/Network/NetworkStack.hpp).

The IA-64 boot stages the pager (and test pager) image through
Ia64InitialUserImageOwner, which refuses more than a header constant of
segments, a segment of more pages than another, and more pages in all than
a third (Source/Platform/Ia64/Executable/Ia64InitialUserImageOwner.hpp);
the refusal shows only as ASTERIA:PAGER-IMAGE-STAGE:FAIL at boot. With
`--aggregate-limit NAME` and `--segment-count-limit NAME` beside
`--segment-limit NAME`, all three are checked, so growth fails the build.
"""
import argparse
import pathlib
import re
import struct
import sys

PAGE = 4096
PT_LOAD = 1


def constant(text: str, name: str) -> int:
    match = re.search(r'\b%s\{(\d+)U?\}' % re.escape(name), text)
    if match is None:
        raise SystemExit('%s is not defined as name{value} in the header' % name)
    return int(match.group(1))


def staged_pages(image: bytes) -> list[tuple[int, int]]:
    """(virtual address, page-rounded pages) of each PT_LOAD of an ELF64 little-endian image."""
    if image[:4] != b'\x7fELF' or image[4] != 2 or image[5] != 1:
        raise SystemExit('not an ELF64 little-endian image')
    phoff, = struct.unpack_from('<Q', image, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', image, 0x36)
    segments = []
    for index in range(phnum):
        kind, _, _, vaddr, _, _, memsz, _ = struct.unpack_from('<IIQQQQQQ', image, phoff + index * phentsize)
        if kind == PT_LOAD and memsz:
            first = vaddr // PAGE
            last = (vaddr + memsz + PAGE - 1) // PAGE
            segments.append((vaddr, last - first))
    return segments


def check_segments(images: list[pathlib.Path], limit: int) -> int:
    """Whether every loadable segment of every image is at most limit pages."""
    failed = 0
    for image in images:
        segments = staged_pages(image.read_bytes())
        detail = ', '.join('%#x: %d' % segment for segment in segments)
        largest = max((pages for _, pages in segments), default=0)
        if largest > limit:
            print('%s has a segment of %d pages (%s), more than the %d one may have' % (image, largest, detail, limit))
            failed = 1
        else:
            print('%s: largest segment %d of %d pages (%s)' % (image, largest, limit, detail))
    return failed


def check_totals(images: list[pathlib.Path], aggregate: int | None, count: int | None) -> int:
    """Whether every image has at most count loadable segments and aggregate pages in all."""
    failed = 0
    for image in images:
        segments = staged_pages(image.read_bytes())
        used = sum(pages for _, pages in segments)
        if aggregate is not None and used > aggregate:
            print('%s stages %d pages in all, more than the %d its staging holds' % (image, used, aggregate))
            failed = 1
        elif aggregate is not None:
            print('%s stages %d of %d pages in all' % (image, used, aggregate))
        if count is not None and len(segments) > count:
            print('%s has %d loadable segments, more than the %d its staging takes' % (image, len(segments), count))
            failed = 1
    return failed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('image', type=pathlib.Path, nargs='+')
    parser.add_argument('--header', type=pathlib.Path, required=True)
    parser.add_argument('--segment-limit', metavar='NAME')
    parser.add_argument('--aggregate-limit', metavar='NAME')
    parser.add_argument('--segment-count-limit', metavar='NAME')
    arguments = parser.parse_args()
    header = arguments.header.read_text()
    if (arguments.aggregate_limit or arguments.segment_count_limit) and not arguments.segment_limit:
        raise SystemExit('--aggregate-limit and --segment-count-limit need --segment-limit')
    if arguments.segment_limit:
        failed = check_segments(arguments.image, constant(header, arguments.segment_limit))
        aggregate = constant(header, arguments.aggregate_limit) if arguments.aggregate_limit else None
        count = constant(header, arguments.segment_count_limit) if arguments.segment_count_limit else None
        return check_totals(arguments.image, aggregate, count) | failed
    if len(arguments.image) != 1:
        raise SystemExit('the staging check takes one image')
    limit = constant(header, 'InitialUserStoragePages') - constant(header, 'InitialUserStackPages')
    segments = staged_pages(arguments.image[0].read_bytes())
    used = sum(pages for _, pages in segments)
    detail = ', '.join('%#x: %d' % segment for segment in segments)
    if used > limit:
        print('%s stages %d pages (%s), more than the %d its staging holds' % (arguments.image[0], used, detail, limit))
        return 1
    print('%s stages %d of %d pages (%s)' % (arguments.image[0], used, limit, detail))
    return 0


if __name__ == '__main__':
    sys.exit(main())
