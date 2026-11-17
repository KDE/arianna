# Speech tests

The normal `arianna-speechcontrollertest` uses an isolated SSIP server. It tests
the native libspeechd connection, queue, voice selection, index callbacks, stale
notifications, EPUB word underlines and the EPUB/PDF selection controls.

EPUB block traversal lives in `qml/speech-blocks.js`; `qml/reader.js` extracts
text, languages and source CFIs for the native controller. The upstream
foliate-js TTS implementation is not part of Arianna. Document viewer cases
exercise the block traversal through the bundled WebEngine resources.

## Bengali through the existing RASA runtime

With **Automatic (text language)** selected, Bengali (`bn`, including `bn-BD`
and `bn-IN`) prefers Speech Dispatcher's `vits` module, as Sanskrit already
does. Voices are discovered from Speech Dispatcher; the existing `bn_female1`
and `bn_male1` registrations use RASA speakers 2 and 3. A manually selected
voice still overrides automatic language selection for all text. Sanskrit
detection, voice selection, number expansion and playback behavior are unchanged.

Mark Bengali text explicitly in the EPUB's XHTML, for example:

```xml
<p lang="bn" xml:lang="bn">দুষ্ট মন! তুমি কিসের বৈষ্ণব?</p>
<p lang="sa" xml:lang="sa">धर्मः रक्षति।</p>
```

Language attributes may also be inherited from an enclosing `span`, `section`,
`body` or `html` element. A fully Bengali EPUB should declare `bn` in its OPF
metadata (`<dc:language>bn</dc:language>`). In multilingual books, tag individual
passages with their own language. Existing `lang`/`xml:lang` extraction and OPF
fallback are reused; Bengali is not inferred from the script, which is also
used for Assamese. No EPUB speech-voice names or model identifiers are needed.

Storage decision: explicit language attributes are source data (A); choosing
an available voice is a transient derived decision based on those attributes,
the user preference and Speech Dispatcher's current voice list. No new cache,
database field, language inference, EPUB modification or migration is added.
Original text, source offsets and CFIs remain unchanged.

`bengaliVoiceSelection` tests regional tags, RASA preference even when the
default module offers a competing Bengali voice, and manual overrides.
`bengaliLanguageSwitching` checks German/Bengali/Sanskrit transitions and
original mark/CFI callbacks. `ARIANNA_TEST_BENGALI=1` enables `realBengali`
against the current daemon, testing automatic routing and the male voice.

## Sanskrit number words

Storage decision under the [derived EPUB data directive](../../AGENTS.md):
the original numeral and explicit document language are source data (A).
Number-word expansion is a deterministic derived representation (B), conditional
on the resolved language, selected voice, lookup table and session preference.
An inferred language remains heuristic data (C), not authoritative EPUB metadata.
The user preference is application data (D), persisted through existing
KConfigXT; it is not disposable analysis cache data.

The expanded text is inexpensive to reproduce and stays in memory only while
preparing/submitting a speech payload. `SanskritNumbers` computes it;
`SpeechController` owns its use and the reading session. The existing original
segments and CFIs remain the basis for repeats, pause/resume and callbacks.
There is no separate book cache, analysis table, saved converted text or EPUB
export. New source segments, changed rules or a new session's options produce
fresh payloads; no result is reused across sessions. Existing session generation
checks reject stale callbacks after cancellation or document changes. No
analysis-data migration or persisted algorithm/model version is required for
these transient results. Manual corrections and annotations are not rewritten.

Settings → Reader → Read Aloud contains **Spell out Sanskrit numbers when
reading aloud**, disabled by default. The saved `Speech.speechExpandSanskritNumbers`
value is captured when a reading session starts, including subsequent batches,
repeats and pause/resume. It expands standalone whole numbers from 0 through 100
when both the resolved passage language and the selected voice are Sanskrit.
Explicit Hindi and other language tags remain authoritative. ASCII `13` and
Devanagari `१३` both become `त्रयोदश`.

`sanskritnumbers.cpp` contains the complete compiled lookup table. The fixed
cardinal forms are for counting and verse/chapter labels: 1–4 use the neuter
forms, and one accepted variant is chosen for each remaining number. There is
no grammatical inflection, ordinal conversion or contextual sandhi. Sources:
[Learn Sanskrit: Number words](https://learnsanskrit.org/guide/nominals/number-words/),
[Sanskrit Documents: cardinal numbers](https://sanskritdocuments.org/learning_tools/numbers_sa.html)
and [cardinal/ordinal comparison](https://sanskritdocuments.org/learning_tools/subantaruupaNi.html).
The table uses the standard `त्रयस्त्रिंशत्` for 33
([Monier-Williams](https://sanskrit.uohyd.ac.in/SKT/MW/110.html)) and normalizes
obvious transcription/sandhi errors in the lists, such as `चत्वारिंशत्`,
`षडशीतिः` and `षण्णवतिः`.

Leading zeros, mixed digit scripts, numbers outside the table, signed values,
percentages, and connected numeric expressions (decimals, chapter/verse
references such as `१.१३`, times, fractions and ranges) are left unchanged.
Only the backend payload changes. Segment text, source offsets, word marks,
CFIs, document bytes and selections retain their original values. SSML slices
use the complete source segment to avoid converting part of a numeric
expression at an index mark. The normal plain-text fallback uses the same rules.
No Codex connection or network access is involved in pronunciation.

`sanskritNumbers` checks counting forms, irregular spellings, punctuation and
excluded expressions. `sanskritNumberPronunciation` checks plain text, SSML,
fallback, opt-out, language/voice selection and original callback coordinates.
`sentencePauseSettings` also tests the new switch and persisted enabled/disabled
values through QML and configuration reloads.

## German abbreviations

Common German abbreviations (`bzw.`, `z. B.`, `u. a.`, `usw.`, `etc.`, `d. h.`,
`ca.`, `Nr.`, `Dr.`, `z. T.`, `i. d. R.` and `u. Ä.`) are expanded while
preparing speech when the segment is German. If the segment has no language
tag, the selected voice language is used. Other languages are left unchanged.
In the name `A. C. Bhaktivedanta`, the initials are spoken as `Ah Zeh`; the
same context-specific rule accepts compact initials and non-breaking spaces.
German numeric ordinals from 1 through 999,999 are spoken as German ordinal
words (`1. Stufe` → `erste Stufe`, `21. Stufe` → `einundzwanzigste Stufe`).
Recognized weak-declension contexts such as `der 3. Tag`, `im 4. Kapitel` and
`am 5. Tag` receive the corresponding `-e` or `-en` ending. Where context is
unclear, the configured fallback is `-e`.
This deterministic text transformation is only applied to the temporary speech
payload; source segments, word marks, CFIs, document bytes and selections remain
untouched. SSML slices map back to source boundaries so an abbreviation split
by a word mark is still expanded consistently. The reader also masks
abbreviation and ordinal periods during sentence detection, so `z. B.` and
`1. Stufe` do not trigger false sentence breaks. A final abbreviation before a
closing quote and a new sentence keeps its final period as a sentence boundary,
so quoted `usw.` does not merge multiple sentences into one speech segment. No
setting, cache, database or EPUB change is needed. `germanAbbreviations` covers
text, SSML marks, the plain-text fallback and the language gate;
`speechAbbreviationSentenceBoundary` covers sentence detection in the EPUB
reader. If a speech backend does not return word index marks, the EPUB reader
temporarily estimates word timing from sentence length and the configured rate.
It uses existing word CFIs or derives them only when DOM words still match the
spoken segment exactly; actual backend marks always take precedence. This is
transient presentation state and is not persisted.

Ctrl-clicking **Read Aloud** reads from the start of the selection to the end
of an EPUB. Ctrl+Enter/Space on the focused menu item also starts this
mode. An ordinary activation uses the chosen reading mode. The same pause,
resume and stop controls apply to all modes.

The continuous-queue test covers refills, empty sections, pause while awaiting
a refill, rate changes and stale replies after Stop. The `epub-document` viewer
case verifies real Ctrl-click input, starting after earlier text, exclusion of
hidden text and non-linear spine items, word-driven page turning, empty
chapters, chapter transitions and batches exceeding 24 sentences.
Normal EPUB and PDF selection reading is covered separately.

EPUB and PDF share `SpeechControlWindow.qml`, a small non-modal native tool
window with a movable title bar. All six speech controls live there; the book
toolbar has no speech launcher. Starting speech from the selection menu or
a formula opens the window automatically.
The rate correction is shown as a signed percentage (`0%`, `+10%`, `-10%`).
The Faster/Slower arrows are stacked to its right, with Faster above Slower.
Both selection menus use the bundled read-aloud icon. The window-manager Close
button stops audio and clears the native queue, including paused playback and
selection repeats. Both viewers cancel the document stream and reject
delayed batches. Its position survives the next start; changing or leaving the
book closes it and cancels speech through the reader's existing lifecycle.

The viewer cases verify the separate window and control ordering, automatic
opening, position retention, queue cancellation when closing during speech or
pause, closing while awaiting a first batch, rejection of stale callbacks and
batches, restarting from the selection menu, and cleanup when the book changes.
Native title-bar dragging remains a compositor
interaction; the offscreen tests verify window flags and position retention.

The speech mode button in both viewers cycles through **Read Selection Once** (linear
arrows), **Repeat Selection** (cyclic arrows), and **Continuous Reading** (∞).
Repeat keeps the selection captured at Start, including all language segments, formulas and
word CFIs; later changes to the live selection do not alter the active loop.
Switching to continuous reading finishes the captured selection and then reads
beyond its end, without restarting the active utterance. Starting a selection
in continuous mode reads from its start through the document.
Leaving continuous mode finishes only the current sentence; switching onward
to repeat loops that sentence, including inline formula and language pieces.
The sentence remains available if it ends between the Once and Repeat clicks;
explicit Stop or a new session discards it.
Later submitted Piper sentences are cancelled at their boundary mark, and queued
or delayed stream batches are discarded. Pause/Resume keeps its place,
and Stop or closing the book cancels the loop. The preference lasts for the
viewer instance and defaults to linear reading. Closing the speech window
stops every mode and keeps the chosen mode for the next start.

In the PDF selection menu, Ctrl-click and Ctrl+Enter/Space start continuous
reading at the first selected text entity. Source positions use Okular's
reading order and selection geometry, so repeated words cannot move the start
to an earlier occurrence. Read-only batches contain up to 24 segments and yield
between pages; textless pages are skipped. Prefetching does not change the
visible page; playback moves it when a segment from a later page starts.
PDF line breaks become spaces for speech and sentence detection, retaining
the original source offsets. Sentences spanning physical lines stay together.
`pdfContinuousReading` tests mouse/keyboard activation, wrapped lines, repeated
text, page transitions, refills, pause, finite modes, stream promotion,
window-close and book-change cancellation with Piper packets and plain-text
fallback. It also verifies unchanged selection geometry and PDF bytes.

Document streams, including their final refill, never repeat. `selectionRepeat`
and `preparedSelectionRepeat` test cycle boundaries, fresh index marks,
language changes, pause/stop, stale callbacks and continuous-stream exclusion.
The `epub-repeat` and `pdf-repeat` viewer cases exercise actual mouse clicks,
selection snapshots, pause/resume and Stop. EPUB additionally verifies original
CFI ranges and word underline geometry over successive cycles.
`selectionToContinuous` verifies uninterrupted extension past the captured
selection. `sentenceModeChanges` covers finishing/repeating the active sentence
with prepared and per-sentence output, while speaking or paused, and rejects
stale refills after the transition.
The `epub-toggle-once` and `epub-toggle-repeat` viewer cases verify the full
selection-to-stream-to-sentence path, original CFIs, rapid clicks before BEGIN,
and repeating a sentence that has already ended. `finishInlineSentence` covers
sentences split around a formula. With `ARIANNA_TEST_SPEECH_MODULE=piper`,
`realSentenceModeChanges` checks both finite targets during actual playback.

For the native `piper` output module, Arianna sends sentences from one EPUB DOM
block (at most 2048 pronounced-text characters together) as explicit SSML `<s>` sentences.
The former four-sentence limit is removed. Speech extraction separates `<p>`
and `<h1>`–`<h6>` before sentence detection, even without final punctuation.
Each segment carries a CFI at its block's start as `blockId` and its HTML tag as `blockTag`;
inline markup and language changes keep the same block identity. Logical packages never
cross blocks and must have the same language, selected voice and chapter.
For paragraph text, up to four logical packages (at most 8192 pronounced-text
characters) are submitted as one preload window. Explicit SSML `<p>` boundaries
retain the individual packages, including pieces of an oversized paragraph;
each sentence remains an explicit `<s>` with its own segment and word marks.
The native Piper producer can therefore prepare audio across paragraph/package
boundaries while the previous audio plays. The configured, speed-adjusted paragraph
pause applies when the DOM block changes; splitting a long paragraph into packages
keeps the sentence pause. Headings, display formulas, language/voice changes
and chapter boundaries end the window. There is no persistent audio cache.
Continuous reading finishes the current block at the usual 24-segment refill
boundary, while retaining the 8192-character refill limit for large content.
Segments without block identity use the per-segment fallback. Math and unmarked segments
remain separate. The updated native module copies PCM and marks into a bounded
queue of two waiting chunks and computes ahead while the current audio plays.
It continues to use in-process libpiper and Speech Dispatcher audio helpers.
Other output modules retain the original per-segment behavior.

Continuous reading requests one additional DOM batch as soon as the last
available Piper window is submitted. The reply is queued without advancing the
highlight or starting another utterance. Preloading only extracts from the
current rendered chapter; a transition requiring navigation is deferred until
the current audio ends. Stop/session changes discard late replies. Audio
inference between separate Speech Dispatcher jobs still incurs startup latency;
the standard client API cannot preload PCM across those jobs.

Sentence-boundary marks drive `segmentStarted`/`segmentFinished`; word callbacks
keep their original segment-local offsets and CFI. Marks before `<s>` preserve
the sentence underline even if that sentence cannot supply word alignments.
The Reader settings provide **Sentence pause length in ms** (default 200) and
**Paragraph pause length in ms** (default 400), independent integers from 0 to
500. These are the base pauses at normal speed. Within a
Piper batch both scale with the module's speech-duration factor: `1 - rate/150`
for faster reading and `1 - rate/50` for slower reading. At +50/-50, a 200 ms
base becomes 133/400 ms; -100 yields 600 ms. Zero omits the additional break.
The 400 ms paragraph base becomes 267/800 ms at +50/-50. A paragraph pause
replaces the sentence pause at that boundary, rather than adding to it.
Changes to the base or rate apply to the next batch, or when a paused batch
is rebuilt on Resume. Already submitted audio retains its original pauses.
Between sentences Arianna sends the resulting `<break time="…ms"/>` before the
next boundary mark. The native module waits during playback, while synthesis
continues ahead; Stop/Pause interrupts this wait. The next sentence's underline
and word callbacks start after the pause. Single sentences have no added pause.
Pause drops prepared audio and resumes at the current sentence, without
repeating completed sentences. Rate changes apply to the next submitted batch;
Pause/Resume applies them when restarting the current sentence.

`sentencePause` checks the emitted SSML at normal, faster and slower rates,
zero pauses, range limits and single sentences. `sentencePauseChanges` covers
live preference/rate changes and rebuilding a paused batch. `sentencePauseSettings`
exercises both actual Reader controls, their defaults, integer bounds, independent
saving and reloading. `paragraphPause` covers distinct sentence/paragraph pauses,
speed scaling, disabling either pause and marker ordering; `paragraphPauseChanges`
covers updates between preload windows and after Pause/Resume.

`preparedSentences` covers grouping, mark routing, language/math boundaries,
pause/resume, stale callbacks and rejected-SSML fallback. `paragraphPackages`
checks six-sentence packages, separate headings/paragraphs, missing block data
and the 2048-character size limit, including expanded IAST pronunciations.
`preloadedParagraphs` checks the four-package window, paragraph pauses, original
word metadata, pause/resume, stale callbacks, Stop and rejected-SSML fallback.
`continuousPreload` verifies early text refills, pause while accepting a refill,
deferred chapter requests and cancellation. The paragraph viewer cases also
verify word underlines across package boundaries and that preloading does not
turn to the next chapter prematurely.
`epub-paragraph-selection` and `epub-paragraph-document`
verify actual EPUB DOM extraction, block CFIs, heading/paragraph boundaries
without punctuation, mixed languages and word underlines. The document case
also sends a 26-sentence paragraph in one package across a refill boundary.
The opt-in
`realPreparedPause:sentences` and `realPreparedPause:paragraphs` check pause/resume
and a rate change against real buffered audio. The paragraph case also reports
the first-word start interval, which can be compared with the module's logged
PCM duration to measure the additional boundary pause.
`realViewerIndexMarks:prepared-selection` and `prepared-document` verify
actual word/CFI underlines for three buffered German sentences, followed by
English and, in continuous mode, another chapter. Run these against the updated
native module; the matching backend tests are `src/tests/piper_runtime.py` in
the Speech Dispatcher checkout. They measure the gap beyond PCM duration and
verify that Stop discards buffered audio and future marks.

The `epub-math-selection` and `epub-math-document` cases consume prepared
MathJax 4 speech labels, including `data-semantic-speech-none` and SSML labels.
They verify inline and display formulas,
SVG-only selections, DOM order, suppression of glyph/assistive-MathML duplicates
and hidden formulas, voice language, pause/stop, and a whole-formula underline
that survives replacement of the SVG. The selection case also delays the
EPUB-owned MathJax loader, startup promise and accessibility-worker result.

Arianna reuses the MathJax instance in the chapter iframe, including an async
`MathJax-script` loaded from a CDN. It waits for startup and `whenReady()` with a
10-second timeout, then uses any available labels. Missing formula labels are
reported in the browser log; the surrounding text continues. Formula speech
is one plain-text segment with no word marks; embedded SSML is stripped when
there is no unmarked label. `data-semantic-locale` identifies the actual
label language; inherited HTML/XML language and book metadata are the fallback.
For chapters served with Arianna's MathJax bootstrap, document and selection
reading request concise ClearSpeak descriptions from the existing SRE worker.
The expanded MathML is captured per expression, so neither EPUB macros nor
rendering run again. Language/style variants are cached outside the DOM.
An explicitly selected Speech Dispatcher voice determines the description
language; Automatic keeps the formula's SRE locale. No EPUB configuration edit
is needed. SVG, original text, geometry and CFIs remain unchanged.
Fresh SRE descriptions can narrate formulas above ten tokens; the legacy
complexity limit still applies when only stored labels are available.
The offline `epubmathtest::baselSeriesReading` cases exercise the supplied
eight-theorem excerpt through real local MathJax/SRE and native SSIP delivery
in English and German, for reader-supplied and EPUB-owned `tex-svg.js`.
`viewerControls:epub-controls` also checks explicit/automatic voice changes
reaching the reader's math-speech options and the formula-window read action.

To additionally load the actual CDN bundle and verify a typeset TeX formula,
its speech label and SVG underline, opt in to this network test:

```sh
ARIANNA_TEST_MATHJAX_CDN=1 \
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
QTWEBENGINE_DISABLE_SANDBOX=1 QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
./build/bin/speechcontrollertest viewerControls:epub-math-selection
```

The real-backend tests are opt-in because they produce audible speech. For
Piper, configure the native `sd_piper` module with alignment-enabled models and
register German and English voices. Set `DefaultModule piper` to test automatic
voice selection in the viewer. Arianna discovers the registered model names
through Speech Dispatcher; no model paths or `-aligned` suffixes belong in the
application. In Settings, **Automatic** follows each segment's language; an
explicit voice remains authoritative.

With automatic voice selection, Devanagari speech runs and EPUB `lang` or
`xml:lang` tags for `sa`/`sa-Deva` prefer the registered Sanskrit voices from
the native `vits` module. No model paths are stored in Arianna. Mixed-script
EPUB paragraphs are split into language runs before sentence segmentation;
surrounding English/German text retains its own voice. Inherited language
tags are respected, including explicit Hindi and other Devanagari languages.
Unmarked Devanagari is treated as Sanskrit for these books. A plain selection
containing only Devanagari has the same fallback in `SpeechController`;
unstructured mixed-script input retains its declared language. An explicitly
selected voice remains authoritative in all cases.

The original DOM, UTF-16 offsets, segment text and word CFIs are preserved.
VITS index callbacks use the same EPUB word underlines as Piper. The native
VITS module has Stop but no resumable Pause; Arianna cancels its current
utterance on Pause and restarts that segment on Resume.

`sanskritVoiceSelection` and `sanskritPauseAndQueue` cover voice preference,
manual voices, inherited metadata fallback, SSIP marks and queue refills.
`viewerControls:epub-sanskrit-selection` and `epub-sanskrit-document` verify
mixed text, nested markup, both language attributes, CFIs, underlines and
chapter transitions through the real EPUB viewer with an isolated SSIP server.
To additionally test the running native VITS module and its actual index marks:

```sh
ARIANNA_TEST_SANSKRIT=1 \
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
QTWEBENGINE_DISABLE_SANDBOX=1 QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
./build/bin/speechcontrollertest realSanskrit
```

For English and German voices, `SpeechController` converts words containing
IAST diacritics to pronunciation approximations immediately before sending
text to Speech Dispatcher. For example, `Śaṅkarācārya`, `Śrī` and `Kṛṣṇa` become
`Shankaracharya`, `Shree` and `Krishna` in English, or `Schankaratscharja`,
`Schri` and `Krischna` in German. The selected voice determines the spelling;
the segment language and explicit voice choice are preserved. NFC and
decomposed input are both accepted. A lone Spanish ñ and words containing
other non-IAST letters retain their original spelling. This is a heuristic
for readable speech, not an exact Sanskrit phonetic transcription.

Original segment text, source maps, word offsets and CFIs remain unchanged.
SSML marks are inserted at original word boundaries and reference the
original word metadata even when a pronunciation alias has a different
length. Plain-text output and rejected-SSML fallback use the same aliases.
`iastPronunciation` tests both languages, explicit voices, Unicode forms and
index callbacks; the EPUB menu and toolbar cases verify original DOM/CFI
underlines across the aliases. `realIndexMarks:de-iast` and `en-iast` verify
these callbacks with the configured real backend.

EPUB crossrefs use reader-only speech aliases. The reader obtains the target
book title by `targetBookId` from the read-only `/books` and `/<bookId>/refs`
endpoints, then replaces the abbreviation in the normalized anchor hint with
that title. Chapter, verse and equation names follow the selected voice's
German, English or Sanskrit language, or the source language in Automatic.
Capitalized publisher titles are prepared as words; Roman volume numbers stay
unchanged. The native backend transcribes Devanagari titles for German/English
voices and recognized IAST Sanskrit titles for Sanskrit voices. These are
pronunciation approximations, not translations of book titles.

Crossrefs remain atomic within the surrounding sentence, including dotted
identifiers. Every alias word mark points to the whole original anchor.
Neither EPUB text, anchor attributes, translation/copy text nor the archive is
modified. Missing metadata falls back to the existing hint, then visible text.
`viewerControls:epub-crossref-de`, `epub-crossref-en`, `epub-crossref-document`
and `epub-crossref-repeat` cover metadata retrieval, all three reference kinds,
modern/legacy anchors, partial selections, original CFIs and underlines,
hidden/ordinary links, metadata invalidation and unchanged archives.
`referencePronunciation` verifies SSML callbacks and rejected-SSML fallback;
`realIndexMarks:de-reference` and `en-reference` test the installed backend.

After building `speechcontrollertest`, run against the current daemon:

```sh
ARIANNA_TEST_SPEECH_MODULE=piper \
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
QTWEBENGINE_DISABLE_SANDBOX=1 QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
./build/bin/speechcontrollertest realIndexMarks realViewerIndexMarks
```

For an isolated test daemon, additionally set
`SPEECHD_ADDRESS=unix_socket:/path/to/private/speechd.sock`.

`realIndexMarks` verifies every word offset with an explicitly selected German
and English voice. `realViewerIndexMarks` selects an actual EPUB paragraph with
`lang="de"`, an inline `xml:lang="en"` span and nested emphasis. It uses the real
toolbar action and verifies all eleven callbacks, exact CFI ranges and the
geometry of the two-pixel word underlines. It also checks automatic language
switching, unchanged document markup and highlight removal after playback.
Its `document` case uses Ctrl-click and continues into a second chapter,
verifying all 17 real word marks across the same speech session. The prepared
cases verify 21 and 27 word marks respectively.

The native module is responsible for mark timing. When validating a Piper
update, its debug log should show one `Synthesizing utterance once` call and
one `Piper inference` per single-sentence segment, irrespective of the number
of word marks. Missing alignment metadata leaves the existing sentence
underline as the fallback.

Inline MathJax formulas share a native SSML sentence with the surrounding
prose. Segment marks still underline the entire formula, and prose word marks
retain their original UTF-16/CFI offsets. The 200 ms break occurs between
sentences, including sentences separated immediately after an inline formula,
and never merely at an inline formula boundary. Display formulas remain
separate utterances.

Inline and display formulas are spoken up to ten mathematical tokens; above that limit,
or when the count cannot be determined from prepared output, Arianna speaks
"zum Sprechen zu komplizierte Formel" for German or
"too complicated formula to speak" otherwise. The count uses MathJax's internal
MathML first, then prepared SVG/CHTML or assistive MathML. Identifiers, numbers
and visible operators each count once; a fraction bar or radical adds one.
Invisible operators, layout nodes and duplicate glyph/assistive renderings do
not add symbols. A complex inline formula keeps its place in the sentence
packet, with an atomic underline while the warning is spoken.

`preparedInlineMath` covers native packet boundaries and pause/resume inside a
formula. The `viewerControls:epub-inline-math-selection` and
`viewerControls:epub-inline-math-document` cases verify extraction, sentence
boundaries, the 10/11-token limit in German and English, CFI word offsets and
whole-formula underlines. Add `ARIANNA_TEST_MATHJAX_CDN=1` to check real MathJax 4
output, including the limit. `realInlineMath` verifies the formula's segment
mark and following prose offsets with the installed aligned Piper backend.
