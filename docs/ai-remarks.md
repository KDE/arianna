# KI-Antworten als verankerte Bemerkungen

## Datenentscheidung

Die KI-Antwort ist zunächst ein flüchtiger Vorschlag (Kategorie C). Mit dem
ausdrücklichen Speichern einer überprüften, gegebenenfalls bearbeiteten Fassung
wird sie zu dauerhaften Benutzerdaten (D). `annotations` bleibt der zuständige
Speicher. Quell-Markdown, bereinigtes HTML5 und Herkunftsmetadaten erweitern diese
bestehende Tabelle durch eine verlustfreie Migration; keine neue Datenbank,
kein Analyse-Cache. Bestehende Klartextbemerkungen bleiben unverändert.

Vor Speichern sind Textbearbeitung und HTML-Vorschau möglich. Abbrechen erzeugt
weder einen EPUB-Anker noch einen Datenbankeintrag. Erst Speichern verwendet
den vorhandenen `AnnotationStore`-/`BookTruthStore`-Weg für Textbemerkungen:
Das EPUB bekommt ausschließlich den Anker; der Bemerkungsinhalt bleibt in der
Datenbank. Änderungen des Quelltexts oder des bestätigten Buchzustands zwischen
Auswahl und Speichern werden abgewiesen. Metadaten einer späteren Auswahl dürfen
die ursprüngliche Zuordnung nicht ersetzen.

Beim Lesen ist der EPUB-Anker maßgeblich; die gespeicherte CFI ist ein Hilfswert.
Strukturelle Änderungen dürfen CFIs verschieben. Solange der Anker erhalten
bleibt, wird die Textstelle daraus neu aufgelöst. Ein fehlender Anker wird als
ungültige Zuordnung gemeldet, statt auf die alte CFI oder einen beliebigen
Texttreffer auszuweichen. Benutzerbearbeitungen werden nicht aus der KI-Antwort
neu berechnet oder durch Cache-Bereinigung entfernt.

Die Anzeige verwendet eine nur bei Bedarf erzeugte WebEngine-Ansicht, festes
lokales Layout und lokales MathJax. Antworttext und gespeichertes HTML führen
keine Editor- oder IPC-Aktionen aus. Die Markdown-Verarbeitung akzeptiert keine
ausführbaren HTML-Inhalte; Bildressourcen und aktive URL-Schemata werden nicht
übernommen. Frage, Originalantwort, Thread/Turn und Quellauswahl bleiben als
Herkunft des ausdrücklich gespeicherten Ergebnisses nachvollziehbar.

## Bedienung und Lebenszyklus

1. Text im EPUB auswählen und die vorhandene KI-Diskussion öffnen.
2. Nach einer abgeschlossenen Antwort **Antwort als Bemerkung speichern…** wählen.
3. Den Markdown-Entwurf bearbeiten und bei Bedarf zur **Vorschau** wechseln.
4. **Bemerkung speichern** bestätigt sowohl den EPUB-Anker als auch die Datenbankbemerkung.
5. Ein Klick auf die Markierung oder den Eintrag in der Bemerkungsübersicht öffnet
   die HTML-Anzeige. **Bearbeiten** öffnet die gespeicherte Fassung erneut.

Die beim Absenden festgehaltene Auswahl und der bestätigte Buchzustand gehören
zum jeweiligen Diskussionsbeitrag. Eine spätere Auswahl oder abgewählte
KI-Kontextfelder ändern das Ankerziel nicht. Frage, Originalantwort und bestätigte
Benutzerfassung bleiben getrennt. Erneutes Speichern derselben Antwort bearbeitet
denselben Datenbankeintrag; reine Inhaltsänderungen schreiben das EPUB nicht neu.

`noteSource` enthält den editierbaren Markdown-Text, `noteHtml` die bereinigte
HTML5-Darstellung und `noteMetadata` die Herkunft mit `schemaVersion: 1`.
`note` bleibt die Klartextfassung für bestehende Übersichten. `anchorId` bezeichnet
den vorhandenen UUID-Anker beziehungsweise dessen Begin-/End-Paar im EPUB.
Die Migration übernimmt vorhandene Klartext-, EPUB- und PDF-Bemerkungen.

Die Ankerauflösung indiziert die aktuellen Spine-Dokumente einmal je geöffnetem
Buch und behält nur Positionen im Speicher. Zeichenpositionen innerhalb
transparenter Ankerhüllen werden als Textknoten-Offsets bestimmt. Fehlende,
unvollständige und doppelte Anker werden zurückgewiesen. Anker und Inhalt werden
nicht automatisch neu erzeugt. Bei einem Datenbankfehler nach erfolgreichem
Ankerschreiben bleibt der Entwurf mit seiner neuen Anker-ID für einen erneuten
Speicherversuch erhalten.

Ein Anker übersteht Strukturänderungen nur, solange der Editor die UUID und bei
mehrteiligen Bereichen beide Grenzmarker erhält. Eine Sicherung beziehungsweise
Übertragung der vollständigen Bemerkungen benötigt EPUB **und** Benutzerdatenbank.
Ein Export des Bemerkungsinhalts als EPUB-Fußnote ist nicht Bestandteil dieser
Funktion. Sigil und der gemeinsame IPC-Vertrag bleiben unberührt.

## Installation

Das Verzeichnis `build` ist der Installationsbuild mit `CMAKE_BUILD_TYPE=Release`,
`BUILD_TESTING=OFF` und `CMAKE_INSTALL_PREFIX=/usr`. Damit bleibt der gewohnte
Aufruf verwendbar:

```sh
cd /home/markus/Projekte/myArianna
ninja -C build && sudo ninja -C build install/strip
```

Die Einstellungen bleiben im CMake-Cache von `build` gespeichert. Falls dieses
Verzeichnis neu angelegt wird, sind Release, deaktivierte Tests und `/usr`
erneut zu konfigurieren. Die automatisierten Tests verwenden weiterhin den
getrennten Prüf-Build `build-ai-notes`.

## Reproduzierbare Prüfung

Der getrennte Prüf-Build heißt `build-ai-notes`. Nach einem Qt-Update muss dieser
frisch konfiguriert werden, damit keine Objekte einer älteren Qt-Version
weiterverwendet werden. Er enthält die Anwendung und die Tests; es wurde weder
installiert noch automatisch committet.

```sh
cmake -S . -B build-ai-notes -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON \
  '-DCMAKE_MAP_IMPORTED_CONFIG_RELEASE:STRING=NONE;RELEASE;' \
  -DARIANNA_OKULAR_SOURCE_DIR=/home/markus/Projekte/okular
cmake --build build-ai-notes --target arianna ainotetest -j 3
ctest --test-dir build-ai-notes -R '^arianna-ainotetest$' --output-on-failure
```

Der Test verwendet ausschließlich temporäre EPUBs und Datenbanken sowie einen
lokalen KI-Testprozess. Der Ende-zu-Ende-Ablauf verwendet den echten BookServer,
Reader und Qt WebEngine: Auswahl → KI-Diskussion → Entwurfsbearbeitung und
Vorschau → Speichern → Neuladen → Anker-/HTML-Anzeige. Zusätzlich geprüft werden
Abbrechen ohne Schreibzugriff, verlustfreie Altdatenmigration, veralteter
Buchzustand, fehlendes Buch/Anker, Unicode, Mehrfachelementbereiche, doppelte IDs,
Navigation nach XHTML-Strukturänderung, lokale MathJax- und Tabellenanzeige sowie
erneutes Speichern ohne weiteren Anker. Die Tests benötigen lokal erlaubte
Loopback-Sockets und Qt WebEngine. Ein interaktiver Test mit einem echten
KI-Dienst ist dadurch nicht ersetzt.

Geprüft am 21.10.2026 mit Qt 6.11.2:

| Testprogramm | QtTest-Ergebnis |
| --- | --- |
| `ainotetest` einschließlich Ende-zu-Ende-Ablauf | 8 bestanden |
| `codexqmltest` | 10 bestanden |
| `epubcontainertest` | 64 bestanden |
| `booktruthstoretest` | 20 bestanden |
| `bookstateimporttest` | 11 bestanden |
| `epubreferenceanchortest` | 3 bestanden |
| `pdfannotationviewtest` | 6 bestanden |

Keine fehlgeschlagenen oder übersprungenen Prüfungen. Die Protokolle und der
gegen den Zustand zu Arbeitsbeginn abgegrenzte Änderungspatch liegen unter
`build-ai-notes/validation/`. JavaScript-Syntax und deutscher Nachrichtenkatalog
wurden zusätzlich geprüft. Im Headless-Test bleiben die bekannten Qt-/Foliate-
Meldungen zu Vulkan, iframe-Sandbox, fehlendem Fixture-Inhaltsverzeichnis und
ResizeObserver sichtbar; die neue HTML-Anzeige lädt MathJax lokal ohne SRE-
Fetchfehler. Ein manueller Desktop-Test mit einem echten KI-Dienst steht aus.
