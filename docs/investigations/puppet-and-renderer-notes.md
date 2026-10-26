# Puppet and renderer findings

The long dated write-ups that used to be here followed bugs that are closed now. What they found, and still matters when
touching puppet rendering (the matching code is described under "Puppets" and "Scene, layout and offsets" in `CLAUDE.md`):

- **Attachments**: a child's world transform is exactly `parent * attachment point * child local` (`CImage::getAttachmentMatrix`,
  `CScene::parentChainMatrix`). The attachment matrix's static rotation orients the point's frame only; no extra pivot
  rotation is needed. Checked on mikasa's eye (3764765600) against a RenderDoc capture of live WE: the eye's MVP rotates by
  2.26 degrees, own angle 44.57 + bone rest -45.09 + animated 2.79.
- **`cropoffset` is never added**: the editor already bakes it into `origin`, puppet or not (gura 2529364511).
- **Paint order** is the scene.json array order (plus `transparentsorting` in perspective scenes). Seams like asagi's eye ring were not
  a sorting problem but the intermediate-buffer premultiply of `CPass`, and a containment heuristic that hid layers was removed.
- **Torn textures** (koshini's bow, ahri's torso) came from the MDLV vertex layout: the stride is the sum of per-flag sizes from the
  25 entry tables in `wallpaper64.exe` (`xmmword_140369CE0` masks, `xmmword_140369D50` sizes), see `docs/rendering/MDL_FILES.md`.
- **Hotswap**: the outgoing `Project` must outlive `setWallpaper()`, the new `CWallpaper` keeps a raw reference into it; when building
  the new wallpaper fails the old one goes back in place.
- Live WE can be traced under Wine (`tools/wecompare/`: bind traces, RenderDoc captures); llvmpipe cannot render some puppets
  correctly, so judge puppet output on the GPU driver or against live WE.
