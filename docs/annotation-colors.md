# Hervorhebungsfarben für Bemerkungen

Die Hervorhebungsfarbe gehört zu den dauerhaften Benutzerdaten (Kategorie D),
nicht zu abgeleiteten EPUB-Daten. Sie bleibt im vorhandenen Feld `color` der
Annotation in der Benutzerdatenbank. Es sind keine neue Tabelle, Migration,
Analysecaches oder Buchmetadaten erforderlich. Ihre Gültigkeit folgt der
zugehörigen Bemerkung; eine Neuberechnung aus dem Buch ist nicht möglich.

`AnnotationColorPalette.qml` stellt dieselben 16 Farben in zwei Reihen mit acht
Feldern für PDF-Bemerkungen, normale EPUB-Bemerkungen und HTML-/KI-Bemerkungen
bereit. Vorhandene Farben werden beim Öffnen übernommen; Gelb bleibt der
Standard neuer Bemerkungen. Auswahlmarkierung, Tooltip, zugänglicher Farbname
und Tastaturbedienung ergänzen die Farbflächen. Bestehende Sondermarkierungen
wie Unterstreichen werden beim bloßen Öffnen nicht ersetzt.

PDF-Bemerkungen bleiben vollständig in der vorhandenen Benutzerdatenbank.
EPUB-Bemerkungen verwenden weiterhin ihre bestehenden Anker. Bei einer neuen
EPUB-Hervorhebung werden Farbe und optionaler Bemerkungstext zunächst im Dialog
gewählt; erst dessen Bestätigung legt den Anker mit dem vorhandenen
`AnnotationStore.createAnchoredAnnotation`-Verfahren an. Abbrechen erstellt
weder eine Bemerkung noch einen Anker. Eine Farbänderung einer bestehenden
Bemerkung schreibt ausschließlich deren Datenbankeintrag und aktualisiert den
vorhandenen Reader-Overlay. Sie verändert weder XHTML noch PDF-Datei, Anker,
CFI, Text oder HTML-Bemerkungsinhalt.

HTML-/KI-Bemerkungen übernehmen beim Speichern ausschließlich den bearbeiteten
Farbwert zusätzlich zu ihrem bisherigen Inhalt. Persistierte Identität,
Anker, Erstellungsdatum und Kontextmetadaten bleiben aus dem vorhandenen
Datensatz erhalten. Ungespeicherte Farben gehören nur zum jeweiligen Dialog;
Abbrechen verwirft sie. Es gibt keine neue globale Farbeinstellung.
