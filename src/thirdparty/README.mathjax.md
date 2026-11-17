<!-- SPDX-FileCopyrightText: 2026 Arianna contributors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->

MathJax runtime provenance
=========================

Browser components from official npm packages:

* `mathjax@4.1.3`, https://registry.npmjs.org/mathjax/-/mathjax-4.1.3.tgz
  integrity `sha512-BN/8Pkgn7G1pIDYJqd9md+JHsE/jydSYbyOZnSdSA0WziuVO8mRxdYiWFumkVVly/8U+hm9DpIIoWuvySverzw==`
* `@mathjax/mathjax-newcm-font@4.1.3`, https://registry.npmjs.org/@mathjax/mathjax-newcm-font/-/mathjax-newcm-font-4.1.3.tgz
  integrity `sha512-gzAB3dFHilHX1l5x2xUqRL+1jDQt3Fyza1DkEMVXWC4E8SvsGdlgEza47HYi2WhVcgfkvf4zgUGzuhbq3Pjlew==`

Included: `tex-svg.js` (the EPUB-owned regression baseline), `tex-mml-svg.js`,
`a11y`, `input`, `sre`, `ui`, and `output/chtml.js` from MathJax; `svg.js`,
`svg/dynamic`, `chtml.js`, and `chtml` (including WOFF2) from the font package.
CHTML is used by the separate formula window when its original MathJax menu
switches renderers. Building and running require no npm installation or network.
MathJax and its bundled Speech Rule Engine use Apache-2.0; see `mathjax/LICENSE`
and REUSE.toml.

When updating packages, update these checksums and regenerate `mathjax.qrc`
with all included runtime files, retaining the `mathjax-bootstrap.js` alias
for `../qml/mathjax-bootstrap.js`. Keep component, font and worker versions
together. Run `arianna-epubmathtest` with external requests blocked.

Local adaptation: `prepare-mathjax-font.py` extracts the unchanged MJX-ZERO
OpenType font from both SVG bundles and replaces its embedded data URL with
`fonts/mjx-zero.otf`, resolved relative to the executing bundle. The original
embedded source is retained for Node execution. This permits SVG inline breaks
under an EPUB's `font-src 'self'` without weakening its CSP or changing glyphs,
geometry or CFI addressing. After updating the bundles, run the script again;
it checks that both bundles contain identical font bytes. Other browser
components remain unmodified.
