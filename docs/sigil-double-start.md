<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Doppelstart bei „Edit Selected Text“ (14. Oktober 2026)

## Ursache vor der Korrektur

Der vollständige Aufrufpfad lautet:

1. `EpubViewerPage.qml`: Kontextmenü → `editBook(backend.selection.text)`.
2. `EditorSessionStore::createOrReuseEditorSession`: unveränderte EPUB-Arbeitskopie
   bzw. Wiederverwendung der aktiven Sitzung.
3. `ExternalProcess::startEditorSession`: erkennt `sigil` und normalisiert
   `--select-text=<text>`; `SigilNavigation::navigate` startet die Dienstsuche.
4. `ListNames` → kein `org.sigil_ebook.Sigil` → `ListActivatableNames` →
   `StartServiceByName`.
5. Die installierte Datei
   `/usr/local/share/dbus-1/services/org.sigil_ebook.Sigil.service` enthält
   `Exec="/usr/local/bin/sigil" --dbus-service`. Der Shellstarter verwendet
   `exec /usr/local/lib/sigil/sigil "$@"`, erzeugt also keinen zweiten Editor.
6. Sigils `main.cpp::GetMainWindow` entfernt `--dbus-service` und erzeugt
   mangels EPUB-Argument ein leeres `MainWindow`. Danach veröffentlicht
   `EditorDBusService::Start` `/Editor`, Interface `org.sigil_ebook.Editor1`.
7. `GetNameOwner` → `GetConnectionUnixProcessID` → `GetCapabilities` →
   `NavigateToText(workingCopyPath, text)`.
8. Sigils Protokoll 1 meldet `openBook=false` und lehnt die ungeöffnete
   Arbeitskopie mit `Error.BookNotOpen` ab.
9. `ExternalProcess::handleSigilNavigation` fällt daraufhin auf
   `launchEditorSession` → `start` → `QProcess::start` mit EPUB und Text zurück.
   Dieser zweite Prozess öffnet das richtige Buch und selektiert den Text.

Damit sind zwei ausdrücklich initiierte Starts im Client vorhanden: die
argumentlose D-Bus-Aktivierung und der anschließende CLI-Start. Weder
`QDesktopServices` noch KIO gehören zu diesem Aufrufpfad. Auch die QML-Aktionen
rufen `editBook` nur einmal auf. `activateWindowForFile` startet lediglich den
Fensteraktivierungshelfer; die PDF- und sonstigen `startDetached`-Aktionen sind
separate Aufrufpfade. Alle direkten Editor-D-Bus-Nachrichten verwenden bereits
`setAutoStartService(false)`; implizite Aktivierung ist nicht die Ursache.

Weitere problematische Fälle: Ein Fehler oder Timeout von `StartServiceByName`
kann trotz laufendem Start zum CLI-Fallback führen. Die bisherige Sperre gegen
doppelte Anfragen verhindert dies innerhalb eines ausstehenden Auftrags nicht.

## Begrenzung des vorhandenen Vertrags

Ein vorhandener Protokoll-1-Dienst kann ein anderes EPUB nicht öffnen.
`BookNotOpen` durch einen weiteren Prozessstart zu reparieren verletzt die
Anforderung, den vorhandenen Sigil-Prozess zu verwenden. Dazu ist eine kleine,
explizite Erweiterung des Sigil-Dienstes erforderlich; `NavigateToText` behält
seine bisherige Bedeutung. Arianna muss ältere Dienste bei diesem Fehler
erhalten und den Fehler melden.

## Datenentscheidung

Die Textauswahl und der Start-/Bereitschaftszustand sind kurzlebige
Anwendungsdaten im Arbeitsspeicher. Die bestehende `EditorSessionStore`-Arbeitskopie,
Sitzungsdaten und Dateiüberwachung werden weiterverwendet. Keine neue Datenbank,
keine Analyse-Caches und keine Änderung am originalen EPUB; keine Migration.
Abbruch oder Prozessende beendet ausstehende Bereitschaftsabfragen.

## Laufzeitnachweis vor der Korrektur

Reproduktion mit dem installierten Sigil auf einem privaten Bus, isolierten
Einstellungen und einem Test-EPUB. `strace -f -tt -e trace=execve` erfasste
Aktivierungsdaemon und Arianna-Testclient getrennt. Der Client verwendet die
produktiven Klassen `ExternalProcess` und `SigilNavigation`.

| Zeit (UTC) | PID | Aufrufpfad | Argumente |
| --- | --- | --- | --- |
| 06:10:32.877 | 3000151 (Client) | `SigilNavigation::call` → `StartServiceByName` | `org.sigil_ebook.Sigil`, `0` |
| 06:10:32.881 | 3000154 | D-Bus-Aktivierung → installierter Shellstarter → `exec` | `--dbus-service` |
| 06:10:33.859 | 3000151 (Client) | `NavigateToText` an Besitzer `:1.1`, Editor-PID 3000154 | Test-EPUB, `नित्यो नित्यानां` |
| 06:10:33.865 | 3000213 | `ExternalProcess::start` → `QProcess` → installierter Shellstarter → `exec` | Test-EPUB, `--select-text=नित्यो नित्यानां` |

Beide Editor-`execve`-Aufrufe waren erfolgreich. Aufrufpfad, Dienst, Besitzer,
PID und vollständige Argumente stehen unter
`build-release/sigil-double-start/{client-before.log,activation-before.trace,cli-before.trace,reproduction.log}`.
Die `strace`-Zeitangaben sind lokale Zeit (Europe/Berlin); die Clientlogs enthalten
explizite UTC-Zeitstempel mit Millisekunden.

## Gezielte Korrektur

- Kein `ListActivatableNames`/`StartServiceByName` mehr in diesem Pfad.
  Alle Editor-D-Bus-Aufrufe bleiben ohne automatische Aktivierung.
- Vorhandenen Besitzer und PID asynchron auflösen, Fähigkeiten und Bereitschaft
  abfragen; weitere Aufrufe bleiben an diesen eindeutigen Besitzer gebunden.
- Fehlt der Besitzer, unter Linux zuerst nach einem bereits startenden
  Sigil-Prozess auf demselben Session Bus suchen (`/proc`, ausführbare Datei
  `sigil`, übereinstimmende Busadresse). Ein solcher Prozess wird abgewartet.
- Andernfalls genau einen `QProcess`-Start mit Arbeitskopie und
  `--select-text=<text>` auslösen. Danach Registrierung, Editorobjekt und
  Bereitschaft asynchron bis zu 15 Sekunden abwarten. Die bereits per CLI
  übergebenen EPUB-/Textargumente werden **nicht** nochmals per IPC gesendet.
- Bei erweitertem Sigil zusätzlich `navigationPending` abwarten, damit eine
  nachfolgende Textauswahl die noch laufende CLI-Auswahl nicht unterbricht.
- Identische ausstehende Anfragen zusammenfassen; andere Textpositionen derselben
  Sitzung in Reihenfolge abarbeiten. Andere Sitzungen währenddessen ablehnen.
- `BookNotOpen`, Ablehnungen, fehlende Fähigkeiten, Bereitschaftstimeout,
  `OpenFailed`, verlorene Antworten und Prozessfehler melden; keinen Ersatzstart
  initiieren. Bereits bekannte Prozessidentitäten bei Abfragefehlern erhalten.
- PID, UTC-Zeitstempel, Programm/Argumente, Dienst/Owner, Methode und Aufrufpfad
  werden bei Prozessstarts und IPC-Abfragen protokolliert.

Sigils vorhandener Dienst wurde dazu additiv um
`OpenBookAndNavigate(QString bookPath, QString text) -> bool` erweitert und meldet
die Methode ausdrücklich in `methods` zusammen mit `openBook=true`.
`NavigateToText` behält seinen bestehenden Vertrag. Ein ungeöffnetes EPUB wird
in einem neuen Fenster **desselben Prozesses** geöffnet; bestehende/ungespeicherte
Bücher werden nicht ersetzt. Ein bereits geöffnetes EPUB wird wiederverwendet.
Eine reservierte verzögerte Antwort verhindert paralleles Öffnen während
reentrantem Laden. Die Antwort erfolgt nach tatsächlicher Textauswahl.
Fehlgeschlagenes Öffnen liefert `OpenFailed`. Ein fehlgeschlagener CLI-Textauftrag
beendet Sigil mit Fehler statt ein leeres Dokument zu hinterlassen.

Ältere installierte Dienste können ungeöffnete Bücher weiterhin nicht per IPC
öffnen. Arianna meldet dort `book-not-open` und erhält den Prozess. Für das volle
Verhalten bei einem bereits laufenden Sigil muss auch der erweiterte Sigil-Build
installiert sein; eine Systeminstallation gehört nicht zu dieser Änderung.

## Validierung am 14. Oktober 2026

- Regressionstest: 46 QtTest-Fälle bestanden, 0 Fehler, 0 übersprungen.
  Geschlossener/laufender Editor, identische und unterschiedliche schnelle
  Anfragen, verzögerte Registrierung/Editorobjekte/Bereitschaft/CLI-Auswahl,
  manuell startender Sigil-Prozess, Startfehler, Öffnungsfehler, fehlende
  Registrierung, inkompatible Dienste, `false`, Busy/AmbiguousBook,
  IPC-Timeout/falsche Antwort und Sitzungs-/Dateiüberwachungslebenszyklus.
- Echtes installiertes Sigil: Kaltstart trotz vorhandener Aktivierungsdatei
  genau ein Editor-`execve`; Bereitschaft abgewartet, keine Aktivierung und
  keine zweite Textnavigation. Log: `cold-installed-after.log`/`.trace`.
- Echter erweiterter Sigil-Build: neues und bereits geöffnetes Test-EPUB,
  IAST/Anführungszeichen, mehrzeilige Auswahl, fehlender Treffer und fehlendes
  EPUB über den produktiven Arianna-Client. Besitzer/PID bleiben gleich, kein
  weiterer Editorstart, ein Öffnen-/Navigationsauftrag je Anforderung.
  Logs: `live-after.log`, `running-*-after.log`/`.trace`.
- Reale Kaltstartfehler: fehlendes EPUB und fehlendes Editorprogramm enden
  mit `start-failed`, ohne leeres Editorfenster, veröffentlichten Dienst oder
  Ersatzstart. Logs: `cold-open-failed-after.log`/`.trace`,
  `cold-start-failed-after.log`/`.trace`.
- Sigils bestehender Integrationstest mit echten MainWindow-/WebEngine-Editoren
  bestand einschließlich tatsächlich ausgewähltem Text, Wiederverwendung des
  neuen Fensters, Öffnungsfehler ohne leeres Fenster, Quellen-/Undo-Prüfung und
  bytegleichen EPUBs. Log: `/tmp/sigil-double-start-integration.log`.
- Reguläre Builds von Arianna und Sigil erfolgreich
  (`build-release/sigil-double-start-build.log`, `/tmp/sigil-double-start-build.log`).
  Eine unabhängige Syntaxänderung in `workstructure.cpp` blockierte den
  Arianna-Build zwischenzeitlich; diese Datei wurde bei der Sigil-Korrektur nicht
  geändert. Nach deren Korrektur im Arbeitsbereich bestand der reguläre Build.
  Der bestehende Release-Cache steht inzwischen auf `BUILD_TESTING=OFF`.
  Die finalen `ExternalProcess`-/`SigilNavigation`-/Testdateien wurden zusätzlich
  separat mit den vorhandenen Buildbefehlen kompiliert und gegen die übrigen
  bestehenden Buildobjekte verlinkt (`sigil-double-start-focused-build.log`).
- Finale fokussierte Tests: `tests-current.log`; Wiederholung mit
  `python3 build-release/sigil-double-start/link-focused.py` und
  `dbus-run-session -- build-release/sigil-double-start/sigilnavigationtest-current`.
  Private D-Bus-/WebEngine-/strace-Prüfungen liefen außerhalb der Socket- und
  ptrace-beschränkten Sandbox. Keine Systeminstallation oder manueller Desktoptest.
