# Verschiebbares Fußnotenfenster

Fußnoten, Endnoten und die bereits erkannten weiteren Verweistypen werden in
einem nicht modalen Qt-Quick-Fenster mit KDE/Kirigami-Steuerelementen angezeigt.
Die native Titelleiste übernimmt das Verschieben, der Fensterrahmen die
Größenänderung. Der Leser bleibt bedienbar.

Kurze Fußnoten passen die Fensterhöhe an ihren gerenderten Inhalt an, einschließlich
Bildern, Formeln, Lesezoom und gegebenenfalls Navigationsknopf. Die automatische
Höhe bleibt auf die bisherige Standardhöhe von 26 Rastereinheiten begrenzt;
längere Fußnoten werden weiterhin gescrollt. Breite und Position bleiben beim
Wechsel der Fußnote erhalten. Manuelles Vergrößern der Höhe bleibt möglich;
solange Inhalt, Breite und Lesezoom unverändert sind, wird es nicht zurückgesetzt.

## Daten und Lebenszyklus

Der Fußnotenausschnitt ist eine deterministische, kurzlebige Präsentationskopie
der von Foliate geladenen EPUB-Quelle. Er bleibt ausschließlich im Arbeitsspeicher
des Lesers und des Vorschaufensters. Weder Original-EPUB noch BookServer-Datenbank
oder Benutzerdaten werden geändert. Es gibt keine neue Persistenz oder Migration.

Die bestehende Foliate-Ansicht hält die Blob-Ressourcen während der Anzeige am
Leben. MathJax wird vor dem Erstellen der Kopie über die bestehende Renderbarriere
abgewartet; Schrift, Buch-CSS, Bilder und bereits erzeugte Formeldarstellung werden
übernommen. Die Vorschau teilt nur das Ressourcenprofil des Lesers und besitzt
keinen privilegierten WebChannel; Buchskripte sind dort abgeschaltet.

Der Inhalt verwendet dieselben Buch- und Leser-Stylesheets wie der Reader,
einschließlich Normal-, Invers- und Systemfarben, Bildinversion sowie dem
konfigurierten Hintergrundbild mit derselben Überlagerung und flächendeckender
Skalierung. Das Bild liegt als feste Ebene hinter dem Text. Themenwechsel
aktualisieren nur die Stylesheets eines offenen Fensters; Inhalt, Verweiskennung,
Fenstergeometrie und Scrollposition bleiben erhalten. Auch diese Darstellung ist
rein temporär und führt keine neuen Einstellungen oder gespeicherten Daten ein.
Der von Chromium gezeichnete Scrollbalken verwendet abgerundete Griffe aus der
jeweiligen Lesefarbe, eine an den Leserhintergrund angepasste Laufbahn und
abgestufte Hervorhebung beim Überfahren und Ziehen. Er wechselt zusammen mit dem
Inhalt das Thema.

Links werden anhand des ursprünglichen EPUB-Abschnitts aufgelöst und als indizierte
Aktionen an genau den zugehörigen Leser zurückgereicht. Eine sitzungsbezogene
Anfragekennung verhindert, dass alte Schließ- oder Navigationsaktionen eine neu
geöffnete Fußnote bedienen. Schließen, Buchwechsel, Neuladen und Verlassen der
Leseransicht geben die Vorschau und ihre Ressourcen frei. Fensterposition und
Größe gelten nur für die aktuelle Leserinstanz.
