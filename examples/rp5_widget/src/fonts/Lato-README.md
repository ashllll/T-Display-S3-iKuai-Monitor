# Lato Regular bitmap subsets

Source: https://github.com/google/fonts/tree/main/ofl/lato
Font: Lato-Regular.ttf; SIL Open Font License, see Lato-OFL.txt.
The generated ui_font_lato_* C data retains the font licensing terms.

Generated with lv_font_conv 1.5.3 (4bpp, no compression, no runtime scaling):

```sh
lv_font_conv --font Lato-Regular.ttf --size 12 --bpp 4 --format lvgl --no-compress --range 32-126 --lv-include lvgl.h --lv-font-name ui_font_lato_12 -o ui_font_lato_12.c
```

Use the same options for size 20. The console uses only 12px and 20px text.
