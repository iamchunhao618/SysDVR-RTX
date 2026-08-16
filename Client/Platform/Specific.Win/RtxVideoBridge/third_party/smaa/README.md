# SMAA provenance

These files are copied without modification from the canonical SMAA repository
at commit `71c806a838bdd7d517df19192a20f0c61b3ca29d`:

https://github.com/iryoku/smaa

Included integration assets:

- `SMAA.hlsl`
- `Textures/AreaTex.h`
- `Textures/SearchTex.h`

The reference implementation and lookup data are MIT licensed. Modification
and source redistribution are permitted when the copyright and permission
notice are retained. `LICENSE.txt` contains the upstream license. The bridge
uses only the spatial SMAA 1x pipeline with the standard High preset: luma edge
detection, blending-weight calculation, and neighborhood blending. No temporal
or multisample SMAA mode is used.
