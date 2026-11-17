# Arbeitsspeicher und Cache-Lebensdauer

## Entscheidung vor der Implementierung

EPUB-Container, temporäre Werkstruktur-Snapshots und Formelvorschauen sind
rekonstruierbare, kurzlebige Lesedaten. Der BookServer besitzt seinen vorhandenen
Container-Cache; die Formelansichten gehören zum jeweiligen QML-Reader. Sie
werden ausschließlich im Arbeitsspeicher gehalten. Es entstehen keine neuen
Datenbanktabellen, persistierten Analysen, Migrationen oder EPUB-Exporte.
Annotationen, manuelle Korrekturen, bestätigte Werkstrukturen und Editor-Sitzungen
bleiben in ihren bestehenden autoritativen Speichern.

Der Container-Cache bekommt eine Obergrenze von acht geladenen Archivobjekten mit Verdrängung
nach letztem Zugriff. Ein gerade verwendeter Container lebt durch den vorhandenen
`shared_ptr` bis zum Ende seiner Nutzung weiter. Ressourcen-URLs und die
Sitzungs-/Buchzuordnung werden bei dieser Verdrängung erhalten: Ein späterer
Medienaufruf öffnet denselben Originalpfad erneut. Dateistand und Delivery-Version
werden weiter über die bestehende Invalidierung geprüft. Für noch gültige
Ressourcen-URLs bleibt nur deren kleiner Datei-/Versionsnachweis erhalten,
damit Wiederöffnen keine unveränderte Ausgabe invalidiert und echte Änderungen
weiter erkannt werden. Einträge ohne Ressourcen-URLs werden vollständig entfernt.
Ein alter Container
wird nicht zurückgeschrieben. Die Grenze begrenzt die Anzahl offener Archive,
nicht den gesamten Speicherverbrauch von Chromium oder eines einzelnen Buchs.

Die WebEngine-Ansicht der separaten Formelvorschau wird erst bei einer
Formelanforderung erzeugt und bei einem Reader-Reset zerstört. Die beiden
MathJax-Menüs erzeugen ihre Zoomansicht ebenfalls erst bei einer Zoomanforderung
und geben sie nach dem Schließen frei. Einstellungen, Quellformeln, CFI,
Textselektion und Sprachausgabe verwenden weiterhin die vorhandenen Wege.

## Ausgangsmessung

Am 19. Oktober 2026 ergab eine lesende `/proc/*/smaps_rollup`-Momentaufnahme
der laufenden Anwendung ungefähr 1,2 GiB PSS insgesamt:

- Reader-Hauptprozess: 605 MiB PSS, 652 MiB RSS.
- Größter WebEngine-Prozess: 501 MiB PSS, 525 MiB RSS.
- BookServer: 48 MiB PSS, 53 MiB RSS.

PSS teilt gemeinsam genutzte Seiten auf die beteiligten Prozesse auf. Einfach
addierte RSS-Werte würden solche Seiten mehrfach zählen. Die Momentaufnahme
belegt den aktuellen Verbrauch, aber keine bereits erzielte Einsparung. Die
laufende Benutzersitzung wird für diese Änderung nicht beendet oder ersetzt.

## Validierung

Anwendung und Testprogramme wurden unabhängig vom vorhandenen Installationsbuild
in `build-memory` als Release gebaut. Die Tests mit HTTP und WebEngine liefen
außerhalb der Socket-Sandbox, mit temporären XDG-Verzeichnissen, eigenen EPUBs
und lokalen Testservern. Es wurde keine Benutzersitzung neu gestartet.

| Testsuite | Ergebnis, einschließlich Aufbau/Abbau |
| --- | --- |
| `bookservercachetest` | 5 bestanden, 0 Fehler |
| `bookserverreferencetest` | 6 bestanden, 0 Fehler |
| `epubcontainertest` | 64 bestanden, 0 Fehler |
| `epubmathtest` | 46 bestanden, 0 Fehler |

Die Cache-Tests beobachten die tatsächliche Zerstörung verdrängter Container,
weiterlebende aktive Verbraucher und erneutes Öffnen ohne EPUB-Änderung. Reale
HTTP-Anfragen prüfen Medieninhalt, Byte-Range-Antworten, Sitzungsschutz und den
Erhalt beziehungsweise die Invalidierung von URLs bei Versionswechseln.

Die vollständige MathJax-Suite prüft unter anderem fehlende Vorschau-/Zoom-
Browser vor dem ersten Aufruf, Zoom schließen und erneut öffnen, Reader-
Kontextmenüs, Formelkopieren, CFI und Sprachausgabe. Beim Deaktivieren der
Mathematik-Vorschau wird über einen `QPointer` die tatsächliche Zerstörung der
Ansicht geprüft. Ein älterer Test erwartete hier noch eine existierende
`about:blank`-Ansicht und wurde an die neue Lebensdauer angepasst.

Die finalen Logs liegen in `build-memory/validation/`. Die laufende Anwendung
verwendet weiterhin ihren bisherigen Build; die Änderungen wurden nicht
installiert oder committed. Eine konkrete RAM-Ersparnis für dasselbe geöffnete
Buch ist noch nicht durch eine Vorher-/Nachher-Messung belegt. Insbesondere
der Bedarf großer Kapitel, MathJax-DOMs, Bilder und Chromium selbst wird durch
diese beiden begrenzten Maßnahmen nicht pauschal gedeckelt.

Reproduzierbarer Build und Testlauf:

```sh
cmake -S . -B build-memory -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON \
  '-DCMAKE_MAP_IMPORTED_CONFIG_RELEASE:STRING=NONE;RELEASE;' \
  -DARIANNA_OKULAR_SOURCE_DIR=/home/markus/Projekte/okular
cmake --build build-memory --target arianna bookservercachetest \
  bookserverreferencetest epubcontainertest epubmathtest --parallel 3
ctest --test-dir build-memory --output-on-failure \
  -R '^arianna-(bookservercachetest|bookserverreferencetest|epubcontainertest|epubmathtest)$'
```

`CMAKE_DISABLE_FIND_PACKAGE_Git` betrifft nur diesen Build und verhindert, dass
die KDE-Konfiguration Git-Hooks im schreibgeschützten `.git` installieren will.
