# Task guide Chinese bitmap font

`main/display/cjk_font_16_data.inc` is a generated 16px monochrome subset,
named **Focus Task CJK16**, of the locally installed **Noto Sans SC** Regular
variable font (`C:/Windows/Fonts/NotoSansSC-VF.ttf`). It contains GB2312 level 1
(3755 Chinese glyphs), printable ASCII and the task guide UI's punctuation and
extra characters: 3864 glyphs, **131376 bytes** of flash data. No glyph table is
copied to RAM; rendering writes directly to the existing RGB565 canvas.

The input's embedded name-table metadata was checked for the Noto Sans SC family
and SIL Open Font License declaration. The generated data header records the
input SHA256 for reproducibility. Microsoft Windows proprietary fonts are not
used. Original upstream: <https://github.com/notofonts/noto-cjk>; license copy:
<https://github.com/google/fonts/blob/main/ofl/notosanssc/OFL.txt>.
Keep `OFL-NotoSansSC.txt` with redistributions of the generated font data.

Regenerate with Python + Pillow:

```powershell
python head/tools/fonts/generate_cjk_font_16.py --font C:/Windows/Fonts/NotoSansSC-VF.ttf
```

An optional `--preview <file.png>` renders a 480x480 sample from the actual
generated monochrome rows. This is a layout/font preview, not a device capture.

The renderer uses 8px ASCII advances and 16px CJK advances. Unavailable glyphs
render as hollow boxes, with at most 16 missing-codepoint logs per boot.
This common subset does not claim full Unicode or all Chinese names coverage.

Native verification (no board commands):

```powershell
./head/tests/test_cjk_font.ps1
```
