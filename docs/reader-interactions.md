# Klickziele und Mauszeiger im EPUB-Reader

Bemerkungen und Querverweise dürfen denselben Textbereich belegen. Ein innerer
Bemerkungsanker in einem äußeren Querverweis ist dabei ein gültiger Fall,
beispielsweise eine Bemerkung zu „Pralambari“ innerhalb des Verweises
„Pralambari Balarama“. Eine vollständig deckungsgleiche Auswahl ist ebenfalls
zulässig.

## Entscheidung und Verantwortung

Die Kollisionsauflösung ist kurzlebiger Reader-Zustand. Vorhandene EPUB-Anker
und persistierte Bemerkungen/Querverweise bleiben autoritativ; es werden keine
neuen Buchdaten, Datenbanktabellen, Migrationen oder EPUB-Auszeichnungen erzeugt.
`reader-interactions.js` sammelt für einen Klick die tatsächlich getroffenen
Bemerkungsbereiche aus dem vorhandenen Overlayer und die umschließenden
Querverweisanker bzw. normalen EPUB-Links. Auch Bemerkungen mit einer bestehenden CFI ohne eigenen
XHTML-Wrapper verwenden denselben geometrischen Treffertest.

Ein einzelnes Ziel wird direkt geöffnet. Mehrere Ziele öffnen die native
Qt-Quick-Auswahl „Bemerkung / Querverweis“; gegebenenfalls ist auch „Formel“
verfügbar. Die ausgewählte Aktion verwendet unverändert den bisherigen
Bemerkungs-, Querverweis- oder MathJax-Aufruf. Escape führt keine Aktion aus.
Normale EPUB-Links, einschließlich Fußnoten, verwenden `View.followLink()` und
damit denselben vorhandenen Link-/Fußnotenpfad wie ein direkter Linkklick.
Markierter Text und Auswahlgesten lösen keinen solchen Klick aus. Versteckte
Treffer, Sprachmarkierungen und Suchumrandungen verdrängen keine Bemerkungen.

Der zentrale Klickhandler läuft vor den bisherigen Link-/Overlay-Handlern.
Diese respektieren `defaultPrevented`, sodass keine zweite Aktion denselben
Klick übernimmt. Nach einem Kapitelwechsel oder geschlossenen Reader werden
veraltete Auswahlantworten verworfen.

## Interner JS/QML-Vertrag

Der bestehende WebChannel-Pfad `show-selection` wird weiterverwendet:

```json
{
  "type": "show-selection",
  "payload": {
    "type": "reader-targets",
    "requestId": 1,
    "choices": [
      {"kind": "annotation", "text": "Pralambari"},
      {"kind": "reference", "text": "S.B. Kapitel 1"}
    ]
  }
}
```

`kind` ist `annotation`, `reference` oder `math`. QML übersetzt die Beschriftung;
`text` dient nur als Tooltip. Aufrufbare Funktionen und DOM-Ranges bleiben im
JavaScript und werden nicht übergeben. `selectionAction(index, requestId)`
antwortet mit dem nullbasierten Index. `selectionAction(undefined, requestId)`
bricht ab. Veraltete IDs und ungültige Indizes führen keine Aktion aus.
Die Request-ID ist ein monotoner, nur für diese Reader-WebChannel-Sitzung gültiger
Zähler. Bisherige Aufrufe `selectionAction(action)` ohne zweiten Parameter bleiben
kompatibel. BookServer-/Sigil-Schnittstellen und ihre Dokumentidentitäten ändern
sich nicht.

## Rechtsklick

Rechtsklick verwendet denselben Treffertest und dieselbe Zielauswahl wie
Linksklick. Bei Überlappungen wird erst „Bemerkung / Querverweis / Formel“
gewählt; anschließend erscheint das betreffende Kontextmenü. Ohne Überlappung
öffnet sich direkt das passende Menü. Bemerkungen bieten Anzeigen (bei
HTML-Inhalt), Bearbeiten, Text auswählen und Löschen. Querverweise und Formeln
verwenden ihre bisherigen Kontextaktionen. Ein bloßer Rechtsklick navigiert
oder verändert noch nichts.

Das vorhandene `show-selection`-Payload für Bemerkungen erhält optional
`contextMenu: true`; ältere Aufrufe bleiben unverändert. Überlappt eine
Bemerkung mit einem normalen EPUB-Link, ist auch dessen Öffnen/Kopieren über
`type: "reader-link", href: "..."` erreichbar. Einfache EPUB-Links ohne
Überlappung und gewöhnlicher Text behalten das WebEngine-Kontextmenü.
Vorhandene Textauswahl außerhalb von Formeln und Modifikatortasten werden nicht
vom neuen Kontextmenü abgefangen. Das Formel-Kontextmenü erhält wie bisher eine
vorhandene Textauswahl. Es enthält ausschließlich „Formeldialog öffnen“,
„Formel vorlesen“ und das MathJax-Vorschau-Untermenü. Die allgemeinen
Browseraktionen einschließlich ihres Trennstrichs sind dort ausgeblendet;
Bild-Kontextmenüs behalten diese Aktionen. Die Menüs verwenden Qt-Quick-Farben
und Theme-Icons.

Antworten und Menüabbrüche verwenden die bestehende Request-ID. Veraltete
Menüantworten verändern keinen neueren Auftrag; auch ein verspätet geladener
Querverweis öffnet nach einer neuen Interaktion kein altes Kontextmenü.
Die gewählte Aktion wird erst nach dem vollständigen Schließen des vorherigen
Menüs übergeben, damit dessen Fokuswiederherstellung das neue Menü nicht stört.
Menüaufrufe bleiben rein temporärer Reader-Zustand. EPUB-Datei und persistente
Bemerkungen/Querverweise werden erst durch eine ausdrücklich gewählte
Bearbeitungs- oder Löschaktion geändert.

## Cursor und CFI-Erhaltung

Neben dem normalen Zeigecursor erscheinen Bemerkung, Verweis oder „√x“ als
kleine SVG-Symbole. Bei Überschneidungen werden die Symbole kombiniert. Die
Symbole werden im äußeren Reader-Dokument gezeichnet. Sie sind dadurch von der
Übernahme eines CSS-Cursorbildes durch Qt/Wayland und von der Bild-CSP des EPUBs
unabhängig. Ein berechneter CSS-Cursor oder ein interner Qt-Bitmapcursor allein
beweist noch nicht, dass das Cursorbild tatsächlich auf dem Desktop sichtbar ist.

Hintergrund, Symbole und Rahmen verwenden die bestehenden Reader-Farbvariablen
`--bg` und `--fg`. Damit folgen sie der normalen, inversen oder KDE-System-
Darstellung und ändern ihre Farben bei einem Themewechsel direkt. Es werden
keine eigenen Farbpräferenzen oder persistenten Buchdaten angelegt.

Im Kapitel bleibt nur das Zustandsattribut des vorhandenen `html`-Elements;
die Regeln für den normalen Zeigecursor kommen über die bestehende
Reader-Stylesheet-Schnittstelle. Es werden keine Textknoten, Inline-Icons oder
zusätzlichen Wrapper in den Buchinhalt eingefügt. Textfluss, Zeichenoffsets und
CFIs bleiben erhalten. Das Symbolfeld nimmt keine Mausereignisse entgegen.
Seine Position berücksichtigt die Transformation des Kapitel-Iframes und den
Reader-Zoom. Horizontal beginnt das Symbolfeld sechs CSS-Pixel rechts vom
Mauszeiger, vertikal bleibt der Abstand bei 16 CSS-Pixeln. Mausereignisse ergänzen
die Pointer-Ereignisse, damit die Anzeige
auch auf Desktop-Pfaden ohne Pointer-Ereignisse aktualisiert wird.

Bei Textauswahl, Maustastendruck, Verlassen des Kapitels, Fokusverlust,
Kapitelwechsel, Scrollen und automatischer Cursor-Ausblendung verschwinden die
Symbole. Die SVG-Daten sind fest im Programm enthalten; es werden keine
Cursorbilder geladen und keine CSP erweitert. Formelquellen werden erst beim
Klick ausgelesen, nicht bei jeder Mausbewegung.

## Prüfung

`AiNoteTest::overlappingReaderTargets` lädt ein Test-EPUB über BookServer in die
echte QML-Readerseite und bedient sie mit nativen Qt-Mausklicks. Abgedeckt werden
die verschachtelten Pralambari-Anker, umgekehrte Verschachtelung mit Devanāgarī,
IAST und kombinierendem Zeichen, vollständige Überlappung, ein Querverweis mit
zusätzlichem `href`, normale EPUB-Links, eine CFI-Bemerkung ohne Wrapper,
einzelne Ziele, die Cursorzustände, nichtinteraktive Such-/Sprachmarkierungen,
Abbruch und veraltete Auswahlantworten. EPUB-Bytes,
Anker, Text und CFI werden vor/nach den Aktionen verglichen.
Rechtsklicks prüfen zusätzlich die Zielauswahl, Anzeigen und Bearbeiten der
Bemerkung, Bearbeiten des Querverweises, Öffnen eines überlagerten EPUB-Links,
Escape in beiden Menüstufen und den Erhalt einer bestehenden Textauswahl.
Die Symbolanzeige wird außerdem als tatsächlich gerenderte Bildfläche geprüft,
auch bei 130 Prozent Zoom, inverser Darstellung, `img-src 'none'`, reinen
Mausereignissen und automatischer Ausblendung.
Ein Regressionstest liefert dieselbe Mausbewegung zuerst an den äußeren Reader
und dann an das Kapitel-Iframe. Die `CursorAutohider`-Klone prüfen ihre letzte
Mausposition jeweils selbst; ein bereits im äußeren Reader verarbeitetes
Ereignis darf das Wiederanzeigen im Kapitel nicht verhindern.
Ist Foliates eigene Cursor-Ausblendung deaktiviert, entfernt sie keinen durch
Arianna gesetzten Ausblendungszustand. Sie gibt nur einen selbst ausgeblendeten
Cursor wieder frei.

`EpubMathTest::paginationWaitsForMath` prüft die Formelaktion und den „√x“-Cursor
mit Reader-MathJax und EPUB-eigenem MathJax in Reflow- und Fixed-Layout-Ansichten,
einschließlich der bestehenden Auswahl-, Geometrie- und CFI-Prüfungen.
`EpubMathTest::viewerFormulaContextMenu` prüft die drei sichtbaren
Formel-Menüeinträge, die Untermenüs, Formelquelle und Vorlesen sowie die
unveränderten Browseraktionen bei gewöhnlichen Bildern.
