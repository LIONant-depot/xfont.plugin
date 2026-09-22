# The Font editor

Double-click a Font resource in the asset browser (or `OpenResourceEditor -Asset <guid>`) and it opens in its own window: the descriptor, the
compiled atlas, and live text drawn with the font. Everything the window changes about the font goes through commands on the editor's own undo
system, so an AI or a script does the same edits from the command console.

```
source/Editor/xfont_editor.h             the editor (session), its registration
source/Editor/xfont_editor_preview.h     the atlas view, text layout, the offscreen text renderer and the effect settings
source/Editor/shaders/                   the MSDF text shader pair and the glyph-bounds wire frame pair
```

A host includes `xfont_editor.h`: that registers the editor for the `font` type and compiles the resource loader (`xfont_xgpu_rsc_loader.cpp`)
into the host. The host provides an `xgpu::device` and the main `xgpu::window` as `xeditor::host` services: the live text is drawn into an
offscreen texture with a render pass on the window before the frame's UI is rendered, and the panel then shows that texture.

The generic half (document, `SetProperty`, `Save`, `Compile`, `Undo`, the window and its dock space) is `xeditor::descriptor_editor`
(`source/Tools/Editor/xeditor_descriptor_editor.h` in the xGPU tree).

## Panels

| Panel | |
|---|---|
| Font Texture | the compiled atlas, zoomable (wheel) and pannable (drag); with `ShowGlyphBounds` every glyph's packed rectangle is outlined |
| Font Live Preview | any text laid out with the font's advances and kerning and drawn through the MSDF shader; the wheel zooms toward the cursor, drag pans |
| Rendering Settings | the text, its size, and the effects: outline, bold, shadow, bevel, glow, italic (effects that need a distance field are hidden for BITMAP fonts) |
| Texture Info | the atlas texture as it is on disk: size, format, compression, mips |
| Description | the font's descriptor |

While the font compiles the panels say `(reloading...)`. The editor lets go of the compiled font when a compile starts and takes it again
when it is done. The atlas is a resource of its own compiled with the font, so the editor follows the atlas' compile as well (and a newer
atlas file on disk is picked up even when no compile announced it).

## Commands

Run as `<resource name>\<Command>` (see `list`) or `ResourceEditorCommand -Asset <guid> -Cmd <base64>`. Paths and values are base64.

| Command | |
|---|---|
| `ListProperties [-Filter text]` | every descriptor property with its value: the paths `SetProperty` takes |
| `SetProperty -Path -Value [-Before]` | one descriptor property (undoable) |
| `ListPreview [-Filter text]`, `SetPreview -Path -Value` | the live text settings (view state: not undoable, never dirties the font) |
| `FontInfo` | ready / reloading / not compiled, and for a ready font: output type, glyphs, kerning pairs, metrics, atlas size |
| `Save`, `Compile` | save the descriptor; validate, save and queue the compile |
| `Undo`, `Redo` | |
| `CompileStatus [-Lines n]` | how the last compile went: state, unsaved changes, validation errors, the end of the log |
| `SetView -View atlas\|live [-Zoom -PanX -PanY -Reset]` | zoom and pan of the two 2D views (view state) |
