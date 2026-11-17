# Buch- und Zielauswahl für Querverweise

## Fehlenden Zielanker im Editor ergänzen

Bei einem fehlenden, eindeutig einem vorhandenen EPUB-Dokument zugeordneten
Zielanker enthält die Fehlerantwort beziehungsweise der Prüfbericht ein
optionales `targetAnchorRepair`-Objekt mit `kind`, `bookId`, `fileName`,
`documentFile`, `anchorId` und `htmlSource`. HTTP-Fehlercode, Ablehnung ungültiger
Ziele und gespeicherte Referenz bleiben unverändert. Das Angebot fehlt bei
mehrdeutigen Ankern, fehlenden Dateien, PDF-Zielen und nicht auflösbaren Büchern.

Die Angaben sind kurzlebige, deterministisch aus der Prüfung und den vorhandenen
Bibliotheksdaten gewonnene Anzeigehilfen. Sie werden nicht automatisch persistiert; der vorhandene
ausdrückliche Berichtsexport darf sie als Diagnoseangaben enthalten. Die
bibliotheksweite Prüfung bleibt eine lesende Momentaufnahme; das neue Angebot
öffnet lediglich auf ausdrücklichen Klick den vorhandenen Editor-Arbeitskopienpfad.
Es ist keine automatische Reparatur und kein neuer Transport. Dateiname und
Ankervorlage werden angezeigt; der Benutzer setzt den Anker an der beabsichtigten
Passage im Editor und speichert. Die vorhandene Import-/Verankerungsprüfung
bleibt zuständig für die Übernahme. Ein fehlender Anker ist kein Navigationsziel;
es wird keine vermeintlich passende Passage gesucht oder automatisch ersetzt.


Das Angebot erscheint bei einer abgewiesenen Eingabe im Erstellungs-/
Bearbeitungsdialog, bei einem fehlerhaften gespeicherten Verweis, im Zielprüfdialog
sowie im Buch-, Import- und Bibliotheksprüfbericht (beide Querverweis-Prüfmodi).
Ein Wechsel von Zielbuch oder Zielort verwirft das vorherige Angebot. Während
des Editoraufrufs wird ein doppeltes Auslösen unterdrückt. Fehlende Konfiguration
und Startfehler bleiben als Meldung sichtbar. Ein fehlgeschlagener Aufruf darf
keine bereits wiederverwendete Editor-/Importkopie abschließen oder verwerfen.

Validierung am 22.10.2026:

- Verankerungsprüfung: 130/0, einschließlich Buchbericht und Bibliotheksbericht,
  Eingabevalidierung, Unicode-/HTML-Escaping, ausdrücklichem Kopieren und
  Editoraufruf mit genau einer geschützten Arbeitskopie des richtigen Buchs.
- BookServer: 13/0, ein optionaler externer EPUB-Test übersprungen. HTTP 409 und
  Bestandsschutz bleiben erhalten; nur ein fehlender Anker erhält das Angebot,
  fehlende Dokumente und doppelte IDs erhalten es nicht.
- Native Wartungsintegration: 5/0 mit echtem `Main.qml` und Wartungsseite.
  Ein Testeditor protokolliert den geöffneten Pfad und kopiert die übergebene
  Datei zur Hashprüfung: Zielbuch korrekt, Originale unverändert, Doppelklick
  startet einmal. Die Berichtsdarstellung wurde im Screenshot kontrolliert.
- Reader-/WebEngine-Ende-zu-Ende: 4/0. Geprüft sind Angebot im fehlerhaften
  Verweis und nach fehlgeschlagener Eingabe, sichtbarer Konfigurationsfehler,
  Verwerfen des Angebots nach Korrektur und weiterhin Navigation/Speichern/Löschen.
- Release-Build, JavaScript-Syntax, deutsche Übersetzungen und `git diff --check`
  bestanden. `build` bleibt Release mit `BUILD_TESTING=OFF`, Präfix `/usr`.

Reproduzierbare Tests:

```sh
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  build-ai-notes/bin/bookreferencechecktest
QT_QPA_PLATFORM=offscreen build-ai-notes/bin/bookserverreferencetest
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  QTWEBENGINE_DISABLE_SANDBOX=1 QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
  build-ai-notes/bin/librarymaintenancetest \
  missingTargetAnchorOffersCorrectEditorInMaintenance \
  storedReferencesIgnoreUnlinkedBooksAndOrphanAnchors \
  referencesCountOnceAndIncludeCompletelyOrphanedRecords
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  QTWEBENGINE_DISABLE_SANDBOX=1 QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
  build-ai-notes/bin/searchnavigationtest nativeReferenceDialogState epubCrossReferenceReload
```

Protokolle, Screenshot und abgegrenzter Patch:
`build/validation/missing-target-editor/`. Keine Systeminstallation und kein
neuer Live-Test mit Sigil in der persönlichen Sitzung. Sigil musste nicht
geändert werden: seine vorhandene Dateipfadöffnung wird verwendet. Der neue
Aufruf übergibt ausschließlich die Arbeitskopie, keinen erfundenen Sprung zu
einer nicht vorhandenen ID und keinen einzufügenden oder zu ersetzenden Text.

## Aufklappbare Zielhierarchie

Die Zielauswahl verwendet einen nativen Qt-Quick-`TreeView` mit einem eigenen
`ReferenceTargetModel`. Das vorhandene Reader-Inhaltsverzeichnis wird dadurch
nicht verändert. Kapitel und Unterkapitel stammen aus `tocPath`; die originalen
Zielzeilen einschließlich `hrefLocation`, PDF-Koordinate, Vorschau und CFI bleiben
an den auswählbaren Knoten erhalten. Unverlinkte TOC-Überschriften dienen nur zum
Aufklappen. Ziele ohne TOC-Zuordnung stehen unter ihrer XHTML-Datei.

Der bestehende `GET /<bookId>/targetLocations`-Vertrag erhält ausschließlich das
optionale Feld `tocIdPath`: eine zu `tocPath` parallele Liste eindeutiger
TOC-Knotenkennungen. Sie unterscheidet gleichnamige Kapitel und umfasst auch
unverlinkte Überschriften; ein Elementziel erbt den Pfad seines TOC-Kapitels.
Die Kennungen stammen aus der jeweiligen EPUB3-Navigation, EPUB2-NCX oder
PDF-Synopsis und gelten nur für die gelesene Quelle. Sie sind keine dauerhaften
Textidentitäten und werden nicht als Zielort gespeichert. Alte Clients
ignorieren das Feld; ohne das Feld verwendet die Anzeige dateibezogene
Kapitelpfade und erhält weiterhin alle genauen Zielwerte.

Baumknoten, Suchschlüssel und Aufklappzustände sind deterministische, temporäre
Darstellungsdaten im Reader-Speicher. Sie werden beim Buchwechsel oder Schließen
verworfen. Es gibt keine Datenbankmigration, zusätzliche Vollanalyse oder
Änderung am EPUB. Die Suche behält passende Ziele samt sämtlichen Vorfahren und
öffnet die passenden Zweige. Ohne Suchtext kehrt die Ansicht zum manuellen
Aufklappzustand zurück und macht ein bereits gewähltes Ziel sichtbar. Die Zielhierarchie wird virtualisiert dargestellt;
die bisherige Begrenzung auf 40 Treffer gilt nur noch für die Buchliste.


Validierung am 22.10.2026:

- Baummodell: 5/0 einschließlich gleicher Kapitelüberschriften, fehlender TOC,
  exakter EPUB- und PDF-Werte, Filterung samt Vorfahren und 30.000 Zielen ohne
  Trefferbegrenzung. Aufbau plus Filterung dauerte im Modelltest 66 ms.
- BookServer: 13/0, ein optionaler externer EPUB-Test im normalen Lauf
  übersprungen. EPUB3-TOC und EPUB2-NCX liefern eindeutige Kapitelpfade,
  Elementziele erben den richtigen Pfad; die Quelldateien bleiben hashgleich.
- Native Bedienung und WebEngine-Ende-zu-Ende: 5/0. Geprüft sind automatisches
  Öffnen, Rückkehr zum manuellen Aufklappzustand, Aufklapppfeil ohne Zielwechsel,
  Tastatur-/Mausauswahl, IAST/Devanāgarī, verspätete Antworten, Speichern,
  Fehlerkorrektur, tatsächlicher Sprung zum exakten Zielanker und Löschen.
  Der Screenshot des nativen Dialogs wurde kontrolliert.
- Zusätzlicher realer Buchabruf an der vorhandenen temporären Bhagavatam-Kopie:
  3/0; 28.191 Ziele in 1.739 ms, Prüfung von
  `9/section_412.html#verse_18-20` in 5 ms, EPUB-Hash unverändert.
- Release-Build, JavaScript-Syntax, `msgfmt --check` und `git diff --check`
  bestanden. `build` bleibt Release mit `BUILD_TESTING=OFF` und Präfix `/usr`.

Reproduzierbare Prüfungen (Test-Build `build-ai-notes`):

```sh
build-ai-notes/bin/referencetargetmodeltest
QT_QPA_PLATFORM=offscreen build-ai-notes/bin/bookserverreferencetest
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  QTWEBENGINE_DISABLE_SANDBOX=1 QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
  build-ai-notes/bin/searchnavigationtest nativeReferenceTargetTree \
  nativeReferenceDialogState epubCrossReferenceReload
```

Protokolle, Screenshot und der auf diese Erweiterung begrenzte Patch stehen unter
`build/validation/cross-reference-toc-tree/`. Es wurde keine Systeminstallation
und kein Test der neu installierten Anwendung in der persönlichen Sitzung
vorgenommen. Der Ende-zu-Ende-Test verwendet den echten BookServer und Qt
WebEngine mit temporären EPUBs und isolierten Benutzerdaten.

## Native Oberfläche seit 22.10.2026

Die Buchauswahl wird nach aktuellem Buch, gleichem Autor, gleichem Hauptthema
und übrigen Büchern gegliedert; jede Gruppe wird nach Titel sortiert. Die
Gruppenüberschriften mit Autor-/Themennamen und horizontalen Trennern sind
temporäre Darstellungsdaten. Hauptthemen sind wie in der Bibliothek die
obersten Segmente der vorhandenen `subjects`-Pfade (vor `/`), keine aus
Buchtiteln erschlossenen Kategorien. Der bestehende `/books`-Endpunkt liefert
dafür zusätzlich `subjects` aus den Bibliotheksmetadaten. Es entstehen keine
neuen persistenten Daten, EPUB-Änderungen oder Vollanalysen.

Jedes Buch wird der ersten passenden Gruppe zugeordnet. Bei mehreren Autoren
oder Hauptthemen genügt eine gemeinsame Angabe mit dem aktuellen Buch. Die
Überschrift nennt die Autoren beziehungsweise Hauptthemen des aktuellen Buchs
einmalig. Unter der Autorenüberschrift stehen nur Buchtitel; Autor und
Themenpfade bleiben im Tooltip und für die Suche verfügbar. Leere Gruppen
entfallen. Auch nach dem Filtern erhält der erste Treffer jeder verbleibenden
Gruppe eine Überschrift; zwischen den Gruppen steht eine horizontale Linie.
Für diese Gruppierung wird kein zusätzlicher Referenzlistenabruf benötigt.

Die Erweiterung wurde am 22.10.2026 mit 11 erfolgreichen Server-/Suchtests
(ein optionaler externer EPUB-Test übersprungen) sowie 4 erfolgreichen
nativen Dialog-/Reader-Prüfungen validiert. Die Tests prüfen insbesondere
Gruppenvorrang ohne doppelte Einträge, Titelsortierung, Themenpfade, fehlende
Metadaten, die einmaligen Überschriften und Trenner nach dem Filtern sowie
weiterhin Speichern, Navigation und Löschen. Der Screenshot des Testdialogs
wurde kontrolliert. Release-Build, JavaScript-Syntax, `msgfmt --check` und
`git diff --check` bestanden. Protokolle und abgegrenzter Patch stehen unter
`build/validation/cross-reference-book-groups/`. Keine Systeminstallation in
diesem Arbeitsschritt.

Erstellen und Bearbeiten laufen in `CrossReferenceEditor.qml`, Anzeige und
Löschbestätigung in `CrossReferenceDialog.qml`. Beide sind Kirigami-Dialoge
mit nativen Qt-Quick-Controls. Die bisherigen HTML-Dialoge und ihre
DOM-/Auswahllistensteuerung wurden entfernt. Die vorhandene native
Referenzübersicht und die Fußnotenanzeige bleiben bestehen.

`ReferenceSearchList.qml` zeigt Buchtreffer direkt unter dem Suchfeld, maximal
40 gleichzeitig. Die Zielauswahl verwendet `ReferenceTargetTree.qml` ohne diese
Begrenzung. Gruppierung, Suchwörter, IAST-Suche ohne Diakritika,
Devanāgarī-Vokalzeichen, expliziter Zielort und bearbeitbarer HTML-Vorschautext
bleiben erhalten. Die Vorschau verwendet die vorhandene isolierte
`NoteHtmlView`; Dialog, Felder, Trefferlisten und Aktionen sind QML.

Der Reader übergibt die erfasste Auswahl einschließlich ihrer Quell-CFI und
bestehender Referenzkennung über den vorhandenen WebChannel.
`BookServerRequest.qml` verwendet die bestehenden HTTP-Endpunkte und den
vorhandenen Sitzungstoken. Es gibt kein neues Serverprotokoll und keine neuen
Datenbankfelder. Die Buchliste hat 15 Sekunden, Zielabrufe und
explizite Schreibvorgänge 60 Sekunden Zeitlimit. Ladeindikatoren, Fehler und
Wiederholen bleiben im Dialog sichtbar. Schreibvorgänge werden nicht automatisch
wiederholt; während des Speicherns ist die Bestätigung gesperrt.

Dialogzustände und Suchschlüssel sind temporäre Anwendungsdaten. Die Ziele
stammen aus dem vorhandenen BookServer-Abruf; nur ihre Darstellung wird lokal
aufbereitet. Beim Schließen werden Leseanfragen abgebrochen und Listen
freigegeben. Buchwechsel und erneutes Öffnen entwerten ältere Antworten.
Manuelle Ziel-/Titeländerungen und bewusst geleerte Vorschauen werden nicht
von verspäteten Antworten überschrieben. Nur ausdrücklich bestätigtes Speichern
oder Löschen verwendet die bestehenden EPUB-/Datenbank-Schreibpfade.

Die folgenden Abschnitte dokumentieren auch die vorherige HTML-Implementierung
und die Ursachen der damit behobenen Fehler.

Validierung der Umstellung am 22.10.2026:

- Querverweis-/HTTP-/Suchtests: 11 bestanden, 0 Fehler; der optionale Test mit
  einer externen EPUB-Datei bleibt standardmäßig übersprungen.
- Native Dialogzustände und Reader-Ende-zu-Ende: 4 bestanden, 0 Fehler.
  Geprüft sind verzögerte/abgebrochene Antworten, erneutes Öffnen, Buchwechsel,
  Timeout und Wiederholen, unveränderte manuelle Felder, IAST/Devanāgarī,
  Buchwahl per Tastatur, Zielwahl per Maus, einmaliges Speichern bei Doppelklick,
  sichtbares Neuladen an derselben Leseposition, Fehlerkorrektur, tatsächlicher
  Sprung zur Zielpassage, Rückkehr und Löschen mit Erhalt des Quelltexts.
  Die bestehende Löschsperre bei Editor-Arbeitskopien bleibt erhalten und wird
  im Dialog angezeigt; Einfügen bei vorhandenen Arbeitskopien funktioniert weiter.
- Bemerkungsregressionen: 8 bestanden, 0 Fehler. Der vorhandene HTML-Renderer
  unterstützt zusätzlich sichere `div`-Container für Querverweisvorschauen.
- Release-Build, JavaScript-Syntax, deutsche Übersetzungen (`msgfmt --check`)
  und `git diff --check` bestanden. `BUILD_TESTING=OFF` und `/usr` bleiben
  im Installations-Build erhalten; `install/strip` bleibt verfügbar.

Reproduzierbarer UI-Test:

```sh
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  QTWEBENGINE_DISABLE_SANDBOX=1 QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
  build-ai-notes/bin/searchnavigationtest nativeReferenceDialogState epubCrossReferenceReload
```

`ARIANNA_REFERENCE_SCREENSHOT=/tmp/cross-reference.png` speichert zusätzlich
einen Screenshot des nativen Erstellungsdialogs. Die Darstellung wurde am
Testfenster mit 1000 × 800 Pixeln kontrolliert. Protokolle, Screenshot und
abgegrenzter Patch: `build/validation/native-cross-reference/`. Keine
Systeminstallation und keine Änderung persönlicher EPUBs oder Bibliotheksdaten
in diesem Arbeitsschritt. Eine manuelle Prüfung der installierten Anwendung
wurde nicht vorgenommen.

## Daten und Verantwortung

Die Buchliste ist eine temporäre Ansicht vorhandener Bibliotheksdaten. Zielorte
sind vorhandene beziehungsweise deterministisch aus dem EPUB ermittelte Ziele.
Der Reader lädt sie weiterhin über die bestehenden BookServer-Endpunkte
`/books`, `/<bookId>/refs` und `/<bookId>/targetLocations`. Die persistente
Datenhaltung bleibt bestehen. Die unten beschriebene Zielprüfung ergänzt den
bestehenden Referenz-Endpunkt um optionale Fehlerangaben.

Ladezustände, Fehler und Anfragezähler sind ausschließlich flüchtige UI-Daten
im Reader-Rahmendokument. Sie gehören weder in die Datenbank noch in einen
zusätzlichen Cache. Das Laden und Auswählen verändert das EPUB nicht. Die
vorhandene ausdrückliche Bestätigung zum Erstellen des Querverweises bleibt
der Schreibvorgang. Die Auswahlsteuerung verändert auch nicht das EPUB-DOM
oder dessen CFIs.

Suchtreffer verwenden dieselben bereits geladenen Auswahloptionen und sind
ebenfalls flüchtige UI-Daten. Trefferlisten werden auf 40 sichtbare Zeilen
begrenzt; es gibt keinen zusätzlichen Buchindex, Datenbankeintrag oder Cache.

## Verhalten

- Über Buch- und Zielauswahl gibt es jeweils ein Suchfeld. Es filtert die bereits
  geladenen Einträge lokal und löst keine zusätzlichen Serveranfragen aus.
- Die Buchsuche berücksichtigt Titel, Autor und Buchkennung. Die Zielsuche
  berücksichtigt Beschriftung, Anker/Dateipfad, Inhaltsverzeichnispfad und den
  vorhandenen Vorschautext, auch jenseits der gekürzten Listenbeschriftung.
- Alle eingegebenen Suchwörter müssen vorkommen; Groß-/Kleinschreibung und
  lateinische Diakritika werden ignoriert. `sri krsna` findet beispielsweise
  `Śrī Kṛṣṇa`. Devanāgarī-Vokalzeichen bleiben bei der Suche bedeutungstragend.
- Beim Tippen erscheinen passende Bücher und Ziele direkt unter dem Suchfeld.
  Bis zu 40 Treffer sind anklickbar; bei mehr Treffern fordert die Anzeige zum
  Eingrenzen auf. Pfeiltasten und Enter wählen einen Treffer. Escape schließt
  zunächst die Trefferliste. Enter speichert dabei keine Referenz.
- Die Suche zeigt eine Trefferzahl beziehungsweise „Keine Treffer“ an und
  blendet leere Buchgruppen aus. Erst die Auswahl eines Treffers verändert das
  gewählte Buch beziehungsweise Ziel. Manuell bearbeitete Zielorte und Vorschauen
  bleiben beim bloßen Filtern erhalten. Leeren zeigt wieder alle Einträge in der
  weiterhin verfügbaren Auswahlliste.
- Suchbegriffe und Trefferzustände sind flüchtige UI-Daten. Beim erneuten Öffnen
  des Dialogs werden beide Suchfelder zurückgesetzt; beim Buchwechsel wird die
  Zielsuche geleert. Während des Ladens ist das jeweilige Suchfeld deaktiviert.
- Der Dialog erscheint sofort mit „Buchliste wird geladen…“ und Warteindikator.
- Beim Laden der Zielorte erscheint separat „Zielorte werden geladen…“.
- Währenddessen sind die betroffene Liste und das Speichern gesperrt. Ein
  weiteres Zielbuch kann während des Ladens der Zielorte gewählt werden.
- Fehler werden im Dialog angezeigt. Buch- und Referenzlisten haben 15 Sekunden,
  Zielabrufe 60 Sekunden Zeitlimit. Ein fehlgeschlagener Abruf der
  Referenzgruppierung verhindert die eigentliche Buchauswahl nicht.
- Zielabrufe unterscheiden Timeout, Verbindungs-/Browsersperre, abgelaufene Sitzung
  und nicht mehr verfügbare Buchdatei. „Ziele erneut laden“ wiederholt den Abruf
  im offenen Dialog und erhält manuelle Ziel-/Vorschaubearbeitungen. Bei einem
  Buchwechsel oder Schließen werden ausstehende Zielabrufe abgebrochen.
- Anfragezähler verhindern, dass Antworten für eine ältere Buchauswahl oder
  einen inzwischen geschlossenen Dialog dessen neueren Zustand überschreiben.
- Der Buchwechsel verwendet ein `change`-Ereignis und löst den Zielabruf einmal
  aus. Bisher führten `input` und `change` denselben Abruf doppelt aus.

Qt WebEngine 6.11 zeigte beim nativen Auswahl-Popup über dem modalen
HTML-Dialog eine nicht bedienbare Liste, obwohl die Optionen vorhanden waren.
Bei Unterstützung von `appearance: base-select` werden Buch- und Zielauswahl
deshalb in Chromiums eigener oberster Darstellungsebene angezeigt. Gruppen,
Vorauswahl und Datenmodell bleiben erhalten. Ältere Chromium-Versionen behalten
ihre native Auswahl. Listenhöhe und -breite sind begrenzt; längere Texte können
umbrechen. Reduzierte Bewegung deaktiviert die Warteanimation.

Nach dem Speichern wird die über WebChannel gelieferte Liste geänderter Bücher
mit `Array.from()` normalisiert. Qt-Listen bestehen nicht zuverlässig den
JavaScript-Test `Array.isArray()`; dadurch war das Neuladen zuvor unterblieben.

## Arbeitskopien und spätere Importprüfung

Das ausdrückliche Einfügen oder Aktualisieren eines Querverweises über
`POST /<bookId>/ref` und das Markieren eines Ziels über
`POST /<bookId>/targetLocations` werden nicht wegen vorhandener Editor-Arbeitskopien
abgebrochen. Das gilt auch für verwaiste Arbeitskopien. Vorhandene Sicherungskopien
sind ebenfalls kein Ablehnungsgrund. Die Kopien und ihre Sitzungsdaten werden
bei diesen Vorgängen nicht verändert. Die gemeinsame Wartungssperre verhindert
weiterhin gleichzeitige Import-, Bereinigungs- und Schreibvorgänge.

Verweise und Bemerkungen bleiben persistente Benutzerdaten in der bestehenden
Datenbank, ihre Anker Bestandteil des ausdrücklich bearbeiteten EPUB. Es gibt
keine neue Datenhaltung und keine automatische Zusammenführung mit Arbeitskopien.

Erst beim späteren Import entscheidet die vorhandene Ankerprüfung von
`BookStateImport`/`BookTruthStore`, ob eine Arbeitskopie die aktuelle Ausgabe
ersetzen darf. Die Prüfung verwendet die aktuellen Datenbankeinträge für
Bemerkungen sowie ausgehende und eingehende Querverweise. Fehlen inzwischen
hinzugefügte Anker oder entstehen andere neue Ankerfehler, wird der Import mit
`anchor-validation-failed` abgewiesen. EPUB, Datenbank und Arbeitskopie bleiben
für die Korrektur erhalten. Der bestehende Prüfbericht unter Wartung/Import
zeigt Bemerkungen und Querverweise einschließlich Quelltextvergleich an.
Nach Korrektur der Arbeitskopie kann der Import erneut ausgeführt werden.
Bereits zuvor bestehende Ankerprobleme werden wie bisher von neu entstandenen
unterschieden; ein unverändertes Editor-Speichern überschreibt die Ausgabe nicht.

## Prüfung und Installation

`SearchNavigationTest::epubCrossReferenceReload` hält echte HTTP-Anfragen im
WebEngine-Test kontrolliert zurück und prüft die sichtbaren Wartezustände und
gesperrten Steuerelemente. Danach werden Bücher per Tastatur und Maus gewechselt,
ein Ziel per Maus gewählt und der Querverweis über den echten BookServer
gespeichert. Der Test prüft außerdem Neuladen, Leseposition und den erzeugten
Anker bei gleichzeitig vorhandenen, unverändert erhaltenen Editor-Arbeitskopien
beider Bücher. Er verwendet temporäre Bücher und verhindert externe Editorstarts.

`BookServerReferenceTest` prüft zusätzlich Ladefehler, Schließen während einer
ausstehenden Antwort und die Kompatibilität vorhandener Vorschautexte und
Referenzfelder. `copyConflictsAreCheckedOnImport` prüft aktive und verwaiste
Arbeitskopien beider Bücher sowie vorhandene Sicherungskopien: Quell- und
Zielanker werden angelegt, Kopien bleiben unverändert, spätere Importe ohne
die neuen Anker oder vorhandene Bemerkungsanker scheitern und korrigierte Kopien
werden erfolgreich importiert. Auch die echte Wartungssperre wird geprüft.
Der Prüf-Build bleibt `build-ai-notes`; `build` bleibt Release
mit `BUILD_TESTING=OFF` für:

```sh
ninja -C build && sudo ninja -C build install/strip
```

Am 21.10.2026 bestanden der vollständige Querverweis-Ablauf (QtTest: 3/0)
und die BookServer-/Dialog-Regressionen (6/0), einschließlich kontrolliert
verzögerter Antworten und Fehlerfälle. Der Release-Build wurde erfolgreich
erstellt. Protokolle und abgegrenzter Änderungspatch stehen unter
`build/validation/cross-reference-dialog/`. Eine Systeminstallation wurde in
diesem Arbeitsschritt nicht vorgenommen.

Die anschließende Arbeitskopien-Anpassung am selben Tag bestand mit 10/0
BookServer-Prüfungen, 11/0 Importprüfungen, 128/0 Prüfberichtsprüfungen und 3/0
im Reader-Ende-zu-Ende-Test einschließlich Arbeitskopien beider Bücher. Der
Release-Build bestand weiterhin. Protokolle und der auf diese Anpassung begrenzte
Patch stehen unter `build/validation/cross-reference-copies/`.

Die zusätzlich ausgeführte gesamte Wartungssuite ergab 74/1: Der unabhängige Test
`databaseCanDeleteOrphanedEntriesFromBothStateTables` scheitert auch isoliert,
weil nach dem Löschen verwaister Buchzustände ein Eintrag für
`missing-current-book-1` in der Historientabelle verbleibt. Dieser nicht am
Querverweis-Schreibpfad beteiligte Fehler wurde hier nicht verändert. Die
Arbeitskopien-, Sicherungskopien- und Ankerprüfungen der Wartung bestanden.

Die Suchfelder werden im selben Reader-Ende-zu-Ende-Test mit einer nach Titel
und Autor gefilterten Buchliste, Maus-/Tastaturauswahl, Ankersuche, langen
Vorschauen, zerlegten IAST-Zeichen und Devanāgarī geprüft. Außerdem prüft er
leere Trefferlisten, unveränderte Auswahl/Vorschau, Enter ohne Speichern,
Rücksetzen beim Buchwechsel und das abschließende Einfügen mit Neuladen. Der
Dialogtest prüft zusätzlich das Rücksetzen beim erneuten Öffnen und den Erhalt
manuell bearbeiteter Ziele und Vorschauen beim Filtern.

Am 21.10.2026 bestanden diese Suchfeld-Erweiterungen mit 10/0 BookServer-/Dialog-
und 3/0 Reader-Prüfungen. JavaScript-Syntaxprüfung und Release-Build bestanden;
`build` bleibt auf Release mit `BUILD_TESTING=OFF`. Der abgegrenzte Patch und
die Protokolle stehen unter `build/validation/cross-reference-search/`. Eine
Systeminstallation wurde nicht vorgenommen.

## HTTP-0-Diagnose und direkte Treffer

`XMLHttpRequest` meldet bei Netzfehlern, Abbruch und Timeout zunächst
`readyState = DONE` mit Status 0. Der bisherige Handler lehnte die Anfrage
schon dort als „HTTP 0“ ab und verdeckte das nachfolgende konkrete Fehlerereignis.
Status 0 wird jetzt durch `onerror`, `onabort` beziehungsweise `ontimeout`
behandelt. HTTP-Fehler wie 401 und 404 bleiben separat erkennbar.

Bei der Live-Prüfung am 21.10.2026 antwortete das zuvor genannte EPUB über
`targetLocations` mit HTTP 200, 13 Zielen und rund 0,98 Sekunden Laufzeit.
Eine spätere Messung fand den registrierten Serverprozess nicht mehr vor und
erhielt „Connection refused“. Damit ist der gemeldete Status 0 kein Nachweis
für einen Timeout aller Bücher. Der Grund der Prozessbeendigung wurde nicht
nachgewiesen. Die neue Fehlermeldung macht diese Unterscheidung sichtbar; ein
beendeter Server muss über einen Neustart von Arianna/BookServer wieder verfügbar
gemacht werden. Es wird hier kein konkurrierender Server automatisch gestartet.

Die aktualisierten Tests erzwingen im echten WebEngine-Ablauf eine blockierte
Zielanfrage, prüfen die Verbindungsfehlermeldung und laden die Ziele per
Wiederholen-Schaltfläche anschließend erfolgreich. Sie wählen ein Buch über die
direkte Trefferliste per Tastatur und ein Ziel per Mausklick, ohne diese
Auswahllisten zusätzlich zu öffnen, und speichern den Querverweis mit Neuladen.
Der XHR-Ereignisfolgetest unterscheidet Timeout, Netzfehler, Abbruch, 401 und Erfolg.

Validierung dieser Erweiterung am 21.10.2026: BookServer-/Dialog-/Transporttests
11/0, Reader-Ende-zu-Ende-Test 3/0. JavaScript-Syntaxprüfung und Release-Build
bestanden. `BUILD_TESTING=OFF` im Installations-Build bleibt erhalten. Der
abgegrenzte Patch und die Protokolle einschließlich der nur lesenden Live-Abrufe
stehen unter `build/validation/cross-reference-direct-results/`. Keine
Systeminstallation in diesem Arbeitsschritt.

## Timeout bei großen EPUBs ohne Werkstrukturdatensatz

Beim EPUB „Srimad Bhagavatam (All 12 Cantos Set)“ (58.650.369 Bytes, 717
XHTML-Dokumente) war die Ursache eine implizite Vollanalyse: `targetLocations`
rief unabhängig vom Vorhandensein eines Werkstrukturdatensatzes
`workStructureSnapshot()` auf. Diese Analyse allein benötigte mit der konkreten
Datei 158.712 ms. Das eigentliche Ermitteln der 28.190 Ziele benötigte dagegen
nur 1.402 ms. Das erhöhte Zeitlimit behob diese Ursache nicht.

`WorkStructure::hasStoredRecord()` prüft jetzt zunächst mit dem vorhandenen
Metadaten-/Datensatzleser, ob überhaupt ein entsprechender Datensatz vorliegt.
Ohne Datensatz geben die passiven Endpunkte `workStructure` und
`workStructure/resolve` sofort den bisherigen Zustand `absent` beziehungsweise
`resolved: false` zurück; auch `targetLocations` benötigt dann keine Analyse.
Die expliziten Analyse-/Commit-Vorgänge und die vollständige Validierung
vorhandener Datensätze bleiben bestehen. Dies schützt auch vor einer unnötigen
Analyse beim Öffnen des Readers, die sonst nachfolgende Zielabrufe blockiert.

Die Anwesenheitsprüfung ist eine kurzlebige, deterministische Abfrage der
EPUB-Quelldaten. Sie ist keine Validierung und kein neuer Cache. Vorhandene
Benutzerdaten und editorische Datensätze werden weder verändert noch gelöscht;
gültige, veraltete und ungültige Datensätze laufen weiterhin durch die vorhandene
Prüfung. Der bisherige HTTP-Vertrag und sämtliche Ziel-/CFI-Koordinaten bleiben
erhalten.

Mit einer unveränderten temporären Kopie dieses EPUBs lieferten die korrigierten
Endpunkte am 21.10.2026 die passive Strukturabfrage in 33 ms und alle 28.190 Ziele
über HTTP in 1.665 ms. SHA-256-Vergleiche bestätigten unveränderte Original- und
Kopiedateien. Der Cache-Test stellt zusätzlich sicher, dass passive Abfragen ohne
Datensatz keine Analyse oder unnötige Neueröffnung des Archivs auslösen. Die
Werkstrukturtests prüfen weiterhin die semantische Anreicherung vorhandener
Datensätze im Zielabruf.

Validierung: Cache 6/0, Werkstruktur 16/0, Querverweis/Transport 11/0, Reader 3/0
sowie die Integration mit dem konkreten großen EPUB 3/0. Der externe EPUB-Test
wird im normalen Lauf übersprungen; für eine reproduzierbare Messung wird eine
entbehrliche Kopie über `ARIANNA_TARGET_TEST_EPUB` angegeben:

```sh
QT_QPA_PLATFORM=offscreen ARIANNA_TARGET_TEST_EPUB=/tmp/book.epub \
  build-ai-notes/bin/bookserverreferencetest externalEpubTargetLookup
```

Mit `ARIANNA_TARGET_TEST_PHASE=targets` beziehungsweise `structure` lassen sich
Zielsammlung und vollständige Analyse getrennt messen. Letztere ist bei diesem
Buch absichtlich weiterhin eine aufwendige explizite Analyse. Der Release-Build
bestand mit `BUILD_TESTING=OFF`. Protokolle und abgegrenzter Patch stehen unter
`build/validation/large-epub-targets/`. Keine Systeminstallation in diesem Schritt.

## Fehlende oder mehrdeutige Zielfragmente

Beim gemeldeten Ziel `9/section_412.html#vers_18-20` existiert die XHTML-Datei,
der Fragmentname ist jedoch falsch: Das Original-EPUB enthält
`id="verse_18-20"`. Der richtige Zielort lautet also
`9/section_412.html#verse_18-20`. Foliate erhielt bisher einen fehlenden
DOM-Anker und zeigte dadurch still den Kapitelanfang.

BookServer prüft dokumentqualifizierte EPUB-Fragmente jetzt direkt im angegebenen
Original-XHTML. `id` und alte `name`-Anker sowie URL-kodierte Unicode-Fragmente
werden unterstützt. Ein Anker muss genau einmal vorkommen. Gleichnamige Anker
in anderen Dateien ersetzen das angegebene Ziel nicht. Es gibt keine unscharfe
Suche oder automatische Korrektur von Tippfehlern.

Die Prüfung ist eine kurzlebige, deterministische Auswertung genau einer Datei.
Sie erzeugt keinen Cache und keine neuen persistenten Daten. Vorhandene
Referenzen, Vorschauen und EPUB-Dateien werden durch diese Prüfung nicht
verändert. Die bestehenden expliziten Schreibvorgänge bleiben erhalten.

Ergänzung des bestehenden HTTP-Vertrags:

- `GET /<sourceBookId>/ref/<sourceAnchorId>` liefert bei einem ungültigen
  Zielfragment weiterhin HTTP 200 und die gespeicherte Referenz, zusätzlich
  `targetNavigationError` (lesbare Meldung) und `targetNavigationErrorCode`.
  Der Reader zeigt die Meldung im Referenzdialog, deaktiviert das Öffnen und
  ermöglicht weiterhin die Bearbeitung.
- `POST /<sourceBookId>/ref` lehnt ein ungültiges explizites Zielfragment vor
  Änderungen an Referenz oder EPUB mit HTTP 409 ab:
  `{"error":"invalidReferenceTarget","reason":"missing-target-anchor","message":"…"}`.
  Der Bearbeitungsdialog bleibt geöffnet und zeigt die Meldung an.
- Fehlercodes sind `missing-target-document`, `invalid-target-document`,
  `missing-target-anchor` und `duplicate-target-anchor`.
- Point-/Range-CFIs, PDF-Koordinaten, reine Kapitelziele und externe URLs
  behalten ihre bisherigen Auflösungspfade. Ohne verfügbaren Zielcontainer
  kann die Fragmentprüfung keine Aussage treffen; das bestehende Verhalten
  für solche Referenzen bleibt erhalten.

Der Server-Regressionstest prüft gültige/fehlende/doppelte Anker, `name`,
Unicode-Kodierung, gleichnamige Anker in verschiedenen XHTML-Dateien und
unveränderte Bestandsdaten bei Ablehnung. Der WebEngine-Test öffnet einen
fehlerhaften gespeicherten Verweis, prüft die gesperrte Navigation und den
Speicherfehler, korrigiert das Ziel im Dialog und prüft anschließend, dass die
Zielpassage weit hinter dem Kapitelanfang sichtbar ist.

Validierung am 21.10.2026: Querverweis-/Dialogtests 12 bestanden, 0 Fehler;
der optionale externe EPUB-Test wird im normalen Lauf übersprungen. Der
WebEngine-Ende-zu-Ende-Test bestand mit 3/0. An einer frischen, hashgleichen
Kopie des aktuellen Bhagavatam-EPUBs (58.650.382 Bytes) wurden beide Namen über
den Referenz-Endpunkt geprüft: `#verse_18-20` ist gültig, `#vers_18-20` liefert
`missing-target-anchor`; beide Prüfungen benötigten jeweils 5 ms. Beide
externen Testläufe bestanden mit 3/0. Die ältere Kopie aus der Timeout-Prüfung
enthielt diesen Anker noch nicht und eignet sich nicht als aktuelle Referenz.

Der externe Test unterstützt zusätzlich `ARIANNA_TARGET_TEST_LOCATION` und
`ARIANNA_TARGET_TEST_ERROR` (leer bei gültigem Ziel). Beispiel:

```sh
QT_QPA_PLATFORM=offscreen ARIANNA_TARGET_TEST_EPUB=/tmp/book.epub \
  ARIANNA_TARGET_TEST_LOCATION='9/section_412.html#vers_18-20' \
  ARIANNA_TARGET_TEST_ERROR=missing-target-anchor \
  build-ai-notes/bin/bookserverreferencetest externalEpubTargetLookup
```

Release-Build und JavaScript-Syntaxprüfung bestanden; `build` bleibt Release
mit `BUILD_TESTING=OFF` und Installationspräfix `/usr`. Patch, Protokolle und
Hashnachweis stehen unter `build/validation/reference-fragments/`. Die
persönlich gespeicherte Referenz wurde nicht automatisch korrigiert; der
Benutzer kann den exakten Zielort im vorhandenen Dialog übernehmen. Keine
Systeminstallation in diesem Schritt.
# Missing target anchor: exact editor handoff

Target repairs additionally prepare Sigil's existing Find and Replace AI panel
with the exact escaped anchor ID, decoded document path and optional editable
verse/target reference. `PrepareAnchorEdit` is an additive operation of the same
Editor1 service; cold starts carry the same context as CLI options. Plain exact
document navigation stays compatible. An unsupported running service reports an
error and is never replaced by another process.

This is transient user interface state owned by Sigil, not derived EPUB data to
persist in BookServer. No database, cache or EPUB migration is needed. Opening
the panel neither sends an AI request nor applies a replacement. Sigil restricts
the proposal scope to Current File. User-edited instructions and active AI work
are retained and cause an explicit conflict/busy result.

The target repair action passes the decoded archive-root `documentFile` along
with the protected EPUB working copy. For Sigil it uses the existing
owner-pinned, activation-free lifecycle with the additive `OpenBookAtLocation`
operation. The authoritative contract is
[`Sigil/docs/DBusNavigation.md`](../../Sigil/docs/DBusNavigation.md).
Only `DocumentOpened` confirms the requested XHTML has loaded. Errors and
timeouts are shown without launching another process. Existing non-Sigil editor
commands retain their file-only behavior. No anchor is inserted automatically.
Request results are short-lived process state; no database migration or new
persistence is introduced. Each active working copy retains its save/import
watcher when another book is opened in the same Sigil process.

