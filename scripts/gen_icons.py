#!/usr/bin/env python3

import io
import sys
import typing
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from material_icons import MaterialIcons, IconStyle
from PIL import Image, ImageSequence

from LVGLImage import ColorFormat, CompressMethod, LVGLImage, RAWImage


@dataclass
class AnimationSpec:
    steps: int
    mask: np.ndarray | None
    direction: typing.Literal['left', 'right', 'up', 'down']


@dataclass
class EmbeddedIcon:
    id: str
    size: int
    rotation: int
    animation: AnimationSpec | None


ICONS: list[EmbeddedIcon] = [
    EmbeddedIcon('battery_full', 48, -90, AnimationSpec(16, None, 'right')),
    EmbeddedIcon('bluetooth_disabled', 32, 0, None),
    EmbeddedIcon('bluetooth_connected', 32, 0, None),
    EmbeddedIcon('bluetooth_searching', 32, 0, AnimationSpec(3, np.logical_and(
        np.triu(np.ones((32, 32))), np.tril(np.ones((32, 32)))[:, ::-1],
    ), 'right')),
    EmbeddedIcon('bolt', 32, 0, None),
    EmbeddedIcon('energy_savings_leaf', 32, 0, None),
    EmbeddedIcon('keyboard', 32, 0, None),
    EmbeddedIcon('keyboard_capslock', 32, 0, None),
    EmbeddedIcon('speed', 32, 0, None),
    EmbeddedIcon('sports_esports', 32, 0, None),
    EmbeddedIcon('usb', 32, 0, None),
    EmbeddedIcon('usb_off', 32, 0, None),
    EmbeddedIcon('wifi_tethering', 32, 0, None),
    EmbeddedIcon('wifi_tethering_off', 32, 0, None),

    EmbeddedIcon('file:scripts/nyan_cat.gif', -1, 0, None),
]
MATERIAL = MaterialIcons()


def force_opaque(im: np.ndarray, mask: np.ndarray | None = None):
    im = im.copy()
    if mask is None:
        mask = np.ones(im.shape[0:2])
    transparent = np.where((im[:, :, 3] < 128) * mask)
    im[:, :, 3][np.where(mask)] = 255
    im[transparent[0], transparent[1], :] = 0
    return im


def generate_embed(icon: EmbeddedIcon, dest: Path):
    png = MATERIAL.get(icon.id, size=icon.size, color="#000", style=IconStyle.ROUND)
    im = np.asarray(Image.open(io.BytesIO(png)).rotate(icon.rotation))

    if icon.animation:
        im = force_opaque(im, icon.animation.mask)
        alpha = im[:, :, 3]
        if icon.animation.mask is not None:
            alpha = alpha * icon.animation.mask
        Y, X = np.where(alpha != 0)
        dir = icon.animation.direction
        if dir == 'up' or dir == 'down':
            axis = sorted(set(Y), reverse=dir == 'up')
        else:
            axis = sorted(set(X), reverse=dir == 'left')
        for i, group in enumerate(np.array_split(axis, icon.animation.steps - 1)):
            for coord in group:
                if dir == 'up' or dir == 'down':
                    sel = Y == coord
                else:
                    sel = X == coord
                im[Y[sel], X[sel], 0:3] = (i + 1) << 4

    file = dest.joinpath(f'{icon.id}.png')
    Image.fromarray(im).save(file)
    LVGLImage().from_png(str(file), cf=ColorFormat.I4).to_c_array(
        str(dest.joinpath(f'{icon.id}.c')),
        CompressMethod.NONE if icon.animation else CompressMethod.RLE,
        f'icon_{icon.id}',
    )
    return f'LV_IMG_DECLARE(icon_{icon.id});'


def generate_raw(icon: EmbeddedIcon, dest: Path):
    file = Path(icon.id[len('file:'):])
    icon.id = file.with_suffix('').name
    if file.suffix.lower() == '.png':
        LVGLImage().from_png(str(file), cf=ColorFormat.I4).to_c_array(
            str(dest.joinpath(f'{icon.id}.c')),
            CompressMethod.RLE,
            f'icon_{icon.id}',
        )
        return f'LV_IMG_DECLARE(icon_{icon.id});'
    elif file.suffix.lower() == '.gif':
        gif = Image.open(file)
        pngs = []
        for i, frame in enumerate(ImageSequence.Iterator(gif)):
            png = dest.joinpath(f'{icon.id}_{i}.png')
            frame.save(png)
            LVGLImage().from_png(str(png)).to_c_array(
                str(dest.joinpath(f'{icon.id}_{i}.c')),
                CompressMethod.RLE,
                f'icon_{icon.id}_{i}',
            )
            pngs.append(i)
        with path.joinpath(f'{icon.id}.c').open('w') as f:
            f.write(f'''#include <lvgl.h>

{'\n'.join(f'#include "{icon.id}_{i}.c"' for i in pngs)}

const lv_image_dsc_t *icon_anim_{icon.id}[] = {{
{'\n'.join(f'  &icon_{icon.id}_{i},' for i in pngs)}
}};
''')
        return f'extern const lv_image_dsc_t *icon_anim_{icon.id}[{len(pngs)}];'
    else:
        raise Exception(f'Unsupported file: {file}')


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <output_dir>')
        exit(1)

    path = Path(sys.argv[1])
    path.mkdir(parents=True, exist_ok=True)
    declarations = []
    for icon in ICONS:
        if icon.id.startswith('file:'):
            declarations.append(generate_raw(icon, path))
        else:
            declarations.append(generate_embed(icon, path))

    with path.joinpath('icons.c').open('w') as f:
        f.write(f'''#define LV_LVGL_H_INCLUDE_SYSTEM 1

#include "icons.h"

{'\n'.join([f'#include "{icon.id}.c"' for icon in ICONS])}
''')

    with path.joinpath('icons.h').open('w') as f:
        f.write(f'''#pragma once

#include <lvgl.h>

{'\n'.join(declarations)}

{'\n'.join([f'#define DATA_LEN_icon_{icon.id} {icon.size ** 2 + 4 * 16}' for icon in ICONS if icon.animation])}
{'\n'.join([f'#define ANIM_STEPS_icon_{icon.id} {icon.animation.steps}' for icon in ICONS if icon.animation])}
''')
