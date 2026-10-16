# 3D model (optional)

The editor's **3D view** button only appears when this folder has a model and
its credit. Without them the site works the same, minus that button.

1. Put the model here, e.g. `psp.glb`. It needs a mesh named `Screen` (the
   preview is drawn on it) and, for clickable buttons, the meshes
   `Arow Buttons`, `PS Buttons` and `L&R Bumpers`. Meshopt-compressed glTF is fine.
2. Add `model.json` next to it, with the credit the license asks for:

```json
{
  "file": "psp.glb",
  "title": "Model title",
  "author": "Author name",
  "url": "https://sketchfab.com/3d-models/...",
  "license": "CC BY 4.0",
  "licenseUrl": "https://creativecommons.org/licenses/by/4.0/",
  "changes": "recolored, screen texture removed"
}
```

Only use a model whose license allows it (for example CC BY, with this credit).
The credit is shown under the 3D view and in the editor's footer.
