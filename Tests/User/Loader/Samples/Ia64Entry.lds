/* SPDX-License-Identifier: Apache-2.0 */
/*Asteria's IA-64 entry is a descriptor rather than GNU ld's code entry.
   The loader rebases the link-time entry and fills gp from DT_PLTGOT. */
SECTIONS
{
    .entry_descriptor : ALIGN(16)
    {
        EntryDescriptor = .;
        QUAD(Entry)
        QUAD(0)
    }
}
INSERT AFTER .data;
