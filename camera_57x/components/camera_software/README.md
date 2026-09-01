# Camera software components

`camera_software` is an in-project SiFli component package. In this demo it
lives at `camera_57x/components/camera_software`; another project can copy the
whole directory to the same relative `components/camera_software` location.

| Directory | Public API | Responsibility |
|---|---|---|
| `isp/` | `camera_sw_isp.h`, `camera_sw_isp_pipeline.h` | RGB565_BE ISP pipeline and deterministic 4x4 Bayer final quantization |
| `jpeg/` | `camera_sw_jpeg_encoder.h`, `camera_sw_jpeg_decoder.h` | Independently switchable JPEG encoding and decoding |
| `isp/example/` | `camera_sw_isp_pipeline.h` | Directly buildable HCPU ISP verification example |
| `jpeg/example/` | `camera_sw_jpeg_encoder.h`, `camera_sw_jpeg_decoder.h` | Directly buildable HCPU JPEG encode/decode verification example |
| `example/acpu/` | `camera_sw_acpu_client.h` | Directly buildable SiFli HCPU/ACPU adapter and verification example |

`isp/` and `jpeg/` use only standard C/C++ types and caller-owned buffers.
They do not depend on RT-Thread, ACPU IPC, sensors, displays, filesystems or
this demo. Each component keeps its own Kconfig, SConscript, public include
directory, source manifest and license materials.

## Kconfig

Source the package once from the consumer project Kconfig:

```kconfig
rsource "../components/camera_software/Kconfig"
```

ISP and JPEG each select HCPU or ACPU independently. An ACPU option requires
`BSP_USING_ACPU`; HCPU is the only supported selection on single-core devices.
The JPEG encoder and decoder options independently control their vendor source
sets.

## SConscript

The project SConstruct resolves and exports one absolute package root:

```python
CAMERA_SOFTWARE_ROOT = os.path.abspath(
    os.path.join(GetCurrentDir(), '../components/camera_software'))
Export('CAMERA_SOFTWARE_ROOT')
```

For components selected on HCPU, load the sibling component SConscript directly.
For any ACPU selection, load the example client on HCPU and its worker in the
ACPU child project:

```python
SConscript(os.path.join(CAMERA_SOFTWARE_ROOT,
                        'example/acpu/hcpu/SConscript'),
           variant_dir='camera_sw_acpu_hcpu', duplicate=0)
SConscript(os.path.join(CAMERA_SOFTWARE_ROOT,
                        'example/acpu/acpu/SConscript'),
           variant_dir='camera_sw_acpu_worker', duplicate=0)
```

The example SConscript files derive their component root from their own
location. They do not require a sibling `src` directory, so a copied package
uses the same two entries in another project.

## ACPU example API

Include `camera_sw_acpu_client.h` from `example/acpu/include`. Its
`camera_sw_acpu_encode_rgb565_be`, `camera_sw_acpu_decode_jpeg`,
`camera_sw_acpu_process_rgb565_be` and `camera_sw_acpu_query_cpu_usage` calls
are synchronous. Encode input, JPEG output, decode input/destination and ISP
pixel buffers are caller-owned; the adapter does not retain them after return.

Cross-core buffers must be accessible to both cores, normally shared PSRAM.
The example's `acpu_run_task()` override performs cache clean/invalidate before
dispatch; callers must not reuse or modify a buffer until the call returns.
The ACPU worker's `acpu_main()` must be linked into the ACPU image. See
[`example/acpu/README.md`](example/acpu/README.md) for its task IDs, capacity
checks, timeout and JPEG decode scaling limit.

## Example verification and license audit

The JPEG component preserves strict separation between BitBank JPEGENC
(`vendor/jpegenc`, `SOURCES-ENCODER.md`, Apache-2.0) and ChaN TJpgDec
(`vendor/tjpgd`, `SOURCES-DECODER.md`, ChaN terms). The ACPU example has its
own MIT license and source manifest. The JPEGENC upstream revision remains
unrecorded, so external publication remains blocked.

Build each standalone example after activating the SDK environment:

```sh
source /path/to/SiFli-SDK/export.sh
scons -C camera_57x/components/camera_software/isp/example/project \
      --board=spi-hdk_lb573ub7n6_hcpu
scons -C camera_57x/components/camera_software/jpeg/example/project \
      --board=spi-hdk_lb573ub7n6_hcpu
scons -C camera_57x/components/camera_software/example/acpu/project/hcpu \
      --board=spi-hdk_lb573ub7n6_hcpu
```

The ISP and JPEG examples print `status=PASS` when their deterministic
in-memory fixtures complete. The ACPU example requires a dual-core board and
must be flashed with both generated images.
