# Third-Party License Materials

This directory holds license texts that do not ship as standalone files in
the vendored packages, plus the distribution rules for every component.

Rules:

1. Every vendored source file must be mapped to a row in its component-local
   `camera_software/<component>/SOURCES.md`.
2. Every row must reference an existing license file. License files are
   distributed unchanged with the component and with any copy moved to
   `SiFli-SDK-Peripherals`.
3. If an upstream package requires or ships a NOTICE file, copy it here
   beside the component license and add the reference to its `SOURCES.md`.
4. The license text must never be replaced by a summary. SPDX identifiers in
   code headers are supplementary, not substitutes.
5. A vendor source with an unknown upstream revision must still record the
   local SHA-256 so the audit can verify the exact vendored bytes.
6. Original copyright and license notices in vendored source files must be
   preserved verbatim in every local copy and in every adapted copy that moves
   into `camera_software` or `SiFli-SDK-Peripherals`. Stripping or rewording
   an upstream copyright header is not allowed.
