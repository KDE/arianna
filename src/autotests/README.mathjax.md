<!-- SPDX-FileCopyrightText: 2026 Arianna contributors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->

EPUB mathematics
================

The Reader settings contain a persistent `mathjax Support` switch
(`Mathjax Unterstützung` in German), enabled by default. The reader sends
`mathJaxSupport=true/false` on each EPUB delivery request, so changing the
preference does not depend on the separate BookServer process reloading its
configuration. The open reader uses its existing reload/location restoration
path after a change. When disabled, BookServer skips math detection/injection;
resource/reference delivery and publisher-owned EPUB scripts retain their
existing behavior. Requests without the parameter remain enabled.

The Appearance settings immediately below that switch contain independent
display and inline formula sizes (50–200%, default 100%), display alignment
(default centered), wide-equation
handling (line breaks, shrink, scrolling or overflow), optional inline line
breaks, and the SVG font cache (local, global or none). These controls are
disabled with reader MathJax support. Preferences are persisted and delivered
as `mathJaxRendering` JSON through the existing BookServer injection route.
Only whitelisted scalar settings are used; no author callbacks/configuration
are imported. The preview button opens the existing isolated formula window
with these preferences and updates it when they change. Separate preview
buttons open display or inline math. The existing `mathJaxScale` preference
remains the display size; `mathJaxInlineScale` is the new independent inline
size, initially 100%. Legacy delivery JSON with just `scale` continues to
apply that value to both types; new requests additionally carry `inlineScale`.
The appropriate size is selected before conversion/metric collection for each
formula, including MathML, EPUB-owned MathJax output and width-dependent reflow.

The paginator waits for display math to be rendered at its actual column
width before publishing geometry or restoring its anchor. Resizing renders
only the visible output from captured expanded MathML; original source nodes
stay in place. Display containers and their SVG roots also stay in place so
CFI-resolved live selections and annotation ranges retain their boundaries.
MathJax's normal stylesheet supplies alignment, overflow and
multiline box heights. Its SVGs are excluded from the generic image-size cap.
Inline line breaks use MathJax's multiple SVG fragments and browser wrapping.

BookServer detects MathML, `\(...\)`, `\[...\]`, `$$...$$`, conservative
`$...$` pairs, legacy `math/tex` data, and Calibre's `text/x-mathjax-config`
marker. Code/preformatted examples and existing `mjx-container` output are
excluded. Each TeX delimiter pair must be in one text node. Mathematics is
not inferred from images or arbitrary author JavaScript.

`EPubContainer::createServerReadyEpub()` injects an external reader bootstrap
only into necessary chapters in the temporary delivery archive. Stored EPUBs
and non-mathematical chapters remain unchanged. The existing resource and
reference rewriting paths are retained. Authenticated
`/mathjax/<session>/<asset>` routes serve embedded components, dynamic SVG
font data, speech workers and speech-rule JSON.

Calibre configuration is never evaluated or merged into reader configuration.
An old `MathJax.js` loader is made inert in the delivery copy. Recognized modern
EPUB loaders are retained and awaited. Without explicit display preferences,
MathJax 4 instances retain their typesetting configuration and rendered DOM.
Menu/zoom and
interactive explorer handlers are disabled on the live instance before
pagination; generated SVG, accessibility output and SRE speech labels remain.
This also overrides persisted click-to-zoom settings. MathJax 4's menu
constructor re-enables the explorer despite `enableExplorer=false`, so the
interactive a11y options must be disabled after startup. Existing output loses
only its event handlers, without retypesetting or replacing its DOM.
Failed/incompatible modern installations
are reported in the browser log; no second installation is started alongside
them. Dynamically inserted author loaders cannot be predicted.

When display preferences are requested for an EPUB-owned installation, its
original rendered tree is retained as the hidden CFI source. The same MathJax
document converts captured MathML into visible reader output; `tex-svg.js`
receives the already bundled MathML input parser if needed. No second MathJax
installation or author startup execution is introduced. An existing global
font cache keeps its CFI index, while newly added cache/output nodes are
excluded. Font-cache changes initialize fresh defs without replacing the
preserved source tree.

EPUB CSP is preserved. Foliate's `allowScript=false` makes the bootstrap inert.
No script permission or CSP exception is added. Initialization failures leave
source content available. Reader TeX packages are fixed to
base/AMS/newcommand/noundefined/configmacros/textmacros; `require`, autoload
and HTML extensions are disabled. The safe extension disables URLs, classes,
CSS IDs and styles in math input.
The unchanged MathJax MJX-ZERO font is also bundled as an OpenType asset.
A documented, reproducible adaptation of the two SVG bundles uses this local
URL instead of a data font in browsers, retaining the upstream Node fallback.
This fixes `font-src 'self'` without adding a data-font CSP exception. Optional
explorer settings are disabled only when that installation defines them;
unknown `voicing` or other a11y options are never introduced by the bootstrap.

For reader-supplied MathJax, asynchronous SVG conversion finishes before
wrappers are committed. Original MathML retains its IDs and children; original
TeX remains in hidden source spans. Transparent outer wrappers contain visible
SVG siblings in normal layout/column flow. Both renderers wait for initialization,
conversion, speech enrichment and fonts before publishing load/geometry events.

CFI compatibility
-----------------

Foliate's existing `getChildNodes()` supports `FILTER_SKIP` to flatten wrappers
and `FILTER_REJECT` to exclude output. Reader math also needs text split around
wrappers to be indexed as its original logical text chunk. Generated SVG
boundaries map to source; restored source boundaries map to visible math.
Formula selections are addressed as the complete source expression.

The serialized CFI format and stored annotation/reference data are unchanged.
EPUB-owned MathJax still addresses its original rendered DOM. The new filter only
excludes reader-created math nodes. The regression baseline checks literal
`epubcfi(/4/2,/3:1,/3:6)` and `epubcfi(/4/2/2)` locations after chapter reload.
The same literal CFIs are tested after applying reader display preferences
and recreating the chapter twice; resolved formula ranges point at visible SVG.

Rendered-DOM and source-DOM CFIs are different coordinate systems. Removing an
EPUB's own installation, changing its structure, or manually switching rendering
strategies can invalidate old CFIs. There is no silent migration or
reinterpretation. Keeping compatible EPUB-owned MathJax preserves that baseline.

Before document/selection reading, Arianna requests concise ClearSpeak speech
from the chapter's existing SRE worker using captured expanded MathML. Responses
are cached per formula, language and style without attaching new attributes,
retypesetting SVG, changing author configuration or modifying EPUB source.
An explicit Speech Dispatcher voice selects the formula-description language;
automatic voice selection retains the formula's SRE locale. Unsupported locales
and worker failures fall back to available labels.
Descriptions flow through existing `reader.js` extraction as atomic
`type: "math"` segments with a stable CFI and actual SRE locale. Existing QML
passes these to SpeechController. Fresh SRE descriptions can narrate formulas
above the legacy ten-token limit; stored-label fallback retains that limit.
Symbol counting uses each formula's captured MathML rather than a transient
conversion item reused by later expressions. Formula highlighting is unchanged.

Validation
----------

`arianna-epubmathtest` uses real QtWebEngine and BookServer. It covers TeX,
MathML, Calibre, archive/prose preservation, offline assets, visible SVG/speech,
source CFIs across chapter realm recreation, old rendered-DOM CFIs, exact SVG
font-outline/geometry parity, both renderer barriers, prose and restrictive CSP.
Formula clicks, double-clicks and context-menu events are tested with both
reader-supplied and EPUB-owned MathJax in reflowable and fixed layouts. They
must preserve SVG DOM, page geometry, restored CFIs and speech while allowing
events to reach Arianna's selection/reference handlers.
An ordinary formula click dispatches `show-math` through the existing reader
bridge. `MathJaxWindow.qml` displays the selected formula in a non-modal native
window. `mathjax-native-menu.js` adapts the pinned MathJax menu model to
`MathJaxMenu.qml`'s Qt Quick Controls menus and dialogs, using the Kirigami theme.
It retains all visible branches, availability checks, radio/checkbox state,
opacity controls, dynamic annotations, speech languages and ClearSpeak
preferences. Source, speech, Braille, SVG, help/about, scale and zoom dialogs
are native; the zoom dialog embeds the already rendered formula in a view
with scripts disabled. Copies use the Qt clipboard. No HTML menu or dialog is
posted. Only opaque action tokens and validated scalar values cross the bridge;
formula requests/menu revisions reject stale actions after reload. The adapter
uses MathJax's existing variable pool and actions, not a separate renderer or
a duplicate set of math settings. Keep the adapter's model/dialog tests when
upgrading the bundled MathJax version. Expanded MathML is captured during
conversion so EPUB macros are not re-executed. LaTeX source remains available
as an annotation and through a native copy button. The preview uses its own
off-the-record WebEngine profile and local MathJax assets, including CHTML
components and fonts for the menu's renderer switch. Settings and rerenders
affect only this preview, including settings persisted by MathJax. No author
configuration is copied. MathML is restricted to recognized MathML elements
and attributes before insertion; HTML, script handlers and external URLs are
excluded. Closing/changing the book clears the preview.
Right-clicking a formula opens the viewer's existing context menu with a
`MathJax (Formula Preview)` submenu containing the same complete native menu
hierarchy. Formula context replaces the image inversion action; ordinary
images retain it. Expanded MathML and the original collapsed CFI are passed
through the existing reader bridge. The formula window prepares its isolated
preview without showing a second window; menu actions and native source,
speech, preference and zoom dialogs are hosted by the viewer. Settings still
affect only the isolated preview. `Open Formula Dialog` shows that prepared
preview, and `Read Formula Aloud` uses its speech description and source CFI.
Closing the context menu cancels a pending model request, while an action
already in flight can deliver its native dialog. Book changes clear both hosts.
The preview's speech language initially follows an explicitly selected speech
voice, otherwise the system locale, and can be chosen independently in the
native window. Short ClearSpeak descriptions use `Fraction_Over` and
`Ellipses_AndSoOn`; detailed descriptions remain selectable. Generated plain
text is shown in a wrapping, selectable field and can be copied or sent to the
existing reader SpeechController. The native read action carries the original
formula's collapsed CFI and section index. EPUB DOM and saved locations remain
unchanged; an explicit read action in the formula window reads the displayed
description.

The suite exercises the actual reader click handler, an active selection,
the native window/menu button, native dialogs and clipboard, close/reopen,
profile isolation, source macros, every menu model type, dynamic annotation
copies, ClearSpeak preferences and rejected values, scale changes, native
zoom geometry, help/about and an offline CHTML/font switch. It checks
that the source book DOM remains unchanged throughout these menu operations.
Viewer context regressions exercise the full reader/WebChannel/QML route,
keyboard navigation through nested submenus, a source dialog and clipboard,
cancellation while loading, switching back to an ordinary SVG image, and
opening the prepared formula window. Selection, source DOM, pagination
geometry and the formula CFI remain unchanged.
Existing speech, EPUB, reference and search suites remain regression baselines.
Additional regressions cover the complete eight-theorem excerpt and its
Basel-series formula in English and German with reader-supplied and EPUB-owned
MathJax. They verify concise speech, the 23-token formula, unchanged DOM,
geometry and saved CFIs, the actual extracted segment's native SSIP delivery,
and explicit/automatic voice synchronization in the viewer. Preview tests cover
language/style changes, the generated native speech signal and the local
zero-font load under `font-src 'self'`. Console checks reject invalid MathJax
options and font-policy violations in these cases.
Display-setting regressions cover native keyboard interaction, persistence,
enabled state, delivery-query forwarding, all font caches, inline breaks,
wide-equation modes, width reflow, stable source CFIs and repeated preview
cache changes. They verify distinct display/inline sizes in TeX and MathML,
independent persisted sliders and both native preview buttons. Reader and
EPUB-owned `tex-svg.js` cases both use real WebEngine.

```
ctest --test-dir build -R 'arianna-(epubmath|speechcontroller|epubcontainer|epubreferenceanchor|bookserverreference|searchnavigation)test' --output-on-failure
```

Run in a session permitting loopback sockets and QtWebEngine. Offscreen tests
cover DOM/layout and native queues; they do not verify desktop selection
gestures or audible playback.
