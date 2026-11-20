"""Background removal for single-object concept images on a plain background (no model, no licence issues)."""
import numpy as np
from PIL import Image
from scipy import ndimage


def CutoutPlainBackground(Source: Image.Image, Tolerance: float = 85.0) -> Image.Image:
    Rgb = np.array(Source.convert("RGB")).astype(np.float32)
    Height, Width, _ = Rgb.shape
    Border = np.concatenate([Rgb[0], Rgb[-1], Rgb[:, 0], Rgb[:, -1]])
    BackColor = np.median(Border, axis=0)
    Distance = np.linalg.norm(Rgb - BackColor, axis=2)
    BackLength = float(np.linalg.norm(BackColor))
    Scale = np.clip((Rgb @ BackColor) / (BackLength * BackLength), 0.0, 1.5)
    ShadowResidual = np.linalg.norm(Rgb - Scale[..., None] * BackColor, axis=2)
    IsShadow = (Scale > 0.5) & (ShadowResidual < 14.0)
    Candidate = Distance < Tolerance
    Labels, _ = ndimage.label(Candidate)
    EdgeLabels = np.unique(np.concatenate([Labels[0], Labels[-1], Labels[:, 0], Labels[:, -1]]))
    Background = np.isin(Labels, EdgeLabels[EdgeLabels != 0])
    Foreground = ndimage.binary_opening(~Background, iterations=1)
    Foreground = ndimage.binary_fill_holes(Foreground)
    Labels, Count = ndimage.label(Foreground)
    if Count > 1:
        Sizes = ndimage.sum(Foreground, Labels, range(1, Count + 1))
        Foreground = Labels == (int(np.argmax(Sizes)) + 1)
    Rows = np.where(Foreground.any(axis=1))[0]
    LowStart = Rows[0] + int(0.55 * (Rows[-1] - Rows[0]))
    LowBand = np.zeros_like(Foreground)
    LowBand[LowStart:] = True
    ShadowLabels, _ = ndimage.label(IsShadow & LowBand & Foreground)
    Grounded = ShadowLabels > 0
    Foreground = ndimage.binary_opening(Foreground & ~Grounded, iterations=2)
    Foreground = ndimage.binary_fill_holes(Foreground)
    Labels, Count = ndimage.label(Foreground)
    if Count > 1:
        Sizes = ndimage.sum(Foreground, Labels, range(1, Count + 1))
        Foreground = Labels == (int(np.argmax(Sizes)) + 1)
    Alpha = ndimage.gaussian_filter(Foreground.astype(np.float32), 0.8)
    Alpha = (np.clip(Alpha * 1.2 - 0.1, 0, 1) * 255).astype(np.uint8)
    Out = np.dstack([Rgb.astype(np.uint8), Alpha])
    return Image.fromarray(Out, "RGBA")
