# DMG background photograph

`dmg-background.png` (1584x990) is the background of the macOS disk image window. `make_banner.py` scales it to
640x400 and 1280x800 and adds the arrow; nothing else is drawn over it.

- Made on 7 November 2026 with OpenAI's built-in `image_gen` tool (through Codex), as a new photorealistic image for
  this project. It does not reproduce a photograph or a film still. The generated original was 1586x992 pixels,
  SHA-256 `dbcfe901bc8f5cde1c77e98dd917fa6e554b64377f39be0407d632bfcef3f500`.
- Edits after generation: a centred crop to 16:10 (1584x990) and an exposure gain of 1.25 in linear light, so that
  Finder's black and white icon labels both keep a 3:1 contrast on the wood. Nothing was painted in or removed.

Prompt:

```text
Use case: photorealistic-natural
Asset type: background photo for a macOS installer window (wide landscape, cropped to 16:10)
Primary request: a real photograph of an old grand-hotel corridor wall late in the evening, camera facing the wall squarely; the door of room 237 at the right stands open about one third and warm lamp light comes out of the room.
Subject: one hotel room door centred at about 73% of the image width, made of the same medium honey-oak as the wainscot, brass number plate "237" on the door at eye level.
Scene/backdrop: dark green and gold damask wallpaper above, dim; a chair rail at about 50% height; below it a medium honey-oak wood wainscot with simple raised panels; a geometric hexagon-patterned carpet in deep red, burnt orange and brown in the bottom 20%.
Composition/framing: straight-on, no one-point perspective; the left half is plain wall with one dim sconce high on the wall; the band between 55% and 75% of the image height (wainscot and the lower half of the door) is an even mid-tone with soft low-contrast grain, lit softly by warm light from below the chair rail, no deep shadows and no hot spots; the upper wall falls off into evening darkness toward the top corners.
Style/medium: documentary interior photography, full-frame camera, 35mm lens, natural grain, real materials.
Lighting/mood: quiet, slightly uncanny evening, warm tungsten; not horror.
Text (verbatim): only "237" on the plate; no other text, letters, signs, logos or watermarks.
Avoid: people, ghosts, twins, blood, CGI or 3D-render look, illustration, HDR, fisheye, dark lower door.
```
