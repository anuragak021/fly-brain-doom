"""ctypes wrapper around liboled_pipeline.so (the exact C pipeline that runs on the ESP32).

Build the library first:  cmake -S host -B host/build && cmake --build host/build
"""

from __future__ import annotations

import ctypes
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

OLED_W = 128
OLED_H = 64
PACKED_BYTES = OLED_W * OLED_H // 8
DOOM_W = 320
DOOM_H = 200
# Rows of the 320x200 frame the OLED shows (op_scale.h: OP_DOOM_CROP_Y, OP_DOOM_CROP_H).
CROP_Y = 200 - 32 - 133
CROP_H = 133
# Nano layout (op_scale.h: OP_DOOM_HUD_CROP_Y/H, OP_HUD_VIEW_H_PX): 3D view above an 8 row HUD.
HUD_CROP_Y = 200 - 32 - 117
HUD_CROP_H = 117
HUD_VIEW_H = 56
WALL_MARKER = 251  # op_mask.h OP_MASK_WALL_MARKER
NANO_IDS = 256  # op_nano.h OP_NANO_IDS
NANO_ID_WEAPON = 255
# op_spr_class_t
SPR_NONE, SPR_MID, SPR_THING, SPR_MONSTER, SPR_MONSTER_HIT = 0, 1, 2, 3, 4
SPR_CORPSE, SPR_ITEM, SPR_FX, SPR_WEAPON = 5, 6, 7, 8

DITHER_BAYER4 = 0
DITHER_FS = 1
DITHER_OUTLINE = 2  # matches DS_STYLE_OUTLINE; host side runs op_outline()
DITHER_NAMES = {
    DITHER_BAYER4: "bayer4",
    DITHER_FS: "floyd_steinberg",
    DITHER_OUTLINE: "outline",
}

_DEFAULT_LIB = Path(__file__).resolve().parent / "build" / "liboled_pipeline.so"


class PipelineError(RuntimeError):
    """Raised when a C pipeline call returns a non-OK op_status_t."""


class _GrayCfg(ctypes.Structure):
    _fields_ = [
        ("black", ctypes.c_uint8),
        ("white", ctypes.c_uint8),
        ("gamma_x100", ctypes.c_uint16),
    ]


GRAY_DEFAULT_BLACK = 16
GRAY_DEFAULT_WHITE = 170


class _OutlineCfg(ctypes.Structure):
    _fields_ = [("fill_shift", ctypes.c_uint8), ("edge_threshold", ctypes.c_uint16)]


class _LayersCfg(ctypes.Structure):
    _fields_ = [
        ("bg_shift", ctypes.c_uint8),
        ("edge_threshold", ctypes.c_uint16),
        ("fg_gain_x16", ctypes.c_uint8),
        ("fg_offset", ctypes.c_uint8),
        ("cov_threshold", ctypes.c_uint8),
    ]


class _NanoCfg(ctypes.Structure):
    _fields_ = [
        ("levels", ctypes.c_uint8),
        ("gain_x16", ctypes.c_uint8),
        ("black", ctypes.c_uint8),
        ("min_level", ctypes.c_uint8),
        ("split_threshold", ctypes.c_uint8),
        ("sprite_threshold", ctypes.c_uint8),
        ("cov_threshold", ctypes.c_uint8),
        ("wall_edges", ctypes.c_bool),
    ]


class _NanoSprites(ctypes.Structure):
    _fields_ = [
        ("cov", ctypes.POINTER(ctypes.c_uint8)),
        ("id", ctypes.POINTER(ctypes.c_uint8)),
        ("cls", ctypes.POINTER(ctypes.c_uint8)),
        ("prev_mode", ctypes.POINTER(ctypes.c_uint8)),
        ("mode", ctypes.POINTER(ctypes.c_uint8)),
    ]


class _Hud(ctypes.Structure):
    _fields_ = [
        ("health", ctypes.c_int32),
        ("kills", ctypes.c_int32),
        ("ammo", ctypes.c_int32),
        ("show_ammo", ctypes.c_bool),
    ]


class _ScaleCfg(ctypes.Structure):
    _fields_ = [
        (name, ctypes.c_uint16)
        for name in (
            "src_w",
            "src_h",
            "src_stride",
            "crop_x",
            "crop_y",
            "crop_w",
            "crop_h",
            "dst_w",
            "dst_h",
        )
    ]


_U8P = ctypes.POINTER(ctypes.c_uint8)


def _ptr(a: np.ndarray) -> ctypes._Pointer:
    if a.dtype != np.uint8 or not a.flags["C_CONTIGUOUS"]:
        raise ValueError("expected a C-contiguous uint8 array")
    return a.ctypes.data_as(_U8P)


@dataclass
class OledPipeline:
    """Runs stages 1-4 through the C library. One instance per palette/gamma setting."""

    lib_path: Path = _DEFAULT_LIB

    def __post_init__(self) -> None:
        if not self.lib_path.exists():
            raise FileNotFoundError(
                f"{self.lib_path} not found; build it with "
                "'cmake -S host -B host/build && cmake --build host/build'"
            )
        self._lib = ctypes.CDLL(str(self.lib_path))
        self._lib.op_gray_build_lut.argtypes = [_U8P, ctypes.POINTER(_GrayCfg), _U8P]
        self._lib.op_scale_doom_default.argtypes = [ctypes.POINTER(_ScaleCfg)]
        self._lib.op_scale_area.argtypes = [ctypes.POINTER(_ScaleCfg), _U8P, _U8P, _U8P]
        self._lib.op_dither.argtypes = [
            ctypes.c_int,
            _U8P,
            ctypes.c_uint16,
            ctypes.c_uint16,
            _U8P,
        ]
        self._lib.op_outline.argtypes = [
            _U8P,
            ctypes.c_uint16,
            ctypes.c_uint16,
            ctypes.POINTER(_OutlineCfg),
            _U8P,
        ]
        self._lib.op_outline_cfg_default.argtypes = [ctypes.POINTER(_OutlineCfg)]
        self._outline_cfg = _OutlineCfg()
        self._check(
            self._lib.op_outline_cfg_default(ctypes.byref(self._outline_cfg)),
            "outline_cfg",
        )
        self._lib.op_layers.argtypes = [
            _U8P,
            _U8P,
            ctypes.c_uint16,
            ctypes.c_uint16,
            ctypes.POINTER(_LayersCfg),
            _U8P,
        ]
        self._lib.op_layers_cfg_default.argtypes = [ctypes.POINTER(_LayersCfg)]
        self.layers_cfg = _LayersCfg()
        self._check(
            self._lib.op_layers_cfg_default(ctypes.byref(self.layers_cfg)), "layers_cfg"
        )
        self._lib.op_mask_diff.argtypes = [_U8P, _U8P, ctypes.c_size_t]
        self._lib.op_mask_coverage.argtypes = [_U8P, _U8P]
        self._lib.op_mask_coverage_cfg.argtypes = [
            ctypes.POINTER(_ScaleCfg),
            _U8P,
            _U8P,
        ]
        self._lib.op_mask_wall_coverage.argtypes = [
            ctypes.POINTER(_ScaleCfg),
            _U8P,
            ctypes.c_uint8,
            _U8P,
        ]
        self._lib.op_mask_ids.argtypes = [ctypes.POINTER(_ScaleCfg), _U8P, _U8P]
        self._lib.op_nano.argtypes = [
            _U8P,
            _U8P,
            ctypes.POINTER(_NanoSprites),
            ctypes.c_uint16,
            ctypes.c_uint16,
            ctypes.POINTER(_NanoCfg),
            _U8P,
        ]
        self._lib.op_nano_cfg_default.argtypes = [ctypes.POINTER(_NanoCfg)]
        self.nano_cfg = _NanoCfg()
        self._check(
            self._lib.op_nano_cfg_default(ctypes.byref(self.nano_cfg)), "nano_cfg"
        )
        self._lib.op_hud_draw.argtypes = [
            ctypes.POINTER(_Hud),
            ctypes.c_uint16,
            ctypes.c_uint16,
            _U8P,
        ]
        self._lib.op_pack_ssd1306.argtypes = [
            _U8P,
            ctypes.c_uint16,
            ctypes.c_uint16,
            _U8P,
            ctypes.c_size_t,
        ]
        self._cfg = _ScaleCfg()
        self._check(
            self._lib.op_scale_doom_default(ctypes.byref(self._cfg)),
            "scale_doom_default",
        )
        self._lib.op_scale_doom_hud_view.argtypes = [ctypes.POINTER(_ScaleCfg)]
        self._hud_cfg = _ScaleCfg()
        self._check(
            self._lib.op_scale_doom_hud_view(ctypes.byref(self._hud_cfg)), "hud_view"
        )
        self.lut = np.zeros(256, dtype=np.uint8)

    @staticmethod
    def _check(status: int, what: str) -> None:
        if status != 0:
            raise PipelineError(f"{what} failed with op_status_t {status}")

    def set_palette(
        self,
        palette: np.ndarray,
        black: int = GRAY_DEFAULT_BLACK,
        white: int = GRAY_DEFAULT_WHITE,
        gamma_x100: int = 100,
    ) -> None:
        """Stage 1: build the gray LUT from a 768 byte PLAYPAL palette with levels and gamma."""
        pal = np.ascontiguousarray(palette, dtype=np.uint8).reshape(768)
        cfg = _GrayCfg(black, white, gamma_x100)
        self._check(
            self._lib.op_gray_build_lut(_ptr(pal), ctypes.byref(cfg), _ptr(self.lut)),
            "gray_build_lut",
        )

    def downscale(self, frame: np.ndarray) -> np.ndarray:
        """Stage 1+2: 320x200 palette indices -> 64x128 gray."""
        src = np.ascontiguousarray(frame, dtype=np.uint8)
        if src.shape != (DOOM_H, DOOM_W):
            raise ValueError(f"frame shape {src.shape}, expected {(DOOM_H, DOOM_W)}")
        dst = np.zeros((OLED_H, OLED_W), dtype=np.uint8)
        self._check(
            self._lib.op_scale_area(
                ctypes.byref(self._cfg), _ptr(src), _ptr(self.lut), _ptr(dst)
            ),
            "scale_area",
        )
        return dst

    def dither(self, gray: np.ndarray, mode: int = DITHER_BAYER4) -> np.ndarray:
        """Stage 3: 64x128 gray -> 64x128 of 0/1, in any style (Bayer, FS, outline)."""
        g = np.ascontiguousarray(gray, dtype=np.uint8)
        bits = np.zeros_like(g)
        if mode == DITHER_OUTLINE:
            status = self._lib.op_outline(
                _ptr(g), OLED_W, OLED_H, ctypes.byref(self._outline_cfg), _ptr(bits)
            )
        else:
            status = self._lib.op_dither(mode, _ptr(g), OLED_W, OLED_H, _ptr(bits))
        self._check(status, "dither")
        return bits

    def sprite_coverage(self, before: np.ndarray, after: np.ndarray) -> np.ndarray:
        """Sprite mask as the board computes it: 320x200 frames before and after the sprites
        were drawn -> 64x128 coverage (0..255) on the OLED grid."""
        if before.shape != (DOOM_H, DOOM_W) or after.shape != (DOOM_H, DOOM_W):
            raise ValueError("expected two 200x320 frames")
        b = np.ascontiguousarray(before[CROP_Y : CROP_Y + CROP_H], dtype=np.uint8)
        a = np.ascontiguousarray(after[CROP_Y : CROP_Y + CROP_H], dtype=np.uint8)
        self._check(self._lib.op_mask_diff(_ptr(b), _ptr(a), b.size), "mask_diff")
        cov = np.zeros((OLED_H, OLED_W), dtype=np.uint8)
        self._check(self._lib.op_mask_coverage(_ptr(b), _ptr(cov)), "mask_coverage")
        return cov

    def layers(self, gray: np.ndarray, cov: np.ndarray) -> np.ndarray:
        """Stage 3, layers style: gray plus sprite coverage -> 64x128 of 0/1."""
        g = np.ascontiguousarray(gray, dtype=np.uint8)
        c = np.ascontiguousarray(cov, dtype=np.uint8)
        if g.shape != (OLED_H, OLED_W) or c.shape != g.shape:
            raise ValueError("expected 64x128 gray and coverage")
        bits = np.zeros_like(g)
        self._check(
            self._lib.op_layers(
                _ptr(g),
                _ptr(c),
                OLED_W,
                OLED_H,
                ctypes.byref(self.layers_cfg),
                _ptr(bits),
            ),
            "layers",
        )
        return bits

    def hud_view(self, frame: np.ndarray) -> np.ndarray:
        """Stages 1-2 for the nano layout: 200x320 palette indices -> 56x128 gray."""
        src = np.ascontiguousarray(frame, dtype=np.uint8)
        if src.shape != (DOOM_H, DOOM_W):
            raise ValueError("expected a 200x320 frame")
        dst = np.zeros((HUD_VIEW_H, OLED_W), dtype=np.uint8)
        self._check(
            self._lib.op_scale_area(
                ctypes.byref(self._hud_cfg), _ptr(src), _ptr(self.lut), _ptr(dst)
            ),
            "scale_area",
        )
        return dst

    def hud_wall_coverage(self, walls_frame: np.ndarray) -> np.ndarray:
        """Wall mask as the board computes it: the 200x320 frame right after the wall pass, with
        WALL_MARKER everywhere walls were not drawn -> 56x128 coverage (0..255)."""
        src = np.ascontiguousarray(walls_frame, dtype=np.uint8)
        if src.shape != (DOOM_H, DOOM_W):
            raise ValueError("expected a 200x320 frame")
        cov = np.zeros((HUD_VIEW_H, OLED_W), dtype=np.uint8)
        self._check(
            self._lib.op_mask_wall_coverage(
                ctypes.byref(self._hud_cfg), _ptr(src), WALL_MARKER, _ptr(cov)
            ),
            "mask_wall_coverage",
        )
        return cov

    def hud_sprite_coverage(self, before: np.ndarray, after: np.ndarray) -> np.ndarray:
        """sprite_coverage() for the nano layout's crop -> 56x128 coverage."""
        if before.shape != (DOOM_H, DOOM_W) or after.shape != (DOOM_H, DOOM_W):
            raise ValueError("expected two 200x320 frames")
        rows = slice(HUD_CROP_Y, HUD_CROP_Y + HUD_CROP_H)
        b = np.ascontiguousarray(before[rows], dtype=np.uint8)
        a = np.ascontiguousarray(after[rows], dtype=np.uint8)
        self._check(self._lib.op_mask_diff(_ptr(b), _ptr(a), b.size), "mask_diff")
        cov = np.zeros((HUD_VIEW_H, OLED_W), dtype=np.uint8)
        self._check(
            self._lib.op_mask_coverage_cfg(
                ctypes.byref(self._hud_cfg), _ptr(b), _ptr(cov)
            ),
            "mask_coverage_cfg",
        )
        return cov

    def hud_sprite_ids(self, id_window: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """Sprite id map of the nano crop (117x320, 0 = no sprite, larger = nearer) as the board
        builds it -> (56x128 coverage, 56x128 front-most id)."""
        ids = np.ascontiguousarray(id_window, dtype=np.uint8)
        if ids.shape != (HUD_CROP_H, DOOM_W):
            raise ValueError("expected a 117x320 id map")
        cov = np.zeros((HUD_VIEW_H, OLED_W), dtype=np.uint8)
        front = np.zeros((HUD_VIEW_H, OLED_W), dtype=np.uint8)
        cfg = ctypes.byref(self._hud_cfg)
        self._check(
            self._lib.op_mask_coverage_cfg(cfg, _ptr(ids), _ptr(cov)), "coverage_cfg"
        )
        self._check(self._lib.op_mask_ids(cfg, _ptr(ids), _ptr(front)), "mask_ids")
        return cov, front

    def nano(
        self,
        gray: np.ndarray,
        wall_cov: np.ndarray,
        spr_cov: np.ndarray,
        spr_id: np.ndarray,
        classes: np.ndarray,
        prev_mode: np.ndarray | None = None,
        hud: tuple[int, int, int, bool] = (100, 0, 50, True),
    ) -> tuple[np.ndarray, np.ndarray]:
        """Nano style: 56x128 gray, wall coverage, sprite coverage and ids, the 256 entry id ->
        op_spr_class_t table and last frame's 256 modes (or None) plus HUD values (health,
        kills, ammo, show_ammo) -> (64x128 frame of 0/1 as the board draws it, 256 modes).
        """
        g = np.ascontiguousarray(gray, dtype=np.uint8)
        wc = np.ascontiguousarray(wall_cov, dtype=np.uint8)
        sc = np.ascontiguousarray(spr_cov, dtype=np.uint8)
        si = np.ascontiguousarray(spr_id, dtype=np.uint8)
        if any(a.shape != (HUD_VIEW_H, OLED_W) for a in (g, wc, sc, si)):
            raise ValueError("expected 56x128 gray, coverages and ids")
        cls = np.ascontiguousarray(classes, dtype=np.uint8)
        prev = np.zeros(NANO_IDS, dtype=np.uint8) if prev_mode is None else prev_mode
        prev = np.ascontiguousarray(prev, dtype=np.uint8)
        if cls.shape != (NANO_IDS,) or prev.shape != (NANO_IDS,):
            raise ValueError("expected 256 entry class and mode tables")
        mode = np.zeros(NANO_IDS, dtype=np.uint8)
        spr = _NanoSprites(_ptr(sc), _ptr(si), _ptr(cls), _ptr(prev), _ptr(mode))
        bits = np.zeros((OLED_H, OLED_W), dtype=np.uint8)
        self._check(
            self._lib.op_nano(
                _ptr(g),
                _ptr(wc),
                ctypes.byref(spr),
                OLED_W,
                HUD_VIEW_H,
                ctypes.byref(self.nano_cfg),
                _ptr(bits),
            ),
            "nano",
        )
        h = _Hud(*hud)
        self._check(
            self._lib.op_hud_draw(ctypes.byref(h), OLED_W, OLED_H, _ptr(bits)), "hud"
        )
        return bits, mode

    def pack(self, bits: np.ndarray) -> bytes:
        """Stage 4: 0/1 pixels -> 1024 SSD1306 page bytes."""
        b = np.ascontiguousarray(bits, dtype=np.uint8)
        out = np.zeros(PACKED_BYTES, dtype=np.uint8)
        self._check(
            self._lib.op_pack_ssd1306(_ptr(b), OLED_W, OLED_H, _ptr(out), PACKED_BYTES),
            "pack",
        )
        return out.tobytes()


def unpack_ssd1306(packed: bytes) -> np.ndarray:
    """Inverse of stage 4, written independently in numpy: what the panel actually lights."""
    if len(packed) != PACKED_BYTES:
        raise ValueError(f"expected {PACKED_BYTES} bytes, got {len(packed)}")
    pages = np.frombuffer(packed, dtype=np.uint8).reshape(OLED_H // 8, OLED_W)
    img = np.zeros((OLED_H, OLED_W), dtype=np.uint8)
    for bit in range(8):
        img[bit::8, :] = (pages >> bit) & 1
    return img


def read_playpal(wad_path: Path) -> np.ndarray:
    """Return palette 0 of the PLAYPAL lump of a WAD as a (256, 3) uint8 array."""
    data = wad_path.read_bytes()
    if len(data) < 12 or data[:4] not in (b"IWAD", b"PWAD"):
        raise ValueError(f"{wad_path} is not a WAD file")
    num_lumps, dir_offset = struct.unpack_from("<ii", data, 4)
    if num_lumps < 0 or dir_offset < 0 or dir_offset + 16 * num_lumps > len(data):
        raise ValueError(f"{wad_path}: corrupt directory")
    for i in range(num_lumps):
        pos, size, raw_name = struct.unpack_from("<ii8s", data, dir_offset + 16 * i)
        if raw_name.rstrip(b"\0") == b"PLAYPAL":
            if size < 768 or pos + 768 > len(data):
                raise ValueError(f"{wad_path}: PLAYPAL too small")
            return np.frombuffer(data, dtype=np.uint8, count=768, offset=pos).reshape(
                256, 3
            )
    raise ValueError(f"{wad_path}: no PLAYPAL lump")
