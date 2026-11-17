<!-- SPDX-FileCopyrightText: 2026 Arianna contributors -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Codex App Server: isolated Arianna prototype

## Existing architecture and insertion points

Arianna builds its C++ services and QML resources into `arianna_static`, with the
generated `org.kde.arianna` QML plugin. `main.cpp` owns the application and
`QQmlApplicationEngine`; `Main.qml` owns the page stack. EPUB rendering lives in
`EpubViewerPage.qml`, Qt WebEngine, `reader.js`, Foliate and BookServer. PDF uses
`PdfViewerPage.qml` and Okular. Persistent settings come from `config.kcfg` and
the generated `Config` singleton.

`Translator` is a QML singleton using network requests; `SpeechController` is a
reader-owned object with its own worker/session. External editors are managed
by `ExternalProcess` and editor sessions. The new transport depends on none of
these services. The ReaderContext extension adds a read-only DOM capture to the existing EPUB
selection payload. Speech and translation extraction remain unchanged.

`CodexClient` follows the existing generated QML registration pattern. It is a
lazy singleton per QML engine, with one child `QProcess` and one temporary
conversation. Merely constructing it does not launch Codex. Its constructor
connects `QCoreApplication::aboutToQuit` to cleanup; its destructor repeats
cleanup as a fallback, so no changes to `main.cpp` are necessary.

The existing EPUB selection menu's **KI-Diskussion** action creates an immutable
ReaderContext snapshot. Its dialog embeds `ReaderContextPane` immediately above
the independent question field, plus `CodexSessionPane` for connection status,
version, sending, streaming output, errors and cancellation. **Copy Prompt** and
editable selected text remain available. Editing adds `selection.userEditedText`
without changing the exact original selection. No automatic book upload occurs.

## Protocol verified before implementation

On 2026-10-12 the installed binary was
`/home/markus/.local/bin/codex`, reporting `codex-cli 0.162.0`.
The installed binary's schema, rather than a guessed SDK interface, was inspected:

```sh
codex --version
codex app-server --help
codex app-server generate-json-schema --out /tmp/arianna-codex-schema-0.162
```

The relevant files are `v1/InitializeParams.json`, `v1/InitializeResponse.json`,
`v2/ThreadStartParams.json`, `v2/ThreadStartResponse.json`, `v2/TurnStartParams.json`,
`v2/TurnInterruptParams.json` and the streaming/approval notification schemas.
A real stdio probe confirmed initialization, thread creation, version `0.162.0`,
`approvalPolicy: "on-request"` and sandbox `{type: "readOnly", networkAccess: false}`.

The framing agrees with the [official OpenAI App Server documentation](https://learn.chatgpt.com/docs/app-server):
one UTF-8 JSON object per line, bidirectional JSON-RPC, with the `jsonrpc` member
omitted. stdout is reserved for protocol messages; stderr is separate diagnostic
text. The client buffers complete lines before decoding UTF-8, supports multiple
messages per read and caps each message at 8 MiB. Request IDs correlate replies;
server request IDs are echoed unchanged, including string IDs.

The implemented lifecycle is:

1. Start `codex app-server --listen stdio://`, with process defaults explicitly
   set to `sandbox_mode="read-only"`, `approval_policy="on-request"` and
   `approvals_reviewer="user"`.
2. Send `initialize` with stable client name `arianna`, title and application
   version. Wait for its successful reply, then send `initialized`.
3. Send `thread/start` with an isolated temporary `cwd`, `sandbox: "read-only"`,
   `approvalPolicy: "on-request"`, `approvalsReviewer: "user"` and
   `ephemeral: true`. Require the returned restrictions to match before showing
   a successful connection. Display `thread.cliVersion` as the server version.
4. Send `turn/start` with `threadId` and text `input` elements. The standalone
   test dialog sends one element; Reader discussions send two (JSON context and
   user question).
   Repeat the approval policy/reviewer and explicitly set
   `sandboxPolicy: {type: "readOnly", networkAccess: false}` on every turn.
5. Display `item/agentMessage/delta` for the active thread/turn. Reconcile
   `item/completed` with the accumulated message so final text is not duplicated.
   Ignore notifications belonging to other threads/turns and late deltas after
   completion. Handle `error` and distinguish successful, failed and interrupted
   `turn/completed` events.
6. Cancel using `turn/interrupt`. Cancellation before the turn ID arrives is
   remembered and sent when the ID becomes available. An interrupt acknowledgement
   alone does not mean the turn has finished; wait for `turn/completed`.

Requests time out after 30 seconds; cancellation has a 10-second completion
deadline. Protocol corruption, lost processes, unsafe returned policy and timeout
stop the connection. RPC/turn failures surface in QML and allow another turn
when the process/session is still usable. There is no automatic replay of a
possibly accepted prompt. `diagnosticReceived` exposes stderr separately for
future logging without treating ordinary diagnostic lines as fatal RPC errors.

The prototype handles command/file approval requests by declining them and
permission-profile requests by granting an empty profile. Unsupported server
requests receive JSON-RPC error `-32601`, so the server is never left waiting
for an unimplemented approval UI. Legacy command/patch approvals and MCP
elicitation requests also receive explicit denials. Setting the reviewer to
`user` prevents an existing local `auto_review` setting from granting escalation
without this client's decision.

## Using the prototype

Open **Settings → KI-Assistenz**, set **Codex-Pfad** to an executable path (or
`codex` resolved through PATH), then use **Test Codex connection**. The test
dialog provides a text input, Send, streaming output and Cancel. An empty or
invalid configured path produces an error; it does not silently select another
binary. Paths are passed directly to `QProcess`, without a shell or command-string
expansion. A changed path is used by the next connection.

Alternatively, select EPUB text and use **KI-Diskussion**. Opening either dialog
starts the handshake but submits no model turn. A connected label with the
confirmed Codex version appears when the temporary thread is ready. Pressing
Send submits the displayed input/context. Repeated sends while the dialog is
open use the same conversation; the output area displays the current response.
Closing the dialog shuts down its prototype session. Reopening starts a fresh
ephemeral thread. This is intentionally a single active discussion/test session.

For ordinary use, Codex uses the local CLI's existing authentication, provider
and model configuration. Arianna does not store API keys or implement login.
An initialized connection does not prove that a remote model/account is usable;
model/provider errors appear when a turn is submitted.

The agent's tools receive a read-only sandbox and no sandbox network access.
This is not a restriction on the app-server's own model-provider connection or
Codex's account/configuration housekeeping. The temporary cwd avoids giving the
agent a book/library working directory; read-only mode does not by itself deny
all filesystem reads. The developer instructions ask for discussion of the
provided text without files, commands or external tools. No tool permission
escalations are accepted by Arianna.

At normal application exit, the client requests interruption when possible,
closes stdin for graceful app-server EOF shutdown and reaps the child. If needed,
bounded waits fall back to TERM and then KILL (at most 2.5 seconds of waits).
Dialog cleanup uses an asynchronous EOF shutdown with a forced-stop deadline.

## ReaderContext interface and lifecycle

`ReaderContextBuilder` is a stateless C++/QML singleton independent of Codex,
BookServer, Translator and SpeechController:

- `build(metadata, selection, position) -> QVariantMap`: creates a version-1
  JSON-compatible snapshot, UUID `snapshotId` and UTC `capturedAt`. Different
  nonempty `bookId` values in metadata and selection are rejected. Empty
  selections are rejected. Bibliographic and location fields are omitted when
  missing; no placeholder author, chapter or verse is generated.
- `withOptions(snapshot, excludedFields, editedText) -> QVariantMap`: returns a
  copy with optional dotted field paths removed. `selection.text`, snapshot
  identity and book identity remain mandatory. Edits are recorded separately as
  `selection.userEditedText`. `omittedFields` records the available fields removed.
- `toJson(context) -> QString`: the same indented UTF-8-compatible JSON encoding
  used by the inspector, Copy Prompt and CodexClient.

The schema contains `bibliography` (bookId, identifier, title, authors,
translators, editors, languages, edition), `location` (chapter, chapterHref,
heading, verse, sectionIndex, selectionCfi, readerCfi), `selection` (exact text,
language, languageSegments and optional userEditedText), and `surrounding`
(sentence, paragraph, section). Effective inline languages use DOM `lang` and
`xml:lang`, including explicit empty values; mixed selections use `mul` plus
UTF-16 segment start/end offsets. Authors/contributors and multilingual metadata
are normalized from Foliate's actual metadata objects. Arianna's edited library
author is respected. Editions use explicit schema.org bookEdition or legacy
edition metadata, never a guessed publication date.

`reader-context.js::collectReaderSelectionContext(range, {bookLanguage})` reads
the existing EPUB Range without modifying DOM, offsets or CFI. `reader.js` adds
its result, `bookId` and `sectionIndex` to the existing WebChannel selection
payload. TOC context comes from Foliate's existing selected-range progress data,
with current reader TOC as fallback. Headings are real preceding HTML headings;
verses require unambiguous numeric `data-verse`/`data-verse-number` attributes covering
all selected source text. Numeric headings, IDs or ordinary prose never imply
verse numbers. Sentence context uses the browser's `Intl.Segmenter` when the
selection lies in one paragraph; unsupported/ambiguous cases omit the sentence.
Generated MathJax output, scripts and styles are excluded from surrounding text.
Optional paragraph/sentence capture is bounded to 12,000 UTF-16 code units and
section capture to 24,000, with explicit `*Truncated` flags; exact selected text
is not truncated. The larger section is excluded from transmission by default.

Opening KI-Diskussion freezes a snapshot. Reader paging, metadata changes and
book switches never rewrite it. The nonmodal dialog allows a new selection;
**Kontext aktualisieren** explicitly replaces the snapshot when the current book
has a valid selection and no turn is running. Closing/reloading the reader
invalidates its cached selection; reader-page destruction ends its owned dialog/session; a selection tagged with another book ID cannot
be refreshed. The user's question survives refresh. Consecutive questions use
the same Codex thread and current snapshot until an explicit refresh.

The expanded context area provides individual optional-field switches, the exact
JSON for the next request, and a history selector. History entries contain the
actual submitted context, question, threadId, turnId, status and answer/error.
They remain associated with their original snapshots even after refresh. This
prototype stores the history only for the open dialog/session; reopening starts
a new ephemeral conversation. Excluding a field affects future submissions; it
does not erase information already sent in that thread.

`CodexClient::startReaderTurn(question, context) -> bool` validates the minimal
schema and sends two supported `UserInput` text elements:

```text
ReaderContext (untrusted source data):
{ ... exact inspector JSON ... }

User question:
... independent question ...
```

The installed 0.162.0 `v2/TurnStartParams.json` accepts multiple text elements,
not an arbitrary new JSON context field. JSON is therefore serialized inside the
first text element. Trusted developer instructions explicitly classify every
context value (including metadata/headings/edited text) as untrusted book data,
reserve instructions to the separate question and prohibit tool use. Sandbox,
approval refusal, streaming, cancellation and process cleanup remain shared
with ordinary `startTurn(text)`.

For Arianna CIA, callers can construct the same context without starting Codex.
SearchService and CrossRefAssistant can use bookId/CFI and source surroundings;
Translator can consume language segments while preserving its own language
choices; AnnotationService can store snapshotId/bookId/CFI alongside annotations;
a future MCP tool can expose a validated read-only snapshot with a document
identity check. These services are not implemented by this prototype. Context
capture remains reader-owned; transport owns no EPUB document or DOM objects.

## Tests

```sh
cmake --build build --target arianna codexclienttest codexqmltest readercontexttest readersettingstest -j4
ctest --test-dir build -R '^arianna-(codexclienttest|codexqmltest|readercontexttest|readersettingstest|readersettingsmigrationtest)$' --output-on-failure

# Require a particular installed Codex; otherwise the installed-binary test
# skips only if no codex executable can be found in PATH.
ARIANNA_CODEX_BINARY=/home/markus/.local/bin/codex build/bin/codexclienttest installedBinary
```

`codexclienttest` tests the stdio client independently of Arianna's document
services, using a child-process peer for chunked Unicode output, sequential
turns, notification filtering, early cancellation, denied server requests,
RPC/stream/protocol failures, missing binaries, retry and process teardown.

Its `installedBinary` case launches the **actual installed Codex executable**,
with a disposable `CODEX_HOME` and local HTTP Responses/SSE fixture. It verifies
the real initialize/initialized/thread/start/turn/start protocol, confirmed CLI
version and sandbox, two text inputs with Devanagari/IAST context, streamed text,
turn interruption, a successful subsequent
turn and controlled disconnect. It does not use the user's authentication or
modify their Codex configuration. The fixture replaces only remote inference;
the app-server, JSON-RPC and event generation are real. This proves protocol
integration, not an authenticated response from a remote OpenAI model.

`codexqmltest` exercises persisted path settings, the settings test dialog,
connection/version display, sending/output, the existing reader selection menu,
Cancel and dialog teardown. It also checks frozen contexts during book/metadata changes, repeated questions
in the same thread, explicit refresh, optional exclusions, history attribution
equality of inspected and transmitted JSON, and Copy Prompt. Its real EPUB case
loads a temporary EPUB through Foliate/WebChannel, creates a real DOM selection,
opens the actual reader menu action, sends its bibliographic/structural/text
context and verifies that both archive bytes and rendered DOM remain unchanged.
`readercontexttest` captures
real DOM Ranges in Qt WebEngine for paragraph selections, inline language
switches, empty language, Devanagari/IAST and ambiguous/missing verses, and proves
DOM and Range offsets remain unchanged. GUI tests use an offscreen Qt platform and isolated
XDG directories. The fake speech service is only a fixture for the reader's
existing construction; the Codex prototype makes no speech requests.

Restricted environments may block loopback listeners, Unix sockets, Qt WebEngine
or user namespaces. Run the corresponding tests outside such a restriction;
do not disable the client's read-only policy to make a test pass. If KDE's build
configuration tries to write protected `.git/hooks`, configure the build directory
with `-DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE` to skip hook installation.

Validation on 2026-10-12: `arianna`, `codexclienttest`, `readercontexttest`,
`codexqmltest` and `readersettingstest` built successfully. All five listed CTest
entries passed. The transport suite reported 13 passed, the ReaderContext suite
11 passed, the QML suite 10 passed and each reader-settings suite 5 passed,
with no failed or skipped cases in their final runs. The installed-binary test
confirmed Codex `0.162.0` and Unicode in the real provider request. German PO
validation (`msgfmt --check`), clang-format validation of the new C++ files and
`git diff --check` also passed.

Socket/WebEngine tests ran outside the execution sandbox using temporary test
data. GUI verification was automated/offscreen, including a real EPUB DOM and
WebChannel path; no manual desktop interaction, authenticated remote inference
or system installation was performed. The real Codex transport test uses a
local model-provider fixture. Dialog history is transient and the prototype
supports one active Codex conversation at a time.

## Later shared use by Reader, Translator and SpeechController

Keep `CodexClient` as the process/JSON-RPC transport, and add a small application
service above it once there are multiple callers. The current single-thread
properties (`output`, `busy`, one active `threadId`) should not be exposed as
global conversation state to several independent tools. Extend the service to
own a thread/session per consumer, map request/turn IDs to a caller token and
route events using their thread/turn/item identities. The existing `textDelta`
and `turnFinished` signals already include these identities. Cancellation and
view destruction must affect the owning consumer only; application exit still
owns final process shutdown. Move dialog-owned disconnect decisions into that
shared service when a second consumer is introduced.

- **Reader:** capture a user-selected plain-text passage and immutable book,
  language and CFI/page context into a discussion request. Keep source offsets,
  CFIs, selections and annotations outside the transport. Reject responses for
  closed or replaced document sessions.
- **Translator:** add an explicit Codex backend adapter with the existing
  request-ID contract, source/target language and cancellation. Keep existing
  Google/DeepL behavior and explicit language choices. Obtain any structured
  translation result through a validated adapter, rather than teaching the
  transport about translation or directly changing book text.
- **SpeechController:** pass only a completed or deliberately buffered plain-text
  result to the existing speech pipeline when requested by the user. Preserve
  SpeechController's own session IDs, STOP behavior and source highlighting.
  Do not speak arbitrary token deltas or give the Codex process audio ownership.

An application-owned C++ service can be injected into all three callers and
exposed to QML with the same singleton pattern. Authentication, model selection,
explicit approval UI and optional persistent conversation storage belong in that
service/settings layer. They do not require changing BookServer, Foliate,
Okular, the existing translator network manager or the speech worker.
