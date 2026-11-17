# PDF-Passwörter und KWallet

KWallet speichert die von Arianna mit „Save password in KWallet“ bestätigten
PDF-Passwörter über Reader- und Anwendungsneustarts hinweg. Eine geschlossene
Wallet kann dabei nach ihrer Entsperrung oder einer Zugriffsfreigabe fragen.
Ohne aktiviertes Speichern gilt das Passwort ausschließlich für die Reader-Sitzung.

## Korrektur der Dokumentzuordnung

Der PDF-Reader erhält vom BookServer eine URL mit einem Sitzungstoken. Bei PDFs
mit automatisch erzeugtem `arianna-pdf-…`-Bezeichner wurde bisher diese komplette
URL als Grundlage des KWallet-Schlüssels verwendet. Ein neuer Sitzungstoken
führte dadurch zu einem anderen Schlüssel für dieselbe Datei.

Der Passwortdienst erhält jetzt die URL der ursprünglichen PDF-Datei aus
`filename`. Der vorhandene Hintergrunddienst bildet bei automatisch erzeugten
Buchkennungen einen SHA-256-Schlüssel über die unveränderten Originaldateibytes.
Temporäre Downloads, Wasserzeichenkopien, BookServer-Adresse und Sitzungstoken
bestimmen diese Zuordnung nicht. Unveränderte Kopien und umbenannte PDFs werden
wiedererkannt. Bei expliziten Buchkennungen bleibt die vorhandene Zuordnung über
die Buchkennung erhalten. Ein gefundenes Passwort wird vor Verwendung durch den
PDF-Parser geprüft; ein falsches Passwort erfordert weiterhin Benutzereingabe.

Passwörter sind geschützte Benutzerdaten (Kategorie D), keine regenerierbaren
EPUB-/PDF-Analysedaten. Zuständig bleibt `PdfPasswordStore` mit KWallet, Ordner
`arianna`, Schlüssel `pdf-v1:…`. Es gibt keine zusätzliche Datenbank, Migration
des Bibliotheksschemas oder Passwortspeicherung im PDF. Während der Sitzung
bleibt das validierte Passwort nur im Speicher. Inhaltsänderungen können bei
der nächsten Sitzung zu einem anderen Dateischlüssel führen.

## Bereits gespeicherte Einträge

Schlüssel mit derselben bisherigen Identitätsgrundlage bleiben verwendbar.
Einträge aus dem fehlerhaften URL-Verfahren enthalten nur einen Hash; die alte
Dateizuordnung und der damalige Sitzungstoken lassen sich daraus nicht
zuverlässig rekonstruieren. Diese Einträge werden weder durchsucht noch gelöscht.
Betroffene Passwörter müssen nach dem Update einmal mit aktiviertem KWallet-Haken
erneut gespeichert werden. Danach funktioniert das erneute Öffnen unabhängig vom
Reader-Sitzungstoken.

Im KWalletManager können einzelne `pdf-v1:…`-Einträge im Ordner `arianna` gelöscht
werden. Das verändert keine PDF; bei erneutem Bedarf muss das Passwort wieder
eingegeben werden. Da alte und gültige Schlüssel dieselbe Form haben, empfiehlt
sich keine pauschale Löschung. Eine automatische Bereinigung in Ariannas Wartung
ist nicht Bestandteil dieser Korrektur.

## Schlossanzeige auf PDF-Covern

Bibliothekskacheln (einschließlich gruppierter und thematischer Ansichten) und
Buchdetails prüfen vorhandene KWallet-Passwörter gegen die ursprüngliche PDF.
Ein erfolgreich geöffnetes, verschlüsseltes Dokument ergibt ein offenes Schloss;
fehlende, falsche, nicht zugängliche oder noch ungeprüfte Passwörter ein
geschlossenes. Das gilt sowohl für den Cover-Ersatz als auch für das kleine
Schloss auf vorhandenen Covern. Tooltip und Barrierefreiheit beschreiben den
Prüfzustand. Die Prüfung darf mit Freigabe des Benutzers eine KWallet-Entsperrung
oder Zugriffsfreigabe anfordern.

Das Prüfergebnis ist deterministisch ableitbare, kurzlebige Information
(Kategorie B). Zuständig bleibt `PdfPasswordStore`: Hashberechnung, KWallet und
PDF-Parser laufen im vorhandenen Hintergrundthread. Die Cover-Anzeige erhält nur
den Wahrheitswert, niemals das Passwort. Ein auf 256 Ergebnisse begrenzter
Arbeitsspeicher-Cache vermeidet erneutes Hashen und Parsen unveränderter Dateien;
es gibt keine neue Datenbank oder Migration. Seine Gültigkeit hängt von
Originalpfad, Buchkennung, Dateigröße, Änderungs-/Metadatenzeit und dem
KWallet-Zustand ab. Dateiüberwachung und Wallet-Signale invalidieren Ergebnisse;
laufende Aufträge werden verworfen. Änderungen während der Prüfung dürfen kein
positives Ergebnis ergeben. Bei Ordneränderungen und erneutem Aktivieren der
Anwendung wird bei offener Wallet nachgeprüft. Eine vom Benutzer geschlossene
Wallet wird dadurch nicht automatisch wieder geöffnet.
Nach verweigertem Zugriff lösen automatische Aktualisierungen keine erneute
Zugriffsabfrage aus. Die erstmalige Anzeige einer neuen Kachel darf erneut prüfen.

Ungeprüfte Passwörter werden weder gelöscht noch verändert. Die PDF wird nur
gelesen. Versteckte Kacheln prüfen nicht weiter; wiederverwendete Kacheln können
keine verspätete Antwort des vorherigen Dokuments übernehmen.

## Prüfung

```sh
ninja -C build-ai-notes pdfpasswordtest bookformatstest pdflinkannotationtest
ctest --test-dir build-ai-notes -R '^arianna-(pdfpasswordtest|bookformatstest|pdflinkannotationtest)$' --output-on-failure
```

Der Regressionstest verwendet echte verschlüsselte PDFs, BookServer, vollständige
QML-Readerseiten, getrennte Readerprozesse und die echte KF6-KWallet-Clientbibliothek.
Ein KWallet-Testdienst auf einem privaten D-Bus enthält ausschließlich synthetische
Testpasswörter. Geprüft werden wechselnde Sitzungstokens, umbenannte Kopien,
unveränderte PDF-Dateien sowie falsche gespeicherte Passwörter. Eine Kontrollprobe
mit der alten URL-Zuordnung reproduziert die erneute Passwortabfrage.
Die persönliche Wallet und ihre Zugriffsregeln werden dabei nicht verwendet.
Zusätzlich lädt der Test echte QML-Cover mit und ohne Vorschaubild und die
Buchdetailseite und prüft
Schlosswechsel, ungültige/gelöschte Einträge, erlaubtes Entsperren, verweigerten
Zugriff, Wallet-Schließen und -Öffnen, ersetzte/fehlende Dateien sowie das
Ausblenden und Wiederverwenden einer Kachel während eines laufenden Auftrags.
