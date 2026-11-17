# Zuletzt geschlossene Reader wieder öffnen

`arianna --open-last-closed-book=3` öffnet die drei zuletzt geschlossenen,
noch verfügbaren Bibliotheksbücher. Die Auswahl erfolgt nach Schließzeitpunkt;
geöffnet werden die ausgewählten Bücher danach in der Reihenfolge, in der ihre
damaligen Reader geöffnet wurden. Gespeicherte Lesepositionen werden übernommen.
`--open-last-closed-book 3` funktioniert ebenfalls. Eine positive ganze Zahl ist
erforderlich. Eine explizite Buchdatei hat Vorrang vor den Wiederöffnungsoptionen.
`--open-last-opened-book` funktioniert weiterhin wie bisher.

Ein verdeckter oder für einen Querverweis suspendierter Reader bleibt geöffnet.
Erst das Entfernen/Zerstören der Reader-Seite oder normales Beenden der Anwendung
erfasst das Schließen. Beim gemeinsamen Beenden werden die noch geöffneten Reader
in ihrer Öffnungsreihenfolge erfasst. Absturz und SIGKILL können keinen neuen
Schließzeitpunkt speichern. Wiederholtes Schließen desselben Buchs aktualisiert
seinen Eintrag; es werden verschiedene Bücher wieder geöffnet, keine Duplikate
früherer Sitzungen. Fehlende Dateien werden übersprungen.

## Datenentscheidung und Migration

Dies sind dauerhafte Benutzer-/Anwendungsdaten (Kategorie D), keine Analyse des
EPUB und kein regenerierbarer Cache. Eigentümer ist der Reader; gespeichert wird
in der bestehenden `library.sqlite`, Tabelle `books`:

- `lastClosedTime`: UTC-Schließzeitpunkt mit Millisekunden.
- `lastClosedOpenedTime`: Öffnungszeitpunkt der zugehörigen Reader-Sitzung.

Die zweite Spalte ist erforderlich, weil `lastOpenedTime` durch späteres Öffnen
und Metadaten aktualisiert werden kann. Die beiden neuen Werte werden beim
Schließen gemeinsam mit einem SQL-UPDATE geschrieben. Gleichzeitige Ereignisse
innerhalb eines Readers erhalten aufsteigende Millisekundenwerte, damit ihre
Reihenfolge erhalten bleibt. Für Zeitgleichheit zwischen getrennten Prozessen
dient der Dateipfad als deterministische Sortierung.

Die bestehende Schemaerweiterung fügt beide nullable Spalten hinzu. Alte Bücher
haben zunächst kein bekanntes Schließdatum und werden erst nach dem nächsten
Schließen berücksichtigt. Pro Datei bleibt der letzte Schließvorgang erhalten;
Bibliotheksentfernung entfernt auch diesen Verlauf. Inhaltsänderungen invalidieren
die Benutzerdaten nicht. Es gibt keinen EPUB-Export und keine Inhaltsänderung.
Sitzungen mit `--open-read-only` oder `--no-library-import` werden wie bisher bei
Leseposition und Öffnungsdatum nicht in der Bibliothek protokolliert oder importiert.

## Laufende Sitzungen aktualisieren

```sh
arianna --reopen-already-opened-books
arianna --reopen-already-opened-books --restart-new-bookserver
```

Ohne weitere Öffnungsargumente sind dies reine Steuerbefehle: kein zusätzliches Readerfenster.
Sie erreichen alle laufenden Arianna-Prozesse am Sitzungs-D-Bus. Der Neustart wird
einmal vom aufrufenden Prozess koordiniert; `--restart-new-bookserver` allein
aktualisiert ebenfalls alle laufenden Sitzungen. Die Optionen lassen sich auch
mit `--open-last-closed-book=n` oder einem Buchpfad kombinieren. `--start-new` und
`--bookserver` dürfen nicht damit kombiniert werden.

Der aktive Reader lädt sein Buch an der aktuellen Position neu. Verdeckt liegende
EPUB-Reader bewahren ihre Position und laden beim nächsten Aktivieren neu. Ihre
Seiten bleiben erhalten, somit ändert ein Refresh weder Schließverlauf noch
Öffnungsreihenfolge. Auch transiente EPUBs werden mit ihrer bestehenden Kennung
neu angemeldet, ohne Bibliotheksimport.

### Steuervertrag (Version 1)

Der bestehende prozessbezogene KDBusService erhält das Objekt `/Reader` mit
Interface `org.kde.arianna.Reader1` und Methode `RefreshReaders(requestId: UUID)`.
Rückgabe: Anzahl aktualisierter Reader (>= 0), -1 bei Fehler/ungültiger UUID,
-2 bei laufendem Refresh. Pro Prozess werden die letzten 128 UUID-Ergebnisse
zwischengespeichert; dieselbe UUID führt den Auftrag nicht erneut aus. Eine
positive Antwort bestätigt angestoßenes Neuladen, nicht den abgeschlossenen
Dokumentabruf. HTTP-/Dokumentfehler zeigt weiterhin der bestehende Reader an.

Die vorhandene HTTP-Schnittstelle wird kompatibel ergänzt:

- `GET /server-control`, Header `X-Arianna-Server-Token`: PID und Protokollversion.
- `DELETE /server-control`, gleicher Header: Antwort `state: Stopping`, danach
  geordnetes Stoppen; ein eigenständiger BookServer-Prozess beendet sich.
- `POST /session` akzeptiert optional `sessionToken` (UUID) zur idempotenten
  Wiederanmeldung. Ohne Feld wird wie bisher ein neuer Token erzeugt.
- `POST /read-only-books` akzeptiert optional `identifier` (UUID). Dieselbe
  Kennung/Datei ist wiederholbar; eine andere Datei unter derselben Kennung wird
  mit HTTP 409 abgewiesen. Die vorhandene Sitzungsauthentifizierung bleibt nötig.

Ein gemeinsames `QLockFile` unterhalb von `XDG_RUNTIME_DIR` serialisiert normalen
Serverstart und Neustart. Erst nach bestätigtem Stoppen wird genau ein neuer
Server gestartet. Es gibt keine Prozess-Kills oder wiederholte Startversuche als
Fehlerkompensation. Lock-, Stop-, Start- und D-Bus-Zeitüberschreitungen ergeben
einen Fehlerstatus. Bereits laufende ältere Arianna-/BookServer-Prozesse ohne
diese Steueroperationen müssen nach dem Versionswechsel einmal beendet werden.

## Reproduzierbare Prüfung

Im separaten Testbuild:

```sh
ninja -C build-ai-notes readerhistorytest readersettingstest nolibraryimporttest bookservercachetest arianna
ctest --test-dir build-ai-notes -R '^arianna-(readerhistorytest|readercontrolintegration|nolibraryimporttest|readersettingstest|readersettingsmigrationtest|bookservercachetest)$' --output-on-failure
```

Der Python-Integrationstest startet selbst einen privaten D-Bus und verwendet
ausschließlich temporäre Daten, Konfigurationen, EPUBs und einen eigenen Port.
Ein bestimmtes Release-Binary lässt sich zusätzlich so testen:

```sh
python3 src/autotests/readercontrolintegration.py --binary build/bin/arianna
```

Geprüft werden Schließen und Neustart, Auswahl/Reihenfolge, unveränderte
Lesepositionen, Datenbankmigration, fehlende Dateien, wiederholte UUIDs,
mehrere Prozesse, wirklicher Wechsel der Server-PID, mehrere Reader in einem
Fenster sowie externe EPUBs ohne Bibliotheksimport. Die Qt-Prüfung verwendet
echte WebEngine-Readerseiten mit dem Offscreen-Backend; sie ersetzt keinen
manuellen Test der installierten Anwendung auf dem Desktop.
