"""Optional PYNQ hardware initialization before importing/creating VART runners.

Pass a raw FPGA .bin or a .bit with matching same-stem .hwh and .xclbin files
from the same hardware build. A complete BOOT.BIN is not an FPGA overlay.
PYNQ's embedded-device .bin handler is available in PYNQ 2.7 and later.
"""

import logging
import os
from pathlib import Path


LOGGER = logging.getLogger("server")


def initialize_dpu_overlay(bitstream_path=None):
    """Download the selected overlay once and return its PYNQ lifetime handle.

    Without a path, retain the existing externally initialized hardware flow.
    XLNX_VART_FIRMWARE selects this process's XCLBIN without rewriting
    /etc/vart.conf. It must be set before VART initializes its native libraries.
    PYNQ discovers the companion metadata by replacing the bitstream suffix.
    """
    if bitstream_path is None:
        LOGGER.info("DPU initialization: using externally loaded hardware")
        return None

    bitstream = Path(bitstream_path).expanduser().resolve()
    if bitstream.suffix not in (".bit", ".bin"):
        raise ValueError("--dpu-overlay must name a raw FPGA .bin or a .bit file")
    if bitstream.name.lower() == "boot.bin":
        raise ValueError("BOOT.BIN is a boot image; provide the raw DPU FPGA bitstream")
    hwh = bitstream.with_suffix(".hwh")
    xclbin = bitstream.with_suffix(".xclbin")
    for path in (bitstream, hwh, xclbin):
        if not path.is_file():
            raise FileNotFoundError(
                f"Missing DPU overlay file: {path}. Place matching .bit/.bin, "
                ".hwh and .xclbin files with the same stem in one directory.")
        if path.stat().st_size == 0:
            raise ValueError(f"DPU overlay file is empty: {path}")

    # Set this before even importing PYNQ: hardware libraries may inspect their
    # environment during import, not just when a runner is constructed.
    previous_firmware = os.environ.get("XLNX_VART_FIRMWARE")
    os.environ["XLNX_VART_FIRMWARE"] = str(xclbin)
    try:
        try:
            from pynq import Overlay
        except ImportError as error:
            raise RuntimeError(
                "PYNQ is unavailable in this Python environment. Use the board's "
                "PYNQ interpreter for --dpu-overlay.") from error
        LOGGER.info("DPU_OVERLAY_LOADING bitstream=%s hwh=%s xclbin=%s",
                    bitstream, hwh, xclbin)
        # Use the base PYNQ Overlay, which handles both .bit and raw .bin files.
        # DpuOverlay helpers in some releases assume a .bit filename and copy
        # firmware to a global location; the explicit environment avoids that.
        overlay = Overlay(str(bitstream), download=True)
    except Exception:
        if previous_firmware is None:
            os.environ.pop("XLNX_VART_FIRMWARE", None)
        else:
            os.environ["XLNX_VART_FIRMWARE"] = previous_firmware
        raise
    LOGGER.info("DPU_OVERLAY_LOADED bitstream=%s XLNX_VART_FIRMWARE=%s",
                bitstream, xclbin)
    return overlay
