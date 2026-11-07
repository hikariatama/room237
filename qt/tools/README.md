# Asset tools

These regenerate files under `qt/app`; the build itself does not run them.

- `gen_assets.mjs`: writes `app/src/generated/icons_data.inc` from the `@tabler/icons` package
  (`node qt/tools/gen_assets.mjs <path-to>/@tabler/icons qt/app/src/generated`). `i18n_table.inc` next to it is
  edited by hand.
- `lottie/*.json`: source animations. `render_lottie.swift` renders every frame of one with lottie-web in an offscreen
  WebKit view (macOS), and `lottie_to_webp.py` joins the frames into the animated WebP in `app/resources/lottie`:

  ```sh
  swiftc qt/tools/render_lottie.swift -o render_lottie
  ./render_lottie lottie.min.js qt/tools/lottie/preloading.json frames/ 512
  python3 qt/tools/lottie_to_webp.py frames/ qt/app/resources/lottie/preloading.webp 60
  ```

  The shipped animations are 512×512; the sources run at 60 fps.
