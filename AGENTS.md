# Project Directives

- For Arianna QML library zoom, use `MouseArea.onWheel`. Do not use `WheelHandler` for this project.

# Projekt-Direktive: Umgang mit abgeleiteten EPUB-Daten

## Grundprinzip

Arianna behandelt das originale EPUB als unveränderliche Quelle (*Source of Truth*).

Informationen, die aus dem EPUB berechnet, erkannt, interpretiert oder transformiert werden, sind zunächst **abgeleitete Daten** und nicht automatisch Bestandteil des EPUB.

Jede neue Funktion, die solche Informationen erzeugt oder verwendet, muss deren Herkunft, Speicherung, Gültigkeit und Lebenszyklus ausdrücklich berücksichtigen.

## 1. Klassifikation der Daten

Unterscheide mindestens:

**A. Quelldaten**

- Originaler EPUB-Inhalt und dessen Metadaten
- Explizite semantische Auszeichnungen im XHTML
- Autoren- und Herausgeberinformationen

**B. Deterministisch ableitbare Daten**

- Normalisierte Textdarstellungen
- Erkennbare Silbengrenzen
- Guru-Laghu-Folgen nach festgelegten Regeln
- Dokumentstrukturen und Textindizes

**C. Heuristisch oder modellabhängig abgeleitete Daten**

- Automatisch erkannte Sprachen
- Identifizierte Sanskrit-Metren
- OCR-Ergebnisse
- Phonem-Alignments und TTS-Zeitmarken
- Automatisch erkannte Referenzen

**D. Benutzer- und Anwendungsdaten**

- Annotationen und Querverweise
- Manuelle Korrekturen
- Lesepositionen
- Persönliche Einstellungen

Benutzerdaten und manuelle Korrekturen dürfen niemals wie beliebig regenerierbare Cache-Daten behandelt werden.

## 2. Entscheidung über Persistenz

Vor der Implementierung ist festzulegen:

1. Ist die Information bereits explizit im EPUB enthalten?
2. Kann sie aus der Quelle zuverlässig rekonstruiert werden?
3. Wie aufwendig ist die erneute Berechnung?
4. Ist das Ergebnis deterministisch oder modellabhängig?
5. Muss es mehrere Sitzungen überdauern?
6. Ist es buch-, benutzer- oder anwendungsspezifisch?
7. Kann es durch eine Änderung der Quelle ungültig werden?
8. Soll es später in das EPUB exportiert werden können?

Nicht jede ableitbare Information muss dauerhaft gespeichert werden.

## 3. Speicherstrategie

Bevorzuge folgende Abstufung:

| Datentyp | Bevorzugter Speicherort |
| --- | --- |
| Kurzlebige Ergebnisse | Arbeitsspeicher |
| Regenerierbare, aufwendige Berechnungen | BookServer-Cache |
| Dauerhafte buchbezogene Analysen | BookServer-Datenbank |
| Benutzerkorrekturen und Annotationen | Persistente Benutzerdatenbank |
| Autoritative editorische Metadaten | EPUB, durch expliziten Editor-Vorgang |
| Anwendungseinstellungen | KConfig/KConfigXT |

Die bestehenden Datenbankstrukturen sind vor Einführung neuer Speichermechanismen zu prüfen.

## 4. Herkunft und Versionierung

Persistierte Analyseergebnisse sollen, soweit erforderlich, folgende Informationen besitzen:

- Buch- und Dokumentidentität
- Referenz auf den betreffenden Textbereich
- Quelltext- oder Inhalts-Hash
- Typ des Analyseergebnisses
- Version des Analysealgorithmus
- Gegebenenfalls Modellversion
- Zeitpunkt der Berechnung
- Validierungsstatus
- Herkunft: explizit, berechnet oder manuell korrigiert

Dadurch können veraltete Ergebnisse erkannt und gezielt neu berechnet werden.

## 5. Invalidierung

Abgeleitete Daten dürfen nur wiederverwendet werden, wenn ihre Abhängigkeiten noch gültig sind.

Bei Änderungen des zugrunde liegenden Textes, des Algorithmus oder des verwendeten Modells ist zu prüfen, welche Ergebnisse ungültig werden.

Benutzerkorrekturen dürfen bei einer erneuten Analyse nicht stillschweigend überschrieben werden.

## 6. EPUB-Integration

Der BookServer verändert originale EPUB-Dateien nicht automatisch.

Das Zurückschreiben abgeleiteter Informationen in ein EPUB ist ein ausdrücklicher editorischer Vorgang, beispielsweise über Sigil.

Dabei ist zu unterscheiden zwischen:

- der Speicherung eines Analyseergebnisses,
- der Bestätigung dieses Ergebnisses durch den Benutzer,
- der Übernahme als autoritative EPUB-Metadaten.

## 7. API und Komponentenverantwortung

Die Erzeugung, Speicherung und Verwendung abgeleiteter Daten sollen soweit möglich voneinander getrennt bleiben.

Eine Komponente darf nicht allein deshalb eine eigene persistente Datenhaltung einführen, weil sie ein Analyseergebnis benötigt.

Prüfe zuerst, ob vorhandene BookServer-Dienste, Datenbanken oder Caches wiederverwendet werden können.

## 8. Verbindliche Codex-Arbeitsweise

Bei jeder neuen Funktion mit abgeleiteten EPUB-Daten:

1. Datenart und Abhängigkeiten identifizieren.
2. Speicherbedarf und Gültigkeitsdauer bestimmen.
3. Verantwortliche Komponente festlegen.
4. Vorhandene Architektur prüfen.
5. Invalidierungs- und Migrationsstrategie festlegen.
6. Erst danach implementieren.

Für kleinere, ausschließlich temporäre Berechnungen genügt eine kurze dokumentierte Entscheidung.

Bei dauerhafter Speicherung oder neuen Datenbanktabellen ist die Architekturentscheidung ausdrücklich zu begründen.

**Leitprinzip: Persistiere nur, was einen dauerhaften Nutzen besitzt, und berechne nur neu, was tatsächlich ungültig geworden ist.**
