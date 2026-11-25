# Third-party notices

## Leia SR (Simulated Reality)

The optional Leia SR stereoscopic output uses the proprietary Leia Simulated
Reality SDK. No SDK or runtime files are distributed with this repository: the
build links against a locally installed SDK (import libraries only), and the
executable delay-loads every SR and OpenCV DLL so machines without the SR
Platform are unaffected and the feature falls back to a plain side-by-side
present. "Leia" and "Simulated Reality" are property of Leia Inc.

## SR-lib

`libs/SR-lib` is [SR-lib](https://github.com/bo3b/SR-lib) by bo3b, vendored as a
submodule (branch `api_expansion`). It wraps the SDK's DX9, DX11, DX12 and
OpenGL weavers, including the exception-safe `SRInterface*` classes and the
`CreateSRInterface*` factories used by this port. See the submodule for its
license and documentation.

## rt64-3D

The stereoscopic rendering and the Leia SR integration follow
[oneup03/rt64-3D](https://github.com/oneup03/rt64-3D), including the
`SetShaderSRGBConversion(false, false)` choice for a compose/swap-chain pipeline
that already carries the game's gamma.
