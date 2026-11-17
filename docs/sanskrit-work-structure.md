<!-- SPDX-FileCopyrightText: 2026 Arianna contributors -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Sanskrit-Werkgliederung: Architektur, Umsetzung und Prüfnachweise

Untersuchung: 13. Oktober 2026. Umsetzung: 14. Oktober 2026. Die freigegebene
Architekturentscheidung wurde im Arbeitsbaum umgesetzt; der Implementierungs-
und Prüfstand steht unten. Die Infrastruktur-Befunde beschreiben den untersuchten
Ausgangsstand einschließlich seiner schon vorhandenen Änderungen. Es wurden keine
Benutzerbücher verändert und keine externe KI-Anfrage zur Werkerkennung gestellt.

**Architekturentscheidung**

Eine optionale, lesende Analyse nach Import oder Normalisierung erzeugt einen
Vorschlag. Ein zentraler BookServer-Dienst liest und validiert die Werkstruktur;
Navigation, Cross-References und TTS verwenden dessen Ergebnis. Die vorhandene
Codex-Anbindung kann unklare Zuordnungen ergänzen. Erst der ausdrückliche
editorische Vorgang „Bestätigen und ins EPUB schreiben“ übernimmt geprüfte
Zuordnungen in eine versionierte Metadatenrepräsentation im EPUB.

Für die erste Fassung bleiben Vorschläge und deren Bearbeitung im offenen
Prüfdialog. Bestätigte und übernommene Daten leben im EPUB. Deshalb sind weder
eine weitere Datenbank noch eine neue Tabelle nötig. Eine vorhandene bestätigte
Struktur wird zuerst geladen und niemals durch automatische Analyse ersetzt.

**1. Vorhandene Infrastruktur und Grenzen**

| Bereich | Bereits vorhanden | Nötige Ergänzung |
| --- | --- | --- |
| Import | `BookListModel::importBookFile()` registriert Buchidentität und Metadaten; mehrere Importwege laufen hier zusammen. | Nach erfolgreichem Import optional eine lesende Analyse anstoßen. Import und KI-Anfrage bleiben getrennte asynchrone Schritte. |
| Normalisierung | `EPubContainer::normalizeForArianna()` repariert leere XHTML-Titel und fehlende `scripted`-Manifestangaben; `BookListModel::checkAndRefreshBookFromFile()` ruft sie auf. | Analyse nach Abschluss auf dem dann gültigen Quellstand. Keine automatische Strukturübernahme in den reparierenden Normalisierer einbauen. |
| OPF | `metadata()`, `manifestItems()`, `items()` und `readData()` liefern Metadaten, Ressourcen und Spine-Reihenfolge. | Gezieltes Lesen verknüpfter Strukturmetadaten und bibliografischer Verfeinerungen. Der C++-Parser verarbeitet derzeit nur ausgewählte EPUB-3-`meta property`-Angaben und keine Metadaten-`link`-Records. |
| Bibliografie im Reader | Foliate `epub.js` verarbeitet Rollen und explizite Edition; `ReaderContextBuilder` erstellt JSON-Snapshots. | Ein Import-Snapshot braucht keine Textauswahl. Deshalb ein eigenes kleines Eingabeschema statt einer künstlichen Reader-Auswahl verwenden. Edition und Herausgeber aus OPF lesen; fehlende Angaben weglassen. |
| Navigation und Ziele | `navigationTargets()` liest EPUB-3-Navigation und NCX. `referenceableTargets()` ergänzt dokumentinterne Ziele samt CFI. `TargetAnchorInfo` enthält `tocPath`, `tocDepth`, HREF und CFI. | Quellknotenidentität und Elternbeziehungen für die Analyse erhalten; Überschriften ohne ID, Versmarker und explizite Semantik ergänzen. |
| BookServer | Authentifizierte Ressourcen-/Zielrouten, `/<bookId>/targetLocations`, Container-Cache mit Dateistand und Delivery-Version. | Zentraler lesender Strukturendpunkt und optionale Strukturfelder an bestehenden Zielen. |
| Buchzustand und Commit | `BookTruthStore` hat Snapshots, Text-/Inhalts-/Dateihashes, Kandidaten-Commit, Ankerprüfung und Rollback; `BookStateImport` importiert Editor-Arbeitskopien. | Ein Metadaten-Kandidat wird vor Veröffentlichung gegen den analysierten Basisstand geprüft. Der aktuelle `commitCandidateBookFile()` nimmt noch keinen erwarteten Basishash entgegen. |
| Persistenz | `BookDatabase` enthält Bibliothek, Annotationen, Referenzen sowie `book_current_state` und `book_state_history`. | Diese Tabellen bleiben für ihren bisherigen Zweck zuständig. Die übernommenen Strukturmetadaten benötigen keine weitere autoritative DB-Kopie. |
| KI | `CodexClient` nutzt lokale Authentifizierung, lesenden Sandbox-Modus, Streaming, Abbruch und Turn-IDs. | Strukturierte Eingabe/Ausgabe und getrennte Sitzungszuständigkeit für Diskussion und Strukturprüfung. |
| TTS | `reader.js` extrahiert Originaltext/CFIs; `SpeechController::pronouncedText()` normalisiert ausschließlich den Sprachpayload. `SanskritNumbers` erweitert einzelne Zahlen 0–100. | Bestätigte Koordinaten vor dem Sprechen auflösen und als quellgebundene Ausspracheinformation übergeben. |

Wichtige Grenzen der vorhandenen Zielauswertung:

- Navigationseinträge werden nach Zieladresse dedupliziert. Nicht verlinkte
  Gruppenüberschriften liefern nur Label-Kontext. Das genügt nicht, um eine
  verlustfreie Werkhierarchie oder Widersprüche zwischen NAV und NCX zu prüfen.
  Die Analyse muss diese Informationen beim vorhandenen Parser zusätzlich
  erfassen, ohne das bisherige Zielmodell umzubauen.
- Dokumentinterne Ziele werden überwiegend über vorhandene IDs/Namen erkannt.
  Nummerierte Verse oder Überschriften ohne ID brauchen eine lesende Extraktion
  mit quellgebundenen CFI-Bereichen. Es werden keine IDs in XHTML eingefügt.
- `reader.js` enthält eigene Kapitel-/Versheuristiken für Referenztitel.
  Bestätigte Werkkoordinaten sollten diese für Sanskrit ersetzen; vorhandene
  manuelle Referenztitel bleiben bestehen.
- `SanskritNumbers` lässt verbundene Zahlenausdrücke absichtlich unverändert.
  Insbesondere ist `१-४` derzeit weder als Werkkoordinate klassifiziert noch
  als solche ausgesprochen.
- `createServerReadyEpub()` erzeugt eine temporäre Auslieferungsfassung mit
  Ressourcenumschreibungen und Reader-Zusätzen. Sie ist kein Exportkandidat
  für autoritative Werkmetadaten.
- `createEpubReplacingFiles()` kann nur vorhandene Archiveinträge ersetzen.
  Für einen neuen JSON-Record ist eine kleine Erweiterung um neue Einträge und
  verlässliche Fehlerprüfung beim Kopieren/Schreiben erforderlich.
- `checkEpubFile()` bietet bereits EPUBCheck-Aufruf und Fehlerauswertung,
  wartet jedoch synchron. Eine längere Import-/Prüfoperation muss den
  Validator außerhalb des GUI-Threads oder asynchron ausführen.

**2. Datenart, Verantwortlichkeit und Lebenszyklus**

| Information | Klasse nach AGENTS.md | Speicherung und Zuständigkeit |
| --- | --- | --- |
| OPF, Navigation, Überschriften, originale Versmarker, explizite Semantik | A | Unveränderte Quelle; `EPubContainer` liest sie. |
| Unicode-/Ziffernnormalisierung, erkannte Begriffsformen, Quellenindex | B | Im Analysesnapshot und bei Bedarf im bestehenden BookServer-Cache. |
| Deutung von „Chapter“ als āśvāsa, aus Nummern erschlossene Ebenen, KI-Vorschläge | C | Im Arbeitsspeicher des Prüfablaufs, jeweils mit Belegen und Unsicherheiten. Lokale Regeln sind nicht automatisch autoritativ. |
| Manuelle Zuordnung, Korrektur und Bestätigung | D | Im offenen Review bis zur ausdrücklichen Übernahme; anschließend im EPUB mit Herkunft und Bestätigung erhalten. Keine Cache-Eviction bestätigter Korrekturen. |
| Übernommene editorische Werkstruktur | A mit dokumentierter Herkunft aus C/D | Versionierter EPUB-Record; BookServer hält nur eine rekonstruierbare Lesefassung im Cache. |
| Aktivierung der optionalen Analyse | D | Vorhandenes KConfigXT, standardmäßig deaktiviert; eine KI-Übermittlung bleibt eine ausdrückliche Aktion. |
| Aussprache einer bestätigten Koordinate | B | Kurzlebiger TTS-Payload; Originaltext, Marks und CFIs bleiben Grundlage. |

Der dauerhafte Nutzen liegt im einmal bestätigten, portablen Werkwissen. Eine
zweite persistente Kopie würde Konflikte zwischen EPUB und Datenbank erzeugen.
Ein separat gespeicherter, noch nicht übernommener Benutzerentwurf wäre dagegen
Benutzerdatum und müsste geschützt in der bestehenden Datenbank gespeichert
werden. Solche sitzungsübergreifenden Entwürfe gehören nicht zur kleinsten
ersten Fassung. Schließen oder Verwerfen eines bearbeiteten Reviews muss den
Verlust dieser noch nicht übernommenen Eingaben ausdrücklich kenntlich machen.

**3. Analyse und optionale KI-Anfrage**

1. Vorhandenen Strukturrecord lesen, Version und Abhängigkeiten prüfen. Eine
   gültige bestätigte Struktur beendet die automatische Analyse. Bei Fehlern
   wird sie erhalten und zur Prüfung angezeigt.
2. Unveränderlichen Snapshot mit Buchidentität, Basis-Dateihash, expliziter
   Bibliografie, Spine, NAV/NCX-Knoten, Überschriften und Versmarkern erstellen.
   Jeder Beleg hat eine lokale ID, Dokumentpfad und vorhandenen HREF oder CFI.
   Auch nicht verlinkte TOC-Elternknoten bleiben enthalten. EPUB-3-NAV ist die
   primäre Navigation; NCX wird als zusätzliche Quelle auf Konflikte geprüft.
3. Begriffe mit einem begrenzten, versionierten Vokabular erkennen, zunächst
   `asvasa`, `adhyaya`, `skandha`, `kanda`, `sloka`, `pada`, mit überprüften
   IAST-/Devanagari-Formen und Schreibvarianten. Vergleichsnormalisierung
   verändert weder die Quelle noch die gespeicherte Anzeigeform.
4. Explizite Semantik und Belege stärker gewichten als reine Labels.
   Versnummern, Nummerierungsneustarts und Elternbeziehungen prüfen. Englische
   oder deutsche TOC-Labels sind Hinweise; „Chapter“ bedeutet nicht stets
   adhyāya. TOC-Tiefe ist keine Sanskrit-Strukturebene.
5. Bei Mehrdeutigkeit einen einsehbaren KI-Request vorbereiten. Er enthält
   Titel, Autor, explizite Edition, relevante TOC-Einträge, nötige kurze
   Überschriften-/Versbelege, lokale Beleg-IDs, erlaubte Begriffe und den
   bisherigen lokalen Vorschlag. Begrenzungen oder ausgelassene Knoten
   müssen sichtbar sein; ein Ausschnitt darf nicht als vollständiges Werk gelten.
6. Erst „Mit KI prüfen“ sendet diesen Snapshot. Fehlende Metadaten werden
   weggelassen; Titelwissen allein legitimiert keine Kapitelzuordnung. Ein
   unbekannter Begriff oder eine nicht belegbare Zuordnung bleibt offen.
7. Die KI liefert Ebenenvorschläge und Zuordnungen ausschließlich zu gegebenen
   Beleg-IDs, Begründung, Unsicherheiten, Alternativen und optionale
   Quellenhinweise. Das JSON-Schema erlaubt ausdrücklich „nicht bestimmbar“.
8. Lokal Schema, Begriffe, IDs, Zielauflösung, Elternbeziehungen, Nummerierung
   und Widersprüche prüfen. KI-Werte für freie Zielpfade, Bestätigung oder
   Quellenverifikation werden nicht übernommen. Ungültige Teile bleiben
   zurückgewiesene Vorschläge und erscheinen nicht in Verbraucher-APIs.

Die installierte CLI `0.162.0` wurde am Untersuchungstag erneut geprüft.
Ihr generiertes `v2/TurnStartParams.json` unterstützt `outputSchema` zur
Beschränkung der abschließenden Antwort. `CodexClient::startInput()` sendet
dieses Feld bislang nicht. Die kleinste Transportergänzung erlaubt ein
JSON-Datensnapshot, ein Ausgabeschema und eine eindeutig zugeordnete fertige
Antwort. Der Domänendienst validiert zusätzlich die semantische Richtigkeit;
ein korrektes JSON-Schema beweist keine zutreffende Werkerkennung.

Diskussion und Strukturprüfung brauchen getrennte Threads mit gebundenen
Auftrag-/Thread-/Turn-IDs und aufgabenspezifischen Instruktionen. Ein kleiner
Sitzungsadapter über dem vorhandenen Transport kann Aufträge serialisieren;
mehrere gleichzeitig laufende Codex-Prozesse sind dafür nicht erforderlich.
Der Strukturauftrag darf weder den Diskussionsthread übernehmen noch dessen
Abbruch/Verbindungsabbau auslösen. Vor Wiederverwendung des vorhandenen
Singletons ist dessen Einzelthread-Zustandsverwaltung entsprechend anzupassen.

Die bisherigen Beschränkungen bleiben erhalten: Buchdaten sind untrusted input,
Datei-/Shellzugriff und Werkänderungen sind der KI nicht zugänglich. Die
vorhandene Sitzung untersagt externe Tools. Quellenhinweise sind deshalb ohne
separate Verifikation ausdrücklich ungeprüft; sie werden nicht zu Belegen
hochgestuft. Tatsächlich verwendetes Modell/Provider, Transportversion und
Prompt-/Analyserversion werden erfasst, soweit vom Protokoll verfügbar. Die
CLI-Version ist keine Modellversion.

**4. Bestätigung und EPUB-konforme Speicherung**

Der Prüfdialog stellt die vorhandene EPUB-Hierarchie neben die vorgeschlagenen
semantischen Ebenen. Pro Zuordnung zeigt er Originalbezeichnung, Ziel,
Belege, Herkunft und Unsicherheit. Nichttriviale/unsichere Zuordnungen sind
zunächst unbestätigt. Manuelle Korrektur, Ablehnung und teilweise Übernahme
bleiben möglich. Unaufgelöste Lücken werden als solche erfasst; Verbraucher
dürfen daraus keine vollständige Hierarchie ableiten.

„Bestätigen und ins EPUB schreiben“ ist der ausdrückliche editorische Vorgang
nach AGENTS.md. Die Analyse-/Bestätigungslogik gehört in Arianna; eine zusätzliche
Sigil-Erweiterung ist für diesen begrenzten Metadatenvorgang nicht nötig. Der
Vorgang erstellt einen Kandidaten aus dem Originalstand und veröffentlicht ihn
über `BookTruthStore`, nicht über die temporäre Reader-Auslieferungsfassung.

Für EPUB 3 wird UTF-8-JSON als lokaler Metadatenrecord neben dem OPF gespeichert:

```xml
<!-- Innerhalb des bestehenden OPF-metadata-Elements;
     href relativ zum Paketdokument. -->
<link rel="record"
      href="metadata/arianna-work-structure.json"
      media-type="application/json"/>
```

Der Record erhält einen eindeutigen Typmarker `arianna.work-structure` und
`schemaVersion: 1`. Beim Lesen wird er unter den lokalen verknüpften JSON-Records
an diesem Marker erkannt. Ein Dateiname allein ist keine Formatbestätigung.
Pfadkollisionen, mehrere konkurrierende Records und unbekannte Versionen
erzeugen einen Prüfzustand; bestehende Ressourcen werden nicht überschrieben.

Ein eigenständiger, ausschließlich verknüpfter Metadatenrecord gehört laut
[EPUB 3.3, Abschnitt 5.5.6](https://www.w3.org/TR/epub-33/#sec-link-elem)
außerhalb des Manifests und der Spine. Damit benötigen JSON und seine interne
Struktur keine sichtbare Fallback-Seite. Die EPUB-3-Modifikationszeit wird gemäß
[Abschnitt 5.5.5](https://www.w3.org/TR/epub-33/#last-modified-date) aktualisiert.

EPUB 2 darf dabei nicht stillschweigend auf EPUB 3 umgestellt werden. Für dessen
kleinste kompatible Variante wird dieselbe versionierte JSON-Repräsentation
XML-escaped im `content` einer zusätzlichen OPF-`meta`-Angabe
`name="arianna:work-structure"` gespeichert. Freie Metadaten sind in
[OPF 2.0.1, Abschnitt 2.2](https://idpf.org/epub/20/spec/OPF_2.0.1_draft.htm#Section2.2)
vorgesehen. Ein EPUB-3-`link` wird nicht in ein OPF-2-Paket eingefügt. Beide
Verpackungsvarianten verwenden denselben JSON-Codec und dieselbe API; sie sind
vor Implementierungsabschluss mit EPUBCheck zu validieren.

Alternativen: Einzelne freie OPF-Eigenschaften bilden ein Ebenenvokabular ab,
werden für konkrete Textbereiche und Provenienz aber unhandlich. OPF-Collections
können Ressourcengruppen ausdrücken, erfordern hier trotzdem ein eigenes
Semantik-/Versionsmodell. JSON-LD in XHTML würde zusätzliche Änderungen am
Autorendokument verlangen. Der externe lokale Record ist für EPUB 3 der
kleinste Eingriff.

**5. Datenmodell und Beispiel**

Der Record trennt Werkebenen, tatsächlich belegte Instanzen und
Koordinatenschreibweisen. Eine Ebene kann in der EPUB-TOC fehlen, und ein
semantischer Abschnitt kann mehrere Spine-Dokumente umfassen. Es werden keine
Verse oder pādas aus einer erwarteten Werklänge erzeugt. Bei Sammelbänden muss
der Geltungsbereich ein bestimmtes Werk bzw. dessen Quellbereich benennen.

Folgende Kurzform illustriert das Modell; Pfade, IDs und Hashwerte sind
Beispiele, keine Feststellungen über eine vorhandene Kṛṣṇa-karṇāmṛta-Ausgabe:

```json
{
  "type": "arianna.work-structure",
  "schemaVersion": 1,
  "revision": "example-revision-1",
  "bookIdentifier": "example-book-id",
  "analysis": {
    "algorithmVersion": "sanskrit-structure-1",
    "sourceBasisVersion": 1,
    "sourceBasisHash": "sha256:<analysis-input-hash>",
    "calculatedAt": "2026-10-13T10:00:00Z"
  },
  "review": {
    "status": "confirmed",
    "confirmedAt": "2026-10-13T10:05:00Z"
  },
  "levels": [
    {"id": "level1", "term": "asvasa", "iast": "āśvāsa", "devanagari": "आश्वास"},
    {"id": "level2", "term": "sloka", "iast": "śloka", "devanagari": "श्लोक"}
  ],
  "nodes": [
    {
      "id": "a1",
      "level": "level1",
      "number": 1,
      "parent": null,
      "locators": [{"href": "Text/part1.xhtml#section1"}],
      "origin": "manual-confirmation",
      "evidenceIds": ["toc-1"]
    },
    {
      "id": "a1-s4",
      "level": "level2",
      "number": 4,
      "parent": "a1",
      "locators": [{"href": "Text/part1.xhtml#verse4"}],
      "origin": "manual-confirmation",
      "evidenceIds": ["verse-4"]
    }
  ],
  "coordinateFormats": [
    {"levels": ["level1", "level2"], "separators": ["-"], "digitSets": ["ascii", "devanagari"]}
  ]
}
```

Die vollständige Fassung braucht zusätzlich die nachvollziehbaren Belege mit
Dokumentidentität/Quellhash, Textbereich, Herkunft (`explicit`, `local-rule`,
`model`, `manual`), Begründung, Unsicherheiten und Validierung pro Zuordnung.
Für KI-gestützte Teile kommen Modell-/Promptversion sowie gegebenenfalls
ungeprüfte Quellenhinweise hinzu. Bestätigung und Herkunft sind getrennt:
ein KI-Vorschlag bleibt als solcher nachvollziehbar, auch nach Bestätigung.
Ein Knoten kann mehrere dokumentübergreifende Locator-Bereiche besitzen.
Ohne vorhandene Fragment-ID verwendet ein Locator einen geprüften CFI-Bereich
und den Hash des zugrunde liegenden Dokuments. Zahlen können editionseigene
Zusätze besitzen; diese dürfen nicht durch laufende Zähler ersetzt werden.

`१-४` ergibt nach Ziffernnormalisierung `[1, 4]`. Nur innerhalb des bestätigten
Geltungsbereichs, mit bestätigtem Koordinatenformat und eindeutig vorhandenem
Knoten `a1-s4` wird daraus „āśvāsa 1 / śloka 4“. Ein Bindestrich allein beweist
keine Koordinate: Zahlenbereiche, Kommentare und konkurrierende Zieladressen
bleiben mehrdeutig. Auflösung benötigt den Kontext des Quellmarkers bzw. der
Referenz. Ohne eindeutigen Treffer bleiben Navigation und TTS unverändert.

**6. Zentrale API und Verbraucher**

Ein kleiner `WorkStructureService` und ein JSON-Codec übernehmen Laden,
Gültigkeitsprüfung, Koordinatenauflösung und Rückgabe. Ein separater
`SanskritStructureAnalyzer` erzeugt Vorschläge; ein Review-Controller besitzt
den temporären Bearbeitungszustand und den optionalen KI-Auftrag. Der Codec
liest/schreibt, der Analyzer interpretiert, der Commit veröffentlicht.

Vorgeschlagene native Schnittstelle, noch nicht implementiert:

```text
structureForBook(bookId, sourceVersion)
    -> record + revision + validity + issues
structureAtLocation(bookId, hrefOrCfi)
    -> confirmed semantic path, or unresolved/ambiguous
resolveCoordinate(bookId, sourceText, sourceContext)
    -> confirmed node + existing href/CFI, or unresolved/ambiguous
```

BookServer stellt die Lesefassung über `GET /<bookId>/workStructure` bereit,
unter denselben Token-/Buchzugriffsregeln wie die bestehenden Zielrouten.
`targetLocations` erhält optional `structureNodeId`, `semanticPath` und
`structureRevision`; seine vorhandenen Felder behalten ihre Bedeutung.
Die gleiche C++-Validierung dient dem serverseitigen Auflösen. Reader und
Cross-Reference-UI übernehmen nur gültige bestätigte Daten aus diesen APIs;
sie halten keine eigenen Werklexika oder konkurrierenden Persistenzschichten.

Der ausdrückliche Übernahmeauftrag wird durch den Review-Controller an den
BookServer übergeben, beispielsweise mit
`POST /<bookId>/workStructure/commit`, bestätigtem Record, Basis-Dateihash
und bisheriger Record-Revision. Der BookServer erzeugt und prüft den Kandidaten
und ruft `BookTruthStore` auf. Der KI-Transport erhält diese Schreibschnittstelle
nicht. Bei einer als read-only registrierten Quelle bleibt nur die lesende
Analyse; eine Übernahme erfordert einen ausdrücklichen Export in eine Kopie.

Die Navigation verwendet weiterhin Foliate und das bestehende TOC-Modell.
Werkkoordinaten führen zu bestehenden Zielen; englische/deutsche TOC-Titel
werden weder im EPUB noch in der sichtbaren Liste umbenannt. Auch manuelle
Annotationen und Referenztitel werden nicht neu erzeugt oder überschrieben.

Für TTS werden geprüfte Koordinateninformationen mit dem ursprünglichen
Quellbereich/UTF-16-Offsets an die vorhandenen Segmente gebunden. Der native
Normalisierer erstellt daraus nur den Aussprachepayload. Strukturauflösung
findet vor Übergabe an den Speech-Worker statt, ohne Netzwerk-/Dateizugriff im
Sprechpfad. Payloadänderungen dürfen weder Source-Marks neu nummerieren noch
CFIs verschieben. Koordinaten müssen wie bestehende atomare Speech-Teile
behandelt werden, damit eine Wortmarkengrenze keine Teilkoordinate expandiert.
Bestehende Sprach-/Stimmwahl und der Sanskrit-Zahlenschalter bleiben maßgeblich.

**7. Gültigkeit, Konkurrenz und Migration**

Es gibt zwei unterschiedliche Prüfgrundlagen:

- Der Basis-Dateihash/BookState und die Record-Revision schützen einen
  konkreten Review-/Commit-Vorgang vor zwischenzeitlichen Änderungen. Die
  Prüfung muss innerhalb des Commit-Pfads erfolgen. Ein anderer Sigil-/Reader-
  Commit oder eine externe Dateiänderung macht den vorbereiteten Kandidaten
  unbrauchbar; er darf keine neuere Ausgabe ersetzen. Dateihashes kurz vor
  Veröffentlichung prüfen, nicht nur den älteren DB-Stand.
- Ein versionierter Analysebasis-Hash prüft die inhaltlichen Abhängigkeiten:
  tatsächlich verwendete bibliografische Felder, geordnete Spine-Identität,
  NAV/NCX und relevante XHTML-Dokumente einschließlich Locator-Abhängigkeiten.
  Der eigene Record, sein OPF-Link bzw. EPUB-2-`meta`, sowie die vom Export
  aktualisierte `dcterms:modified`-Angabe sind ausgeschlossen. Das normale
  `getContentHash()` umfasst sämtliche Archiveinträge und wäre für diese
  Prüfung wegen der Selbstinvalidierung ungeeignet; es bleibt für BookState
  und Auslieferungscache sinnvoll.

BookServer und UI laufen bereits in getrennten Prozessen. Der gegenwärtige
statische `bookMutationMutex` ist nur prozesslokal. Für den neuen Commit müssen
bestehende Arianna-Schreiber auf einen gemeinsamen Commit-Eigentümer oder eine
buchbezogene prozessübergreifende Sperre abgestimmt werden. Der erwartete
Dateistand wird unter dieser Sperre geprüft; eine bloße Prüfung vor dem
HTTP-Aufruf genügt nicht. Externe Editoränderungen werden weiterhin über
Arbeitskopien und den vorhandenen Importpfad eingebunden.

Änderungen von Text/Markup/Navigation lösen eine Prüfung der betroffenen
Mappings aus. Veraltete Daten bleiben mit ihren manuellen Korrekturen erhalten,
werden aber nicht als gültige Navigations-/TTS-Zuordnung verwendet. Eine neue
Analyse wird getrennt als Vorschlag angeboten. Eine neue Algorithmus- oder
Modellversion invalidiert abgeleitete Vorschlags-Caches; sie überschreibt
keinen bestätigten editorischen Record mit weiterhin gültigen Quellbezügen.
Eine explizite Neubewertung zeigt den Vergleich zur bestätigten Fassung.

Unbekannte Schema-Hauptversionen bleiben unverändert im EPUB und werden als
nicht unterstützt gemeldet. Eine unterstützte Migration geschieht zunächst
in der Lesefassung; ein dauerhaftes Formatupdate ist wiederum ein expliziter
editorischer Commit. Eine DB-Migration entfällt in der ersten Fassung.

**8. Unveränderlichkeitsgarantien und erforderliche Verifikation**

Der Metadatenexport darf nur das OPF-Metadatenelement und den eigenen Record
ändern. XHTML-, NAV-/NCX-Bytes, Spine-Reihenfolge/-IDs, Buchidentität und
bisherige Annotationen/Referenzen bleiben unverändert. Es werden keine neuen
Spine- oder Package-Geschwister vor der bestehenden Spine eingefügt. Das
bewahrt die Grundlage sowohl der Dokument-CFIs als auch ihres Paketanteils.
Alle nicht freigegebenen Archiveinträge sind vor Commit auf Bytegleichheit zu
prüfen; das gilt auch für Autorenressourcen, die beim Kopieren unlesbar wären.

Der Kandidat muss vor Commit sowohl die bekannten Ankerprüfungen als auch
EPUBCheck bestehen. Der bestehende Anker-Commit prüft nicht automatisch die
vollständige EPUB-Konformität oder jede mögliche CFI; dafür sind zusätzliche
gezielte Prüfungen erforderlich. ZIP-`mimetype` muss zuerst und unkomprimiert
geschrieben sein. Datenbankfehler müssen den bisherigen Stand wiederherstellen;
ein wiederholtes Übernehmen identischer Struktur erzeugt keinen neuen Record
oder unnötigen Buchzustand. Verbraucher-Caches werden nach erfolgreichem
Commit mit dem neuen BookState/der Record-Revision aktualisiert.

Für die Implementierung sind folgende fachliche Tests erforderlich:

- EPUB 2/3 mit NAV, NCX und widersprüchlichen Hierarchien; nicht verlinkte
  Eltern, doppelte Ziele, Überschriften ohne ID und mehrere Verse pro Datei.
- IAST/Devanagari, Nummerierungsneustarts und Sondernummern; richtige Auflösung
  von `१-४` sowie Ablehnung von Zahlenbereichen und fehlenden Zielversen.
- Abweichende Editionen, Teilwerke, Vorwort/Kommentar und fehlende Ebenen;
  keine Erzeugung von Versen oder pādas aus vermeintlichem Standardwissen.
- KI-Fixtures mit unbekannten Begriffen, erfundenen Beleg-IDs, unvollständigem
  JSON, ungeprüften Quellen, Abbruch und Antworten auf einen alten Snapshot;
  isolierte Diskussion und Strukturprüfung mit dem vorhandenen Client.
- Roundtrip beider Speicherformen, unbekannte Version und Pfadkollision;
  unveränderte XHTML-/TOC-Bytes, CFI-Paketpfade und Original-Annotationen.
- Metadatencommit mit konkurrierender Quelldatei, wiederholter Übernahme,
  Schreib-/DB-Fehler und Rollback; Schutz bestätigter Korrekturen bei Reanalyse.
- EPUBCheck der erzeugten EPUB-2/3-Kandidaten sowie echter WebEngine-Roundtrip:
  Navigation/Cross-Reference zur Koordinate, Auswahl und Annotation am selben
  CFI vor/nach Übernahme, TTS-SSML und Plaintext mit Original-Highlighting.

Diese Liste enthält die Abnahmekriterien des ursprünglichen Vorschlags. Zum
Zeitpunkt der Untersuchung wurden Quellcode, lokale Codex-Protokollschemas und
EPUB-Spezifikationen geprüft. Builds und Laufzeittests folgten erst bei der
nachstehenden Umsetzung. Das Programm `epubcheck` fehlt im PATH; der installierte
Validator `/usr/share/java/epubcheck.jar` ist verfügbar.

**9. Kleinster Implementierungsumfang**

1. JSON-Modell/Codec, lesende Extraktion und lokale Regeln; an bestehende
   `EPubContainer`-Parserschnittstellen anbinden und Quellbelege erhalten.
2. Zentralen BookServer-Lesezugang ergänzen und bestehende Zielantworten um
   optionale Semantikfelder erweitern.
3. Review-Dialog nach optionalem Import-/Normalisierungsschritt, einschließlich
   manueller Zuordnung und expliziter Metadatenübernahme. Archivhelfer um neue
   Dateien ergänzen; Commit um einen erwarteten Basisstand und gezielte
   Erhaltungsprüfung erweitern. EPUB-2/3-Writer und Validierung anschließen.
4. Vorhandenen Codex-Transport um `outputSchema` und getrennte Sitzungen
   erweitern; Snapshotadapter und strikte Ergebnisprüfung darüber legen.
5. Bestätigte Koordinaten in Navigation, Cross-References und bestehendem
   Sanskrit-TTS-Payload verwenden und die vollständige Benutzerkette prüfen.

Es werden weder ein neuer KI-Provider noch ein zweiter Sprachdienst, eine
zusätzliche Werkdatenbank oder ein eigener TOC-Editor benötigt. Die eigentliche
neue Domänenlogik ist das versionierte Strukturmodell mit belegter Zuordnung.


**Implementierung im Arbeitsbaum**

- `workstructure.h/.cpp` stellt den gemeinsamen JSON-Codec, die lesende
  Quellenaufnahme, lokale Vorschläge, die semantische Prüfung, Koordinaten-
  und Zielauflösung sowie die Vorbereitung eines Metadaten-Kandidaten bereit.
  Das Schema verwendet die stabilen Term-IDs direkt als Ebenen-IDs, z. B.
  `asvasa` und `sloka`; jeder Knoten hat eine explizite `coordinate`-Liste.
  Knoten-IDs entsprechen ihren Quellbeleg-IDs. Die obige Kurzform illustriert
  das Modell und ist kein direkt importierbarer vollständiger Datensatz.
- `workstructurecommit.h/.cpp` besitzt den temporären EPUB-Kandidaten und
  den asynchronen EPUBCheck-Prozess. Der konfigurierte Validator wird genutzt.
  Wenn das übliche alleinige Kommando `epubcheck` fehlt, kann die installierte
  `/usr/share/java/epubcheck.jar` über Java verwendet werden. Individuelle
  Kommandos und ausdrücklich leere Werte bleiben maßgeblich.
- `BookTruthStore::commitCandidateBookFile()` unterstützt einen erwarteten
  Datei-Hash und den Modus für ausschließlich eigene Strukturmetadaten. Eine
  gemeinsame buchbezogene `QLockFile` schützt die drei BookTruthStore-
  Mutationspfade auch zwischen Prozessen. Der neue Commit prüft Quelle,
  Anker, Quellerhaltung und die gültige Struktur vor Veröffentlichung.
- `EPubContainer::cfiForNode()` erzeugt neue Strukturlocators unterhalb der
  XHTML-Wurzel und berücksichtigt die transparenten Arianna-Anker. Bestehende
  Annotations-/Referenz-CFIs werden nicht umgeschrieben. Der Archivhelfer
  unterstützt neue Dateien, prüft jeden Kopier-/Schreibschritt und schreibt
  `mimetype` zuerst und unkomprimiert.
- BookServer bietet die beschriebenen `workStructure`-, `analyze`-, `resolve`-
  und `commit`-Routen. Bestätigte Verse ohne XHTML-ID werden mit ihrem CFI in
  `targetLocations` angeboten. EPUB-Identität und das vorübergehende Alias eines
  schreibgeschützt geöffneten Buchs werden getrennt behandelt.
- `WorkStructureReview` und `WorkStructureDialog.qml` implementieren den
  temporären Review, manuelle Zuordnung, einzelne Bestätigungen, Quellenhinweise,
  unveränderte alte Records und die ausdrückliche Übernahme. Ein eigener
  `CodexClient`-Aufruf nutzt dieselbe Transportimplementierung und Anmeldung
  wie der Reader, mit einer unabhängig besessenen temporären Sitzung. Dies
  hält Abbruch und Dialogschließung vom Reader-Chat getrennt, ohne dessen
  vorhandene Sitzungszuständigkeit umzubauen.
- Der KI-Kontext enthält höchstens 512 Belege: bevorzugt TOC/Überschriften,
  deren Eltern sowie Versbeispiele. Die Begrenzung ist im einsehbaren Kontext
  als `subset` angegeben. Das Ausgabeschema erlaubt nur die tatsächlich
  übermittelten Beleg-IDs. Quellenhinweise bleiben ausdrücklich ungeprüft.
- In den Buchdetails gibt es „Sanskrit-Werkgliederung prüfen“. Unter
  „KI-Assistenz“ kann der Review nach Import/Normalisierung aktiviert werden;
  der Standard ist aus. Keine KI-Anfrage wird automatisch gesendet.
- Trennzeichen aus terminalen Versmarkern werden im Vorschlag vorausgewählt
  und im Review sichtbar bestätigt. Ein belegtes `१-४` aktiviert den Bindestrich;
  normale Zahlenbereiche im Fließtext dienen nicht als Beleg.
- Der Reader bietet für eine gültige Struktur „Zu Werkkoordinate springen“.
  Cross-Reference-Titel verwenden bestätigte semantische Pfade. TTS behandelt
  den tatsächlich belegten terminalen Versmarker als atomaren Abschnitt,
  während Text, UTF-16-Quellmarke und CFI erhalten bleiben. Gleichlautende
  Zahlen im Verstext werden nicht als Marker verwendet. Die Aussprache
  berücksichtigt die bisherige Sprach-/Stimmwahl und den Zahlenschalter.
- Nicht verlinkte TOC-Gruppen können ihren ersten tatsächlich verlinkten
  Unterpunkt als Navigationsanfang verwenden. Diese Ableitung wird im
  Vorschlag ausdrücklich als unsicher angegeben und muss bestätigt werden.
  Verwandte Ebenen dürfen denselben Anfang besitzen; gleiche Ziele für
  unabhängige Koordinaten bleiben ein Fehler.

Die Implementierung benötigt keine neue Datenbanktabelle und verändert keine
Benutzerbücher während der Analyse. Permanente Änderungen werden ausschließlich
nach dem editorischen Übernahmeauftrag veröffentlicht.

**Prüfnachweise der Umsetzung, 14. Oktober 2026**

Der Release-Build liegt separat unter `/tmp/arianna-work-structure-build`.
Die vorhandene Projektkonfiguration wurde nicht umgestellt. Die lokale
KF6-Installation enthält ältere Release-Importdateien neben den aktuellen
Bibliotheken; wie im bestehenden Build wird deshalb
`CMAKE_MAP_IMPORTED_CONFIG_RELEASE=NONE;RELEASE;` verwendet. `arianna` und alle
nachstehend genannten Testprogramme wurden aus dem aktuellen Stand gebaut.

- `arianna-workstructuretest`: 16 Fälle bestanden, kein Fehler und kein Skip.
  Geprüft sind lokale Erkennung, explizite pāda-Eltern, unverlinkte TOC-Gruppen,
  NAV/NCX-Konflikte, erfundene Modellzuordnungen, unbekannte/stale Records,
  EPUB-2/3-Roundtrip mit echtem EPUBCheck, unveränderte Inhalts-/TOC-Bytes und
  CFIs, erhaltene Annotationen, konkurrierende Dateiänderungen und Prozesssperre,
  asynchroner Commit, Validator-Ablehnung ohne Quelldateiänderung und wiederholte
  Übernahme ohne Änderung. BookServer-Tokenprüfung, schreibgeschützte Buchaliase,
  bestätigte CFI-Ziele ohne XHTML-ID und der tatsächliche QML-Review sind enthalten.
  Der WebEngine-Reader navigiert nach `१-४`, extrahiert genau einen belegten Marker
  mit ursprünglicher Quellmarke und übergibt dem simulierten Sprachdienst
  „आश्वास एकम्, श्लोक चत्वारि“. Der gleiche Zahlenwert im Verstext bleibt normaler
  Quelltext; der sichtbare DOM-Text ändert sich nicht.
- Neun weitere CTest-Gruppen bestanden: `codexclienttest`, `readercontexttest`,
  `codexqmltest`, `bookimporttest`, `epubcontainertest`, `bookserverreferencetest`,
  `booktruthstoretest`, `bookstateimporttest`, `readersettingstest`. Darunter sind
  schema-gebundene Modellausgabe, die tatsächlich installierte Codex-CLI mit
  lokalem Provider-Fixture und die bisherige Reader-Diskussion.
- 31 gezielte Sprachtestfälle bestanden (`sanskritNumbers`,
  `sanskritNumberPronunciation`, `referencePronunciation`, `preparedInlineMath`,
  `indexMarks`). Der vorhandene vollständige Sprachtest ist nicht insgesamt grün:
  `viewerControls(epub-controls)` scheitert im Headless-Lauf an der Sichtbarkeit
  des Auswahlmenüs. Derselbe Fehler wurde auch mit dem Reader-/Viewer-Ausgangsstand
  ohne diese Erweiterung nachgewiesen.
- Das gebaute Hauptfenster startete mit eigenem BookServer, freiem lokalen Port,
  temporären Einstellungen und privater D-Bus-Sitzung. Beide Prozesse wurden
  anschließend beendet. `msgfmt --check` und `git diff --check` bestanden.

Die Laufzeittests verwenden ausschließlich temporäre EPUBs und isolierte
Testdatenbanken. Eine externe Modellinferenz, hörbare Ausgabe mit einer echten
Sanskrit-Stimme und eine manuelle Prüfung sämtlicher Dialogdarstellungen wurden
nicht durchgeführt. Die Anwendung wurde nicht installiert.
