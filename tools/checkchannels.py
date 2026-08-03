#!/usr/bin/env python
"""
Which RenderChannelOrder does this setup need?

Answers it by measurement instead of argument. Eyeballing a render does not
work: the scene-wide colour bias is only a few units, so a red/blue swap is
invisible in flat areas and obvious only on saturated or reflective surfaces -
which is exactly where "that is just a warm reflection" is a believable excuse.
It also cannot be settled from the swapchain format, because ReShade normalises
some formats on copy and not others, and guessing wrong double-swaps.

    python checkchannels.py <reshade_screenshot.png> <rendered_frame.png>

The two do NOT have to be the same instant or the same sharpness - the test is
on CHROMA (R-G and B-G), which survives motion blur, exposure differences and
video compression. It does not survive a colour grade applied to one and not the
other, so use a plain ReShade screenshot rather than one from another tool.
"""
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow:  python -m pip install pillow")


def chroma(path):
    img = Image.open(path).convert("RGB")
    w, h = img.size
    # Centre only. The editor screenshot carries a timeline and a button strip
    # that the render does not, and those are large flat UI colours that would
    # drag the averages around.
    img = img.crop((int(w * .15), int(h * .25), int(w * .85), int(h * .80)))
    n = img.width * img.height
    r, g, b = (sum(c.histogram()[i] * i for i in range(256)) / n for c in img.split())
    return r - g, b - g, r - b


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    s_rg, s_bg, s_rb = chroma(sys.argv[1])
    r_rg, r_bg, r_rb = chroma(sys.argv[2])

    print(f"screenshot   R-G={s_rg:+6.2f}   B-G={s_bg:+6.2f}   R-B={s_rb:+6.2f}")
    print(f"render       R-G={r_rg:+6.2f}   B-G={r_bg:+6.2f}   R-B={r_rb:+6.2f}")
    print()

    # Under a swap the render's R-B is the screenshot's negated; without one they
    # match. Score both and take the closer - the sign alone is enough when the
    # bias is clear, but a near-neutral scene needs the distances compared.
    same = abs(r_rb - s_rb)
    swap = abs(r_rb + s_rb)
    print(f"  matches unswapped by {same:5.2f}")
    print(f"  matches swapped   by {swap:5.2f}")
    print()

    if abs(s_rb) < 0.5 and abs(r_rb) < 0.5:
        print("INCONCLUSIVE - both frames are almost perfectly neutral, so a swap "
              "would not change them. Retry on a shot with some colour in it: "
              "foliage, sky, a coloured car.")
    elif swap < same:
        print("RESULT: red and blue ARE swapped.  Set RenderChannelOrder=2")
    else:
        print("RESULT: channels are correct.      Leave RenderChannelOrder=0")


if __name__ == "__main__":
    main()
