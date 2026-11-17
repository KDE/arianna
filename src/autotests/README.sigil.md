<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Sigil-Textnavigation über den Session Bus

## Bestandsaufnahme

Alle EPUB-Aufrufe, einschließlich Textstellen, Ankern und Wartungsaktionen,
verwenden den einzigen konfigurierten EPUB-Editor (`editorCommand`, Vorgabe
`sigil`). In den Einstellungen gibt es dafür ein Befehlsfeld mit Dateiauswahl.
Der gesonderte Textstellenbefehl und der ältere Pfad-Fallback entfallen;
vorhandene Werte für `editorCommand` bleiben erhalten. Die PDF-Konfiguration
bleibt eigenständig. `EditorSessionStore` kopiert das maßgebliche EPUB unverändert
nach
`CacheLocation/editor-sessions/<sessionId>/`, prüft den Dateihash und schreibt
das bestehende Sitzungsmanifest. Eine aktive Sitzung derselben Arianna-Instanz
wird jetzt tatsächlich wiederverwendet; fremde und aufgegebene Arbeitskopien
unterliegen weiterhin den bisherigen Schutzprüfungen.

Der CLI-Aufruf verwendet `QProcess` mit einer Argumentliste. Im untersuchten
Sigil-Checkout `/home/markus/Projekte/Sigil` akzeptiert `src/main.cpp` den
Parameter **`--select-text=<text>`**. Dies wurde für den Fallback bestätigt.
Die bisherigen Argumente anderer Editoren, einschließlich Calibre, bleiben
erhalten. Sigil erkennt weitere Instanzen über `QSharedMemory`; der untersuchte
Linux-Startpfad erstellt dabei ein neues Hauptfenster und bietet keine
verlässliche Weiterleitung der Textnavigation an eine vorhandene Arbeitskopie.
Der aktuelle Checkout implementiert inzwischen `GetCapabilities()`,
`NavigateToText()` und eine Aktivierungsdatei für `org.sigil_ebook.Sigil`.
Nur der erste registrierende Prozess veröffentlicht diesen festen Dienstnamen;
weitere lokale Sigil-Prozesse sind darüber nicht adressierbar.

Qt mindestens 6.8 und KF mindestens 6.12 waren bereits erforderlich;
`KF6::DBusAddons` war bereits eingebunden. Qt6 DBus wird nun zusätzlich explizit
gesucht und als `Qt6::DBus` verlinkt. Das Flatpak-Manifest erlaubt den Zugriff
auf den konkreten Dienst `org.sigil_ebook.Sigil`.

## Ablauf nach der Doppelstartkorrektur vom 14. Oktober 2026

Ursache, vollständiger Aufrufpfad und PID-/Zeit-/Argumentprotokolle stehen in
[docs/sigil-double-start.md](../../docs/sigil-double-start.md). Die frühere
argumentlose D-Bus-Aktivierung erzeugte ein leeres Fenster; der anschließende
CLI-Fallback nach `BookNotOpen` startete einen zweiten Editor.

`SigilNavigation` aktiviert keinen Dienst mehr. Der Ablauf ist:

1. `ListNames` → vorhandenen Besitzer mit `GetNameOwner` und
   `GetConnectionUnixProcessID` auflösen; alle Editoraufrufe an diese eindeutige
   Adresse binden und automatische Aktivierung ausschalten.
2. `GetCapabilities` verlangt Protokoll 1, `NavigateToText`, `navigateToText=true`
   und boolesches `ready`. Bereitschaft und spät erscheinende Editorobjekte
   asynchron abwarten (15 Sekunden Budget, 100 ms Polling, 3 Sekunden je Abfrage).
3. Bei erweitertem Dienst (`openBook=true` und `OpenBookAndNavigate` in `methods`)
   EPUB und Text mit **einem** Öffnen-/Navigationsauftrag übergeben. Ältere Dienste
   verwenden `NavigateToText` nur für bereits geöffnete Bücher.
4. Ohne veröffentlichten Dienst unter Linux zuerst nach einem schon startenden
   `sigil`-Prozess auf demselben Session Bus suchen und dessen Registrierung
   abwarten. Andernfalls den konfigurierten Editor genau einmal mit Arbeitskopie
   und `--select-text=<text>` starten. Danach nur Registrierung/Bereitschaft
   prüfen; die bereits per CLI übergebene Anforderung nicht per IPC wiederholen.
5. Identische ausstehende Anfragen zusammenfassen. Unterschiedliche Textauswahlen
   derselben Sitzung in Reihenfolge abarbeiten. Der erweiterte Dienst meldet
   `navigationPending`, damit nachfolgende Auswahlen auf die CLI-Suche warten.

```text
Session Bus: org.sigil_ebook.Sigil
Objekt: /Editor
Interface: org.sigil_ebook.Editor1
GetCapabilities() -> a{sv}
NavigateToText(s bookPath, s text) -> b found
OpenBookAndNavigate(s bookPath, s text) -> b found (ausdrücklich beworbene Erweiterung)
```

`found=false`, `BookNotOpen` alter Dienste, abgewiesene oder inkompatible
Schnittstellen, Öffnungsfehler, Bereitschaftstimeout und IPC-Fehler erzeugen
keinen weiteren Prozessstart. Startfehler beenden den ausstehenden Auftrag ohne
Wiederholung. Die Antwort auf eine Navigation darf 18 Sekunden benötigen, da
Sigil seiner Suche 15 Sekunden gibt. Unicode, Zeilenumbrüche, Anführungszeichen
und Shell-Sonderzeichen bleiben QString-/QProcess-Argumentdaten.

Die vorhandene Arbeitskopie, Dateiüberwachung, Prozessidentitätsprüfung und
Sitzungsmanifest-Integration bleiben zuständig. UTC-Zeit, PID, Programm und
Argumente sowie Dienst, Owner, Methode und Aufrufpfad werden protokolliert;
`editorSessionNavigationFinished` meldet das endgültige Ergebnis. `started`
bestätigt Dienstbereitschaft nach CLI-Übergabe, `found`/`not-found` den jeweiligen
abgeschlossenen IPC-Suchlauf.

Für das Öffnen eines anderen EPUBs im **bereits laufenden** Prozess ist der
entsprechend erweiterte Sigil-Build nötig. Das ältere installierte Sigil wird
bei `BookNotOpen` erhalten und meldet den Fehler. Der korrigierte Kaltstart
funktioniert auch mit diesem älteren installierten Sigil ohne Doppelstart.

## Tests ausführen

```sh
cmake --build build --target arianna sigilnavigationtest librarymaintenancetest -j 6
ctest --test-dir build -R '^arianna-sigilnavigationtest$' --output-on-failure
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software build/bin/librarymaintenancetest \
    cacheSessionsBlockDuplicatesAndRecoverLegacyCopies \
    cacheCopiesStayAvailableUntilAllImportChecksFinish \
    legacyCfiRemarkWithTextOpensEditorAtPassage
```

CTest startet für den D-Bus-Test automatisch `dbus-run-session`. Ein
nachgebildeter Dienst läuft auf einem zweiten Busanschluss beziehungsweise
in einem separaten Prozess. Der CLI-Fallback startet einen echten Testprozess,
der seine unveränderten Argumente in JSON aufzeichnet. Die Tests benötigen
`dbus-run-session` und `/usr/bin/python3`.

Die aktuellen 46 Testfälle decken richtige/falsche Arbeitskopien,
`found=false`, inkompatible Fähigkeiten, fehlende Schnittstellen, Zugriffsfehler,
fehlenden Session Bus, Start-/Öffnungsfehler, Timeouts und ungültige Antworten ab.
Zusätzlich werden identische und unterschiedliche schnelle Anforderungen,
Kaltstart ohne Aktivierung trotz vorhandener Aktivierungsdatei, verzögerte
Registrierung/Bereitschaft/Editorobjekte, laufende CLI-Auswahl, ein schon manuell
startender Prozess, Besitzerwechsel, Abbruch und die vorhandene
Sitzungs-/Dateiüberwachung geprüft. Der Text enthält Devanāgarī, IAST,
Zeilenumbrüche, Anführungszeichen und Shell-Sonderzeichen.

Die abschließenden Build- und echten Editorprüfungen vom 14. Oktober stehen in
[docs/sigil-double-start.md](../../docs/sigil-double-start.md). Der reguläre
Arianna-Build bestand nach Behebung einer unabhängigen Syntaxänderung im
Arbeitsbereich. Der finale QtTest-Client wurde auch separat gebaut, da der
bestehende Release-Cache inzwischen `BUILD_TESTING=OFF` verwendet. Die folgenden
Prüfungen vom 11. Oktober dokumentieren den damaligen Stand.

## Prüfung am 11. Oktober 2026

- Arianna und die betroffenen Testprogramme wurden mit Qt 6.10.2 und GCC 15.2 gebaut.
- `sigilnavigationtest`: 40 bestanden einschließlich Testaufbau/-abbau,
  0 Fehler, 0 übersprungen; Laufzeit etwa 18 Sekunden. Das vollständige QtTest-Log
  liegt in `build-release/sigil-activation-qt.log`.
- `booktruthstoretest`: 19 bestanden; `bookstateimporttest`: 11 bestanden.
  Die drei gezielten Arbeitskopien-/Sitzungs-/QML-Aufruftests in
  `librarymaintenancetest` bestanden ebenfalls (5 einschließlich Aufbau/Abbau).
  Die vier gezielten Referenzeditor-/Reimporttests in `bookreferencechecktest`
  bestanden alle Datenfälle (10 einschließlich Aufbau/Abbau).
- Der ursprüngliche CLI-Test gegen Sigils `TestSelectTextMainWindow.cpp`
  bestand alle neun Fälle: normaler Start, tatsächliche Prozessargumente,
  wörtliche Regex-Zeichen, Unicode in einem anderen Kapitel, Option nach dem
  Dateinamen, Zeilenumbrüche, äußere Leerzeichen, kein Treffer und leerer Text.
  Sigil-Dateien wurden dabei nur gelesen.
- D-Bus und Qt WebEngine wurden mit isolierten Einstellungen außerhalb der
  Sandbox geprüft, weil die Sandbox lokale Unix-Sockets blockiert.
- `git diff --check`, die Formatprüfung der neuen C++-Dateien sowie der
  geänderten `ExternalProcess`-Dateien und die JSON-Prüfung des Flatpak-Manifests
  bestanden.
- Der finale Arianna-Client wurde zusätzlich gegen den echten vorhandenen
  Sigil-Build auf einem privaten Bus geprüft: Fähigkeitenabfrage, Wiederverwendung
  des laufenden Besitzers, Devanāgarī, IAST mit beiden Arten von Anführungszeichen,
  mehrzeiliger Text, kein Treffer, falscher Buchpfad und echte Kaltaktivierung
  über eine lokale `.service`-Datei bestanden. Nach Kaltaktivierung lieferte das
  ungeöffnete EPUB erwartungsgemäß `BookNotOpen`. Beide Test-EPUBs blieben
  bytegleich. Alle Einstellungen, Testbücher und Aktivierungsdateien liegen
  unter `build-release/sigil-live-check/`; das Ergebnis steht in
  `build-release/sigil-live-check/result.log`. Kein manueller Desktoptest und
  keine Systeminstallation wurden durchgeführt.
- Nach der Vereinheitlichung des EPUB-Editorbefehls wurde der vollständige
  Release-Build erneut erfolgreich ausgeführt. Die gezielten Wartungstests
  bestanden mit 5, die Referenz-/Reimporttests mit 15 und der bestehende
  QML-Einstellungstest mit 3 Fällen einschließlich Aufbau/Abbau. Der
  Sigil-D-Bus-Test bestand ebenfalls erneut. Die Logs liegen unter
  `build-release/single-editor-*.log`.

## Release-Linkfehler durch veraltete KF6-Exportdateien

Die auf diesem Rechner installierten KF6-CMake-Exporte enthalten sowohl
aktuelle `Targets-none.cmake` für 6.24.0 als auch alte
`Targets-release.cmake` für 6.1.0. Ein gewöhnlicher Release-Build wählt dadurch
teilweise Bibliotheken aus 6.1.0, obwohl Header und andere Bibliotheken aus
6.24.0 stammen. Das verursacht unter anderem undefinierte Verweise auf
`KColorSchemeManager::instance()`, `KJob::startElapsedTimer()` und
`KWaylandExtras::xdgActivationToken()`.

Für diese lokale Installation ist folgende CMake-Zuordnung erforderlich:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    '-DCMAKE_MAP_IMPORTED_CONFIG_RELEASE:STRING=NONE;RELEASE;'
cmake --build build-release --parallel 6
XDG_CACHE_HOME="$PWD/build-release/test-cache" \
    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ctest --test-dir build-release \
    -R '^arianna-(sigilnavigation|bookformats|booktruthstore|bookstateimport)test$' \
    --output-on-failure
```

Die Zuordnung bevorzugt `NONE`, verwendet andernfalls `RELEASE` und lässt mit
dem abschließenden leeren Listeneintrag auch Bibliotheken ohne eigene
Buildkonfiguration zu. Sie entspricht der
[CMake-Dokumentation](https://cmake.org/cmake/help/latest/prop_tgt/MAP_IMPORTED_CONFIG_CONFIG.html)
und bleibt im jeweiligen Build-Cache gespeichert. Sie wurde auch im bestehenden
`build`-Cache hinterlegt; dessen Debug-Konfiguration blieb erhalten.
Systembibliotheken und deren CMake-Dateien wurden nicht verändert.

Die Release-Prüfung am 11. Oktober 2026 ergab:

- Der vollständige Build einschließlich aller Testprogramme bestand
  (269 Buildschritte; `build-release/build.log`). Alle 21 explizit eingebundenen
  KF6-Bibliotheken stammen aus Version 6.24.0.
- `ldd -r build-release/bin/arianna` meldete keine fehlenden Bibliotheken oder
  undefinierten Symbole (`build-release/runtime-link-check.log`).
- Die vier CTest-Ziele `sigilnavigationtest`, `bookformatstest`,
  `booktruthstoretest` und `bookstateimporttest` bestanden. Der Sigil-Test dauerte
  etwa 11 Sekunden. Der beschreibbare Testcache ist für den BookTruthStore-Test
  nötig, da dieser eine Wartungssperre im Qt-Cacheverzeichnis anlegt.
