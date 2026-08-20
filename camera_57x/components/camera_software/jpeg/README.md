# Camera software JPEG

The JPEG component keeps BitBank JPEGENC and ChaN TJpgDec as independently
switchable and auditable codec paths. Both use caller-owned buffers and have
no RT-Thread, IPC, camera, display or storage dependency.

| Codec | Source manifest | Vendor path | License |
|---|---|---|---|
| BitBank JPEGENC encoder | `SOURCES-ENCODER.md` | `vendor/jpegenc` | `LICENSES/jpegenc/Apache-2.0.txt` |
| ChaN TJpgDec decoder | `SOURCES-DECODER.md` | `vendor/tjpgd` | `LICENSES/tjpgd/tjpgd-license.txt` |

The exact JPEGENC upstream tag or commit is not yet recorded. Local SHA-256
values identify the checked-in files only, so external publication remains
blocked until the upstream revision is supplied. The two codec licenses and
source audits must not be combined.
