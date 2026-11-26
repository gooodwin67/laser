# Vendored Three.js r181

This folder contains the unmodified official Three.js **r181** ES-module distribution:

- `three.module.js` — WebGL renderer and public Three.js module.
- `three.core.js` — required sibling import used by `three.module.js`.
- `LICENSE` — original MIT license.

Upstream source:
- https://github.com/mrdoob/three.js/tree/r181/build
- https://github.com/mrdoob/three.js/blob/r181/LICENSE

The game imports it via `../vendor/three-r181/three.module.js`, relative to
`game/index.html`. Do not remove `three.core.js` — this version of Three.js
uses `import ... from './three.core.js'` internally.

Both modules are stored in the repository and served by GitHub Pages.
No npm, external CDN or build step is required to run the game.

Other backgrounds in the current HTML are embedded `data:image/jpeg`
assets. The biathlon and duck scenes reuse existing local backgrounds to avoid
runtime Unsplash image requests. Visible Unsplash attribution links are still
ordinary links and do not load any external assets unless clicked.
