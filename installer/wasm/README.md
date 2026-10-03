Build artifacts of the shader tools go here (and are ignored by git): `scan`, `hlsl`, `pack` and `dxc_web`, each a
`.mjs` and a `.wasm`. `.github/workflows/installer.yml` builds them with Emscripten and publishes them with the page;
for a local run see ../README.md ("Building the WebAssembly tools").
