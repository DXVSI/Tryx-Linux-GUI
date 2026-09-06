# Feature proposal: декомпозиция runtime и развитие функций

## Статус

- Статус: feature freeze, подготовка релиза 2.3.0 после полного прогона
  накопленных изменений от 2.2.0. Новые функции отложены до выпуска.
- Дата фиксации: 9 августа 2026 года; обновлено 6 сентября 2026 года.
- Базовая версия приложения: `2.2.0`.
- Исходная база на момент составления:
  `feature/panorama-1011-support`, commit `1e93479`.
- Текущая рабочая ветка: `refactor/runtime-decomposition`, создана от
  `production` / `v2.2.0`, commit `1062305`.
- Документ объединяет архитектурную декомпозицию, аудит KANALI 2.4.0 и
  оставшийся Linux feature backlog.
- Общий backlog одобрен 20 августа 2026 года. Реализация идёт по одному
  проверяемому срезу; hardware queries и writes по-прежнему требуют отдельного
  явного согласия для точной модели.
- Каждый архитектурный refactor и каждая пользовательская функция должны
  выполняться отдельными небольшими изменениями. Один общий rewrite запрещён.
- Предложение A8, A9 и B5 согласовано 5 сентября 2026 года с уточнённым B5:
  тихая проверка при запуске GUI и каждый час, без opt-in и self-update.
  Implementation contract ниже также согласован; A8, A9 и локальная реализация
  B5 завершены 6 сентября. Desktop delivery B5 не выдается за доказанную
  изолированными тестами. B6 research завершён отдельно 6 сентября:
  публичные vendor sources проверены, но remote firmware updater остаётся
  NO-GO до подтверждения authenticity, compatibility и recovery contract.
- 6 сентября пользователь выбрал следующий feature: C16, пользовательский
  текст существующих бейджей. Технический контракт «Автоматически / Свой текст»
  ниже подтверждён запросом «делай бейдж»; software-реализация завершена.
- Затем 6 сентября пользователь выбрал подготовку релиза текущего набора.
  Приоритет: [UI-чек-лист и release gates 2.3.0](2026-09-06-release-2.3.0-ui-checklist.md),
  исправление найденных дефектов, upgrade/rollback, version metadata и пакетный
  прогон. Новый feature/research scope, дальнейшие крупные refactor и новые
  модели до релиза не добавлять. Software/headless evidence не заменяет
  desktop/device/installed acceptance; hardware разрешения остаются отдельными.
  Исправленная локальная сборка установлена; пользователь подтвердил работу и
  запросил релиз. `VERSION` и package metadata обновлены до 2.3.0 локально,
  commit/push в production и тег v2.3.0 разрешены пользователем отдельно.
  Публичная публикация draft остаётся отдельным шагом. 1815 software tests прошли;
  exact-source distro CI и непроверенные hardware cases остаются отдельными gates.

Этот документ является новым каноническим планом. Он заменяет
`todo-next.md` как подробный источник задач и уточняет
`2026-07-31-kanali-feature-audit.md` там, где KANALI 2.4.0 дала новые
подтверждённые сведения. Старые документы остаются историческими источниками.

## Предположения и цель

Предполагается, что требуется:

1. уменьшить связанность `DeviceManager` и `PrinterProtocol`, не меняя
   подтверждённое поведение устройства;
2. оформить все найденные полезные функции единым backlog;
3. добавлять функции вертикальными, проверяемыми срезами;
4. не объявлять поддержку модели или wire-команды без физического устройства
   и независимого подтверждения.

Целевой результат:

- `DeviceManager` остаётся стабильным runtime façade, но делегирует session,
  operations и persistence отдельным владельцам состояния;
- device, media и display UI продолжают работать через Manager1/Manager2
  D-Bus, firmware UI через отдельный Firmware1, а desktop settings и dashboard
  telemetry остаются локальными Qt Quick функциями;
- runtime остаётся единственным владельцем обычного printer transport;
  firmware updater может получить hardware ownership только через отдельный
  exclusive quiesce handoff;
- новые Linux-функции получают явные acceptance criteria;
- аппаратно неподтверждённые направления остаются research tracks и не
  выполняют mutating USB writes; даже немутирующий protocol query с USB OUT
  разрешается только для подтверждённого transport и с явным согласием;
- vendor code, assets, credentials и закрытые endpoints не переносятся.

## Текущая подтверждённая база

### Поддерживаемые printer-class профили

| Устройство | USB ID | Текущий контракт |
|---|---|---|
| PANORAMA SE / PASE | `391a:1021` | Полный подтверждённый путь media, display, metrics и разрешённый firmware gate |
| PANORAMA | `391a:1011` | Community-tested `2240x1080`, media, display и metrics; firmware flashing запрещён |
| TURRIS 620 | `391a:2011` | Community-tested `1280x720` MXHD upload и немедленная activation; остальные возможности запрещены |

Legacy `cm01_se` serial/ADB остаётся отдельным поддерживаемым транспортом.

### Уже реализованные функции

- Qt 6 Quick и Quick Controls GUI без Electron и Wine.
- Full Screen, Split Screen, Mirror и Waterfall.
- Single, Loop и Shuffle для поддерживаемых профилей.
- До трёх метрик на область и CPU/GPU badges.
- Brightness и Backlight On/Off с readback.
- Image, GIF и video import.
- Fit, Fill, Crop, Stretch, rotation, zoom, pan и background.
- Device media catalog, Export, Edit, Save as new, Replace и Delete для
  catalog-capable профилей.
- Origin-aware catalog, progress, cancellation, manual retry и crash-safe
  recovery.
- Единый vendor-neutral GPU inventory и optional bounded `nvidia-smi` provider
  для NVIDIA temperature, usage, graphics frequency, power и VRAM реализованы
  software-only; отдельный real NVIDIA smoke ещё не выполнен.
- Native Linux tray, notifications и language; background runtime стартует
  через user-systemd, а GUI login-start независимо управляется через XDG
  Autostart.
- Локальная firmware ZIP validation и gated flashing для разрешённых моделей.

### Реальный размер архитектурной проблемы

- `src/devicemanager.cpp`: `14878` строк.
- Секция самого `DeviceManager`: около `10104` строк и `170` методов.
- Локальный constructor: около `2719` строк и `89` `connect()`.
- `DeviceManager` содержит около `75` полей.
- Универсальный `OperationRecord` содержит ещё `46` полей.
- `DeviceWorker` и `PrinterMediaPreparer` являются отдельными классами, но
  находятся в том же файле.
- `src/printerprotocol.cpp`: `7589` строк, включая transport, framing,
  discovery, wire workflows и udev monitor.
- `tests/printerprotocol_tests.cpp`: `19039` строк и один QTest suite для
  нескольких подсистем.

Главная причина роста: printer-class runtime, Qt Quick operation workflow и
Turris были добавлены тремя крупными feature checkpoints без последующего
extraction-прохода. Сложность safety contract реальна, но её владение
сосредоточено в одном mutable coordinator.

## Что действительно изменилось в KANALI 2.4.0

Официальный источник:
<https://www.tryx.com/en/support/release-notes/kanali-2-4-0>.

На 20 августа 2026 года `2.4.0` от 22 июля 2026 года остаётся последней
публичной версией; более новых официальных release notes нет.

Подтверждённые пользовательские изменения:

1. GIPHY доступен для PANORAMA series, HOLO, TURRIS и STAGE.
2. Добавлены три варианта device-side шрифта для HOLO, TURRIS и STAGE.
3. Выбранная яркость сохраняется после перезапуска устройства.
4. Улучшена надёжность обнаружения ROTA.
5. Уменьшена частота Electron main-process JavaScript error.

Статический diff 2.3.1 и 2.4.0 дополнительно показывает:

- отдельный UI/backend groundwork для `PASEV2`;
- неполный backend scaffolding для `PANOV2` и `PAWBV2` без полноценных routes
  и OTA;
- task-based media progress, watchdog зависших media sessions и более явные
  terminal states;
- обновлённый GIF path через MediaX;
- более управляемую cloud library synchronization.

Эти внутренние сигналы не являются публичной support matrix. Нельзя заявлять,
что KANALI 2.4.0 официально поддерживает V2, или выводить USB ID V2 только из
имени внутреннего product type.

Все пункты Settings, показанные в KANALI 2.4.0, существовали уже в 2.3.1:
temperature unit, 12/24H, close behavior, recording path, shortcuts, log
export, app update, firmware update и legal links. Они являются parity backlog,
но не новинками релиза 2.4.0.

## Неизменяемые архитектурные инварианты

При любом refactor или feature изменении необходимо сохранить:

- одновременно существует только один hardware owner;
- обычные printer-class writes сериализованы worker context, а внешний
  firmware updater получает эксклюзивное владение только после quiesce
  runtime и освобождения обычного transport;
- QObject thread affinity текущего worker и preparation process;
- physical-generation fencing и немедленную отмену stale I/O;
- bounded timeouts, source bounds и cancellation;
- точные terminal outcomes `NotStarted`, `Succeeded`,
  `FinalizationUnknown` и `PartialOrUnknown`;
- запрет автоматического replay mutation после потерянного ACK;
- запись crash journal до потенциально необратимой команды;
- exact device identity, PID, generation, inode и SHA-256 checks;
- Manager1 compatibility и Manager2 API 8 до отдельного одобрения API bump;
- monotonically increasing D-Bus revisions и отбрасывание stale events;
- firmware-exclusive interlock;
- fail-closed capability gates для каждого product profile.

Разнесение строк по новым `.cpp` без передачи владения состоянием отдельным
компонентам не считается завершённой декомпозицией.

### Политика эволюции runtime D-Bus API

- `Manager1` является замороженным legacy interface и не получает новых
  методов, сигналов или изменений существующих signatures.
- Опубликованная форма `Manager2` из `v2.2.0` является baseline API 8.
  Значение 8 обозначает major compatibility generation, а не feature или
  release version.
- Существующие methods, signals, positional structs, error semantics,
  revisions, ownership и retry behavior не меняются внутри API 8.
- Новые функции добавляются через новые versioned methods и новые wire types.
  Существующие positional structs не расширяются. Поле `schemaVersion` внутри
  struct не разрешает менять его D-Bus signature.
- Post-v2.2 функции включаются только после runtime capability handshake.
  Возможность для текущего устройства является пересечением runtime
  capability, device capability и локальной поддержки GUI.
- Отсутствие capability method на старом API 8 распознаётся только по
  `org.freedesktop.DBus.Error.UnknownMethod` и означает пустой набор новых
  capabilities. Любая другая ошибка оставляет новые функции выключенными.
- Runtime повторно проверяет device identity, physical generation и product
  capability перед mutation. GUI gate не является safety boundary.
- Несовместимое изменение wire shape или observable semantics, включая side
  effects, errors, ownership, idempotency, revisions и retry behavior, требует
  отдельного одобрения и нового `Manager3`. Существующий `Manager2` не
  переписывается под API 9 и по возможности остаётся доступен на migration
  window.

## Целевая runtime-архитектура

```text
Runtime D-Bus adaptors
        |
        v
DeviceManager facade
        |
        +-- LegacySessionController
        |
        +-- PrinterSessionController
        |       |
        |       +-- один serialized DeviceWorker
        |               |
        |               +-- PrinterProtocol
        |
        +-- PrinterOperationCoordinator
        |       |
        |       +-- MediaPreparationService
        |       +-- MediaCatalogStore
        |       +-- DeviceMediaArtifactStore
        |       +-- DeleteIntentStore
        |       +-- RetryCacheStore
        |       +-- TryxReplaceJournal
        |
        +-- FirmwareBridge / firmware-exclusive gate
```

`DeviceManager` публикует стабильный façade и связывает компоненты. Session
controller владеет generation/reconnect state. Operation coordinator владеет
operation lifecycle. Persistent stores выполняют bounded atomic persistence,
а transient stores владеют только своим bounded runtime lifecycle; ни те, ни
другие не эмитят UI-сигналы. Protocol слой не владеет GUI или persistent
operation state.

## Workstream A: безопасная декомпозиция runtime

### A1. Зафиксировать characterization baseline

**Статус:** software baseline выполнен 20 августа 2026 года. Штатный
`package-check` дополнен проверкой обязательных защитных сценариев, а
hardware-only acceptance вынесен в отдельный перечень
`tests/runtime-refactor-baseline.md`. Реальные hardware-write и power-cycle
проверки этим статусом не считаются выполненными.

**Зависимости:** нет.

**Scope:** разделить перечень существующих тестов по ответственности и
зафиксировать обязательные invariants до перемещения кода.

**Acceptance:**

- сохранены текущие protocol, Quick, runtime bootstrap, tray и journal suites;
- отдельно перечислены hardware-only проверки;
- ни один текущий fail-closed test не ослаблен и не удалён.

**Verify:** полный `package-check`, текущий installed/runtime smoke и
`git diff --check`.

### A2. Удалить старый remote-mode из DeviceManager

**Статус:** выполнено 20 августа 2026 года. Remote client, его состояние и все
недостижимые ветви удалены. Revision-test перенесён в живой `RuntimeClient` и
дополнен проверками duplicate revision `0`, stale snapshot после legacy event
и reset на новом service epoch. Полный software package gate прошёл; hardware
smoke не выполнялся и не требовался для refactor без новых USB writes.

**Зависимости:** A1.

**Scope:** удалить `createRemote()`, `initializeRemote()`, remote handlers,
`remoteMode_` branches и неиспользуемое remote state. Единственный relevant
revision test перенести к `RuntimeClient`.

**Acceptance:**

- в production и tests нет call site `DeviceManager::createRemote`;
- Qt Quick продолжает использовать только `RuntimeClient`;
- Manager1/Manager2 wire shape и revisions не меняются;
- runtime binary не получает нового GUI/D-Bus client dependency.

**Ожидаемый эффект:** удаление примерно `800-1000` строк и десятков веток без
изменения hardware runtime.

### A3. Вынести PrinterMediaPreparer

**Статус:** выполнено 20 августа 2026 года. Объявление и реализация
`PrinterMediaPreparer` перенесены в отдельные translation units без изменения
тела класса, сигналов, slots, queue order и thread lifecycle. Временный private
adapter `printermediapreparersupport_p.h` оставляет helper implementations в
`devicemanager.cpp`; эта link-зависимость удаляется первой в A4. Полный
software `package-check` прошёл, hardware smoke не выполнялся и не требовался
для refactor без новых USB writes.

**Зависимости:** A1.

**Scope:** перенести уже самостоятельный QObject в
`printermediapreparer.{h,cpp}` без изменения сигналов, queue order или
thread affinity.

**Acceptance:** все preparation, thumbnail, Turris MXHD, cancellation и retry
validation tests проходят без изменения observable behavior.

### A4. Вынести stateless media/config helpers

**Статус:** выполнено 20 августа 2026 года. MXHD format, media file
integrity, media identity, recovered-media validation, PASE overlay config,
runtime apply codec и private runtime paths вынесены в семь owning modules.
Временный `printermediapreparersupport_p.h` удалён, обратной link-зависимости
`PrinterMediaPreparer` от `DeviceManager` больше нет. `devicemanager.cpp`
уменьшен с 13 038 до 11 741 строки на этом этапе. Adversarial review также
закрепил exact ffprobe geometry, bounded final diagnostic drain, независимые
JSON/SHA fixtures и прямые `0700`/symlink/hardlink/`RENAME_NOREPLACE` tests.
Полный software `package-check` прошёл: 264 printer protocol cases, 10 replace
journal, 31 QML, 42 Quick client, 22 runtime bootstrap и 11 tray. API 8 и
Manager1/Manager2 contracts не менялись. Hardware smoke не выполнялся и не
требовался, поскольку этап не добавляет USB writes и не меняет wire bytes.

**Зависимости:** A3.

Последовательно, по одному owning module и его прямым consumers:

1. `turrismediaformat` как единственный владелец MXHD metadata codec, writer,
   frame-count parsing и strict pre-upload validation;
2. `printermediafileintegrity` для SHA, source hashing и size bounds;
3. `printermediavalidator` для recovered H264 и bounded validation process без
   изменения синхронного выполнения на `DeviceWorker`;
4. `printermediaidentity` для media naming/profile/fingerprint helpers и
   удаление временного
   `printermediapreparersupport_p.h`;
5. `paseoverlayconfig` и `runtimeapplyrequestcodec` для PASE overlay/apply
   config codecs и fingerprints;
6. последними Linux private-path primitives с сохранением `lstat`, ownership,
   mode, link-count, inode и `renameat2(RENAME_NOREPLACE)` проверок.

Лимит размера применяется к production scope одного slice: один owning module
и только его прямые consumers. Build manifests, characterization tests и
canonical docs не входят в этот подсчёт, поскольку общий compiled module уже
требует двух qmake manifests. Header-only реализация и размещение helpers в
неподходящем существующем модуле запрещены.

Название stateless не означает отсутствие I/O: validator, writer и path
primitives остаются синхронными bounded functions. A4 не меняет их thread
affinity, порядок cancellation checks, atomic writes, USB dispatch или
observable errors.

**Acceptance:**

- MXHD magic, description, fields, kinds, FPS и geometry имеют одно production
  definition; writer output принимается strict validator, malformed input
  отклоняется до первого USB write;
- recovered validation сохраняет size/hash/NAL/geometry checks, deadlines,
  bounded diagnostics и cancellation semantics;
- временный `printermediapreparersupport_p.h` и migrated definitions отсутствуют
  в `devicemanager.cpp`;
- старые golden fixtures, retry, preparation, staged-source и API 8 tests
  проходят без изменения wire bytes и D-Bus contracts.

**Out of scope:** перенос process work с `DeviceWorker`, descriptor-pinning
hardening, persistence stores и operation lifecycle. Они требуют отдельных
behavior-changing этапов A5/A6/A7/M5.

### A5. Выделить state stores

**Статус:** выполнено. Срезы A5.1-A5.7 завершены 20-26 августа 2026 года:
выделены persistent `MediaCatalogStore` и `PaseMetricsConfigStore`, transient
`DeviceMediaArtifactStore`, stateless `DeleteIntentStore`,
`RetryCacheTransitionStore`, canonical `RetryCacheStore` v11 и internal v2
`TryxReplaceJournal`. Все выделенные store остаются синхронными обычными
классами без `QObject`, собственных потоков и доступа к USB. Live snapshots,
revisions, D-Bus signals, generation fences и operation lifecycle остались в
`DeviceManager`.

На первом срезе `devicemanager.cpp` уменьшен с 11 741 до 11 301 строки.
Persistent records сериализуются только по allowlist; cross-device identity,
unsafe names, path-подобные conversion profiles, malformed, oversized и
future-version indexes отклоняются. Неподдерживаемый или небезопасный index
переводит store в read-only fail-closed режим и не может быть перезаписан
следующей mutation. V1 rollback сохраняет исходные байты точно, а invalid
thumbnail удаляется только после успешного commit очищенного index. Суммарная
startup-проверка thumbnail ограничена 128 MiB; превышение сохраняет index и
файлы без изменений и отключает запись.

`PaseMetricsConfigStore` единолично владеет legacy-файлом
`pase-metrics.json`, строгим чтением v1/v2, сериализацией v2 и одним
привязанным к serial overlay record. Это намеренно не multi-device history:
запись для нового устройства заменяет единственный record, а disable на другом
устройстве не удаляет сохранённую настройку. V1 мигрируется только в памяти и
не переписывается автоматически. Runtime-only labels, values, units и badge
text не сериализуются.

Load ограничен 64 KiB и проверяет открытые file descriptors, ownership,
regular-file type и отсутствие hard links. Symlink, special file, malformed,
oversized и future-version state отклоняются fail-closed; mutations остаются
запрещены до явного повторного load или перезапуска runtime. V2 parser
проверяет типы и обе области, а writer канонизирует неактивную правую область,
ограничивает serial и гарантирует, что собственный output можно загрузить
снова. Cache меняется только после успешного `QSaveFile` commit или безопасного
удаления. Stale USB generation и device-change decisions по-прежнему
отсекаются в `DeviceManager` до обращения к store; persistent TTL, CAS и
revision в существующую v2-схему не добавлялись.
Path-based `QSaveFile` и remove по-прежнему предполагают единственного writer
под тем же UID: pre-existing symlink и unsafe destination отклоняются, но
защита от конкурентной подмены ancestor-каталога hostile same-UID процессом
требует отдельного descriptor-pinning hardening.

На втором срезе `devicemanager.cpp` уменьшен с 11 301 до 11 022 строк. Полный
software `package-check` прошёл: 297 printer protocol cases, 10 replace
journal, 31 QML, 42 Quick client, 22 runtime bootstrap и 11 tray. API 8 и
Manager1/Manager2 contracts не менялись. Hardware smoke не выполнялся и не
требовался, поскольку persistence extraction не меняет USB calls или wire
bytes.

Третий срез, `DeviceMediaArtifactStore`, является transient: reservations,
ownership, leases, operation holds, revocation и expiry records живут только в
памяти и не вводят новый persistent format. D-Bus owner watcher, sweep timer,
operation history, revisions, signals, product/device/generation и firmware
gates, worker и preparer остались в `DeviceManager`.

Startup cleanup лениво обрабатывает не более 4096 непосредственных entries
outbox за один вызов и продолжается bounded timer batches. После успешной
инициализации новые reservations разрешены и исключаются из продолжающегося
sweep вместе со своими exact canonical и `.part` paths. Owner-owned regular
files и symlink leaves удаляются без перехода по ссылке; directories, special и
foreign-owned entries сохраняются. Collision не перезаписывает существующий
canonical или partial path. Удаление одного runtime artifact обращается только
к его exact canonical и `.part` path и не перечисляет весь outbox.

Finalize открывает файл через `O_NOFOLLOW | O_NONBLOCK`, проверяет owner, mode
`0600`, regular type, link count и size и фиксирует device/inode. Открытый
read-only descriptor удерживается до удаления record или уничтожения store,
включая отозванный lease и неудачный cleanup, чтобы unlink/recreate не мог
повторно использовать зафиксированный inode. Если descriptor нельзя удержать,
finalize оставляет reservation незавершённой и сообщает ошибку. Claim и
operation hold повторно проверяют identity и SHA-256 через открытый descriptor,
а expiry повторно проверяется после потенциально долгого hash до изменения
state. Ошибка unlink не превращается в ложный success: lease отзывается,
revoked record остаётся для следующего sweep, а API сообщает ошибку. Это
исправляет нарушение уже задокументированного API 8 Release contract, а не
задаёт новую успешную семантику.

Owner watcher регистрируется до liveness check. Исчезновение requesting D-Bus
unique name отзывает все его artifacts и отменяет связанные active operations.
Повторная liveness-проверка не позволяет ни позднему Stage callback опубликовать
ownerless artifact, ни Recovered/Replace начать работу после disconnect.

API 8 wire shape не менялся. `TryxRuntimeDeviceMediaArtifact.localPath` остаётся
абсолютным path, `leaseExpiresUtcMs` остаётся UTC epoch milliseconds, unclaimed
TTL остаётся 5 минут, а claimed lease 2 минуты. Повторный Claim возвращает тот
же lease без продления; Renew продлевает его. Скрытая замена UTC на monotonic
semantics в этом refactor не выполнялась.

На третьем срезе `devicemanager.cpp` уменьшен с 11 022 до 10 904 строк. Полный
software `package-check` прошёл: 303 printer protocol cases, 10 replace
journal, 31 QML, 42 Quick client, 22 runtime bootstrap и 11 tray. Hardware
smoke не выполнялся и для extraction без новых USB writes или wire bytes не
требовался.

Оставшиеся ограничения A5.3:

- опубликованный API 8 возвращает абсолютный `localPath`, поэтому hostile
  same-UID процесс не входит в обеспечиваемую модель конфиденциальности и может
  создать path race после закрытия проверенного descriptor;
- SHA-256 файла размером до 500 MiB всё ещё вычисляется синхронно на
  D-Bus/manager thread;
- UTC wall-clock jumps могут досрочно завершить либо фактически продлить lease;
  изменение этой observable semantics требует отдельного решения;
- aggregate outbox quota, абсолютного lifetime поверх Renew и bounded live
  owner/expiry scan пока нет;
- transient ошибка lstat/unlink во время startup scan логируется, но тот же path
  повторно проверяется только после следующего запуска runtime;
- Cancel/Retry ownership сохраняет существующую per-user runtime семантику и
  намеренно не ужесточается внутри API 8.

Четвёртый срез выделил `DeleteIntentStore`. От общего
`OperationRecoveryStore` отказались: delete intent записывается и manager, и
worker thread непосредственно перед `FileRemove`, retry cache имеет десять
выпущенных версий и собственные media artifacts, а replace уже обслуживает
самостоятельный `TryxReplaceJournal`. Один stateful façade смешал бы разные
threading, crash-ordering и migration contracts, не сократив сложность.

`DeleteIntentStore` является stateless и привязан только к exact path. Новый
внутренний формат v2 хранит canonical `productId`, operation и device identity,
generation, exact media identity и монотонный mutation stage. Это исправляет
restart recovery для PANORAMA `391a:1011`: старый v1 не содержал product ID и
всегда восстанавливался как `0x1021`. Legacy v1 по-прежнему читается только как
`0x1021`, поскольку безопасно вывести его исходную модель после restart
невозможно; обе реально выпущенные формы v1, с `createdUtc` и без него,
принимаются без автоматического rewrite.

V2 намеренно допускает ровно одну цель, как и публичный Delete workflow.
Поддержка latent multi-target protocol loop не объявляется persistent
контрактом, который runtime после restart не способен полностью продолжить.
Legacy v1 parser сохраняет чтение ранее допустимых списков только для
read-only reconciliation текущей цели.

Начальная запись допускается только как `Preflight` с
`mayHaveStarted=false`. Worker обязан durably перевести matching record в
`Dispatch` с `mayHaveStarted=true` непосредственно перед USB mutation; ошибка
load/write/commit останавливает операцию до первого `FileRemove`. После этого
progress-записи диагностические и не ослабляют уже установленный recovery
barrier. Immutable identity, deleted-name prefix, index, timestamp и stage не
могут двигаться назад, а clear требует exact operation и device identity.
Invalid, future-version или unsafe state нельзя перезаписать либо удалить через
обычную mutation.

V2 сериализуется по allowlist, ограничен 256 KiB, создаётся как owner-only
`0600`, проверяется повторным descriptor-based load и сопровождается parent
directory `fsync`. Для совместимости owner-owned regular single-link v1
принимает выпущенные безопасные `0600`/`0640`/`0644`, но не переписывает их.
Symlink, hardlink, special file, group/world-writable state, unsafe parent,
malformed и oversized payload отклоняются fail-closed. Path-based создание
каталога и `QSaveFile` по-прежнему не защищают от конкурентной подмены ancestor
или leaf hostile same-UID процессом; это остаётся общей отдельной
hardening-задачей.

На четвёртом срезе `devicemanager.cpp` уменьшен с 10 904 до 10 843 строк.
Полный software `package-check` прошёл: 326 printer protocol cases, 10 replace
journal, 31 QML, 42 Quick client, 22 runtime bootstrap и 11 tray. Public API 8,
Manager1/Manager2 wire contracts и USB command bytes не менялись. Hardware
smoke не выполнялся и для persistence/schema среза без новых USB-команд не
требовался.

**Зависимости:** A2, A4.

Каждый store выполняется отдельной задачей:

1. `MediaCatalogStore` для index, thumbnails и origin metadata, выполнено.
2. `PaseMetricsConfigStore` для versioned device-scoped overlay settings,
   выполнено. Формат остаётся single-record v2; multi-device history потребует
   отдельной версии схемы и не входит в этот рефакторинг.
3. `DeviceMediaArtifactStore` для transient ownership, UTC leases, operation
   holds и bounded outbox cleanup, выполнено.
4. `DeleteIntentStore` для typed v2 delete crash barrier и совместимого чтения
   v1, выполнено.
5. Перед extraction выполнен отдельный v10-compatible retry safety slice.
   Все четыре перехода к `requestPrinterUploadPrepared` обязаны сначала
   durably сохранить консервативный rollback record, эквивалентный
   `PartialOrUnknown`, но не менять live terminal outcome до ответа текущего
   worker generation. Ошибка commit останавливает dispatch до первого USB
   write. После доказанного worker-ответа `NotStarted` record можно безопасно
   уточнить; после crash он остаётся только recovery-required и использует
   новое remote name. `RecoveredMediaUpload` и `ReplaceDeviceMedia` на
   persistence/restart boundary нормализуются в upload-only: Apply, Delete,
   replace continuation и metrics не восстанавливаются и не повторяются.
   `RetryCacheTransitionStore` владеет crash/downgrade-переходом вокруг
   legacy v10 slot: защищает предыдущий кандидат в закрытом дочернем
   `suspended-v10`, проверяет backup bytes и hashes, durably переносит lineage
   текущего dispatch при Retry и возвращает typed `Conflict`, который блокирует
   все device mutations. `Restored` является cleanup-only состоянием, поэтому
   последующий запуск новой версии не воскрешает кандидат, уже удалённый или
   заменённый выпущенным v10. При совпавшем старом operation ID восстановление
   разрешено только для byte-identical защищённого manifest; повторное
   использование UUID с другой lineage сохраняется как конфликт без
   перезаписи root или backup. После restore совпавший UUID чужой live
   operation не считается владением retry: manifest повторно валидируется и
   durably получает новый ID. Если такой remap нельзя записать, mutation gate
   остаётся закрытым даже без активного `suspended-v10` transition.
6. `RetryCacheStore` получил новую canonical v11-схему в отдельном
   `prepared-media/v11/`. Manifest хранит только direct-child artifact names,
   bounded sanitized subject и primary error, но не пользовательский
   `sourcePath`, произвольные diagnostics или абсолютные canonical v11 paths.
   Legacy v1-v10 читается fail-closed и не разрешает mutation replay.
   Совместимое представление для выпущенного v10 обеспечивается отдельным
   консервативным shadow в legacy slot
   `prepared-media/retry-manifest.json`: identity-bound
   состояния кодируются форматом v10, а unbound legacy recovery сохраняется в
   консервативном формате v6. Это явно разрешённое временное исключение для
   runtime-owned абсолютных prepared/thumbnail paths, поскольку выпущенный v10
   не умеет читать relative v11 references. Shadow никогда не может быть
   permissive относительно canonical v11 state. Наличие shadow само по себе не
   разрешает package downgrade: после появления Split geometry требуется
   отдельный explicit C4 preparation gate.
7. `TryxReplaceJournal` остаётся самостоятельным store. Для новых операций на
   `391a:1011` реализована отдельная внутренняя v2 с `productId`; legacy v1
   остаётся совместимым только с `0x1021`. Выполнено.

Контракт седьмого среза:

- новый record всегда имеет `formatVersion=2` и canonical `productId` из четырёх
  lowercase hex digits без префикса, как в `DeleteIntentStore`; Replace
  допускается только для PANORAMA `391a:1011` и PASE `391a:1021`, но не для
  Turris `391a:2011`;
- v1 принимается только с точной старой allowlist-формой и в памяти получает
  `formatVersion=1`, `productId=0x1021`. Сам load не меняет bytes. Первый
  последующий доказанный монотонный переход явно повышает record до v2 и
  сохраняет выведенный `1021`; format-only rewrite, включая terminal v1,
  отклоняется;
- `productId` является частью immutable identity. Новая Replace operation
  копирует его из уже зафиксированного product profile, а restart recovery
  восстанавливает operation product только из принятого journal. Продолжение
  разрешено лишь при одновременном совпадении Replace journal, связанного delete
  intent, operation и текущего USB по operation ID, product, device serial,
  generation и исходной media identity;
- missing, numeric, non-canonical, неизвестный или неподдерживаемый v2
  `productId`, future version, лишнее поле и duplicate top-level JSON key
  сохраняют существующие bytes и блокируют обычные write/clear. Попытка сменить
  product отклоняется без замены принятого journal. Ошибка durable upgrade также
  не разрешает повторять Apply, `FileRemove` или другую USB mutation;
- public API 8, Manager1/Manager2 wire types, USB command bytes, operation
  lifecycle и thread ownership не меняются. Срез добавляет только внутреннюю
  schema, product binding и restart/cross-product regression tests.

**Статус седьмого среза:** `TryxReplaceJournal` v2 реализован 26 августа 2026
года. Новые Replace operations на `391a:1011` и `391a:1021` записывают
canonical product до продолжения saga. V1 загружается byte-preserving только
как `0x1021` и повышается до v2 лишь при следующей durable progress-записи.
Restart operation восстанавливает product из принятого journal, а read-only
reconciliation требует общей identity Replace journal, delete intent,
operation и текущего USB. Product входит в immutable transition identity;
`0x2011`, zero, numeric/prefixed/missing product, future version, duplicate keys
и cross-product переходы отклоняются fail-closed.

Полный software `package-check` прошёл: 777 printer protocol cases, 17 replace
journal, 31 QML, 42 Quick client, 22 runtime bootstrap и 11 tray. Public API 8,
Manager1/Manager2 wire contracts и USB command bytes не менялись. Hardware
smoke не выполнялся: schema/product-binding срез не добавляет USB-команд, а
отдельная hardware acceptance остаётся в D6.

**Статус шестого среза:** `RetryCacheStore` v11 реализован 24 августа 2026
года. Store остаётся синхронным обычным классом без `QObject`, собственных
потоков, USB и operation lifecycle. Он единолично владеет strict parse/serialize,
durable filesystem transitions, миграцией v1-v10, canonical/shadow
reconciliation, clear и consume. `DeviceManager` сохраняет UI и D-Bus policy,
operation state, асинхронный запуск валидации, device identity/generation и
последнюю проверку непосредственно перед USB dispatch. Между ними передаются
только typed load, validation, snapshot и mutation results; `QJsonObject` и
независимо задаваемые shadow flags через эту границу не проходят.

Disk layout намеренно разделяет canonical v11 artifact и совместимую копию,
которую выпущенный v10 имеет право удалить:

```text
prepared-media/
  retry-manifest.json
  shadow-<lineage>-prepared.*
  shadow-<lineage>-thumbnail.*
  suspended-v10/
  v11/
    retry-manifest.json
    prepared-<lineage>.*
    thumbnail-<lineage>.*
```

Canonical manifest хранит только сгенерированные store direct-child artifact
names. Root `retry-manifest.json` является консервативным compatibility shadow,
читаемым выпущенным v10: identity-bound состояния кодируются форматом v10,
unbound legacy recovery сохраняется в консервативном формате v6. Он содержит
минимальные runtime-owned абсолютные пути только к отдельным root shadow
artifacts.
Предпочтителен проверенный hard link на canonical inode; если filesystem его не
поддерживает, разрешена owner-only копия с повторной проверкой size и SHA-256.
Ошибка обоих способов останавливает dispatch до USB. Shadow никогда не
указывает на единственный canonical path: clear/consume выпущенного v10 должен
удалить лишь shadow link или copy. `RetryCacheTransitionStore` используется
через явный adapter canonical и shadow identity, получает только допустимые
legacy direct-child paths и владеет cleanup только объявленных shadow entries.
Его внутреннее состояние v2 раздельно связывает защищённый A и текущий B с
exact `dispatchId` и `deviceGeneration`; изменение operation ID B при restart
без явного durable adoption считается конфликтом. Legacy v1 принимается
только после exact сверки canonical A+B, backup A и допустимого для текущей
phase root disposition, затем до любой mutation durably переписывается как v2.
Если после старого post-tombstone crash `dispatchId` A уже недоказуем, store
остаётся fail-closed и не синтезирует identity.
Exact A+B identity является dispatch/root-mutation authority только до
durable commit canonical cleanup tombstone. После него retired B больше не
может быть отправлен или восстановлен, а единственная разрешённая mutation
состоит в локальном cleanup: canonical tombstone задаёт allowlisted artifact
name вместе с exact device/inode, surviving A всё ещё проверяется exact, а
transition удаляет только собственные фиксированные backup entries. Поэтому
мертвые `dispatchId`/generation B не дублируются в cleanup tombstone и не
используются как право на device mutation.

V11 manifest содержит не один взаимозаменяемый slot, а три bounded области:

- `retryCandidate` для предыдущего доступного кандидата A;
- `inFlightDispatch` для подготавливаемой, выполняемой или восстанавливаемой
  попытки B;
- `cleanupPending` для exact allowlisted names, device/inode identity и
  directory-sync состояния ещё не завершённого cleanup.

Immutable identity включает отдельный `lineageId` кандидата и новый
`dispatchId` каждой попытки. CAS дополнительно проверяет ожидаемую phase,
operation ID, product, device identity/generation, artifact name, size и hash.
Если B является Retry A, он содержит `retriesLineageId`; UUID операции сам по
себе никогда не доказывает lineage или ownership.

Одновременно API 8 показывает не более одного retry. Во время live `Preparing`
текущая операция B уже имеет приоритет, а retry candidate A остаётся durably
сохранённым, но скрытым. После restart до переключения root shadow B не мог
достичь USB, поэтому retry surface снова показывает A; после durable shadow
switch, в `DispatchArmed` и recovery приоритет остаётся у B:

| Durable состояние | Видимый и сохраняемый результат |
|---|---|
| Только A | A доступен для явного Retry |
| B в `Preparing`, root shadow всё ещё A | B не мог достичь USB; после restart B очищается, A остаётся |
| Root shadow уже B, но canonical ещё не `DispatchArmed` | B консервативно становится `PartialOrUnknown`, A скрывается |
| B в `DispatchArmed` во время crash | B становится `PartialOrUnknown`, A сохраняется скрытым |
| Независимый B успешно завершён либо доказанно отменён до USB dispatch | B удаляется, A восстанавливается |
| B является Retry A и доказанно отменён до USB dispatch | A восстанавливается с прежним `lineageId` |
| B является Retry A и успешно завершён | A и B retire вместе |
| B получил retryable terminal outcome | Для Retry-A сохраняется `lineageId` A с новым `dispatchId`; независимый B заменяет A; последующий clear B не воскрешает A |
| Ожидаемый shadow B исчез после downgrade | B не разрешается повторять, но остаётся recovery fence; A возвращается только после read-only reconciliation или reconnect |
| Future, malformed или unsafe canonical v11 | Все bytes сохраняются, fallback к shadow запрещён, mutations блокируются |

`persistPrepared` сначала создаёт или принимает descriptor-validated canonical
artifact и durably синхронизирует его. Только после этого canonical manifest
получает `Preparing` с A и B. Перед arm старый shadow A защищается через
совместимый `suspended-v10`, создаются и повторно проверяются shadow artifacts,
после чего durably записывается консервативный v10 manifest. Затем canonical
переходит в `DispatchArmed`, синхронизируются оба parent directory и manager
повторно проверяет live operation, product, device identity и generation.
Любая ошибка до завершения этой последовательности даёт ноль USB mutation
requests.

Terminal commit следует risk lattice, а не одному фиксированному порядку:

- `DispatchArmed` и `PartialOrUnknown` кодируются в shadow как
  `PartialOrUnknown` с recovery и fresh-name ограничениями;
- `FinalizationUnknown` остаётся exact finalization-only состоянием, которое до
  read-only FileList reconciliation не разрешает повтор Data/End;
- `NotStarted`, `Rejected` и `Cancelled` кодируются exact либо строже;
- при повышении риска сначала ужесточается shadow, затем canonical;
- при доказанном снижении риска сначала уточняется canonical, а временно более
  строгий shadow допустим;
- clear, acknowledged success и consume сначала durably retire или заменяют
  root shadow, затем меняют canonical и только после этого удаляют artifacts;
  cleanup failure оставляет `cleanupPending` tombstone до успешного unlink и
  parent-directory fsync. Пока tombstone не закрыт, store разрешает только
  bounded повтор exact cleanup и блокирует ordinary mutations.

Отсутствующий ожидаемый shadow не считается доказанным success. В
поддерживаемой single-writer модели это сигнал clear/consume выпущенного v10:
v11 не воскрешает повторный upload B, но сохраняет его recovery fence до
read-only reconciliation либо физического reconnect с обязательным новым
remote name. Только после снятия fence разрешено восстановить защищённый A.
Shadow той же lineage принимается лишь в более консервативном состоянии после
сравнения полного artifact и transition identity; чужая lineage, неоднозначный
rewrite или несовместимый `suspended-v10` дают typed `Conflict`.
`compatibilityId` используется только как быстрый сигнал и не заменяет это
сравнение.

Load всегда сначала классифицирует parent `v11` и canonical manifest. Future,
malformed, oversized, duplicate-key, symlink, special-file, unexpected-link и
unsafe state блокирует mutations до обращения к legacy helper или shadow и
сохраняется byte-for-byte. Перед `QJsonDocument` bounded structural scanner
отклоняет duplicate object keys, превышение размера, глубины, количества
элементов и строк. После parse действуют рекурсивные version-specific exact key
allowlists. Subject и primary error очищаются и ограничиваются по UTF-8 bytes;
control characters, пользовательские абсолютные paths и произвольная
diagnostics не сериализуются. Origin identity обязательна для Turris и
`EnsureMediaAndApply`, а для остальных профилей может отсутствовать только там,
где это принимает выпущенный v10.

Legacy v1-v10 мигрируется только при отсутствии canonical v11. Исходный
manifest и artifacts не меняются до полного commit: hash и staging copy читают
один `O_NOFOLLOW` descriptor, фиксируют device/inode/size/hash, проверяют hash
destination и перед commit выполняют CAS точных manifest bytes и исходной file
identity. Strip continuation разрешён только для исчерпывающе перечисленных
выпущенных форм; malformed или tampered continuation блокирует миграцию.
Все legacy-формы с continuation, включая strict v9/v10 `UploadAndApply`,
`RecoveredMediaUpload` и `ReplaceDeviceMedia`, нормализуются в upload-only.
Apply, Delete, replace continuation и metrics не переносятся и никогда не
запускаются автоматически после restart или Retry. `FinalizationUnknown`
сначала допускает только read-only reconciliation.

Поддерживаемый runtime остаётся единственным disk writer: D-Bus service
регистрируется до создания `DeviceManager`. Новый lock не добавляется, потому
что выпущенный v10 его всё равно не соблюдает. Одновременный ручной запуск
чужого runtime, hostile same-UID подмена ancestor/leaf и внешнее удаление
`CacheLocation` остаются явно принятыми границами хранения retry. Copy fallback временно
требует дополнительный объём до размера prepared artifact; ошибка записи или
нехватка места останавливает dispatch. Future writer обязан поддерживать
собственный совместимый shadow. Произвольный downgrade не гарантируется даже
для валидного v11 state: C4 разрешает его только после атомарной проверки
`Empty` или exact terminal Full retry и фиксации отдельного durable marker.

Реализация выполнена RED-first в существующем `printerprotocol-tests`, без
нового test binary. Store-level tests покрывают canonical/shadow outcome
matrix, каждую миграцию v1-v10, future/unsafe byte preservation, path escape и
file identity, реальные restart после искусственных fsync failures,
exact-lineage clear/consume, A+B и Retry-A переходы, cleanup tombstones и
поведение замороженного consumer из выпущенного v10 при downgrade round-trip
для PASE v10, identity-unbound PASE v6 и Turris v10 с полным origin.
Manager tests проверяют оба durable barrier в момент единственного upload emit,
а transition tests проверяют exact identity A+B, запрет неявного operation-ID
remap, phase-aware durable upgrade legacy v1 в v2 и fail-closed `fsync` error
до mutation.

Однослотовая схема отклонена, потому что во время нового dispatch обязаны
одновременно пережить crash старый A и неизвестный B. Shadow, указывающий на
единственный canonical artifact, отклонён, потому что выпущенный v10 удаляет
prepared path при clear/consume. Перенос cache root и новый межпроцессный lock
не входят в A5.6: первый меняет установленную storage/downgrade границу, второй
не может связать уже выпущенный v10.

После шестого среза полный software `package-check` прошёл: 767 printer
protocol cases, 10 replace journal, 31 QML, 42 Quick client, 22 runtime
bootstrap и 11 tray; перевод собран из 1 363 завершённых строк без
`unfinished`. Public API 8, Manager1/Manager2 wire contracts и USB command
bytes не менялись. Hardware smoke не выполнялся: срез меняет persistence и
recovery до существующего upload dispatch, но не добавляет USB-команд.

**Acceptance:**

- atomic `QSaveFile` boundaries сохранены;
- каждый upload dispatch имеет durable conservative barrier, а ошибка его
  записи даёт ноль USB mutation requests;
- restart retry для recovered/replace не удаляет prepared artifact и никогда
  не повторяет Apply/Delete continuation;
- corrupt, stale-identity, oversized и cross-device records отклоняются;
- завершённые store сериализуют только allowlisted data без абсолютных
  пользовательских paths и secrets; v11-срез сохраняет совместимое чтение
  выпущенным v10, а rollback shadow содержит только минимальные runtime-owned
  absolute artifact paths. Разрешение самого downgrade принадлежит отдельному
  C4 preparation gate;
- unknown, future-version или unsafe canonical v11 state сохраняется
  неизменным и блокирует mutation вместо destructive cleanup;
- stores возвращают typed results и не меняют operation state самостоятельно.

### A6. Выделить PrinterOperationCoordinator

**Статус:** реализовано 5 сентября 2026 года. Software acceptance завершён;
hardware smoke для software-only refactor не требовался и не выполнялся.

**Зависимости:** A5.

**Пользовательская цель:** уменьшить связанность `DeviceManager` и получить
одного явного владельца полного operation lifecycle без изменения функций,
видимых пользователю. Этот boundary разблокирует последующие A7-A9, но сам не
переносит session либо protocol ownership.

#### Подтверждённое состояние до реализации

- `src/devicemanager.cpp` содержит 14 257 строк, а
  `src/devicemanager.h` содержит 1 100 строк. `DeviceManager` одновременно
  связывает worker threads, владеет discovery/session state и реализует
  operation scheduler, progress, completion, retry и recovery.
- Публичные queue, retry, cancel и snapshot methods вызываются существующим
  `RuntimeBridge`. Сигналы `operationChanged` и `operationRemoved` напрямую
  публикуются через текущий Manager2 API 8 boundary.
- `OperationRecord`, `operations_`, `operationOrder_`, `activeOperationId_` и
  `operationRevision_` находятся внутри `DeviceManager`. Тесты напрямую
  используют часть этих private fixtures через `TRYX_PROTOCOL_TESTING`.
- Старое описание четырёх completion workflow больше не покрывает текущий
  код. После C4-C6 и C12 operation state меняют preparation, staging/artifact,
  upload/finalization, FileList/catalog, Delete/Replace, Apply/Saved Layout,
  metrics, cache cleanup, retry-cache validation, owner disconnect, session
  loss и generation-change callbacks.
- `DeviceWorker` остаётся единственным serialized USB executor в отдельном
  thread. `PrinterMediaPreparer` выполняет cancellable local processing во
  втором thread. Их queued work и direct thread-safe cancellation gates уже
  разделены и не должны менять порядок.
- A5 stores уже изолируют bounded atomic persistence. Однако их результаты,
  in-memory snapshots и delete/replace/retry reconciliation сейчас
  оркестрирует `DeviceManager`.

#### Выбранная архитектура и ownership

Выбрано поэтапное полное выделение, а не thin wrapper и не одномоментный
big-bang move. Каждый компилируемый срез имеет только один authoritative
operation ledger; временное копирование state между двумя владельцами
запрещено.

- Новый `PrinterOperationCoordinator` является `QObject`, живёт в том же
  runtime thread, что и `DeviceManager`, и владеет `OperationRecord`, map,
  order, revision, active ID, terminal-history pruning и всеми transient
  operation latches.
- Coordinator владеет operation-facing in-memory state retry cache,
  delete/replace reconciliation, artifact holds, cache cleanup и deferred
  catalog publication. Существующие store classes остаются единственными
  владельцами своих persistent formats и вызываются через текущие typed APIs;
  их on-disk paths, versions и atomic boundaries не меняются.
- `DeviceManager` сохраняет public façade и прежние signatures. Его operation
  methods становятся delegates, а worker/preparer callbacks передают
  coordinator typed result вместе с текущим immutable operation context.
- Внутренний context формируется только `DeviceManager` и содержит ровно
  необходимые session facts: device path, trimmed identity, product ID и
  capabilities, physical generation, readiness/recovery state и действующие
  firmware/runtime gates. Coordinator не читает поля `DeviceManager` через
  back-pointer и не кэширует context как новую session authority.
- Discovery, generation increments, reconnect, keepalive, overlay restoration,
  firmware quiesce и physical session recovery остаются в `DeviceManager` до
  A7. При session event manager передаёт coordinator явное событие и context;
  operation decision и terminal result принадлежат coordinator.
- Coordinator публикует operation changes и request intents. `DeviceManager`
  синхронно forwarding-ит public operation signals и связывает intents с
  существующими worker/preparer signals. Обычный work остаётся queued в owning
  thread, а уже thread-safe cancel/generation fences остаются direct.
- `operationChanged` сохраняет synchronous reentrancy. После публикации
  `Uploading` coordinator повторно находит record и сверяет cancel, identity,
  generation, artifact и durable dispatch barrier до единственного USB emit.
- Все перенесённые пользовательские строки используют существующий
  `DeviceManagerMessages` translation context. Новая Qt translation context и
  массовое изменение `.ts` не допускаются.
- Shutdown остаётся упорядоченным: сначала останавливаются preparer и worker
  threads, затем явный coordinator shutdown освобождает owned source paths и
  transient artifacts. Destructor не инициирует новый cross-thread work.

#### Этапы реализации

1. RED structural checks требуют новый coordinator в runtime и test qmake
   targets, запрещают operation ledger в `DeviceManager` и фиксируют
   отсутствие новых D-Bus/wire contracts. Существующие critical operation
   tests остаются обязательными.
2. Добавляются `printeroperationcoordinator.h/.cpp`; ledger, snapshots,
   publish/finish/reject и history pruning переносятся первыми. Public
   `DeviceManager` getters и signals сохраняются как façade.
3. Переносятся queue, idempotent deduplication, retry, cancel и immediate
   worker/preparer cancellation fences. Busy, firmware, recovery и capability
   решения используют переданный context.
4. Completion families переносятся законченными вертикальными срезами:
   preparation и staging; upload, FileList и retry finalization; Delete и
   Replace; Apply, Saved Layout и metrics; cache cleanup. Внутри одной family
   terminal/reconciliation branch не делится между двумя owners.
5. Переносятся startup retry validation, persisted Delete/Replace recovery,
   artifact owner disconnect, generation/session events и ordered shutdown.
6. Удаляются прежние `OperationRecord`, containers, active ID, operation
   helpers и большие completion lambdas из `DeviceManager`. Private test
   fixtures переходят к coordinator только под `TRYX_PROTOCOL_TESTING`, без
   production debug getters.

#### Инварианты и acceptance

- `TryxRuntimeOperationInfo`, `TryxRuntimeOperationsSnapshot`, API version `8`,
  Manager1/Manager2 signatures, runtime adaptor behavior, protobuf и USB bytes
  побитно не меняются.
- String contracts `kind`, `state`, `stage`, `errorCategory`, `retryMode` и
  `terminalOutcome`, revision order, parent/attempt linkage, progress counters
  и history limit сохраняются без normalization либо typed migration.
- Только coordinator может создать, изменить, завершить или удалить operation
  record. В `DeviceManager` отсутствуют authoritative operation containers и
  прямые terminal transitions.
- Сохраняется ровно одна foreground operation. Duplicate operation ID имеет
  прежнюю idempotency/collision семантику; concurrent request получает прежний
  `Busy` result и не достигает worker.
- Product, device identity и physical generation fences применяются перед
  каждым dispatch и completion. Stale callback не публикует новый state и не
  продолжает работу на другом endpoint.
- Retry-cache shadow, upload barrier, delete intent и replace journal
  сохраняются до соответствующей USB mutation. Ошибка persistence даёт ноль
  USB requests.
- `NotStarted`, `Rejected`, `Cancelled`, `VerificationFailed`,
  `FinalizationUnknown` и `PartialOrUnknown` сохраняют текущие terminal либо
  reconciliation outcomes. Неопределённый результат не становится success;
  Apply, Delete или upload не replay-ятся автоматически.
- Cancel во время hashing/conversion немедленно закрывает local preparation;
  cancel во время USB использует существующий operation cancellation gate;
  Delete reconciliation после возможного FileRemove остаётся non-cancellable.
- Indexed thumbnails, retry/recovery records, journals, active artifact holds
  и live leases не удаляются cache cleanup. Partial cleanup публикует прежние
  counters и освобождает exclusive latch ровно один раз.
- Existing local paths, owner-only permissions, artifact ownership, lease
  expiry и D-Bus unique-owner cancellation остаются fail closed.
- Runtime shutdown сохраняет prepared retry candidate и неизвестный upload
  outcome; owned temporary source освобождается только после остановки его
  consumers.

#### Результат реализации

- Добавлен `PrinterOperationCoordinator` в runtime и test qmake targets. Он
  стал единственным владельцем operation ledger, revision/order/active ID,
  retry-cache surface, Delete/Replace recovery, artifact holds, cache-cleanup
  latch и deferred catalog publication. Копии authoritative state в
  `DeviceManager` не осталось.
- `DeviceManager` сохранил public API и session ownership, формирует свежий
  `PrinterOperationContext`, синхронно forwarding-ит operation signals и
  связывает coordinator intents с прежними worker/preparer entry points.
  Единственная точка подготовленного USB upload dispatch теперь находится за
  durable barrier внутри coordinator.
- `src/devicemanager.cpp` уменьшен с 14 257 до 6 956 строк, header с 1 100 до
  904 строк. Новый coordinator содержит 7 890 строк реализации и 747 строк
  header; дальнейшее session/worker/protocol разбиение остаётся задачами
  A7-A9.
- Добавлен A6-specific test единого ledger и синхронного manager façade.
  Canonical product-profile predicate сохранён для всех completion, retry и
  recovery fences. API version 8, Manager1/Manager2 wire signatures,
  persistence formats, translation context и USB bytes не менялись.
- Чистые runtime и `printerprotocol-tests` сборки прошли; полный protocol suite
  завершился результатом 875 passed, 0 failed. Полный `package-check`,
  translation catalog, runtime-refactor baseline и `git diff --check` также
  прошли на итоговом срезе.

#### Проверки

- В существующем `printerprotocol-tests` добавляются A6-specific ownership и
  façade tests с текущим fake USB/store harness. Отдельный hardware-dependent
  test binary не создаётся.
- Обязательны уже существующие D-Bus round-trip, scheduler collision,
  cancellation, reentrant pre-dispatch, generation change, finalization-only
  reconciliation, retry-cache restart, artifact owner, Delete/Replace,
  Saved Layout, metrics и cache-cleanup scenarios.
- После каждого implementation slice выполняется соответствующий focused
  test set. Финальные gates: fresh runtime/test build, полный
  `make package-check`, translation catalog check, runtime-refactor baseline и
  `git diff --check`.
- Hardware smoke не требуется для software-only refactor и не объявляется
  выполненным. Существующие hardware acceptance tasks остаются отдельными.

#### Риски и rollback

- Главный риск: изменение signal order или превращение synchronous publication
  в queued boundary. Это может разрешить USB dispatch после reentrant cancel.
  Mitigation: одинаковый thread affinity, explicit direct forwarding и
  повторная validation после каждого внешнего signal.
- Второй риск: stale operation context после session transition. Context не
  хранится как session truth и передаётся заново на каждый command/callback;
  record дополнительно проверяет captured generation, product и identity.
- Перенос translation calls может незаметно изменить context и оставить
  untranslated text. `DeviceManagerMessages` и translation gate являются
  обязательными.
- Большой механический diff повышает риск потерять редко используемый recovery
  branch. Workflows переносятся целиком, а старый код удаляется только после
  focused parity tests; dual execution path не добавляется.
- Rollback удаляет coordinator и возвращает delegates/state в
  `DeviceManager`. Persistent schema, paths, API и USB protocol не меняются,
  поэтому data migration и compensation не требуются.

**Out of scope:** A7 session ownership, A8 legacy/printer worker split, A9
transport/framing/protocol decomposition, public API 9 или Manager3, typed
operation enum/variant, изменение строковых outcomes, новая retry policy,
автоматический replay, persistent schema migration, новый USB operation,
firmware behavior, QML/Quick feature, hardware support statement и unrelated
C/B/D workstreams.

### A7. Выделить PrinterSessionController

**Статус:** реализовано 5 сентября 2026 года по подтверждённой пользователем
спецификации. Чистые runtime/test builds, 893 protocol tests, полный
`package-check`, translation catalog и structural baseline прошли.
Hardware smoke для software-only refactor не выполнялся.

**Зависимости:** A6.

**Цель:** выделить одного владельца runtime-состояния подключения и управления
сессией, сохранив текущий порядок команд, защиту операций и поведение
публичного API. `DeviceManager` продолжает связывать компоненты runtime.

#### Подтверждённое состояние перед реализацией

- После A6 `devicemanager.cpp` содержит 6 956 строк, header 904 строки.
  Operation ledger и retry/delete/replace recovery принадлежат
  `PrinterOperationCoordinator`, но session state остаётся в manager.
- `handlePrinterSnapshot`, `connectDevice`, `disconnectDevice`,
  `requirePrinterRecovery` и обработчики worker-сигналов совместно меняют
  selected endpoint, identity, product, generation, active/lost flags и
  признаки наблюдавшегося physical removal.
- `PrinterDeviceMonitor` подавляет одинаковый обычный discovery snapshot,
  но принудительно публикует snapshot при removal текущего endpoint, даже
  если устройство вернулось по тому же sysfs path. `currentEndpointRemoved`
  приходит перед соответствующим `snapshotChanged`.
- `printerGeneration_` является существующим счётчиком отмены и смены
  контекста. Он увеличивается при принятом snapshot, явном restart/disconnect,
  первом входе в recovery, firmware acquire, runtime downgrade и shutdown.
  Публичное имя `physicalGeneration` уже использует этот счётчик.
- Поэтому исходная формулировка «один physical remove/add создаёт ровно одну
  новую generation» неточна для полной последовательности событий.
  Раздельные snapshots `Absent` и `Ready` дают по одному increment каждый;
  coalesced same-path removal даёт один forced snapshot. A7 сохраняет эту
  семантику и запрещает дополнительные increments из-за forwarding между
  manager и controller. Смена схемы нумерации потребовала бы отдельного
  изменения поведения.
- Есть два разных keepalive-механизма: manager-таймер для legacy и
  `DeviceWorker`-таймер printer session. Printer bootstrap, обязательный
  post-bootstrap keepalive, overlay activation, foreground pause и USB
  cancellation выполняются в одном worker thread.
- Firmware acquire сначала публикует lease, закрывает generation gate,
  затем ставит quiesce в worker queue. Release сохраняет lease до ответа
  отдельного release fence. Запоздалый quiesce не должен закрыть уже
  возобновлённый transport.
- Device Specifications cache привязан к exact path, identity, product и
  generation. Display/metrics state и overlay restoration также используют
  текущую session identity; их invalidation входит в session transition.

#### Выбранная архитектура и границы владения

`PrinterSessionController` реализован как `QObject` в том же runtime thread,
что manager и operation coordinator. Перенос выполнен законченными
компилируемыми срезами, с одним владельцем каждого поля на любом срезе.

| Компонент | Ответственность после A7 |
|---|---|
| `PrinterSessionController` | Принятый discovery snapshot, selected path/raw identity/product, generation, connection/auto-connect flags, session active/lost и removal/resume state, firmware lease/release/interlock, legacy keepalive timer, session-scoped specifications/display/metrics state и overlay restoration policy |
| `DeviceManager` | Прежние public methods/signals, создание компонентов и threads, wiring, построение свежего operation context, process-wide downgrade/presentation/saved-layout orchestration |
| `PrinterOperationCoordinator` | Единственный operation ledger, operation decisions и outcomes, retry validation/reconciliation, Delete/Replace journals, artifacts и cleanup |
| `DeviceWorker` | Единственная очередь обычного USB/legacy I/O, рабочая transport session и cancellation gates, printer keepalive/overlay execution, quiesce и release fence |
| `PrinterDeviceMonitor` | Udev observation, batching, discovery fingerprint и forced same-path removal event |
| Существующие stores | Прежние persistent formats, paths, ownership checks и atomic commit boundaries |

- Manager getters становятся delegates. Production-код manager не меняет
  private session fields controller; тестовые fixtures доступны только под
  `TRYX_PROTOCOL_TESTING`.
- Controller принимает конкретные команды и события, публикует snapshots и
  intents. Он не получает back-pointer к manager, не владеет worker thread
  и не вызывает `PrinterProtocol` для I/O.
- Для operation blockers и результатов reconciliation используются узкие
  синхронные callbacks через manager, по существующему A6-паттерну.
  Controller не читает operation ledger и не хранит копию retry truth.
  Вход в physical recovery принадлежит controller; решение о достаточности
  доказательства для stored upload по-прежнему принимает coordinator.
- Свежий `PrinterOperationContext` формируется из controller state и текущих
  process-wide gates на каждый command/callback. Он не становится вторым
  владельцем session state. Raw identity для exact saved-layout и cache
  checks сохраняется; прежняя trim-семантика отдельных consumers не меняется.
- Firmware acquisition получает актуальную assessment operation blockers
  до публикации lease, в том же runtime event-loop transition. Только
  controller меняет lease, release-pending и reconnect flags.
- Downgrade marker и его fail-closed process latch остаются в manager.
  Controller выполняет session invalidation для downgrade/shutdown;
  manager сохраняет существующий blocking worker fence перед записью marker.
  Отдельный generation counter для этих путей не создаётся.
- Session-scoped metrics/display projections переносятся вместе с их
  publish/reset logic. `PaseMetricsConfigStore` остаётся владельцем
  сохранённого overlay; controller использует его текущие typed методы.
  Presentation preferences и Saved Layout store не переносятся в controller.
- Сигналы runtime-компонентов forwarding-ятся синхронно. USB/preparation
  work остаётся queued; уже thread-safe generation/cancel gates закрываются
  direct. Перед продолжением после внешнего синхронного callback повторно
  проверяются актуальные generation, selection и gates.
- Перенесённые пользовательские строки сохраняют `DeviceManagerMessages`
  translation context; новые `.ts` entries из-за имени класса не появляются.

Альтернатива с переносом transport session FSM и printer keepalive в runtime
thread смешала бы policy с I/O и изменила порядок USB. Она не выбрана.
Выделение worker policy остаётся A8, protocol layers остаются A9.

#### Порядок событий и acceptance

1. На каждый принятый для обработки `snapshotChanged` controller выполняет
   ровно один increment, принадлежащий обработке snapshot. После prepared
   downgrade snapshot по-прежнему игнорируется. `currentEndpointRemoved`
   фиксирует removal evidence/count, но сам дополнительно не увеличивает
   generation. Повторный обычный rescan и чужое USB removal не перезапускают
   session; отдельные recovery/quiesce transitions сохраняют свои gates.
2. Forced same-path snapshot нельзя отбросить сравнением значений внутри
   controller: он обозначает новый endpoint даже при одинаковых path,
   identity и product. Старые queued callbacks и preparation results не
   активируют новую session и не продолжают старую operation.
3. Предварительное завершение/отмена operation на session event сохраняет
   A6-порядок. Structured Apply outcome публикуется до session-loss handling;
   `NotStarted`, `FinalizationUnknown` и `PartialOrUnknown` не меняют смысл.
4. Lost/recovery state разрешает восстановление только после текущего
   доказательства removal и проверки identity/product. Выбор Auto сам по
   себе не является доказательством physical reconnect.
5. Retry validation, restricted read-only session и promotion after proof
   сохраняют текущие ограничения. Controller не повторяет Upload, Apply,
   Delete либо Replace после неопределённого результата. Существующий
   подтверждённый overlay restoration остаётся отдельным session workflow.
6. Порядок `configure`, restore-overlay intent, start/bootstrap, mandatory
   keepalive и session-ready сохраняется. Foreground operation приостанавливает
   printer keepalive/metrics как прежде; Turris не получает PASE traffic.
7. Firmware lease видим до закрытия gate и queued quiesce. Подтверждение
   quiesce принимается только для exact lease/generation. Lease снимается
   после соответствующего release fence; stale result не снимает новую
   блокировку. Non-resuming release запрещает passive reconnect, explicit
   recovery acknowledgement дожидается fence.
8. Specifications/display/metrics инвалидируются при смене session и
   firmware/downgrade boundary с сохранением revision order и текущих
   unavailable/unsupported состояний. Новый SysConfig query не добавляется.
9. API version 8, Manager1/Manager2/Firmware1 signatures, capability tokens,
   operation string contracts, wire bytes и persistent schemas неизменны.
10. Shutdown прекращает session timers и закрывает generation gate до остановки
    consumers. Operation-owned paths освобождаются по A6-порядку после
    остановки preparer/worker; destructor controller не запускает новый work.

#### Этапы реализации и проверки

1. Зафиксировать точную event matrix до переноса: unchanged rescan, forced
   same-path event, отдельные Absent/Ready, Auto restart, disconnect,
   repeated recovery и firmware acquire/release. Дополнить существующий
   fake-monitor/USB harness проверками числа transitions и порядка сигналов.
2. Добавить `printersessioncontroller.h/.cpp` в runtime и существующий
   protocol-test qmake target. Перенести session state и getters; manager
   делегирует, authoritative копии состояния не остаётся.
3. Перенести discovery/attach/detach/connect/disconnect и generation/cancel
   boundary целиком. Сохранить monitor batching и legacy fallback policy.
4. Перенести worker result/session callbacks, specifications/display/metrics
   projections, overlay restoration и retry-gated session recovery.
5. Перенести firmware acquire/quiesced/release/acknowledgement и legacy
   keepalive. Подключить controller к downgrade и ordered shutdown.
6. Добавить structural baseline: controller включён в оба target, session
   authority удалена из manager, direct forwarding сохранён. Переадресовать
   private fixtures без удаления или ослабления существующих assertions.
7. После срезов запускать относящиеся к ним focused tests. Финальные gates:
   свежие runtime/test builds, полный `make package-check`, translation
   catalog, runtime-refactor baseline и `git diff --check`.

Обязательные существующие сценарии включают
`unrelatedUsbRemoveDoesNotRestartPaseSession`,
`samePathReenumerationCancelsOldGeneration`,
`lostPrinterSessionRequiresObservedRemovalBeforeReconnect`,
`productChangeDoesNotReuseSessionOrRecovery`,
`generationChangeWaitsForStructuredApplyOutcome`,
`runtimeDeviceSpecificationsCacheIsGenerationBounded`,
`restoredOverlayWaitsForKeepaliveBeforeSessionReady`,
`restoredOverlayFailureBecomesLostWithoutReplay`,
`foregroundOperationPausesMetricsAndKeepalive`,
`turrisWorkerSessionSendsNoPaseTraffic`,
`firmwareExclusiveGateRejectsUnresolvedDeviceState`,
`firmwareReleaseFenceWaitsForLateQuiesce` и
`firmwareRecoveryAcknowledgementWaitsForReleaseFence`.
Новые A7 tests должны проверять single ownership и синхронный manager façade,
точные generation deltas и reentrant disconnect/quiesce перед dispatch.

Hardware smoke для software-only extraction не требуется и не объявляется
выполненным. Физические queries/writes не входят в эту проверку.

#### Реализация и проверенный результат

- Добавлены `printersessioncontroller.h/.cpp` в runtime и protocol-test target.
  Controller единолично хранит session state, firmware lease/fence state,
  metrics/display/specifications projections и legacy keepalive timer.
  Manager читает const state view и делегирует session commands/events.
  Operation ledger и recovery decisions остаются в A6 coordinator.
- Общие lifecycle logging helpers вынесены в `printerlifecycle_p.h` без
  изменения формата сообщений и общего monotonic clock. Worker остаётся
  единственным исполнителем обычного transport I/O.
- `devicemanager.cpp` уменьшился с 6 956 до 5 743 строк, header с 904 до 869.
  Public declarations manager неизменны. Отдельная token-level сверка
  подтвердила неизменность тел 65 worker methods и 482 прежних test methods
  после нормализации перенесённых private fixtures.
- Characterization test сохраняет generation deltas для unchanged rescan,
  forced same-path event, отдельных Absent/Ready, Auto restart, disconnect
  и повторного recovery. Disconnect по-прежнему разрешён при firmware
  recovery interlock, который запрещает именно подключение.
- Новые reentrancy tests сначала воспроизвели stale dispatch после
  синхронного disconnect/quiesce. После внешних сигналов и callbacks
  controller повторно проверяет generation/gates. Вытесненный обработчик
  прекращает старый переход; teardown не перезаписывает новое подключение.
- Отдельная матрица воспроизвела release fence перед quiesce при синхронной
  отмене firmware acquire. Такой release теперь сохраняет pending lease,
  а его fence отправляется только после постановки quiesce в worker queue.
  Existing late-quiesce и recovery-acknowledgement tests также прошли.
- Чистые сборки runtime и protocol tests выполнены отдельно. Полный protocol
  suite: **893 passed, 0 failed, 0 skipped**. Итоговый `make -j4 package-check`
  завершился с exit 0; translation completeness, именованные QML/runtime
  suites, structural baseline и `git diff --check` прошли.
- Первый package gate и отдельный полный QML повтор выявили timing-sensitive
  сбой `MetricSelector::test_escapeCancelsAndRestoresOriginFocus` на клике.
  Изолированный test и весь его файл проходили. В тест добавлен bounded
  `waitForRendering` перед mouse input; существующие проверки открытия,
  Escape, неизменности выбора и возврата фокуса сохранены. После правки
  прошли два полных QML повтора и итоговый package gate. Production QML
  не менялся; существующие RU-only skips в запуске без перевода покрываются
  отдельными локализованными baseline checks.

Реализация A6 сохранена. На момент acceptance A7 commit/push не выполнялись;
по последующему запросу пользователя A6 и A7 фиксируются отдельными коммитами.
Push требует отдельного запроса.
Следующий архитектурный этап: A8; он не входит в завершённую реализацию A7.

#### Риски, откат и границы

Основной риск связан с signal order, reentrancy и stale context при переходе
через новый объект. Защита: один runtime thread, прямой forwarding, свежие
callbacks/contexts и проверяемая event matrix. Второй риск: потерять forced
same-path generation или ошибочно снять firmware lease до release fence.
Соответствующие focused tests обязательны до финального package gate.

Откат возвращает session methods/state в manager и удаляет controller wiring.
On-disk migration не нужна. Уже выполненный A6 сохраняется; его изменения
не должны быть потеряны или откатаны вместе с A7. Commit/push выполняются
только по отдельному запросу пользователя.

**Out of scope:** перенумерация публичного generation, автоматический USB reset
или mutation replay, новый reconnect/keepalive policy, worker/protocol split
A8/A9, дополнительные USB writers, новые hardware capabilities, firmware
download/flash features, schema/API migration, QML redesign, GIPHY и recorder.

### A8. Разделить legacy и printer-class worker policy

**Зависимости:** A7.

**Статус:** документ согласован 5 сентября 2026 года; реализация и software
acceptance завершены 6 сентября 2026 года. A8 закрыт без hardware qualification.

#### Подтверждённая база и цель

На базе `c841bf9` A6 и A7 уже вынесли operation coordination и runtime session
lifecycle из manager. Перед A8 `DeviceWorker` находился в `devicemanager.{h,cpp}`
и совмещал legacy `panorama::Device`, printer `PrinterProtocol`, четыре таймера,
printer FSM, metrics и немедленные cancellation/generation gates.

Legacy serial/ADB и printer-class code получают отдельные session/policy
объекты, сохраняя один контролируемый worker context и очередь I/O. Разделение
меняет владельцев реализации, но не протокол, режимы поддержки или retry policy.

#### Владение и границы

Названия ниже задают целевые внутренние модули, а не новый публичный API.

| Модуль | Собственное состояние и ответственность | Граница |
|---|---|---|
| `DeviceWorker` в `deviceworker.{h,cpp}` | Существующий QObject façade, очередь команд, немедленные gates, cancellation eventfds и общий quiesce | Единственная точка dispatch для обеих policy; существующие slots/signals и их порядок сохраняются |
| `LegacyDeviceSession` | `panorama::Device`, legacy handshake, команды serial/ADB и legacy metrics timer | Исполнение только в worker context; нет собственной очереди допуска или reconnect authority |
| `PrinterClassSession` | Один `PrinterProtocol`, printer FSM, keepalive/recovery/metrics timers, SystemMonitor, overlay и foreground state | Исполнение только в том же worker context; не владеет runtime generation или firmware lease из A7 |

Композиция сохраняет текущие различия протоколов без общей базовой policy,
plugin framework или второго worker thread. Каждая session имеет одного
владельца, все её QObject children перемещаются вместе с worker. Получатели
команд не обращаются назад к manager и не обходят очередь I/O.

`DeviceWorker` остаётся единственным владельцем атомарных generation/readiness
gates, cancellation set/mutex/eventfds и опубликованных presentation preferences.
Policy используют узкий заимствованный доступ к этому состоянию; второй копии
gates или независимого решения о допуске операции не появляется. Немедленные
thread-safe entry points не превращаются в queued calls: они должны прервать
уже ожидающий ответ I/O, не дожидаясь освобождения worker.

Общий quiesce останавливает обе policy и закрывает transport до существующего
подтверждения firmware handoff. При уничтожении worker все callbacks и transport
ожидания завершаются до закрытия cancellation fds и уничтожения shared context.
Точный порядок observable signals и fences сохраняется по текущему baseline.

#### Инварианты и критерии закрытия

- Один runtime I/O owner; две policy не создают двух независимых USB writers.
  Переключение legacy/printer, disconnect, downgrade и firmware quiesce проходят
  через существующий контролируемый dispatch.
- A6 operation ledger и A7 generation/session/lease authority не возвращаются
  в worker. API 8, identities, revisions, persistence и translations сохраняются.
- Passive printer worker не отправляет frames; foreground operation по-прежнему
  приостанавливает периодические metrics/keepalive. Отмена операции не отменяет
  независимое session recovery и не теряется при drain следующей команды.
- Same-path re-enumeration немедленно инвалидирует старый generation; stale
  completion не возобновляет timers и не снимает новый firmware fence.
- Сохранены DeviceInfo readiness, bootstrap-once, overlay activation, retry
  deadlines, Turris transfer-only policy и запрет автоматического mutation replay.

#### Последовательность реализации и проверки

1. Вынести worker façade в собственный модуль, обновить qmake wiring и tests,
   сохранив исходное поведение и один worker thread.
2. Отделить legacy session; проверить connect/disconnect, handshake, команды,
   metrics timer и общий transport quiesce через test doubles, без hardware I/O.
3. Отделить printer session; сохранить shared gates, signal forwarding и
   destruction order. Проверить обе policy в одном worker context, отсутствие
   I/O после quiesce и неизменность отмены уже заблокированного request.
4. Выполнить чистые runtime/test builds, полный protocol suite и `package-check`.
   В focused matrix обязательны passive/foreground, cancel/drain, same-path
   generation, восстановление overlay и late firmware quiesce/release fence.

Каждый проверенный срез сохраняется отдельно; commit/push выполняются только
по отдельному явному запросу пользователя.
Адаптация private test fixtures допустима, ослабление assertions, deadlines
или отключение тестов ради extraction запрещено. Закрытие A8 требует фактического
переноса policy state, а не только forwarding к прежнему общему worker body.

Первый срез выполнен: `DeviceWorker` перенесён в `deviceworker.{h,cpp}`;
сравнение с `2066d1a` подтвердило неизменность всего class declaration и всех
method bodies. Runtime и test build прошли, полный protocol suite дал
893 passed / 0 failed / 0 skipped, structural baseline прошёл. Появилась
отдельная source guard на worker module и qmake wiring; существующая проверка
`VerifyingSavedLayout` перенесена вслед за реализацией без ослабления.
Физический hardware I/O не выполнялся.

Разделение policy завершено в следующем срезе:

- `LegacyDeviceSession` владеет `panorama::Device` и legacy timer;
  `PrinterClassSession` владеет единственным `PrinterProtocol`, printer FSM,
  тремя timers, SystemMonitor, overlay и foreground state. В worker остаются
  прежние public slots/signals, синхронный dispatch и общий quiesce.
- `DeviceWorkerSessionContext` даёт printer policy только заимствованный доступ
  к gates worker. Atomics, cancellation set/mutex, два различных eventfd и
  published preferences не скопированы; cancel и generation publication
  по-прежнему прерывают уже ожидающий ответ I/O из другого потока.
- Legacy использует тот же SystemMonitor через borrowed reference; общий
  formatter выделен в `deviceworkermetrics`, второго collector нет. Sessions
  и все QObject children перемещаются с worker. Legacy уничтожается раньше
  telemetry provider, обе sessions уничтожаются до закрытия cancellation fds.
- Сравнение с `412efa0` подтвердило неизменность публичного façade и 61 method
  body после нормализации нового владельца, signal emitter и borrowed access.
  Остальные три метода изменены только на границах общего quiesce и двух gate
  queries. Translation contexts сохранены явным `DeviceWorker::tr`;
  существующие `QObject::tr` остались прежними.
- Новые PTY/socket fixtures проверяют legacy handshake/commands, закрытие
  обоих transports внутри firmware ACK callback, отсутствие записи из
  отложенного legacy callback после quiesce, один I/O context, teardown до
  закрытия gates, отмену blocked foreground response и stale completion без
  перезапуска timers. Независимое read-only review не нашло замечаний и
  подтвердило сохранение всех прежних test methods и assertions.
- Чистые runtime/test builds, полный protocol suite и fresh `package-check`
  прошли: 898 protocol cases, 0 failed, 0 skipped. Translation catalog и
  structural baseline прошли. Пять RU-only QML cases пропускаются в общем
  английском прогоне и отдельно успешно выполняются с русским каталогом.

Изменения этого продолжения проверены в worktree, без commit/push, установки,
перезапуска установленного runtime или обращений к физическому устройству.
A9 и B5 остаются следующими отдельными задачами.

Общий источник SystemMonitor уже использовался и legacy, и printer metrics.
При выделении policy legacy сохраняет заимствованный доступ к тому же provider
через worker wiring; отдельный telemetry collector не создаётся. QObject
parentage обязателен для переноса обеих session и timers вместе с worker,
см. [Qt QObject thread affinity](https://doc.qt.io/qt-6/qobject.html#thread-affinity).

Основной риск: lifetime, thread affinity и порядок событий при переходе через
новые объекты. Его проверяют behavioral fixtures и teardown/reentrancy cases.
Откат ограничен коммитами A8, сохраняет A6/A7 и не требует on-disk migration.

**Out of scope:** новые capabilities, USB reset, новые retries, firmware writes,
protocol split A9, изменение runtime API, UI redesign и физическая qualification.

### A9. Разделить PrinterProtocol по слоям

**Зависимости:** A7; в согласованной последовательности выполняется после A8
и остаётся последним архитектурным этапом.

**Статус:** реализовано 6 сентября 2026 года. Чистые runtime/test builds,
902 protocol tests, полный `package-check`, translation и structural baseline
прошли. Независимое read-only review не выявило high/medium замечаний.
A9 закрыт как software-only refactor; hardware qualification не выполнялась.

#### Подтверждённая база и цель

На базе `c841bf9` в `printerprotocol.cpp` ещё находятся discovery, udev monitor,
native/scripted libusb backend, `LibusbAsyncTransport`, frame codec, общий
`PrinterProtocol::Impl` и PASE/Turris workflows. `turrismediaformat.{h,cpp}` уже
владеет MXHD container write/validation и переиспользуется без второго формата.

Стабильный façade `PrinterProtocol` сохраняет используемые методы, nested value
types, profiles и test entry points. Runtime API 8 и вызывающий worker не должны
зависеть от новых внутренних transport/client классов.

#### Целевые слои

| Слой | Владение | Не входит в ответственность |
|---|---|---|
| `UsbPrinterTransport` | Один libusb handle/claimed interface, byte I/O, native/test backend, async transfer lifetime, cancel wakeup и transport failure latch | Frames, track IDs, protobuf payload и model policy |
| `PrinterFrameCodec` | Stateless frame encode/decode и существующий payload limit | Transport, запросы к устройству и session lifecycle |
| Frame/transaction channel | Единственный transport, receive buffer, track IDs, response matching, transaction deadlines, optional-response drain и outbound activity clock | Выбор product capabilities и пользовательского media/config workflow |
| Discovery и `PrinterDeviceMonitor` | Passive enumeration, sysfs/udev identity, monitor fd/notifier и publication snapshot | Открытие USB transport или protocol OUT |
| PASE config/media client | Подтверждённые PASE/PANORAMA readiness, config, overlay, metrics, media и readback workflows | Собственный transport, очередь или независимая нумерация транзакций |
| Turris media client | Подтверждённый transfer-only workflow с существующим MXHD helper | PASE bootstrap, catalog, brightness, metrics, presets или новые capabilities |

Façade владеет одним channel и model clients; clients заимствуют channel на
срок, меньший его lifetime. Transport принадлежит channel и живёт в том же I/O
context, что A8 worker. Codec и общие value types имеют по одному определению;
source compatibility существующих имён сохраняется через заголовки/aliases.
Общие model-neutral transfer helpers переиспользуются без копирования одной
upload/ack последовательности в каждый client и без второго I/O owner.

Model clients возвращают прежние typed результаты. Channel сохраняет различие
NotSent, Cancelled, PartiallySent, AcknowledgementTimeout, TransportFailure,
InvalidResponse, SentOutcomeUnknown, Rejected и Acknowledged. Преобразование
в публичный MutationOutcome остаётся прежним, включая FinalizationUnknown
и PartialOrUnknown; потеря final ACK не становится разрешением повторить запись.

#### Инварианты и критерии закрытия

- Wire fixtures остаются побитно совместимыми: payload, frame boundaries,
  tracked/untracked requests, response validator и profile-specific traffic.
- Сохраняются fragmented/concatenated/malformed frames, bounds receive buffer,
  wrong-track rejection, optional ACK/Pong drain и строго ограниченный fallback
  unframed response после transport error.
- Partial write, cancel, timeout и неизвестный исход не смешиваются. In-flight
  keepalive и FileTransmit boundary deadlines не меняют текущую последовательность.
- Одно USB claim на connection epoch; no-op display activation close остаётся
  no-op. IN transfer policy, cancellation teardown и persistent input failure
  latch сохраняются, включая scripted backend tests.
- Monitor устанавливается до первого rescan, отличает физический remove/add
  от собственных bind/unbind, сохраняет forced same-path snapshot и fail-closed
  MonitoringUnavailable. Monitor никогда не выполняет I/O за model client.
- Turris upload не получает PASE traffic, catalog reconciliation или retransmit
  после lost final ACK. Проверка MXHD до USB сохраняется.

#### Последовательность реализации и проверки

1. Вынести profiles/discovery/monitor и frame codec с текущими fixtures.
2. Отделить byte transport и его native/scripted backend; сохранить fd test seam,
   claim lifetime и существующие failure/cancel scenarios.
3. Выделить один transaction channel из `Impl`, проверив response matching,
   partial sends, drain, cancellation и outcome mapping до переноса clients.
4. Отделить PASE и Turris workflows; façade только выбирает профиль/делегирует,
   а mutable transport/channel state не остаётся общим скрытым model `Impl`.
5. Проверить qmake runtime/test wiring, чистые builds, весь protocol suite,
   translation/structural gates и полный `package-check`. Старые fixtures
   остаются обязательными; новые тесты проверяют реальные границы владения.

Каждый слой проверяется отдельно до следующего переноса; commit/push требуют
отдельного запроса пользователя и в этом продолжении не выполняются.
Основные риски связаны
с lifetime callbacks, потерей latched error или изменением ambiguous-outcome
semantics. Для каждого переноса обязательны focused tests до следующего слоя.
Откат выполняется в обратном порядке переносов A9, сохраняя A8 и A6/A7;
on-disk migration не требуется.

#### Реализованные границы, 6 сентября 2026 года

- `printerproductprofile`, `printerdiscovery` и `printerframecodec` содержат
  единственные реализации профилей, discovery/monitor и stateless framing.
  Публичные nested types и `PrinterDeviceMonitor` остаются доступны через
  прежний `printerprotocol.h`; второй набор wire/value types не создан.
- `UsbPrinterTransport` владеет native/scripted backend, async callback state,
  claim и fd test seam. Cancellation fd только заимствуется. Persistent latch
  сохраняется при close; bounded cancellation drain и abandoned-callback
  safety path перенесены без изменения policy.
- `PrinterTransactionChannel` владеет transport, receive buffer, track counter,
  deadlines, response matching/drain и outbound clock. PASE readiness/retry
  state и media-pull limits из channel убраны; in-flight keepalive получает
  frame через указатель на статическую функцию, без захвата lifetime клиента.
- `PaseConfigurationClient`, `PaseMediaClient` и `TurrisMediaClient` заимствуют
  один channel и уничтожаются раньше него. `printermediaupload` содержит один
  model-neutral upload/ACK цикл; Turris передаёт MXHD validator и fixed track
  ID, а capability/name preflight остаётся перед открытием source и USB I/O.
- Добавлены прямые owner-boundary tests и structural guards. Existing wire,
  outcome, cancellation, readiness, media и udev fixtures не ослаблялись.

#### Итог проверки

- `PrinterProtocol` сокращён с 7528 до 307 строк реализации; façade хранит
  один channel и три borrowed-channel clients, без скрытого model `Impl`.
- Focused gates выполнены после каждого переноса: codec/discovery, duplex
  transport, fd ownership, response matching/deadlines, readiness/media и
  Turris/PASE upload outcomes. Новая проверка non-copyable owners прошла
  RED/GREEN; close/destructor и shared-buffer fixtures проходят напрямую
  через новые классы.
- После clean build полный `printerprotocol-tests`: 902 passed, 0 failed,
  0 skipped. Все 488 исходных тестовых методов сохранены без изменения тела
  после нормализации только A8 ownership access paths.
- Runtime, CLI и Quick собраны; итоговый `make -j2 package-check` завершился
  с кодом 0. Пять RU-only QML cases пропущены в английском запуске и отдельно
  проверены русским baseline. Translation completeness и все structural
  invariants прошли; `git diff --check` чист.
- Предыдущие A8 edits сохранены. Установка, restart установленного service,
  hardware I/O/qualification, commit и push не выполнялись. Следующий
  согласованный пункт вне A9: B5; он этим срезом не реализуется.

**Out of scope:** новая wire protocol revision, API 9, USB reset/retry redesign,
новые model capabilities, второй transport writer, новые библиотеки/стек,
hardware queries/writes или qualification неизвестных профилей.

## Workstream B: Linux Settings и диагностика

| ID | Функция | Зависимости | Acceptance |
|---|---|---|---|
| B0 | API 8 capability handshake | A1 | Зафиксированы API 8 wire и behavioral baseline; старый API 8 без `GetRuntimeCapabilities()` сохраняет baseline-функции и получает пустой набор только при `UnknownMethod`; остальные ошибки fail closed; device capabilities привязаны к identity, connection revision и physical generation; неизвестные и malformed tokens не включают функции; API остаётся 8 |
| B1 | Реальная модель и live version | A5 | Без изменения API 8 Settings публикует уже доступные product ID, mapped model, firmware и app version; serial/chip ID не экспортируются открыто |
| B2 | °C/°F и 12/24H | B0, B1 | Dashboard и runtime overlay используют одну persisted setting; timezone остаётся системным; runtime работает после закрытия GUI |
| B3 | Redacted support bundle | A5, B0, B1 | Локальный JSON не больше 1 MiB содержит host/app/runtime versions, безопасный runtime snapshot и bounded typed lifecycle events текущего запуска; raw journal, environment, usernames/home paths, serial, chip ID, operation/media identifiers и содержимое media не экспортируются |
| B4 | Close behavior | нет | Пользователь выбирает Hide to tray или Quit GUI; Hide недоступен без StatusNotifier host; runtime не останавливается |
| B5 | GitHub release notification | нет | Тихая проверка при старте основного GUI и каждый час, включая tray; без opt-in, progress UI и видимых сетевых ошибок; только более новая стабильная версия, одно уведомление о той же версии за запуск и ссылка на релиз; без self-update, фоновой сети в runtime и restart active runtime |
| B6 | Remote firmware availability research | B1 | Сначала определить официальный source, authenticity/signature, compatibility manifest и rollback contract; до отдельного proposal нет remote download, update badge или flash |
| B7 | Legal/About links | нет | Показываются только существующие project License, Privacy или User Agreement URLs; отсутствующая политика не выдумывается |
| B8 | GUI autostart option | B4 | Фоновый systemd service и запуск GUI разделены; состояние доступно и обратимо через user session |
| B9 | Полный NVIDIA telemetry backend | A1 | Stable per-GPU identity, temperature, usage, frequency, power и VRAM публикуются только при реальном источнике; bounded timeout; отсутствие driver/tool даёт unavailable, а не ноль; dual-GPU ordering тестируется |
| B10 | Versioned Device Specifications contract | B0, A1, B1 | Для известных `1011/1021` декодируется и кэшируется уже полученный bootstrap SysConfig response без второго USB query; geometry и подтверждённые fields публикуются новым versioned method и новым wire type; дополнительный query OUT допустим только для исследуемого профиля после D1, подтверждения transport и явного согласия |
| B11 | Дополнительные локализации | нет | Каждый язык имеет отдельный Qt translation catalog, native-speaker review основных workflow и fallback на English; machine-only перевод не объявляется полноценной локализацией |
| B12 | Local headless CLI | A5, B0, B3 | Команда `tryx` не требует display и работает в локальном терминале того же Linux-хоста через существующую user-session D-Bus; inspection-срез read-only, а последующие state-changing команды требуют отдельного versioned capability contract; сетевой listener и удалённый D-Bus не добавляются; remote terminal qualification вынесена в backlog |

### B0. Acceptance capability handshake

- `tryxRuntimeApiVersion()` остаётся равным `8`.
- Golden tests фиксируют signatures всех существующих API 8 structs, methods
  и signals, а также их revision и error semantics.
- Runtime capabilities публикуются отдельным additive method, а device
  capabilities отдельным versioned method с identity, connection revision и
  physical generation.
- Старый API 8 без capability method остаётся совместимым и сохраняет весь
  baseline UI. Только `UnknownMethod` означает пустой post-v2.2 capability
  set; timeout, disconnect, access denied и invalid signature fail closed.
- Capability reply привязан к тому же unique D-Bus owner и service epoch, что
  API handshake; stale reply после owner change отбрасывается.
- GUI хранит отдельное `capabilitiesReady`: отсутствие или ошибка новых
  capabilities не превращает совместимый API 8 baseline в incompatible, а
  optional signals подключаются только после успешного handshake.
- Capability tokens точные, case-sensitive, versioned и bounded по числу и
  длине. Неизвестные или malformed tokens игнорируются.
- Device capabilities инвалидируются при disconnect, смене identity или
  generation. GUI использует их только как presentation gate, а runtime снова
  проверяет product profile, generation и capability перед mutation.
- Новый GUI с runtime `v2.2.0` и старый GUI `v2.2.0` с новым runtime сохраняют
  baseline-функциональность.
- Ни один существующий positional API 8 type не получает новых полей.

#### Выбранный контракт, 28 августа 2026 года

**Статус:** выполнено 28 августа 2026 года.

API остаётся равным `8`. `Manager1`, все существующие методы, сигналы и
positional-типы `Manager2` не меняются. В существующий
`org.tryx.Panorama.Manager2` добавляются только два новых метода:

- `GetRuntimeCapabilities() -> as`;
- `GetDeviceCapabilitiesV1() -> (usttas)`.

Второй метод возвращает новый frozen tuple
`TryxRuntimeDeviceCapabilitiesV1`: `schemaVersion`, `deviceIdentity`,
`connectionRevision`, `physicalGeneration`, `capabilities`. Поля существующих
tuple не расширяются. Отдельный D-Bus interface не используется: новый GUI
должен отличать старый совместимый runtime именно по
`org.freedesktop.DBus.Error.UnknownMethod`; `UnknownInterface` не является
разрешённым legacy-fallback.

Runtime публикует токен `runtime.device-capabilities.v1`. Device reply
использует только точные profile-derived токены:

- `device.media-upload.v1`;
- `device.media-catalog.v1`;
- `device.display-configuration.v1`;
- `device.overlay-metrics.v1`;
- `device.firmware-flash.v1`.

Токены case-sensitive, не нормализуются и не обрезаются. Клиент рассматривает
не более первых 32 элементов, каждый длиной не более 64 символов; duplicates,
unknown и malformed значения не включают capability. Сервер строит device
набор только из текущего `PrinterProductProfile`. Marketing name, firmware
version и наличие элемента UI capability не открывают.

#### Handshake и invalidation

1. GUI получает unique owner well-known runtime service и отправляет
   `GetRuntimeApiVersion()` этому unique owner.
2. Ответ принимается только для того же `serviceEpoch` и всё ещё текущего
   unique owner. Версия обязана быть ровно `8`.
3. Совместимый API 8 немедленно запускает существующий baseline refresh;
   capability negotiation не блокирует B1/B4/B7/B8 и существующий media flow.
4. `GetRuntimeCapabilities()` вызывается у того же unique owner.
   `UnknownMethod` завершает handshake как `capabilitiesReady=true` с пустым
   post-v2.2 набором. Timeout, disconnect, access denied, invalid signature и
   любая другая ошибка оставляют `capabilitiesReady=false` и пустой набор, но
   не превращают уже подтверждённый API 8 в incompatible.
5. `GetDeviceCapabilitiesV1()` вызывается только после успешного runtime
   handshake и наличия `runtime.device-capabilities.v1`.
6. Device reply принимается только при `schemaVersion == 1`, непустой exact
   identity, exact `connectionRevision`, ненулевой `physicalGeneration` и
   совпадении с текущим printer-class connection snapshot. Иначе он
   отбрасывается fail closed.
7. Disconnect, service owner change, новая connection revision или identity
   очищают device capability state. Для новой revision выполняется новый
   bounded local D-Bus read; stale reply прежней revision или generation не
   включает UI.

`capabilitiesReady`, `deviceCapabilitiesReady`, отфильтрованные runtime/device
списки и exact-query helpers доступны QML как presentation gates. Runtime
остаётся окончательным policy owner и повторно применяет существующие
product/generation проверки перед каждой mutation.

#### Этапы реализации и проверки

1. Зафиксировать API 8 golden baseline для прямых методов и сигналов обоих
   adaptors, а также точные D-Bus signatures существующих tuple.
2. Добавить новый device-capability tuple, регистрацию metatype и D-Bus
   round-trip test.
3. Добавить read-only `DeviceManager` snapshot, profile mapping и два additive
   метода `Manager2`, не выполняющие USB I/O.
4. Добавить owner-bound client state machine и RED-тесты для нового runtime,
   legacy `UnknownMethod`, non-legacy error, malformed/unknown tokens, stale
   epoch, identity/revision mismatch и invalidation.
5. Выполнить focused protocol/Quick suites, runtime baseline, translation
   check и полный `package-check`; затем провести независимое read-only review.

Acceptance требует, чтобы старый API 8 сохранил baseline UI, новый API 8
опубликовал только известные capability, stale/malformed ответы остались
закрытыми, а introspection не показал изменения существующих signatures.
Hardware smoke не требуется: B0 не добавляет USB query, write или новый
device-side protocol.

#### Риски, откат и out-of-scope

- Главный compatibility-риск: случайно изменить существующий positional tuple
  или считать `UnknownInterface` разрешённым legacy-runtime. Это блокирует
  acceptance и ловится golden/introspection tests.
- Главный race-риск: принять capability от сменившегося well-known owner либо
  от старой connection revision. Unique-owner destination и exact epoch,
  identity, revision и generation binding обязательны.
- Откат состоит в удалении двух additive методов, нового tuple и client gates;
  API version, persistence, Manager1 baseline и USB bytes остаются прежними.
- B0 не включает B2/B3/B9/B10/C12, новые optional mutations, новый сигнал,
  `Manager3`, persistence migration, network, firmware или hardware testing.

#### Результат реализации, 28 августа 2026 года

- API остался равным `8`. В `Manager2` аддитивно добавлены
  `GetRuntimeCapabilities()` и `GetDeviceCapabilitiesV1()` с новым frozen
  tuple `(usttas)`; signatures ранее опубликованных методов, сигналов и
  positional types сохранены golden-тестами.
- Runtime строит bounded allowlisted capability-набор из текущего
  `PrinterProductProfile` без USB I/O. GUI выполняет handshake с exact unique
  owner, принимает только точный legacy `UnknownMethod`, а остальные ошибки,
  malformed tokens и stale owner/device replies оставляет fail closed.
- Device gates привязаны к identity, connection revision и physical
  generation. Disconnect, новая revision, существующий
  `PrinterOperationsCancelled` и повторная service registration немедленно
  инвалидируют старое состояние и fenced pending replies.
- Полный `package-check` прошёл: 782 protocol, 17 replace-journal, 84 QML,
  96 Quick, 37 runtime-bootstrap, 17 capability-handshake и 20 tray тестов;
  также прошли `qmllint`, translation completeness, runtime characterization
  baseline и offscreen smoke. Независимое повторное code review завершилось
  вердиктом APPROVE / GO.
- Hardware smoke не выполнялся и не требуется для B0: реализация не добавляет
  device-side protocol, USB query или mutation.

### B1. Реальная модель и live version

**Статус:** выполнено 26 августа 2026 года. Settings показывает live product
ID, модель, firmware и app version из уже существующего API 8 connection
snapshot. Product ID `1021`, `1011` и `2011` отображаются как `PANORAMA SE`,
`PANORAMA` и `TURRIS 620`; legacy-коды `cm01` и `cm01_se` также отображаются
как `PANORAMA SE`. Неизвестная модель или отсутствующая версия честно
показываются как `Not reported`/`Не указано`.

Для printer-class bootstrap используются `firmwareVersion` и `appVersion` из
уже полученного `DeviceInfo`: второй USB query не добавлен. Автоматический путь
публикует только эту безопасную проекцию, проходит existing physical-generation
fence и не испускает detailed `DeviceInfo` с serial/chip ID. Новая physical
generation той же device identity сначала очищает версии, поэтому неуспешный
повторный bootstrap не оставляет в UI устаревшие значения. Изменение snapshot
не меняет семантику замороженных Manager1 signals: новый `RuntimeClient`
перечитывает snapshot по уже существующим lifecycle events, включая
`MediaListUpdated` на generation boundary и `UploadStatus` при неактивной
printer-session. Новый D-Bus member не добавлен. Существующий explicit
diagnostic request и API 8 wire shape не менялись. `RuntimeClient` не
экспортирует serial или chip ID в QML.

Версия самого Linux-менеджера по-прежнему находится отдельно в About. Полный
software `package-check` прошёл: 778 printer protocol cases, 17 replace
journal, 32 QML, 44 Quick client, 22 runtime bootstrap и 11 tray. Hardware
smoke не выполнялся; B1 не добавляет USB-команд и не меняет их wire bytes.

### B2. Runtime-owned °C/°F и 12/24H

**Статус:** выполнено 28 августа 2026 года. Software acceptance и три
независимых read-only review завершены с вердиктом GO.

Dashboard и PASE overlay используют одну подтверждённую настройку, которой
владеет отдельный runtime. `SystemMonitor` продолжает публиковать
физические значения в градусах Цельсия, а источником даты, времени и timezone
остаётся `QDateTime::currentDateTime()` с системным локальным часовым поясом
Linux. Отдельная timezone-настройка, новая clock-карточка Dashboard и
device-side синхронизация часов в B2 не входят.

#### Выбранный API 8 контракт

API остаётся равным `8`. `Manager1`, все ранее опубликованные методы, сигналы
и positional-типы `Manager2` не меняются. Runtime аддитивно публикует exact
case-sensitive capability `runtime.presentation-preferences.v1` и новый
frozen tuple `TryxRuntimePresentationPreferencesV1` с D-Bus signature
`(utss)`:

1. `schemaVersion`, всегда `1`;
2. `revision`, ненулевая owner-local revision;
3. `temperatureUnit`, только `Celsius` или `Fahrenheit`;
4. `timeFormat`, только `24H` или `12H`.

В `org.tryx.Panorama.Manager2` добавляются:

- `GetPresentationPreferencesV1() -> (utss)`;
- `SetPresentationPreferencesV1(t expectedRevision, s temperatureUnit,
  s timeFormat) -> (utss)`;
- `PresentationPreferencesChangedV1((utss))`.

Setter использует compare-and-swap. Для реального изменения
`expectedRevision` обязан совпасть с текущей revision. Повтор той же пары
возвращает текущий snapshot без записи, увеличения revision и сигнала.
Runtime сначала атомарно сохраняет обе настройки, затем меняет память,
увеличивает revision, обновляет worker-local snapshot и испускает сигнал.
Ошибки имеют отдельные имена:

- `org.tryx.Panorama.Error.InvalidPresentationPreferences`;
- `org.tryx.Panorama.Error.PresentationPreferencesConflict`;
- `org.tryx.Panorama.Error.PresentationPreferencesPersistenceFailed`.

Preference revision не связана с USB connection revision или physical
generation. После рестарта runtime она может снова начинаться с `1`, потому
что новый unique D-Bus owner образует внешний epoch fence.

#### Persistence и совместимость

Runtime является единственным writer отдельного versioned файла
`runtime-presentation-preferences.json` в
`panorama::sharedApplicationDataLocation()`. Общий GUI/runtime `config.json`
не расширяется: его существующие GUI load-modify-save операции создали бы
lost-update race со вторым writer.

Формат persistence версии `1` содержит только обе allowlisted строки и
записывается одним `QSaveFile` без direct-write fallback. Семантика загрузки:

- отсутствующий файл означает `Celsius` и `24H`, revision `1`; файл не
  создаётся до первого реального изменения;
- корректный файл загружает обе настройки как одну пару;
- malformed, oversized, future-version, symlink, hardlink, FIFO, чужой owner
  или иной unsafe path дают совместимые defaults, отключают запись до
  рестарта и оставляют предупреждение в журнале;
- данные не мигрируются из PASE overlay или с устройства. Behavioral migration
  уже совпадает с прежними hardcoded `Celsius` и `HH:mm`.

Старый совместимый API 8 без capability продолжает показывать Dashboard в
`°C`; controls в Settings видимы, но отключены с понятным объяснением.
Ошибка нового getter не делает базовый API 8 incompatible и не разрешает
setter.

#### Fencing клиента и UI

GUI вызывает getter только после точного capability handshake и только у того
же unique owner. Getter, setter reply и optional signal принимаются лишь при
совпадении `serviceEpoch`, `handshakeAttempt`, unique owner, schema и exact
enum values. Optional signal подключается к unique owner после capability
handshake, а при смене owner отключается. Меньшая revision игнорируется;
равная revision с другой парой считается protocol violation.

В один момент допускается только один setter. ComboBox не меняет
подтверждённое состояние оптимистично. Conflict, timeout или disconnect не
повторяют mutation автоматически: клиент выполняет новый read-only getter и
сообщает пользователю, что исход операции нужно подтвердить. Последнее
подтверждённое значение остаётся доступным до owner invalidation.

Settings получает две строки в General:

- `Temperature unit`: `Celsius (°C)` или `Fahrenheit (°F)`;
- `Time format`: `24-hour` или `12-hour`.

Controls отключены до валидного snapshot и во время сохранения, имеют
видимые подписи, `Accessible.name`, `Accessible.description`, keyboard
navigation и текстовое объяснение недоступности. При ширине 760 px они не
выходят за `settingsContent`. Dashboard форматирует raw Celsius через общий
pure formatter RuntimeClient; недоступный sensor по-прежнему показывает
`—`. Канонический metric token остаётся `Date&Time`, включая его
пользовательскую подпись в Panorama.

#### Runtime formatting и граница USB

Общий formatter использует:

- для Celsius берёт raw Celsius, для Fahrenheit ровно один раз вычисляет
  `F = C * 9 / 5 + 32`, после чего округляет ближайшим целым с половиной от
  нуля; предварительного округления Celsius нет;
- единицы `°C` или `°F`;
- `HH:mm` для `24H`;
- `h:mm AP` для `12H`;
- `QLocale::ShortFormat` для даты.

PASE initial layout, display keepalive/overlay rebuild и каждый штатный
metrics batch получают текущий worker-local snapshot. Изменение настройки само
по себе не начинает USB mutation и не конкурирует с foreground operation.
Если foreground operation активна, первый уже предусмотренный batch после её
завершения использует новую пару. После закрытия GUI отдельный runtime
продолжает обновлять PASE overlay.

Legacy serial/ADB намеренно остаётся на прежнем Celsius contract. Его
`send_full_config(..., "Celsius")` является отдельной device-side mutation с
неподтверждённой conversion/readback semantics. B2 не вызывает
`set_temperature_unit`, не меняет legacy wire bytes и не расширяет hardware
scope.

#### Этапы реализации и acceptance

1. Зафиксировать RED-тестами новый tuple `(utss)`, additive Manager2 manifest,
   capability filtering, strict значения, formatter и store failure modes.
2. Реализовать отдельный atomic store, authoritative snapshot и CAS mutation
   в `DeviceManager`; persistence failure не меняет state, revision, worker
   или signal count.
3. Передать snapshot в worker/PASE initial и periodic paths без новой USB
   команды и без изменения raw `SystemMonitor` или legacy transport.
4. Добавить owner-fenced getter, setter и optional signal в RuntimeClient;
   проверить stale owner/reply/signal, conflict и unknown outcome без
   автоматического retry.
5. Подключить Settings и Dashboard, переводы, accessibility и narrow-layout
   проверки.
6. Выполнить focused protocol/Quick/QML suites, runtime baseline, translation
   check, полный `package-check` и независимое read-only review.

Acceptance требует одновременно:

- API остаётся `8`, а signatures всех старых членов и tuple не меняются;
- четыре пары предпочтений переживают restart runtime одним atomic snapshot;
- `0°C = 32°F`, `-40°C = -40°F`, `100°C = 212°F`, а
  unavailable sensor не превращается в ноль;
- `00:05` и `13:07` отображаются как `00:05`/`13:07` для `24H` и
  `12:05 AM`/`1:07 PM` для `12H`, а `12:05` как `12:05 PM`, при системной
  local timezone;
- Dashboard, initial PASE layout и daemon metrics batch используют одну
  подтверждённую пару;
- malformed persistence, stale revision/owner и setter failure остаются fail
  closed;
- hardware smoke не требуется: B2 не добавляет USB query, отдельную mutation
  или новый device-side protocol.

#### Результат реализации, 28 августа 2026 года

- API остался равным `8`. Runtime публикует capability
  `runtime.presentation-preferences.v1`, frozen tuple `(utss)`, owner-local
  CAS setter и optional signal через `Manager2`; старые signatures и default
  wire fixtures сохранены golden-тестами.
- Единственный writer хранит пару в отдельном atomic
  `runtime-presentation-preferences.json`. Malformed и unsafe state отключает
  запись до рестарта; group/other-writable paths отклоняются, а atomic commit
  привязан к проверенному directory descriptor. Persistence failure и
  исчерпание revision не публикуют новое состояние, worker snapshot или signal.
- Dashboard, PASE initial layout и daemon periodic batch используют общий
  formatter и подтверждённую пару. Raw telemetry и legacy serial/ADB остаются
  в Celsius, а недоступный sensor не превращается в числовое значение.
- RuntimeClient выполняет capability-gated getter/setter, не применяет
  optimistic state и после неизвестного результата делает только read-only
  reconciliation. Getter, setter и signal fenced по epoch, handshake и exact
  unique owner; signal дополнительно проверяет фактического D-Bus sender из
  `QDBusContext`.
- Settings получил доступные с клавиатуры переключатели Celsius/Fahrenheit и
  24-hour/12-hour, disabled fallback для старого runtime и русские переводы.
- Fresh `package-check` прошёл: 790 printer protocol, 17 replace journal, 87
  Material QML, 96 Quick client/model, 37 runtime bootstrap, 24 capability
  handshake и 20 Linux tray тестов. Также прошли `qmllint`, translation
  completeness, runtime characterization baseline и offscreen GUI smoke.
- Три независимых read-only review завершились вердиктом GO после отдельных
  regression-проверок stale signal и worker publication race. Hardware/USB
  smoke не выполнялся и для B2 не требуется.

#### Риски, откат и out-of-scope

- Главный persistence-риск: добавить поля в общий `config.json` и получить
  два writer. Отдельный runtime-owned файл является обязательной границей.
- Главный race-риск: оптимистично принять setter либо signal от старого owner.
  CAS, unique-owner destination и epoch/revision fencing обязательны.
- Главный device-риск: распространить host preference на неподтверждённый
  legacy temperature command. Этот transport исключён из B2.
- Откат удаляет capability, новый tuple/methods/signal, отдельный store и UI
  controls. Defaults возвращают прежние `Celsius` и `HH:mm`; API 8,
  `config.json`, raw telemetry и default wire fixtures остаются совместимыми.
- При default `Celsius/24H` существующие wire fixtures остаются
  byte-identical. Non-default preference намеренно меняет только текст
  value/unit/time в существующем initial RunConfig и
  `BatchGroupLabelUpdatePb`; новый USB command, query, envelope или device
  configuration не добавляется, а setter сам не начинает USB write.
- B2 не включает timezone selector, новую Dashboard clock, sensor backend,
  device clock write, B3/B9/B10, remote update или hardware testing.

### B3. Redacted support bundle

**Статус:** выполнено 30 августа 2026 года. Software acceptance, полный
`package-check` и три независимых read-only review прошли; hardware smoke для
этого cached/read-only среза не требовался.

**Цель:** дать пользователю один читаемый локальный JSON для issue или ручной
передачи разработчику. Экспорт не должен сам загружать файл, выполнять USB
query, читать device logs или раскрывать идентификаторы пользователя,
устройства, операций и media.

#### Контракт runtime

- API остаётся 8. Runtime публикует additive capability
  `runtime.support-snapshot.v1` и новый versioned method
  `Manager2.GetSupportSnapshotV1()`; существующие tuples, methods и signals не
  расширяются и не меняют semantics.
- Snapshot строится только из уже закэшированного состояния. Дополнительный
  USB OUT, `RequestDeviceInfo`, `FilePull` или другая hardware-команда
  запрещены.
- Разрешены: schema/runtime/API versions, product ID и mapped model, firmware
  и device-app versions, connection/session booleans, connection revision,
  physical generation, recovery flags и только количественные store/operation
  summaries.
- Не больше 32 последних операций представляются только категориальными
  полями: kind, state, stage, error category, terminal outcome, retry mode,
  attempt и apply-after-upload. Operation ID, subject, filename, media name,
  path, SHA-256, byte counts и free-form error text запрещены.
- Ответ строго валидируется GUI. Malformed, oversize или stale-owner reply не
  добавляется в report. На старом runtime без capability экспорт остаётся
  доступным как host-only report со статусом `unsupported`.

#### Безопасные lifecycle events

- Runtime ведёт отдельное in-memory кольцо событий текущего запуска. В него в
  момент события копируются только известный event enum и allowlisted
  enum/numeric/boolean/timestamp fields. Оно не строится из `MESSAGE`, stderr,
  `journalctl` или journal metadata и не сохраняется на диск.
- Существующий human-readable журнал продолжает работать независимо, но его
  строки никогда не попадают в bundle. Regex-redaction и fallback на raw data
  запрещены.
- Лимиты применяются до сериализации: не больше 256 последних полных событий,
  16 полей на событие, 256 KiB для всего runtime JSON и 24 часа по timestamp.
  Unknown event/field/value и control/bidi characters приводят к пропуску
  значения, а не к копированию или усечению исходного текста.
- Serial, chip ID, sysfs/device paths, username, home path, environment,
  secrets, configuration JSON, operation/artifact IDs, media metadata и
  свободный diagnostic text запрещены независимо от их внешнего вида.

#### Host report и экспорт

- Host section содержит application version, distribution, kernel,
  architecture, Qt version и текущий Qt platform `wayland`/`xcb`; environment
  и полный process command line не читаются.
- Итоговый canonical JSON ограничен 1 MiB. Имя создаёт приложение. Файл
  создаётся с mode `0600`, без следования symlink, без FIFO и без перезаписи
  существующего файла; hardlink/no-overwrite races завершаются ошибкой.
- Export запускается только явным действием пользователя и не выполняет
  network upload. Успех сообщает точный локальный путь, ошибка остаётся
  понятной и не раскрывает собранные данные.

#### Acceptance и out-of-scope

- Canary tests доказывают отсутствие `/home/user`, serial, chip ID, media
  names, operation/artifact IDs, hashes и secret-like значений во всём JSON.
- Покрыты stale owner, unknown capability, malformed/oversize reply,
  overflow/age bounds кольца событий, неизвестные lifecycle fields, symlink,
  FIFO, hardlink race и no-overwrite.
- QML tests покрывают ready, exporting, partial/unsupported, success и error,
  включая keyboard/accessibility path.
- B3 не включает remote support session, telemetry upload, crash dump,
  environment dump, device log download, firmware/update check, hardware
  evidence queries или автоматическое открытие issue.

Runtime публикует additive `runtime.support-snapshot.v1` при неизменном API 8.
Snapshot сериализует только cached state и отдельное typed in-memory кольцо;
GUI валидирует exact schema, типы, размер и unique-owner epoch, различает
`available`, legacy `unsupported` и runtime `unavailable`. Malformed,
oversize, stale-owner или failed runtime reply не создаёт файл.

Settings получил folder-only picker без поля имени и overwrite. Новый файл
публикуется с mode `0600` относительно pinned directory descriptor после
покомпонентного `openat(O_NOFOLLOW)` walk, `fsync` и
`renameat2(RENAME_NOREPLACE)`, затем проверяются directory identity, inode,
owner, link count, mode и size. Детерминированные tests закрывают symlink/FIFO,
prepared hardlink, target race, замену выбранного каталога и ancestor-symlink
race. Keyboard path использует настоящие Return/Space/Escape события, а
terminal success/error объявляются accessibility alert без раскрытия report.

Полный software `package-check` на свежесобранных binaries прошёл: 794 printer
protocol cases, 17 replace journal, 92 Material QML, 100 Quick client/model,
37 runtime bootstrap, 30 capability/handshake и 20 tray. Translation gate
собрал 1 641 завершённый русский перевод, 0 `unfinished`; расширенный runtime
baseline отдельно закрепляет B3 source, security, controller и QML contracts.

### B4. Close behavior

**Статус:** выполнено 26 августа 2026 года. В Settings пользователь выбирает
persisted-режим закрытия окна: `hide-to-tray` или `quit-gui`. Для старого
конфига без поля сохраняется прежнее поведение Hide to tray. Некорректное
GUI-only поле безопасно нормализуется в Hide to tray и не отбрасывает валидные
runtime-настройки из общего конфига.

Hide доступен только при зарегистрированном StatusNotifier host. Если host
исчезает, выбранная настройка сохраняется, но фактическое закрытие завершает
только GUI; уже скрытое окно при потере host восстанавливается. Quit GUI
завершает event loop и не останавливает отдельный runtime. Это подтверждено
process-level bootstrap-caller probe: процесс, вызвавший bootstrap, завершается,
а build-tree runtime остаётся владельцем D-Bus service.

D-Bus API 8 и USB protocol не менялись. Полный software `package-check`
прошёл: 778 printer protocol cases, 17 replace journal, 33 QML, 44 Quick
client, 23 runtime bootstrap и 13 tray. Hardware smoke не выполнялся; B4 не
добавляет USB-команд и не меняет их wire bytes.

### B5. Тихое уведомление о новой версии приложения

**Статус:** документ согласован 5 сентября 2026 года после уточнения
пользователем startup/hourly policy. После завершения A8/A9 реализация B5
завершена и локально проверена 6 сентября. Полный `package-check`, переводы,
EN/RU QML и read-only review прошли. Реальная доставка popup на desktop не
проверена; это отдельная граница квалификации, описанная ниже.

#### Подтверждённая база и принятое решение

До B5 в GUI уже были версия из `VERSION` через `TRYX_APP_VERSION`, Settings/About,
проверенные project links, single-instance guard и системные уведомления через
`LinuxTrayController`. Release checker отсутствовал; сетевые настройки B5
не добавляются и после реализации.
`ConfigManager` перезаписывает общий `config.json`, который используют GUI и
runtime; новый network state туда не добавляется.

Принятое поведение заменяет прежнее предложение ручной проверки/opt-in раз
в сутки. Проверка обязательна для обычного GUI, но невидима пользователю до
обнаружения обновления. Это только уведомление о приложении, не о firmware.

#### Пользовательский контракт и расписание

- После успешного старта основного GUI process запускается одна асинхронная
  проверка, не блокирующая первый показ окна или работу интерфейса.
- Пока этот process работает, проверка повторяется раз в час, в том числе
  при скрытом в tray окне и GUI autostart. Повторный запуск, лишь активирующий
  существующий single instance, не создаёт второй checker или дополнительный poll.
- Используется monotonic interval 60 минут, не wall-clock дедлайн. Одновременно
  разрешён один request; после suspend или задержки event loop пропущенные
  интервалы не накапливаются в очередь запросов. Quit отменяет request и timer.
- Нет opt-in, первого диалога согласия, переключателя, ручной кнопки проверки,
  spinner, состояния «проверяем» или сообщений «обновлений нет».
- Новая стабильная версия вызывает одно системное уведомление. В Settings/About
  появляется строка доступной версии и кнопка открытия соответствующего релиза
  в системном браузере. До подтверждённого обновления этот UI отсутствует.
- Та же версия не вызывает повторное уведомление при следующем часовом poll.
  Если за этот запуск появится ещё более новая версия, она получает отдельное
  уведомление. После нового старта GUI неустановленное обновление можно сообщить
  снова. Закрытие popup не удаляет строку и ссылку из Settings.
- При отсутствии notifications service или при его отказе сохраняется строка
  в Settings; окно принудительно не раскрывается, modal fallback не появляется.
  Настройки desktop/DND могут скрыть popup; реальную доставку нельзя обещать
  только по успешной отправке D-Bus запроса.
- Offline, TLS/DNS errors, timeout, rate limit, отсутствие релиза и malformed
  response не показывают ошибок пользователю и не означают «последняя версия».
  Неудачный poll не уничтожает ранее подтверждённое уведомление этого запуска.
- Полностью закрытый GUI ничего не проверяет. Runtime, CLI, firmware flow,
  `--version`, export helper и smoke/offline tests не получают release polling.

#### Архитектура, данные и сетевые границы

Один GUI-owned `ReleaseUpdateController` использует асинхронный Qt Network
client и timer; Qt Network подключается к GUI/test target, не к USB worker.
Существующий `LinuxTrayController` отправляет системное уведомление, Settings
получает только подтверждённые version/URL и наличие обновления. Worker,
runtime D-Bus API, device state и single-instance policy остаются без изменений.

Endpoint фиксирован: HTTPS `api.github.com`, путь
`/repos/DXVSI/Tryx-Linux-GUI/releases/latest`. Публичный latest-release endpoint
предоставляет опубликованный non-draft/non-prerelease release без токена.
Дата release/commit не заменяет сравнение версии. Источник проверен
5 сентября 2026 года: [GitHub REST, Get the latest release](https://docs.github.com/en/rest/releases/releases#get-the-latest-release).
Для запроса используются `Accept: application/vnd.github+json`, неперсональный
application User-Agent и документированный `X-GitHub-Api-Version: 2026-03-10`.

Контракт обработки внешних данных:

- Один GET с общим deadline 10 секунд и ограничением полученного тела 1 MiB.
  Размер ограничивается во время чтения, а не только по Content-Length.
  Нет бесконечного retry; после неудачи остаётся следующий часовой poll.
- TLS проверяется штатно, redirects не следуются, endpoint не настраивается
  через пользовательские URL/env. GitHub credentials, cookies, serial/chip ID,
  system/device logs и media metadata не отправляются. Сетевая проверка сама
  по себе раскрывает GitHub обычные данные HTTP соединения, включая IP; это
  поведение документируется в README без нового GUI consent flow.
- HTTP 200 принимается только как bounded JSON object с корректными типами,
  `draft=false`, `prerelease=false`, непустой датой публикации и строгим version
  tag. Принимаются `vX.Y.Z` и `X.Y.Z`; beta/RC, suffixes, пустые/некорректные
  компоненты и overflow не становятся обновлением. Версия GUI сравнивается
  численно по major/minor/patch, не строкой и не по `published_at`.
- Только release version строго больше версии исполняемого GUI публикует
  availability. Равная/более старая версия и непарсируемая локальная dev version
  не вызывают ложного предложения обновиться или понизить версию.
- Release URL строится из фиксированного prefix
  `https://github.com/DXVSI/Tryx-Linux-GUI/releases/tag/` и проверенного tag.
  Произвольные `html_url`, release body/Markdown/HTML, assets, картинки и
  redirect locations не открываются, не отображаются и не загружаются.
  Браузер открывается только по действию пользователя на проверенной ссылке.
- ETag сохраняется лишь вместе с прошедшим проверку release snapshot. HTTP 304
  использует только этот snapshot; без него обновление не объявляется. 404,
  403/429, 5xx и invalid response остаются тихими неуспешными проверками.
  Rate-limit Retry-After/reset, если они валидны, могут отложить следующий poll
  позже часа, но не запускают дополнительные запросы или видимый отсчёт.

Данные живут только в памяти основного GUI process: validated release snapshot
и ETag, текущий request/timer, cooldown и максимальная уже объявленная версия.
Так hourly polling не дублирует уведомление и не сообщает старую версию после
новой. Нужды в persisted opt-in, suppression или новом GUI settings file нет;
общий `config.json`, runtime stores и on-disk schemas не меняются. Последний
успешный ответ с равной/старой стабильной версией снимает availability;
сетевой/parse error не выдаётся за такой ответ.

#### Реализация, acceptance и откат

1. Добавить typed parsing/version comparison с deterministic fixtures для
   newer/equal/older, `2.10.0` против `2.9.0`, optional `v`, malformed fields,
   draft/prerelease, неизвестной локальной версии и untrusted URL/body.
2. Добавить GUI controller и сеть с test-only transport/clock seams. Проверить
   startup + 60-minute schedule без настоящего часового ожидания, один in-flight,
   teardown/cancel, delayed reply, timeout, oversized body, redirects, 200/304,
   404, rate limit и 5xx. Tests не обращаются к живому GitHub.
3. Подключить controller только к primary GUI. Проверить hidden/tray/autostart,
   secondary invocation, smoke/offline/helper paths, отсутствие runtime/USB
   вызовов и отсутствие новой periodic systemd job.
4. Подключить notification и условный Settings UI, RU/EN strings, keyboard focus
   и безопасный browser handoff. Проверить duplicate suppression, новую версию
   в том же запуске, notification failure fallback и невидимость no-update/error.
5. Обновить README с startup/hourly network behavior и ручным обновлением через
   страницу релиза. Выполнить чистые GUI/runtime/test builds, Quick/QML/network
   fixtures, translations, release/structural checks и полный `package-check`.

Сетевые тесты не доказывают появление popup на реальном desktop. При закрытии
B5 отдельно сообщаются результаты GUI/notification smoke и недоступные проверки;
hardware traffic для этой функции не требуется. Изменения B5 сохраняются отдельным
feature/test diff, не смешанным с архитектурными A8/A9. Commit и push без
отдельного запроса пользователя не выполняются.

Риски: silent errors могут задержать уведомление; desktop policy может скрыть
popup; publisher может выбрать latest tag, не превосходящий локальную версию.
Защита: bounded checks, строгая проверка ответа, числовое сравнение, постоянная
условная ссылка в Settings и deterministic error-path tests. Откат коммитов B5
удаляет GUI wiring/client/UI; runtime и сохранённые пользовательские данные
не затрагиваются, cleanup или migration не нужны.

**Out of scope:** self-update, asset download, package installation, restart GUI
или runtime, firmware notification/update, changelog renderer, telemetry,
проверка при закрытом GUI, opt-in/manual controls, GIPHY и recorder.

#### Реализовано и проверено 6 сентября 2026 года

- `ReleaseUpdateController` и чистый `ReleaseUpdates` parser находятся только
  в Quick target. `main.cpp` создаёт их после single-instance guard и запускает
  queued poll только после успешной загрузки QML, вне smoke branch. Quit
  останавливает timer и request; скрытие окна не меняет расписание.
- Строгий parser принимает только три ASCII numeric компонента без leading
  zeroes и suffixes, с проверкой `quint32` overflow. ETag ограничен 1024 байтами,
  проверяется как HTTP entity tag и хранится только с валидным snapshot.
  Retry-After seconds/HTTP date и primary reset конвертируются один раз в
  monotonic cooldown; HTTP `GMT` нормализуется для Qt RFC2822 parser.
- Settings/About показывает условную EN/RU строку и обычную keyboard-accessible
  кнопку. Browser handoff происходит только по её сигналу и использует URL
  validated controller. Исчезновение строки возвращает фокус к Open GitHub.
- `LinuxTrayController::showNotification` теперь отправляет фиксированный
  `QDBusMessage` через `asyncCall(..., 2000)`. Review выявил синхронный Introspect
  в прежнем `QDBusInterface` constructor; отдельный slow-service test сначала
  воспроизвёл блокировку на 1008 ms, затем прошёл после исправления. Fresh client
  process исключает ложный успех из-за Qt introspection cache. Отказ Notify не
  удаляет Settings state, не раскрывает окно и не вызывает повторный popup.
- Чистые GUI/runtime builds на локальном Qt 6.11.2, новая offline network suite
  (77 passed), protocol suite (902 passed), полный `package-check` (exit 0),
  translation completeness и structural/EN/RU baseline прошли. Основной QML
  прогон: 155 passed, 0 failed; 6 RU-only сценариев проверены отдельными
  translated baseline runs, а не объявлены покрытыми английским прогоном.
- Дополнительный `strace` в отдельном user/network namespace подтвердил
  отсутствие AF_INET/AF_INET6 calls для `--version`, `--smoke-test`, сочетания
  smoke/autostart и обоих helper entry points с некорректными аргументами.
  Smoke/version завершились с 0, helpers ожидаемо с 2. Ни runtime service,
  ни USB, ни live GitHub endpoint для этих проверок не запускались.
- В текущем Wayland computer-use provider screenshot недоступен. Проверен
  offscreen GUI и изолированный freedesktop notification contract; внешний
  браузер, реальный popup/DND и сборка на Qt 6.4 в этом срезе не проверялись.
  API выбран совместимый с 6.4; это не подменяет сборку на минимальной версии.
- Итоговый read-only review не оставил high/medium findings. Логи текущего
  прогона: `/tmp/tryx-b5.WzJ1SM/package-check-final.log`; дополнительные entry
  traces: `/tmp/tryx-b5-entry.9H9zVB/*-isolated.trace`. Commit, push, установка,
  restart и переход к B6 не выполнялись. Предыдущие A8/A9 changes сохранены.

Официальные API повторно проверены 6 сентября: [GitHub API versions](https://docs.github.com/en/rest/about-the-rest-api/api-versions#supported-api-versions),
[GitHub rate limits](https://docs.github.com/en/rest/using-the-rest-api/rate-limits-for-the-rest-api),
[Qt Network request attributes](https://doc.qt.io/qt-6/qnetworkrequest.html#Attribute-enum)
и [Qt reply streaming/lifecycle](https://doc.qt.io/qt-6/qnetworkreply.html).
Анонимный ETag poll не обещает бесплатную quota: GitHub гарантирует исключение
304 из primary limit только для корректно авторизованных запросов.

### B6. Remote firmware availability research

**Статус:** исследование завершено 6 сентября 2026 года. Найдены официальные
публичные страницы TRYX, но supported machine-readable firmware source для
целевых моделей и полный контракт безопасного обновления не подтверждены.
Remote download, firmware availability badge и cloud updater остаются
**NO-GO**. Завершение research не означает разрешение на их реализацию или
на flash устройства.

#### Проверенные публичные источники

Проверка ограничена публичными HTML-страницами и встроенными данными страниц
TRYX на указанную дату. Основные страницы ответили HTTP 200. ZIP/EXE, firmware
и PDF manuals не скачивались; vendor JavaScript, KANALI, firmware API и
оборудование не запускались и не опрашивались.

| Источник | Подтверждено | Что этим не доказано |
|---|---|---|
| [Global Downloads](https://www.tryx.com/en/downloads), [CN Downloads](https://www.tryxzone.com/downloads) | KANALI для Windows 10/11, label `v2.4.0` / `2.4.0`, дата `2026-07-22`; есть ссылки на manuals PANORAMA, PANORAMA SE и TURRIS. Из проверенных записей категории firmware найден только ROTA Upgrade Tool `V8` от `2025-01-09` | ZIP установщика KANALI не является идентифицированным firmware payload. Пакеты обновления целевых моделей в текущем списке не найдены; ROTA вне этого scope |
| [Offline Firmware Upgrade](https://www.tryx.com/en/support/help-documents/offline-firmware-upgrade) | Разные инструменты для SE и NON-SE: `PANORAMA SE - Firmware Upgrade Tool v1.0.3` и `PANORAMA - Firmware Upgrade Tool v1.0.11`; перед отдельным Windows updater требуется закрыть KANALI | Это версии в названиях tools, не доказанные revisions firmware. Ссылка ведёт на общий Downloads, где эти два инструмента сейчас не перечислены; прямой firmware URL и актуальность tools не подтверждены |
| [KANALI 2.4.0 release notes](https://www.tryx.com/en/support/release-notes/kanali-2-4-0), [KANALI 2.2.0 release notes](https://www.tryx.com/en/about/news/kanali-release-notes) | Первый документ подтверждает дату приложения `2026-07-22`; второй от `2026-05-14` описывает unified application, backend overhaul и firmware flash при установке. Производитель запрещает прерывать flash, отключать устройство или выключать ПК | Общая поддержка семейства приложением не связывает конкретный payload с board revision, backend, регионом или допустимым переходом версий |
| [Warranty, пункт 12](https://www.tryx.com/en/support/warranty) | Производитель отдельно предупреждает о последствиях прерывания питания, выключения и отключения кабелей во время обновления, а также неофициальной firmware | Предупреждение не является техническим power-loss/recovery/rollback contract; правовой вывод о конкретном гарантийном случае здесь не делается |

Также сверены публичные страницы [PANORAMA ARGB](https://www.tryx.com/en/products/cooling/panorama/panorama-argb/white-240),
[PANORAMA SE](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama-se/white-360)
и [TURRIS 620](https://www.tryx.com/en/products/liquid-cooling/turris/turris-620/white).
Они подтверждают product names, но не hardware qualification или firmware
compatibility manifest. В изученных материалах не подтверждён vendor alias
`PASE`; в этом проекте он используется по существующему локальному evidence.

В перечисленных источниках не найдены firmware checksum/signature,
опубликованный trust anchor, подписанный manifest, mapping на hardware
revision, min-version/backend/region constraints или процедура восстановления
после прерванной записи. Это **граница проверки страниц**, а не утверждение,
что внутри vendor archives или устройств таких механизмов нет. Offline guide
советует проверить полноту загрузки/распаковки и обратиться в поддержку при
ошибке; это не доказательство восстановления после потери питания.

#### Исторические сведения не являются live distribution contract

[Engineering brief, раздел 7](tryx-engineering-brief.md#7-firmware-archive-distribution-and-sm2-api)
фиксирует прежнее изучение KANALI: обычные ZIP archives, application-level
SM2 envelope для metadata/download-URL API и наблюдавшиеся catalog entries
`36/PANO/V2.0.0`, `37/PAWB/V2.0.0`, `38/PASE/V2.0.1`. B6 не повторял запросы
к этому API, загрузку или flash. Эти записи не объявляются текущими latest
versions или разрешённым публичным API.

SM2 envelope и signed download URL не доказывают publisher signature самого
firmware payload. Они также не доказывают отсутствие другого официального
способа распространения. Ключи, закрытые endpoints и vendor binaries не
переносятся в проект. Версия KANALI, версия updater tool, API catalog version,
firmware build и device app version имеют разный смысл: parser и сравнение
`vX.Y.Z` из B5 нельзя автоматически применять к firmware availability.

#### Что уже гарантирует локальный updater

Сверены текущие [FirmwareBridge](../src/firmwarebridge.cpp),
[FirmwareUpdater](../src/firmwareupdater.cpp) и
[product profiles](../src/printerproductprofile.cpp), без запуска устройства.

| Существующая проверка | Граница гарантии |
|---|---|
| Legacy OTA: `pre-device=cm01_se`, непустые build metadata; Rockchip: обязательные files, `RK3568`, product marker из `rootfs:/usr/bin/panorama` | Проверка формата и известных признаков пакета. Marker является эвристикой, не подписью; hardware revision и разрешённый upgrade/downgrade path этим не подтверждаются |
| SHA-256 выбранного файла, повторная identity validation и private staging; для ADB также размер и SHA-256 скопированного ZIP | Approval связан с проверенными байтами. Hash вычислен локально, а не получен из доверенного vendor manifest, поэтому это не проверка издателя |
| PASE-only Rockchip approval, firmware-capable device identity до Loader, повторная Loader identity fence перед writes | Среди текущих printer profiles firmware разрешён только `391a:1021`; `391a:1011` и `391a:2011` не получают firmware support. Cold unidentified Loader и Maskrom не превращаются в допустимую цель |
| Recovery journal до dispatch, exclusive gate, сохранение журнала после irreversible failure и updater success | Journal удерживает runtime fail-closed. Он не сохраняет старые partition images, не выполняет rollback и не доказывает успешный boot; acknowledgement пользователя очищает journal и разрешает новую connection attempt |

В проверенном host path нет проверки firmware signature по vendor trust
anchor. Стандартная [AOSP OTA signing model](https://source.android.com/docs/core/ota/sign_builds)
предусматривает проверку recovery package по ожидаемым публичным ключам.
Это не доказывает конфигурацию конкретного TRYX recovery: используемые ключи,
release/test trust и политика downgrade в B6 не проверены. Аналогично, порядок
Rockchip writes и запрет отмены после irreversible boundary не доказывают
атомарность обновления или устойчивость к потере питания.

#### Что нужно до отдельного proposal

1. Поддерживаемый TRYX способ получения metadata и пакетов без извлечения
   внутренних ключей: публичный manifest/API или документированная публикация.
   Нужны условия доступа; mirror и redistribution требуют отдельного разрешения.
2. Проверяемая связь manifest, конкретных bytes/size/hash и издателя: формат
   подписи, доверенный публичный ключ/certificate и способ его обновления или
   отзыва. Наличие HTTPS URL само по себе этот контракт не определяет.
3. Точное соответствие payload моделям и hardware revisions, boot/backend,
   региональным вариантам и допустимым source/target versions. Неизвестные
   поля не должны превращаться в разрешение обновлять устройство.
4. Официальная процедура recovery после потери питания и условия rollback,
   либо явное подтверждение его запрета; необходимые инструменты, доступ к
   recovery package и критерии успешного boot/version verification. Сохранение
   пользовательских media не считается backup firmware.
5. Для Linux: поддерживаемый flasher и ясные права его распространения либо
   разрешённая альтернатива. Наличие текущего внешнего `upgrade_tool` в PATH
   не решает вопрос доверия к cloud packages или redistribution.

Этот список уточняет существующий [запрос к TRYX](2026-06-10-kanali-firmware-request.md);
сообщение производителю не отправлялось. Даже после получения данных будущий
proposal должен отдельно определить bounded download/archive handling и
trust/recovery tests. Hardware qualification и каждый flash требуют отдельного
явного согласия для точной модели; B6 не расширяет текущие capabilities.

В этом срезе изменена только документация. Runtime, GUI, API, firmware code и
тесты сохранены без изменений; сборки и hardware checks для docs-only research
не запускались. Предыдущие результаты A8/A9/B5 не являются доказательством
подлинности удалённой firmware или power-loss recovery.

### B7. Legal/About links

**Статус:** выполнено 26 августа 2026 года. About показывает два постоянных
project-owned внешних действия: главную страницу проекта и опубликованный MIT
`LICENSE` из ветки `production`. Локальный `LICENSE` существует в source tree и
также входит в native packages. Privacy Policy и User Agreement в проекте
отсутствуют, поэтому ссылки на них не показываются и не выдумываются.

Обе ссылки являются readonly HTTPS URL и открываются только после явной
активации стандартной Qt Quick `Button` через системный browser handler.
Фоновой сети, загрузки содержимого внутрь приложения и нового controller/API
нет. QML-тест фиксирует точный allowlist из двух URL, не допускает третью
action-ссылку и проверяет границы кнопок при ширине 760 px.

D-Bus API 8, runtime и USB protocol не менялись. Полный software
`package-check` прошёл: 778 printer protocol cases, 17 replace journal, 34 QML,
44 Quick client, 23 runtime bootstrap и 13 tray. Hardware smoke не выполнялся;
B7 не взаимодействует с устройством.

### B8. Отдельный автозапуск GUI

**Статус:** выполнено 26 августа 2026 года. Переключатель Settings теперь
управляет только owner-managed XDG Autostart entry для GUI. Состояние фонового
`tryx-panorama.service` не читается и не изменяется; уже выбранное пользователем
enable-state runtime service сохранено.

**Цель:** пользователь отдельно включает запуск desktop GUI при входе в
графическую user session. Фоновый runtime по-прежнему запускается GUI on demand
и может быть независимо включён вручную через `systemctl --user`. Управление
переключателем GUI-autostart не вызывает `systemctl` и не меняет enable-state
`tryx-panorama.service`. Сам GUI, в том числе запущенный через XDG Autostart,
проходит обычный bootstrap и при необходимости может выполнить
`systemctl --user start tryx-panorama.service`.

Выбран стандарт
[XDG Desktop Application Autostart](https://specifications.freedesktop.org/autostart/0.5/),
а не второй systemd user unit. Управляемая запись находится в первом writable
[`QStandardPaths::GenericConfigLocation`](https://doc.qt.io/qt-6/qstandardpaths.html),
под именем `autostart/tryx-panorama-manager.desktop`;
на обычном Linux это
`$XDG_CONFIG_HOME/autostart/tryx-panorama-manager.desktop`, либо
`~/.config/autostart/tryx-panorama-manager.desktop`, если переменная не задана.
Пакет не устанавливает запись в `/etc/xdg/autostart` и сохраняет GUI autostart
выключенным по умолчанию.

Управляемая desktop entry содержит `Type=Application`, имя и icon существующего
launcher, `TryExec` с точным `QCoreApplication::applicationFilePath()`, `Exec` с
тем же корректно экранированным executable и аргументом `--autostart`,
`Terminal=false`, `StartupNotify=false`, `Hidden=false` и product-owned marker
`X-TRYX-Panorama-Managed=true`. Поэтому установленная сборка запускает стабильный
`/usr/bin/tryx-panorama-manager`, а build-tree проверка не перенаправляется на
старую установленную версию. Effective lower system entry учитывает `Hidden`
до остальных полей, `OnlyShowIn`/`NotShowIn` относительно
`XDG_CURRENT_DESKTOP` и optional `TryExec`: отсутствующий ключ сам по себе не
отключает entry, а указанный, но неразрешимый executable отключает её.

Namespace записи изменяется относительно удерживаемого
`O_DIRECTORY|O_NOFOLLOW` descriptor каталога `autostart`. Подготовленный файл
имеет mode `0600` и синхронизируется до commit; отсутствующий leaf создаётся
только через `renameat2(RENAME_NOREPLACE)`, replace использует
`RENAME_EXCHANGE` с проверкой точного inode и байтов обоих leaf, а remove
сначала переносит проверенный target в уникальное отсутствующее имя через
`RENAME_NOREPLACE`. До syscall и после него повторно проверяются target,
подготовленный inode и содержимое; при одной инжектированной namespace-гонке
foreign entry восстанавливается в canonical path либо остаётся там и никогда
не удаляется. Это bounded защита от конкурентного изменения, а не security
boundary против процесса с тем же UID, который имеет те же права на весь
каталог и может продолжать менять namespace после любой проверки.
Новый `$XDG_CONFIG_HOME` и каталог `autostart`, если приложение создаёт их само,
получают owner-only mode `0700` независимо от `umask`; mode существующих
каталогов не переписывается. Существующий config home обязан быть реальным,
принадлежать текущему пользователю и не разрешать запись группе или остальным;
его непосредственный родитель также обязан быть реальным, не разрешать такую
запись и принадлежать текущему пользователю либо root. Leaf обязан быть
отсутствующим либо обычным managed-файлом текущего пользователя без group/other
write.
Symlink, directory, special file, чужой owner, group/other-writable mode,
отсутствующий marker или несовместимый foreign entry дают состояние unavailable
и понятную ошибку без перезаписи или удаления. Отключение удаляет только
распознанный managed user entry. Если одноимённая system-wide XDG entry когда-либо
появится, отключение вместо удаления создаёт managed user override с
`Hidden=true`, чтобы не раскрыть нижележащий автозапуск снова.

Состояние On для managed user entry требует exact
`QCoreApplication::applicationFilePath()`: запись от другой существующей сборки
показывается как Off, а enable безопасно перепривязывает её к текущему GUI.
Relative `TryExec` с path component отклоняется согласно XDG Autostart.

Обычный запуск приложения остаётся видимым. Запуск с `--autostart` начинает со
скрытого окна только когда сохранён режим B4 `hide-to-tray` и реально доступен
StatusNotifier host. При `quit-gui` или отсутствии tray host окно показывается,
чтобы не оставить невидимый GUI-процесс. Потеря host после скрытого старта
использует уже зафиксированное B4-поведение и восстанавливает окно.
Single-instance socket создаётся только внутри реального owner-owned runtime
directory с mode `0700`; пустой путь, replaceable non-sticky ancestor или другая
небезопасная иерархия дают fail-closed завершение до GUI. Guard удерживает
descriptor и identity каталога, а lock и socket разрешает через
`/proc/self/fd`, поэтому подмена строкового runtime path не перенаправляет
операцию в новый каталог. Lifetime lock сериализует acquisition; свидетельство
lease обновляется и для владельца, появившегося и упавшего во время bounded
startup wait.

Socket создаётся native `AF_UNIX` listener с mode `0600`, проверяется по точному
inode и передаётся Qt через `QLocalServer::listen(descriptor)`. Stale recovery
требует metadata прежнего lease и точного stale inode, готовит уже listening
replacement под уникальным именем и атомарно публикует его через
`RENAME_EXCHANGE`; canonical endpoint не проходит через absent/unlink окно.
Если живой same-UID владелец заменяет stale endpoint непосредственно перед
exchange, exact postcheck обнаруживает вытесненный inode, обратный atomic
exchange возвращает владельца в canonical path, а собственный replacement
удаляется только после точной проверки.
Launch intent является первым bounded newline-delimited frame, последующие
байты не интерпретируются как второй intent. `NotifiedExisting` возвращается
только после точного ACK, отправленного владельцем после разбора и принятия
intent. Если lease, доставка или ACK недоступны за bounded срок, новый процесс
завершается до второго GUI.

**Этапы реализации:**

1. RED-first C++ tests с изолированными `XDG_CONFIG_HOME` и `XDG_CONFIG_DIRS`
   фиксируют absent/enable/reload/disable round trip, текущий executable,
   desktop-entry escaping, idempotency, write failure и сохранение unsafe или
   foreign paths; fake `systemctl` доказывает отсутствие runtime-service calls.
2. `AppSettingsController` заменяет systemctl state machine на bounded
   filesystem state, сохраняя существующий QML-facing autostart API и отдельные
   config errors.
3. `--autostart`, initial visibility, tray fallback и QML-тексты получают
   проверки контроллера видимости, изолированного single-instance socket и QML.
   Переключатель явно называется запуском GUI, а пояснение сообщает, что
   background runtime не меняется.
4. README, man page, Arch install message, package verifier, baseline и
   переводы приводятся к новому контракту; полный `package-check` выполняется на
   свежесобранных binaries.

**Acceptance:**

- отсутствие user и system XDG entry отображается как available и Off;
- enable атомарно создаёт валидную managed entry, новый controller видит On,
  login-start использует текущий GUI executable и `--autostart`;
- disable действительно даёт Off и не раскрывает system-wide entry;
- unsafe, malformed и foreign entries сохраняются побайтно и дают unavailable,
  а offline/smoke mode не читает и не пишет реальную user config;
- ни query, ни enable, ни disable не вызывают `systemctl` и не меняют
  `tryx-panorama.service`; его существующий enable/active state сохраняется;
- скрытый login-start возможен только при выбранном Hide to tray и доступном
  host, иначе окно остаётся доступным пользователю;
- конкурентный manual/login запуск либо передаёт строгий launch intent живому
  владельцу, либо fail-closed завершается без перехвата активного socket;
- D-Bus API 8, runtime process ownership, USB protocol и wire bytes не меняются.

Полный software `package-check` прошёл на свежесобранных binaries: 778 printer
protocol cases, 17 replace journal, 34 QML, 82 Quick client, 37 runtime
bootstrap и 18 tray. Строгий catalog gate собрал 1 865 текущих завершённых
русских переводов без `unfinished` и отдельно доказал, что актуальная строка не
может скрыться как `vanished`. Отдельный staged-install verifier подтвердил
launcher, runtime unit и отсутствие package-owned `/etc/xdg/autostart` entry.
Hardware smoke не выполнялся; B8 не отправляет USB-команды. Живая
пользовательская конфигурация XDG и user systemd session во время проверки не
изменялись.

**Откат:** перед возвратом к версии без B8 GUI-autostart отключается новым
переключателем. Если GUI уже недоступен, managed user entry можно удалить
только при отсутствии одноимённой entry в system XDG config dirs. При наличии
нижнего system-wide autostart сохраняется user `Hidden=true` override, иначе
удаление снова включит GUI login-start. Runtime unit не отключается и не
перезапускается. Package upgrade или uninstall не обходит user ownership и не
удаляет конфигурацию всех пользователей.

**Out of scope:** UI для enable-state фонового runtime, автоматическая миграция
или отключение уже enabled `tryx-panorama.service`, system-wide GUI autostart,
desktop-specific KDE/GNOME extensions, запуск/закрытие текущего GUI при
переключении, новые D-Bus methods и любые USB-команды.

### B9. Полный NVIDIA telemetry backend

**Статус:** программная реализация B9.1 завершена 31 августа 2026 года.
Dedicated provider, protocol, Quick/QML, translation, baseline и полный fresh
`package-check` прошли. Полный B9 остаётся hardware-pending до отдельного real
NVIDIA smoke; fixture-тесты и сам факт запуска GUI не являются hardware
acceptance.

**Цель:** добавить честный необязательный источник NVIDIA temperature, usage,
graphics frequency, board power и VRAM, одновременно устранив расхождение
между именем primary GPU и источником его значений. Отсутствие backend,
неподдерживаемое поле, timeout и потеря GPU не должны превращаться в числовой
ноль или бесконечно сохранять последнее значение как актуальное.

#### Подтверждённое текущее состояние

- Runtime и GUI остаются разными процессами с независимыми `SystemMonitor`.
  Runtime переводит NVIDIA polling между `Discovery`, `Active` и `Off` по
  состоянию PASE session и уже подтверждённой GPU metric; Quick GUI использует
  `Active` только для видимой, не свёрнутой Dashboard Home page.
- Единый DRM inventory объединяет optional provider rows только по normalized
  PCI BDF. `GpuMetrics` несёт generation-scoped `entityKey`, optional provider
  UUID и отдельный `vramAvailable`.
- Model, badge и values берутся из одной и той же стабильно отсортированной GPU
  row. Поздний UUID обогащает существующую identity, а rebind после topology
  change разрешён только по exact UUID без fallback на другую карту.
- PASE уже имеет четыре неизменяемых GPU tokens: `GPU Temperature`,
  `GPU Frequency`, `GPU Usage` и `GPU Power`. VRAM существует только в host
  model и не является подтверждённым device overlay token.
- На development host 31 августа 2026 года обнаружены две AMD GPU; executable
  `nvidia-smi` и NVML library отсутствуют. Этот хост проверяет fallback, но не
  является NVIDIA acceptance evidence.

#### Выбранный backend

B9.1 использует отдельный асинхронный bounded `nvidia-smi` provider. Основной
runtime не загружает NVML in-process и не получает link-time dependency от
`libnvidia-ml.so`.

- Production resolver принимает только `/usr/bin/nvidia-smi`, не читает PATH,
  environment или пользовательский config. Symlink допустим, только если сам
  link принадлежит root, а canonical regular-file target и все resolved parent
  directories принадлежат root и не writable для group/other. Final target
  также не writable для group/other. Fake executable передаётся только через
  test-only constructor seam и недоступен production configuration.
- `nvidia-smi` запускается напрямую без shell внутренним Linux process
  supervisor на основе `posix_spawn`, `pipe2(O_CLOEXEC)` и
  `waitpid(WNOHANG)`. `O_NONBLOCK` устанавливается только на parent read ends
  перед регистрацией в event loop; child write ends, перенаправленные через
  `dup2` в stdout/stderr, остаются blocking, чтобы child не получил `EAGAIN`
  при заполнении pipe. Supervisor живёт вне GUI и USB-owning threads и не имеет
  деструктора, выполняющего blocking wait. `QProcess` здесь не используется:
  его официальный destructor убивает child и ждёт termination, что несовместимо
  с bounded shutdown при unreaped process:
  [Qt QProcess lifecycle](https://doc.qt.io/qt-6/qprocess.html). Разделение
  blocking child writes и nonblocking parent reads следует Linux
  [pipe semantics](https://man7.org/linux/man-pages/man2/pipe.2.html).
- Синхронный `posix_spawn` изолирован в lifetime-independent supervisor worker:
  request deadline начинается до dispatch, а состояние `Spawning` уже считается
  единственным inflight request. Provider callback привязан к request
  generation и weak receiver, поэтому worker не удерживает GUI/runtime object.
  Если deadline наступил до возврата spawn, sample сразу инвалидируется,
  request без PID переходит в quarantine, breaker запрещает новый spawn, а
  shutdown помечает request cancelled и не join-ит worker. Late failure только
  освобождает quarantine; late success не может публиковать snapshot, получает
  `SIGKILL` и передаёт PID в тот же shared nonblocking reap record. Shared state
  живёт до late completion/reap независимо от receiver. Это учитывает
  синхронный контракт без timeout у
  [POSIX posix_spawn](https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawn.html).
- Child получает ровно fd `0/1/2`: stdin file action открывает `/dev/null`,
  stdout/stderr сначала перенаправляются через `dup2` на blocking pipe write
  ends, затем `posix_spawn_file_actions_addclosefrom_np(actions, 3)` или
  эквивалентный race-free close-all закрывает остальные descriptors. Обход
  `/proc/self/fd` и перечисление текущих fd в multithreaded parent запрещены.
  Поэтому subprocess не наследует serial/recovery descriptors, даже если в
  существующем коде они открыты без `FD_CLOEXEC`. Сохранение open fd без close
  action является стандартным поведением
  [posix_spawn](https://man7.org/linux/man-pages/man3/posix_spawn.3.html).
- Subprocess получает минимальный `envp` только с `LC_ALL=C` и `LANG=C`;
  `PATH`, `LD_PRELOAD`, `LD_LIBRARY_PATH` и остальные inherited variables не
  передаются.
- Один запрос получает все доступные provider NVIDIA physical GPU с exact
  `--query-gpu` keys `uuid`, `pci.bus_id`, `temperature.gpu`,
  `utilization.gpu`, `clocks.current.graphics`, `power.draw`, `memory.used` и
  `memory.total`, а также `--format=csv,noheader,nounits`. Единицы после снятия
  суффиксов остаются соответственно °C, %, MHz, W и MiB.
- Identity columns обязательны. Для measurement exact `N/A` означает
  unavailable только этого поля. Пустое поле, `[Not Supported]`, invalid
  UTF-8/CSV, duplicate или malformed identity, overflow и неверное число строк
  отклоняют весь snapshot.
- Numeric parser принимает только finite values: temperature integral
  `0..255°C`, utilization integral `0..100%`, graphics clock integral
  `0..100000 MHz`, decimal power `0..10000 W`, used/total VRAM integral
  `0..16777216 MiB`, причём total больше нуля и used не больше total. `NaN`,
  `Inf`, отрицательные и выходящие за эти bounds значения отклоняют snapshot.
- Одновременно разрешён только один request в состояниях `Spawning`, `Running`
  или `Quarantined`. Non-zero `posix_spawn` result до deadline является
  immediate terminal `FailedToStart`: snapshot инвалидируется, включается
  обычный error backoff, а kill/quarantine не выполняются. Hard completion
  deadline, включая spawn, равен 2 секундам для первого discovery и 1 секунде
  для обычного sample. Success требует exit code zero, EOF обоих pipes,
  successful nonblocking reap и полного valid snapshot.
  Stdout ограничен 64 KiB, stderr 16 KiB, snapshot 32 GPU. Если real hardware
  не укладывается в эти bounds, реализация останавливается и возвращается на
  docs gate вместо молчаливого увеличения timeout.
- На deadline provider в том же event turn инвалидирует numeric snapshot и
  публикует unavailable, затем отправляет child `SIGKILL`. Reap grace равен
  250 ms и использует только `waitpid(WNOHANG)`. Если process не reaped, его PID
  и pipe descriptors остаются в quarantined supervisor record, breaker открыт
  до фактического reap, новый child запрещён. Record продолжает nonblocking
  poll, но shutdown приложения закрывает descriptors, повторяет `SIGKILL` и
  выходит без blocking wait; после выхода ОС перепривязывает оставшийся child.
  Injectable process-supervisor fake отдельно удерживает spawn до получения PID
  и после kill не сообщает reap, доказывая bounded deadline/quarantine/shutdown
  и stale late-result fencing отдельно от обычного SIGKILL test.
- После первой завершившейся ошибки polling приостанавливается на 30 секунд;
  три последовательные ошибки открывают breaker на 5 минут. Успешный sample
  сбрасывает counter. Topology change может сбросить timed breaker, но не
  quarantine ещё не завершившегося process.
- Runtime делает первый discovery при подключённой overlay-capable PASE session
  и обнаруженном NVIDIA PCI device. Без выбранной GPU metric он повторяет probe
  не чаще раза в 10 секунд с TTL 15 секунд; с выбранной metric опрашивает раз в
  2 секунды с TTL 5 секунд. Completion отдельно публикует live availability и
  сам по себе не отправляет USB write. GUI использует интервал 2 секунды и TTL
  5 секунд, только пока host view потребляет live NVIDIA values. Поэтому
  selector получает initial availability до выбора первой NVIDIA metric, а два
  процесса всё равно имеют независимые rate limits.
- Любая завершившаяся probe error инвалидирует sample немедленно, не ожидая
  TTL. Истечение TTL также публикует unavailable; last-good numeric data не
  выдаётся за свежее измерение.
- Raw stdout/stderr, UUID, PCI BDF и driver paths не попадают в обычный журнал,
  B3 report или UI error. Диагностика использует только bounded category и
  counters.

Такой выбор сохраняет hard deadline на границе отдельного процесса. NVIDIA
указывает, что `nvidia-smi` построен поверх NVML, но формат CLI output не имеет
того же compatibility contract, поэтому parser является versioned и fail
closed: [nvidia-smi documentation](https://docs.nvidia.com/deploy/nvidia-smi/index.html).
Обычные NVML device queries не принимают deadline; timeout является параметром
отдельных wait APIs, поэтому прямой вызов NVML в event loop не выполняет
bounded contract B9:
[NVML device queries](https://docs.nvidia.com/deploy/nvml-api/group__nvmlDeviceQueries.html),
[NVML event wait](https://docs.nvidia.com/deploy/nvml-api/group__nvmlEvents.html).
Если позднее частота или overhead CLI окажутся неприемлемыми, допустима замена
внутреннего provider на private NVML helper process с тем же контрактом, но не
in-process migration без отдельного review.

#### Identity, ordering и availability

- Общая inventory row получает неизменяемый в пределах topology generation
  `entityKey`, построенный из generation и normalized PCI BDF, optional
  `providerUuid` и явный `vramAvailable`. Numeric NVIDIA index и `cardN` не
  являются identity и нигде не сохраняются.
- NVIDIA row из `nvidia-smi` соединяется с существующей DRM/sysfs inventory row
  только по unique normalized PCI BDF и атомарно обогащает её immutable GPU
  UUID. Поздний UUID не заменяет `entityKey` и не создаёт вторую GPU. Duplicate
  BDF/UUID, stale generation result или несовпадение vendor отклоняют snapshot.
- NVIDIA документирует UUID как immutable и предупреждает, что numeric
  enumeration order может меняться между reboot:
  [NVML device identity](https://docs.nvidia.com/deploy/nvml-api/group__nvmlDeviceQueries.html).
- Model name, badge и values всегда берутся из одной inventory row; отдельного
  lookup для badge больше нет. B9.1 намеренно заменяет текущий VRAM-first
  primary comparator на неизменяемую topology policy: фиксированный vendor
  priority NVIDIA/AMD/Intel, уже используемый как текущий secondary key, затем
  `boot_vga`, PCI BDF и подтверждённый UUID. Это пользовательски заметное
  изменение на mixed/multi-GPU host и является частью этого docs gate. VRAM,
  availability и completion order не меняют primary после старта topology
  generation.
- При создании или cold restore active overlay runtime закрепляет `entityKey`
  выбранной primary row в памяти. Badge и periodic values используют только
  её. Delayed provider result обогащает ту же pinned row и не требует reapply.
  Исчезнувшая row даёт unavailable и `--`, а другая GPU не подставляется
  скрытым periodic update.
- После topology generation change runtime может перепривязать pin только к
  row с exact ранее подтверждённым NVIDIA UUID. Если UUID ещё не был получен
  или не совпал, старый pin остаётся unavailable до explicit overlay reapply.
  UUID не становится пользовательским persistent setting в B9.1.
- Provider возвращает cached completed snapshot без ожидания из
  `SystemMonitor::update()`. Успешная completion и TTL expiration инициируют
  отдельную nonblocking availability publication. Last-good можно использовать
  только как внутреннюю topology metadata, но не как свежую telemetry.
- Если ранее выбранный sensor становится unavailable, periodic PASE batch
  обновляет уже существующий value label значением `--`. Пропуск label и
  сохранение stale числа запрещены; `0` остаётся допустимым только реальным
  валидным измерением.

#### API, UI и packaging boundaries

- API остаётся равным `8`. `TryxRuntimeMetricsState`,
  `TryxRuntimeApplyRequest`, Manager1/Manager2 signatures, capability tuples,
  protobuf, PASE group IDs и `pase-metrics.json` v2 не меняются.
- Device overlay продолжает использовать только существующие четыре GPU
  tokens. GPU VRAM и power дополнительно показываются в локальной Dashboard
  GPU card через существующий host model; новый device token для VRAM не
  добавляется.
- NVIDIA driver, NVML и `nvidia-smi` не являются hard package dependency.
  RPM `Requires`/`Recommends`, DEB `Depends`/`Recommends` и Arch `depends`
  запрещены. Допустимы только неавтоматические RPM/DEB `Suggests`, Arch
  `optdepends` либо полное отсутствие hint. Missing executable, driver not
  loaded и отсутствие NVIDIA GPU являются нормальным unavailable state.
  `nvidia-smi` и NVML поставляются NVIDIA driver, а не приложением:
  [NVIDIA installed components](https://download.nvidia.com/XFree86/Linux-x86_64/610.43.03/README/installedcomponents.html).
- Generic hwmon остаётся допустимым частичным provider для будущего C15, но не
  считается полным B9: Linux hwmon channels optional и не гарантируют usage,
  clocks, power и VRAM одновременно:
  [Linux hwmon ABI](https://docs.kernel.org/hwmon/sysfs-interface.html).

#### Этапы реализации

1. RED tests фиксируют parser, exact argv/envp/fd set, output bounds,
   spawn/failure/exit/kill/reap deadlines, one-inflight, TTL и breaker на fake
   executable без реального NVIDIA driver. Sentinel non-`CLOEXEC` parent fd
   должен давать child `EBADF`. Отдельный injectable supervisor fake удерживает
   spawn до PID, возвращает late success и удерживает unreaped state, доказывая
   bounded deadline/shutdown, stale fencing и отсутствие destructor wait.
2. RED inventory tests фиксируют mixed AMD+NVIDIA, две одинаковые NVIDIA,
   более объёмную VRAM у AMD при наличии NVIDIA, `boot_vga`/BDF tie-breakers,
   перестановку `cardN`, duplicate UUID/BDF, immutable `entityKey` при delayed
   UUID/VRAM enrichment и совпадение badge/value identity. Tests явно фиксируют
   новую topology policy вместо прежнего VRAM-first поведения.
3. Общий asynchronous provider подключается к обоим существующим
   `SystemMonitor`, не блокируя GUI event loop или USB-owning worker. Отдельный
   test покрывает fresh NVIDIA-only host, empty persistence и delayed successful
   discovery до выбора первой GPU metric. Второй test восстанавливает persisted
   GPU overlay на mixed AMD+NVIDIA host из empty provider cache и доказывает,
   что delayed UUID enrichment продолжает тот же badge/value `entityKey` без
   reapply.
4. Runtime pin-ит один `entityKey` для badge и values активного overlay,
   сохраняет существующие GPU tokens и при потере availability отправляет `--`;
   Quick Dashboard показывает подтверждённые GPU power и VRAM.
5. Выполняются focused protocol/Quick tests, frozen API manifest, translation
   gate, runtime baseline и fresh-binary `package-check`.
6. Отдельный read-only smoke на реальной NVIDIA GeForce проверяет actual
   temperature, usage, frequency, power и VRAM. Exact `N/A` и поддерживаемый
   числовой ноль обязательно доказываются fixtures; hardware evidence фиксирует
   их, только если такие значения естественно наблюдались. Mixed/dual ordering
   обязательно доказывается fixtures и дополнительно проверяется на hardware,
   если такой host доступен. До real smoke B9 имеет только software-complete,
   hardware-pending status.

#### Acceptance

- Valid sample публикует temperature, usage, graphics frequency, power и VRAM
  с independent availability; unsupported power не скрывает остальные поля.
- Missing executable, non-zero exit, timeout, overflow, malformed output,
  driver reload и GPU lost никогда не публикуются как числовой ноль.
- Hung fake process не блокирует GUI/runtime, не создаёт второй process и
  завершается через подтверждённый kill/reap path либо открывает breaker.
- Badge, model name и values относятся к одному stable GPU. Порядок не зависит
  от `cardN`; duplicate identity отклоняется fail closed. Delayed snapshot и
  topology reorder не меняют pinned identity активного overlay. Mixed-GPU
  primary выбирается по утверждённой vendor/`boot_vga`/BDF policy, а не по
  asynchronous VRAM.
- После TTL UI показывает unavailable, а активный device label получает `--`,
  не сохраняя stale number.
- Clean Fedora, Ubuntu/Mint DEB и Arch install без NVIDIA продолжают работать;
  package manager не устанавливает proprietary driver ради приложения.
- API 8 signatures, PASE token names/group IDs, persistence и default wire
  fixtures остаются прежними; полный `package-check` проходит.
- B9 получает итоговый статус выполнено только после real NVIDIA smoke с
  фактическими измерениями; различение числового нуля и unavailable независимо
  проверяется deterministic fixtures и дополнительно hardware evidence, когда
  оба состояния естественно наблюдаются.

#### Риски, откат и out-of-scope

- CLI format может измениться. Exact field count, bounds и fail-closed parser
  не позволяют принять сдвинутые данные как другую метрику.
- Cold или sleeping GPU может отвечать дольше и выходить из power-saving.
  Demand-driven polling, discovery deadline и backoff ограничивают влияние;
  если утверждённые hard deadlines недостаточны по зафиксированным hardware
  measurements, работа возвращается на docs gate.
- Runtime и GUI не делят process cache. Независимый one-inflight и rate limit
  обязательны, чтобы не запускать до полутора subprocess в секунду постоянно.
- Rollback удаляет optional provider и дополнительные host presentation fields,
  возвращая существующий AMD sysfs path. API/schema migration, rewrite
  сохранённых layouts и compensating USB mutation не нужны.

**Out of scope B9.1:** in-process NVML, NVIDIA process telemetry, MIG
aggregation, per-GPU selector/persistence, memory clock как `Memory Frequency`,
CPU/GPU voltage, FPS/frametime, network/disk device tokens, 4+ overlay groups,
pages, rotation, новые protobuf fields и RivaTuner parity.

### B10. Versioned Device Specifications contract

**Статус:** `software-complete, hardware-pending` с 31 августа 2026 года.
Реализация, fresh software `package-check` и независимые read-only review
server/protocol/API и client/QML завершены. Реальные `1021` bootstrap capture и
`1011` community/maintainer smoke ещё не выполнялись; hardware acceptance
остаётся отдельным незакрытым gate.

#### Цель и подтверждённое текущее состояние

Settings уже показывает mapped model, product ID, firmware и device app version
из B1. B10 добавляет отдельный read-only подраздел `Device Specifications` с
данными, которые само устройство уже возвращает во время обычного bootstrap:

- device-reported product name;
- video-output width и height;
- screen type `LCD` или `OLED`;
- состояние USB automatic keepalive.

Для `391a:1011` и `391a:1021` используется один `OverlayLayout` bootstrap.
Существующий untracked SysConfig request имеет version `1`, нулевые track ID и
CRC, body `102`, `dummy="NA"` и exact frame
`545259580b0000000a020801b206040a024e41`. Ответ с body `502` уже принимается
ровно один раз после DeviceInfo readiness и до DeviceAuth. Сейчас project-owned
`SystemConfiguration` пуст, а decoded response передаётся вызывающему коду как
`nullptr`, поэтому protobuf unknown fields уничтожаются вместе с временным
`Response`.

Новый query не нужен. B10 сохраняет уже полученный response, нормализует только
разрешённую проекцию и кэширует её для текущей physical generation. Повторный
D-Bus getter читает только этот cache и не обращается к USB.

Репозиторий не содержит сохранённого raw SysConfig fixture с реального `1011`
или полного fixture со всеми выбранными полями для `1021`. Поэтому выбранная
wire-таблица является документированным clean-room implementation assumption.
Hand-authored byte fixture проверяет parser относительно этой таблицы, но не
доказывает реальные field numbers или nesting. Mapping остаётся
hardware-unverified до независимого raw capture; hardware-tested статус
присваивается только после отдельного real-device smoke. Для `1011` общий
bootstrap path подтверждён кодом и community testing, но maintainer hardware
evidence отсутствует.

#### Clean-room wire table V1

Production и tests используют только самостоятельно названную минимальную
schema в `protocol/wire-v1/configuration.proto`. Локальная сравнительная
recovered schema не является build, test, package или release dependency и не
копируется. Сохраняются только необходимые field numbers, wire types и nesting:

| Project-owned semantic | Wire path | Wire type | Использование |
|---|---:|---|---|
| `reported_product` | `1` | length-delimited string | Bounded device-reported name |
| `board_summary` | `9` | length-delimited message | Контейнер только для panel summary |
| `board_summary.display_panel` | `2` | length-delimited message | Presence screen information |
| `display_panel.kind` | `1` | optional varint | `0 = LCD`, `1 = OLED`; иное invalid |
| `video_output` | `12` | length-delimited message | Presence output geometry |
| `video_output.width` | `1` | optional uint32 varint | Width `1..16384` |
| `video_output.height` | `2` | optional uint32 varint | Height `1..16384` |
| `runtime_behavior` | `14` | length-delimited message | Контейнер runtime behavior |
| `runtime_behavior.usb_auto_keepalive` | `1` | optional bool varint | Present `false` отличается от absent |

Scalar fields обязаны иметь generated presence checks. Proto3 default нельзя
интерпретировать как полученное значение: отсутствующие `0` и `false` не
означают `LCD`, валидную нулевую geometry или выключенный keepalive.

`reported_product` после trim обязан иметь длину `1..128` code units. Запрещены
Unicode categories control, format, line separator и paragraph separator, в том
числе C0 `U+0000..U+001F`, DEL/C1 `U+007F..U+009F`, ALM `U+061C`, LRM/RLM
`U+200E/U+200F`, line/paragraph separators `U+2028/U+2029`, bidi
embeddings/overrides/PDF `U+202A..U+202E`, isolates `U+2066..U+2069` и
deprecated bidi format controls `U+206A..U+206F`. QML отображает строку только
как `Text.PlainText`, но plain-text rendering само по себе не считается защитой
от bidi spoofing. Unknown enum, отсутствующее обязательное поле, unsafe string и
geometry вне bounds не ломают успешный device bootstrap, но оставляют B10
snapshot в `Unavailable`.
Malformed protobuf/envelope по-прежнему является bootstrap failure по
существующим transport rules.

В V1 не описываются power-on/standby media, storage paths, VDEC/VPSS internals,
video/UI rotation, mirror/flip, I2C/PWM, cooler/fan/pump topology и ambient RGB.
Они не нужны утверждённому UI, часть из них является mutable configuration, а
не характеристиками. Serial number, chip ID, USB/sysfs paths и raw unknown
fields никогда не входят в новый public tuple или QML.

#### Выбранный API 8 contract

Runtime API остаётся равным `8`. `Manager1`, все существующие Manager2 methods,
signals и positional types остаются без изменений. В существующий
`org.tryx.Panorama.Manager2` аддитивно добавляются:

- capability `runtime.device-specifications.v1` в bounded case-sensitive
  runtime allowlist;
- cache-only `GetDeviceSpecificationsV1()`;
- новый frozen `TryxRuntimeDeviceSpecificationsV1` с D-Bus signature
  `(usttssuusb)`.

Поля tuple строго фиксируются в таком порядке:

1. `schemaVersion: u`, всегда `1`;
2. `deviceIdentity: s`, только для client fencing;
3. `connectionRevision: t`;
4. `physicalGeneration: t`;
5. `status: s`;
6. `reportedProductName: s`;
7. `videoOutputWidth: u`;
8. `videoOutputHeight: u`;
9. `screenType: s`;
10. `usbAutoKeepalive: b`.

`status` принимает только exact case-sensitive значения:

- `Disconnected`: текущего connection context нет; `deviceIdentity` пуст,
  `connectionRevision` равен текущей revision connection adaptor и может быть
  `0` до первого snapshot, `physicalGeneration=0`, все specification fields
  пустые/нулевые;
- `Unsupported`: подключённое устройство не является printer-class profile
  `1011/1021`; для printer-class identity, connection revision и ненулевая
  physical generation обязаны точно описывать текущий context, для legacy
  physical generation равна `0`; все specification fields пустые/нулевые;
- `Unavailable`: `1011/1021` подключён, но для exact generation нет полностью
  валидного cached SysConfig; identity непустая, connection revision и
  physical generation ненулевые и exact, все specification fields
  пустые/нулевые;
- `Ready`: identity непустая, revision/generation ненулевые и exact, все четыре
  пользовательских значения присутствовали в том же bootstrap response и
  прошли validation.

`Ready` с `usbAutoKeepalive=false` означает реально полученный `false`, а не
protobuf default. Partial public snapshot не публикуется в V1: при отсутствии
любого обязательного поля status остаётся `Unavailable`, чтобы frozen tuple не
создавал неоднозначной per-field presence semantics. Дополнительные поля позже
получают самостоятельный V2, а не расширяют этот tuple.

Отдельный `device.*` token и новый D-Bus signal не добавляются. Runtime token
сообщает наличие метода, а typed status сообщает состояние текущего устройства.
Normal `Disconnected`, `Unsupported` и `Unavailable` возвращаются typed reply,
не D-Bus error.

Новый клиент не вызывает getter, если runtime token отсутствует либо старый API
8 вернул exact `org.freedesktop.DBus.Error.UnknownMethod` на
`GetRuntimeCapabilities()`. Если token advertised, любая ошибка самого getter,
включая `UnknownMethod`, является B10 failure и остаётся fail closed, не делая
совместимый API 8 runtime целиком incompatible.

Client presentation имеет отдельное локальное состояние `RuntimeUnavailable`.
В него переходят non-`UnknownMethod` capability-handshake error, advertised
specifications token без принятого `runtime.device-capabilities.v1` snapshot,
ошибка advertised getter и malformed current-context reply. Это terminal UI
state до следующего существующего lifecycle refresh, не spinner и не
`Unsupported`. Stale reply прежнего owner/revision/generation молча отбрасывается
после уже выполненной lifecycle invalidation и не может заменить состояние
текущего context.

#### Cache, ownership и invalidation

Data path остаётся односторонним и read-only:

1. `bootstrapSession()` сохраняет уже принятый body `502` вместо `nullptr`.
2. `PrinterProtocol` немедленно преобразует protobuf в bounded internal
   `DeviceSpecifications`; raw protobuf выше protocol layer не передаётся.
3. Успешный `startDisplaySession()` возвращает normalized data вместе с уже
   существующим DeviceInfo. Failed bootstrap или post-bootstrap activation не
   публикует новый cache.
4. `DeviceWorker` передаёт snapshot отдельным generation-tagged signal.
5. `DeviceManager` принимает его только для exact device path, product ID,
   identity и текущей physical generation и хранит один current snapshot.
6. `GetDeviceSpecificationsV1()` строит reply только из этого cache и текущего
   connection revision.

Cache очищается до публикации новой generation и при detach/disconnect, identity
или product change, firmware quiesce/recovery, session loss и runtime shutdown.
Неуспешный bootstrap новой generation не может оставить в UI данные предыдущей.

`RuntimeClient` вызывает getter только после принятого
`GetDeviceCapabilitiesV1()` той же session, чтобы иметь trusted expected
physical generation. Запрос направляется exact unique D-Bus owner и fenced по
`serviceEpoch`, `handshakeAttempt`, отдельному specifications attempt,
device identity, connection revision и точному равенству physical generation.
Owner replacement, `PrinterOperationsCancelled`, disconnect или новая revision
инвалидируют snapshot и pending reply через тот же lifecycle, что device
capabilities.

Первый `Unavailable` не запускает polling или timer. После завершения bootstrap
существующий `DisplaySessionChanged`/connection refresh получает новую revision,
повторно запрашивает device capabilities и затем specifications у того же exact
owner. Новый signal и blind retry не нужны.

Legacy connection не получает device-capabilities snapshot и поэтому никогда не
вызывает specifications getter. Если runtime token принят, client вычисляет для
legacy presentation `Unsupported` локально. Для printer-class connection
advertised specifications token без успешно принятого device-capabilities token
и reply даёт `RuntimeUnavailable`; подставлять generation `0`, вызывать getter
или показывать `Unsupported` запрещено.

`deviceIdentity` не экспортируется в QML. Client публикует только support/status,
`deviceSpecificationsReady`, reported product, geometry, screen type и
keepalive. Geometry сравнивается с уже выбранными `mediaTargetWidth/Height`
только для предупреждения. Несовпадение не меняет `PrinterProductProfile`,
encoder target, upload/apply capability или поддержку модели автоматически.

#### Settings UX

Существующие Model, Product ID, Firmware version и Device app version остаются
неизменными. Под ними появляется подраздел `Device Specifications`:

- `Device-reported product`;
- `Video output`, например `2240 × 1080`;
- `Screen type`;
- `USB automatic keepalive`, `Enabled` или `Disabled`.

Состояния отображаются честно и без бесконечного spinner:

- runtime token отсутствует: `Not supported by the active runtime`;
- capability/getter/malformed contract error: `Device specifications are unavailable because the runtime response could not be verified`;
- device disconnected: `Connect a supported device`;
- status `Unsupported`: `Not available for this device`;
- status `Unavailable`: `Not reported for the current connection`;
- status `Ready`: четыре validated значения;
- geometry отличается от текущего media target: отдельное неблокирующее
  предупреждение о несовпадении, без скрытой смены profile.

Все строки получают English source и законченный русский Qt translation.
Длинный product name переносится внутри card, остаётся plain text и не выходит
за narrow Settings content. Старый совместимый runtime сохраняет весь B1 UI.

#### Этапы реализации

1. RED wire/bootstrap tests и минимальная project-owned presence-aware schema;
   capture body `502`, parser и internal snapshot без изменения request bytes.
2. RED generation/cache tests; generation-tagged worker path, bounded
   `DeviceManager` cache и его invalidation.
3. RED D-Bus round-trip/golden tests; новый tuple, runtime token и cache-only
   Manager2 getter при неизменном API 8 и Manager1.
4. RED owner/generation client tests; exact-owner request, validation,
   invalidation и QML properties.
5. Settings UI, English/Russian translations, QML state/layout/accessibility
   tests.
6. Focused suites, full fresh `package-check` и независимые read-only API,
   security и final-package reviews.

Каждый implementation slice сохраняет накопленные пользовательские изменения и
не смешивается с A6-A9 refactor, B11, C12 или hardware research.

#### Acceptance и проверки

- Для synthetic `1011` и `1021` один populated SysConfig response из обычного
  bootstrap даёт `Ready`; exact SysConfig OUT frame и request count `1`
  остаются прежними, getter не создаёт USB I/O.
- Независимый byte fixture строится из clean-room table, а не сериализуется тем
  же generated type, которым затем парсится. Он проверяет реализацию parser, но
  не считается hardware evidence реальных field numbers/nesting. Unknown extra
  fields не ломают V1.
- Empty body, absent scalar, present `false`, present enum `0`, unknown enum,
  unsafe product, zero/oversized geometry и malformed envelope имеют отдельные
  deterministic cases без false `Ready`.
- `1011/1021` cache привязан к exact product, identity и generation;
  disconnect, quiesce, session loss и stale worker result очищают либо не
  публикуют его. `2011`, legacy и unknown profile остаются `Unsupported`.
- Repeated D-Bus getter и GUI refresh не увеличивают protocol request counters.
- Initial `Unavailable`, за которым следует существующий display-session refresh
  той же generation с валидным cache, переходит в `Ready` без polling, нового
  signal или дополнительного USB request.
- Valid decoded SysConfig, после которого обязательная post-bootstrap activation
  завершается ошибкой, ни на мгновение не публикует cache или `Ready`.
- D-Bus round-trip фиксирует `(usttssuusb)`; golden manifest доказывает
  неизменность Manager1, прежних Manager2 members и всех старых wire signatures.
- Client отбрасывает wrong schema/status, unsafe string, invalid values,
  identity/revision/generation mismatch, stale owner/epoch и delayed reply.
- QML tests покрывают token-absent, `RuntimeUnavailable`, disconnected,
  unsupported, unavailable и ready states, geometry mismatch, English/Russian
  text, narrow layout и plain-text rendering.
- `qmake6 tryx-panorama-all.pro; and make -j2 package-check` проходит на fresh
  binaries; `git diff --check` не находит ошибок.

Software acceptance не означает hardware verification. Для `1021` после
отдельного разрешения выполняется обычный application bootstrap без
дополнительного SysConfig query и фиксируются только четыре безопасных значения
и geometry-match result. Для `1011` нужен отдельный community или maintainer
smoke. До этого B10 может иметь только `software-complete, hardware-pending`
status, а поддержка новой geometry/model не заявляется.

#### Риски, откат и out-of-scope

- Главный evidence-риск: field map пока не подтверждён сохранённым real raw
  fixture. Clean-room byte tests доказывают только соответствие parser
  утверждённой implementation table, а hardware capture/smoke остаётся отдельным
  release/support gate для реального mapping.
- Главный data-риск: принять proto3 default за присутствующее значение. Presence
  checks и all-or-unavailable V1 закрывают эту неоднозначность.
- Главный race-риск: показать cache предыдущего owner или USB generation.
  Exact owner, epoch, identity, revision и generation fences обязательны.
- Главный compatibility-риск: расширить существующий positional tuple или
  изменить Manager1. Новый самостоятельный type и golden manifest блокируют это.
- Device string является недоверенным input. Bounds, control/bidi rejection и
  plain-text QML обязательны.

Rollback удаляет новый runtime token, getter, самостоятельный tuple, client
state и Settings subsection. API version, Manager1, старые Manager2 signatures,
USB request sequence, persistent formats и device state не меняются. Добавленные
read-only protobuf fields не используются для writes; data migration и
compensating device action не нужны.

**Out of scope B10 V1:** новый или повторный USB query; SysConfig write;
автоматическая смена media geometry/profile; TURRIS specifications; VDEC/VPSS,
storage, rotation, mirror/flip, RGB, fan/pump/cooler controls; serial/chip/path
export; support-report schema change; CLI output change; API 9/Manager3;
публичное заявление о hardware verification до реального smoke.

### B12. Local headless CLI

**Статус:** реализовано 31 августа 2026 года после software gates. Отдельный
`tryx` binary, JSON v1, exact-owner D-Bus client, B3 export, package integration
и process-level tests готовы. Remote terminal qualification через SSH или VS
Code Remote SSH решением владельца проекта от 31 августа 2026 года перенесена в
backlog и не входит в поддержку или acceptance B12. Clean-HEAD source archive
gate пройден 1 сентября 2026 года после создания локального Git snapshot:
archive script принял чистое дерево, собрал committed `HEAD` и подтвердил
наличие обязательных CLI members.

#### Контекст и результат

B12 добавляет не headless-режим GUI, а отдельный локальный one-shot
`QCoreApplication` client `tryx`, установленный на том же хосте, что и runtime.
Запуск из удалённого терминала, VS Code Remote SSH, GUI forwarding и remote
desktop не являются частью текущего support contract и вынесены в backlog.

Package identity и графическая команда не переименовываются:

- native package остаётся `tryx-panorama-manager`;
- GUI остаётся `/usr/bin/tryx-panorama-manager`;
- новый CLI устанавливается как `/usr/bin/tryx` и документируется в
  `tryx(1)`;
- alias `tryx-panorama-cli` не добавляется, поскольку опубликованного старого
  интерфейса нет.

CLI переиспользует frozen API 8 Manager contracts, B0 capability handshake и
B3 redaction/writer. Новый D-Bus method, API bump, schema migration, network
service или второй hardware owner не нужны. QtGui может остаться link
dependency только ради существующего B3 report builder; `QGuiApplication`,
QPA plugin, `DISPLAY` и `WAYLAND_DISPLAY` не создаются и не требуются.

#### Public command contract

```text
tryx --help
tryx --version
tryx [--json] status
tryx [--json] capabilities
tryx [--json] operations
tryx [--json] support-report --output-dir ABSOLUTE_DIR
```

`--json` является единственной общей опцией первого среза и указывается не
более одного раза до команды. Short options, неявная команда, лишние
positional arguments и смешение `--help`/`--version` с рабочей командой
отклоняются с exit `2`. CLI не выполняет shell expansion, не принимает stdin,
URL, произвольное имя файла, `--force`, `--yes`, `--bus-address` или публичный
`--timeout`. `--help` и `--version` работают до D-Bus и display
инициализации; version output имеет вид `tryx <VERSION>`.

Human-readable success выводится в stdout и не является parseable API.
Human-readable failure выводит в stderr только фиксированную bounded
категорию, не повторяя argv value, raw D-Bus error или runtime diagnostic.

Для parser-valid вызова с `--json` stdout всегда содержит ровно один compact
UTF-8 JSON object и завершающий newline. Success envelope v1:

```json
{"schema_version":1,"command":"status","ok":true,"data":{},"error":null}
```

Failure envelope v1:

```json
{"schema_version":1,"command":"status","ok":false,"data":null,"error":{"code":"RUNTIME_UNAVAILABLE"}}
```

`command` принимает только `status`, `capabilities`, `operations` или
`support-report`. `error` при ошибке содержит только стабильный ASCII `code`.
Localized message, raw payload, D-Bus owner и internal attempt не являются
частью JSON. Порядок object keys не является контрактом, array order является.
Изменение exact keys, types или enum semantics требует `schema_version: 2`.
При argv error JSON не создаётся: stdout пуст, фиксированный usage идёт в
stderr.

Command-specific `data` v1:

- `status` содержит `generated_at_utc` и exact B3 objects `runtime`,
  `device`, `recovery` и `counts`. Device object ограничен `product_id`,
  `model`, `firmware_version`, `app_version`, `connected`,
  `printer_class_connected`, `printer_class_device_present`,
  `display_session_active`, `connection_revision` и
  `physical_generation`;
- `capabilities` содержит `runtime_api_version`,
  `legacy_runtime_capabilities`, `runtime_capabilities`,
  `device_capabilities_status` и `device_capabilities`.
  `device_capabilities_status` принимает `available`, `not_connected` или
  `unsupported`. Capability arrays имеют canonical allowlist order;
- `operations` содержит `generated_at_utc`, `operation_count`,
  `returned_count`, `truncated` и `items`. Каждый item имеет только `kind`,
  `state`, `stage`, `error_category`, `terminal_outcome`,
  `primary_error_category`, `retry_mode`, bounded `attempt` и
  `apply_after_upload`. `items` содержит не больше 32 последних операций в
  oldest-to-newest order внутри bounded tail;
- `support-report` содержит только опубликованный absolute remote `path` и
  `runtime_snapshot_status` со значением `available`, `unsupported` или
  `unavailable`. Сам путь не включается в содержимое B3 report.

#### Источники данных и redaction

`status` и `operations` вызывают только `Manager2.GetSupportSnapshotV1()`,
затем обязаны полностью пройти `supportSnapshotV1IsValid()` перед извлечением
allowlisted полей. Они не используют raw `GetConnectionSnapshot()` или
`GetOperations()` для вывода. Это сохраняет одну B3 redaction boundary и
исключает serial, chip ID, operation/parent/active ID, subject, result name,
message, path, hash, media name и byte counters.

`capabilities` выполняет `GetRuntimeApiVersion()` и
`GetRuntimeCapabilities()`. Только exact
`org.freedesktop.DBus.Error.UnknownMethod` для второго вызова означает
совместимый legacy API 8 с пустым runtime capability list и
`legacy_runtime_capabilities: true`. Любая другая ошибка fail closed.

Если advertised `runtime.device-capabilities.v1` доступна и printer-class
device подключён, raw connection snapshot используется только внутри процесса
для проверки `GetDeviceCapabilitiesV1()`: schema `1`, непустая exact identity,
connection revision и nonzero physical generation должны совпасть. Identity и
raw snapshot никогда не сериализуются, не логируются и не попадают в stderr.
Runtime/device tokens проходят существующие bounded case-sensitive allowlists;
unknown, duplicate, oversized и malformed tokens не публикуются.

`status` и `operations` требуют advertised
`runtime.support-snapshot.v1`. Legacy API 8 или отсутствие token дают
`UNSUPPORTED`, а не независимый второй sanitizer поверх raw tuples.

#### D-Bus owner и remote boundary

Одна рабочая команда:

1. подключается только к уже существующей user-session D-Bus того же Unix
   пользователя через `QDBusConnection::sessionBus()`;
2. разрешает well-known `org.tryx.Panorama` в exact unique owner вида `:1.N`;
3. отправляет каждый method call только exact owner и выставляет
   `setAutoStartService(false)`;
4. использует один attempt и один общий monotonic D-Bus deadline;
5. после каждого reply и непосредственно перед stdout или file publish
   повторно проверяет ownership well-known name;
6. при owner replacement отбрасывает собранный результат, не публикует success,
   не создаёт report и не выполняет automatic retry.

CLI не запускает `RuntimeBootstrap`, `systemctl`, build-tree runtime, новый
session bus или `dbus-run-session`; не регистрирует `org.tryx.Panorama`, не
создаёт `DeviceManager` и не синтезирует путь к bus при отсутствии session
environment. `sudo tryx`, system bus, peer/remote D-Bus, TCP, Unix listener,
web panel и SSH server не поддерживаются.

Оператор может отдельно запустить установленный runtime командой
`systemctl --user start tryx-panorama.service`. Текущий support contract
ограничен локальным terminal того же пользователя при доступной его
user-session bus. CLI не меняет linger, PAM, desktop-session или service
enablement policy.

#### Stable exit codes

| Code | JSON error code | Значение |
|---:|---|---|
| 0 | отсутствует | Команда или host-only report успешно выполнены |
| 2 | не создаётся | Неверные argv; stdout пуст, usage выводится в stderr |
| 3 | `SESSION_BUS_UNAVAILABLE` | User-session D-Bus недоступна |
| 4 | `RUNTIME_UNAVAILABLE` | Владелец runtime отсутствует |
| 5 | `INCOMPATIBLE_RUNTIME` | Runtime API не равен `8` |
| 6 | `UNSUPPORTED` | Команда или capability не поддерживается |
| 7 | `TIMEOUT` | Общий D-Bus deadline истёк |
| 8 | `OWNER_CHANGED` | Runtime owner сменился во время команды |
| 9 | `RUNTIME_CALL_FAILED` | D-Bus method завершился другой ошибкой |
| 10 | `INVALID_REPLY` | Reply malformed, oversized или нарушает context |
| 11 | `UNSAFE_DESTINATION` | Output directory или аргумент пути небезопасен |
| 12 | `ALREADY_EXISTS` | No-clobber publication обнаружила collision |
| 13 | `EXPORT_IO` | Локальная write, fsync или publish операция не завершена |
| 70 | `INTERNAL_ERROR` | Нарушен внутренний software invariant |

Все D-Bus calls одной команды делят фиксированный deadline 5000 ms: очередной
call получает только оставшееся время. Это bounded collection contract, а не
5000 ms на каждый method. Публичный timeout knob не добавляется.

`support-report` после collection напрямую использует синхронный B3 writer.
`open`, `write`, `fsync` и `rename` на локально смонтированном NFS/FUSE могут
зависнуть в kernel вне контроля Qt timer, поэтому жёсткий wall-clock deadline
всего export не обещается. Такой deadline потребовал бы отдельного helper
process и reconciliation неизвестного publish outcome; это не входит в B12.

#### B3 report export

`support-report --output-dir` принимает только существующий absolute directory
удалённого хоста. CLI не создаёт каталог и всегда генерирует имя через
`generatedFileName()`. `buildReportV1()` и `writeNewReport()`
переиспользуются напрямую, поэтому сохраняются:

- B3 report limit 1 MiB и validated runtime snapshot limit 256 KiB;
- покомпонентный `O_NOFOLLOW`, exact canonical directory и owner UID;
- запрет group/other-writable destination;
- pinned directory device/inode;
- private temporary regular file `0600` с `nlink == 1`;
- full write, file/directory `fsync`, `RENAME_NOREPLACE` и post-publish
  identity verification;
- отсутствие overwrite, symlink, hardlink, FIFO и directory replacement race.

Host-only export сохраняет уже принятое поведение B3:

- нет user bus или runtime owner: report со статусом `unavailable`, exit `0`;
- API несовместим, legacy API 8 или B3 capability отсутствует: report со
  статусом `unsupported`, exit `0`;
- runtime advertised B3, но call завершился timeout/error, owner сменился или
  snapshot невалиден: report не создаётся, exit ненулевой.

Исключение host-only относится только к успешному созданию запрошенного
диагностического файла. `status`, `capabilities` и `operations` при отсутствии
нужных runtime data завершаются соответствующим ненулевым code.

#### Build, package и test contract

Implementation добавляет отдельный `tryx-cli.pro`, output
`build/cli/tryx`, небольшой CLI runner, process-level tests и
`packaging/tryx.1`. Aggregate `tryx-panorama-all.pro` собирает runtime, CLI и
GUI, а `package-check` запускает отдельный `cli-check` до Quick suites.

Install/package gates требуют `/usr/bin/tryx` и ровно одну compressed или
uncompressed `tryx(1)`, проверяют exact `tryx <VERSION>`, executable bit,
`ldd` и отсутствие Qt Widgets/QML/Quick linkage. RPM `%files`, Debian clean,
package-content verifier, source archive и Fedora/DEB/Arch CI smoke получают
явное CLI coverage. Desktop entry, AppStream launchable, user-systemd unit и
preset не меняются.

Software acceptance включает:

- `--help`/`--version` без bus, `DISPLAY` и `WAYLAND_DISPLAY`;
- no bus, no owner, timeout, API mismatch и exact legacy `UnknownMethod`;
- отсутствие autolaunch и подключения для unset, TCP/nonce-TCP,
  abstract/system bus, fallback list и Unix socket в небезопасном каталоге;
- C1 controls, ALM/LRM/RLM и Unicode line/paragraph separators в output path
  дают `UNSAFE_DESTINATION` до D-Bus collection и не попадают в output;
- exact unique-owner destination, replacement между любыми replies и delayed
  stale reply;
- capability identity/revision/generation validation и canonical ordering;
- B3-only status/operations, limit 32 и sensitive canaries во всех streams;
- JSON v1 golden tests и стабильные exit codes;
- host-only `unavailable`/`unsupported` export;
- отсутствие файла после advertised-B3 failure, invalid reply или stale owner;
- существующие no-clobber, symlink, FIFO, hardlink и directory-race writer
  tests;
- доказательство отсутствия runtime bootstrap и нового listening socket;
- fresh `dbus-run-session -- make package-check`, staged install verifier,
  `git diff --check` и build-tree `build/cli/tryx`.

Remote terminal qualification через SSH или VS Code Remote SSH вынесена в
backlog и не является release gate B12. Software tests доказывают только
локальный headless/user-bus contract.

Реализация прошла process-level CLI suite, fresh
`dbus-run-session -- make package-check`, staged install verifier,
`git diff --check` и независимые read-only reviews. Clean-HEAD source archive
создан после локального Git snapshot: fail-closed archive script принял чистое
дерево, упаковал committed `HEAD` и подтвердил обязательные CLI members.

#### Отклонённые варианты, rollback и дальнейшее развитие

Shell wrapper вокруг `busctl` отклонён: он не даёт достаточной typed
validation, JSON versioning, owner fencing, redaction parity и secure B3
publication. Переиспользование всего Quick `RuntimeClient` отклонено: он
включает GUI/bootstrap lifecycle, subscriptions и raw refresh paths, которые
не нужны one-shot CLI. Network D-Bus, web API и GUI forwarding расширяют
attack surface и не нужны для локального terminal client.

Rollback удаляет CLI subproject, `/usr/bin/tryx`, `tryx(1)` и package entries.
D-Bus API, runtime, persistent data и device state не меняются, поэтому data
migration, runtime downgrade или compensating USB mutation не нужны.

Любая будущая mutation, включая upload, Apply, Delete, firmware, power или
явный runtime start, получает отдельный proposal: exact capability/device
identity, интерактивное подтверждение, idempotency/operation identity,
terminal/readback result и запрет automatic replay после неизвестного исхода.
Общий неинтерактивный `--yes` не добавляется как обход safety gates.

**Out of scope первого среза:** изменение device state, remote terminal
qualification через SSH или VS Code Remote SSH, запуск Qt GUI через SSH, remote
desktop, browser UI, network/peer D-Bus, привилегированный daemon,
автоматическая передача локальных файлов, output filename/overwrite, firmware
mutation, service enablement/linger и обход desktop/user-session permissions.

## Workstream C: Display и media workflow

| ID | Функция | Зависимости | Acceptance |
|---|---|---|---|
| C1 | Per-side styling | B1 | Full использует один style block с color, alignment и Waterfall Top/Bottom; Split получает независимые Left/Right color и alignment при фиксированном размещении сторон; отправляется одна verified Apply operation |
| C2 | Dirty-state guard | C1 | При route/close предлагаются Apply, Discard и Stay; закрытие не применяет USB mutation автоматически |
| C3 | API-8-safe media origin UI | A5 | Уже доступные source, size и thumbnail показываются как User media или Device preset; сумма sizes не называется free space |
| C4 | Split-aware crop | B0, C3 | отдельный versioned transform profile и честный area canvas; full origin не переиспользуется как split transform |
| C5 | Saved layouts | B0, C1, C2, C3 | versioned, device-scoped layouts; fresh catalog validation; только explicit Apply; никакого replay после reconnect |
| C6 | Safe cache management | A5 | очищаются только inactive Quick previews, unindexed thumbnail orphans и expired/revoked idle artifacts; indexed thumbnails и весь recovery state сохраняются, а active operation, retry, journal или действующая lease блокируют cleanup |
| C7 | GIPHY integration (отменено) | нет | Не планируется: ни встроенный API/search, ни внешний browser handoff не входят в scope; существующий импорт локальных media-файлов сохраняется |
| C8 | Wayland screen recorder (отложено) | C3 | Не входит в активный scope: перед возвратом нужна отдельная матрица поддержки XDG ScreenCast Portal, PipeWire и desktop backends с честным unavailable fallback |
| C9 | Global shortcuts (ожидает C8) | C8 | Не планируется отдельно от recorder; отсутствие portal не должно ломать GUI, а shortcut не должен обходить системное разрешение |
| C10 | Device-side fonts | B0, D2 или D4 | allowlisted enum и exact readback только на модели, где selector и protocol подтверждены; для PANORAMA/PASE не включать по данным KANALI 2.4.0 |
| C11 | Passive `play_finished` diagnostics | D1 | сначала read-only trace; событие не запускает automatic Apply или replay |
| C12 | Extended media metadata contract | B0, C3 | Dimensions, duration и FPS подготовленной device-копии получают bounded local `ffprobe` после explicit FilePull и публикуются artifact-scoped методом `GetDeviceMediaMetadataV1`; старый API 8 tuple не меняется |
| C13 | Display frame-rate control | B0, D1 | только allowlisted значения и exact readback на отдельно подтверждённом product profile; неподдерживаемая модель не получает generic write |
| C14 | Linux display sleep policy | B4, D1 | отдельно исследуются GUI Quit, runtime stop, suspend и shutdown; политика opt-in и model-gated, не переписывает standby media и не обещает USB mutation после начала poweroff; resume не повторяет предыдущую mutation |
| C15 | Extended telemetry and metric pages | C15.1: B0, C1, B9.1 software; 4+/pages: B9 hardware acceptance, D1 | Сначала расширяются подтверждённые host telemetry sources и явный selector с честным лимитом Full 3, Split 3+3; 4+ метрик, pages или rotation допускаются только через model-scoped versioned capability после hardware captures/readback, а старый API 8 сохраняет максимум 3 |
| C16 | Пользовательский текст бейджей | B0, C1, C2, C5 | Для существующих CPU/GPU slots доступны Auto и Custom с bounded plain text; один явный Apply, сохранение и восстановление текста без подмены Auto; новые versioned API/types без изменения старых tuples; software acceptance отдельно от hardware glyph/placement проверки |

### C1. Per-side styling

**Статус:** реализовано 26 августа 2026 года. Software acceptance завершён;
визуальная hardware-проверка exact RGB/alignment и Full Waterfall placement не
выполнялась и требует отдельного явного разрешения на Apply.

**Цель:** раскрыть в Qt Quick уже существующие API 8 style fields без нового
D-Bus или protobuf-контракта. Full Screen получает один редактируемый style
block. Для поддерживаемого printer-class Split каждая сторона получает свой
точный RGB24 color и horizontal alignment. Любое изменение media, metrics,
badges и styling отправляется одним явным Apply, а не последовательностью
независимых foreground mutations.

#### Подтверждённое текущее состояние

- `TryxRuntimeApplyRequest` и `TryxRuntimeDisplayState` уже содержат primary
  `settingsPosition`, `settingsColor`, `settingsAlign` и secondary поля с
  суффиксом `2`. Их positional D-Bus serialization входит в API 8 и не
  расширяется.
- `RuntimeClient` публикует в QML шесть подтверждённых style fields обеих
  областей. `applyFullScreen()` и `applySplitScreen()` принимают явные style
  drafts и включают их в один immutable apply request.
- `PaseOverlayConfig`, `PaseMetricsConfigStore` v2, retry/apply codec и
  RunConfig builder уже хранят и передают обе области. Миграция persistent
  schema не требуется.
- `PanoramaPage` редактирует style вместе с media, metrics и badges и вызывает
  только основной Apply. Блок Live metrics показывает sampling status только
  для чтения и больше не вызывает `QueueMetricsConfig` как второй способ
  изменения layout.
- Установленный KANALI 2.4.0 проверен непосредственно по `app.asar`.
  Active Customization показывает vertical selector только для Full Screen при
  включённом Waterfall. Renderer применяет `locationUpDown` только для
  однообластного Waterfall. В Split он фиксирует Left в нижней, а Right в
  верхней физической половине и игнорирует отдельный vertical placement.
  Текущий Linux RunConfig builder повторяет эту geometry.
- Firmware не предоставляет query для `OverlayLayout`. Успешная operation
  подтверждает tracked activation layout и точный readback запрошенных
  `UserConfiguration` fields, но не exact device-side readback color,
  alignment или placement. После успеха runtime объединяет фактический display
  readback с принятым и сохранённым overlay state.
- Frozen Manager1 `SetScreenConfig` и legacy serial adapter передают только
  один style block. Legacy Split использует один общий editor, а различающиеся
  Left/Right значения отклоняются в client до dispatch. Независимый Right style
  доступен без изменения API только через существующий Manager2/API 8
  printer-class request.

#### Принятое решение

1. API 8 types, tuple order, Manager1/Manager2 methods, protobuf wire schema и
   persistent formats не меняются.
2. `RuntimeClient` публикует шесть read-only QML properties из подтверждённого
   `DisplayState`: Left/primary и Right/secondary position, color и alignment.
3. `PanoramaPage` хранит отдельные Full, Left и Right drafts, как уже делает
   для metrics и badges. При получении нового подтверждённого DisplayState:
   - Full и Left инициализируются primary style;
   - Right инициализируется secondary style;
   - отсутствующие значения получают канонические defaults
     `Top/#dcdcdc/Left` для Full/Left и `Top/#dcdcdc/Right` для Right.
4. Один переиспользуемый QML style editor показывает color swatch, exact
   `#RRGGBB` input и Left/Center/Right. Full дополнительно показывает
   Top/Bottom только для подтверждённого Waterfall и явно сообщает, что в
   обычной orientation этот selector не действует.
5. Printer-class Split показывает два независимых адаптивных editor для Left и
   Right color/alignment. Top/Bottom в Split не показывается: стороны уже
   определяют фиксированные физические области, а новая geometry без
   hardware-backed контракта не изобретается. Сохранённые internal placement
   values не переписываются отдельной скрытой mutation.
6. Legacy Full и Split используют один общий style editor. Прямой legacy вызов
   с различающимися Left/Right style values отклоняется до dispatch, а не
   молча зеркалируется. Независимый Right editor не изображается работающим.
7. Existing `applyFullScreen()` и `applySplitScreen()` получают явные typed
   string arguments для style drafts, формируют один immutable
   `TryxRuntimeApplyRequest` и для printer-class выполняют ровно один
   `QueueApplyWithMetrics`. Legacy выполняет один frozen Manager1
   `SetScreenConfig`. Full семантически использует только primary block; Split
   требует оба.
8. Alignment допускает только `Left`, `Center`, `Right`; position только
   `Top`, `Bottom`; color только полный шестизначный `#RRGGBB` без пробелов и
   alpha. UI и `RuntimeClient` валидируют черновик, а runtime повторяет
   allowlist validation на D-Bus boundary до worker и первого USB write.
   Непустые невалидные значения больше не нормализуются молча в Left/Top.
   Пустые style fields старых API 8 и internal callers считаются отсутствующим
   значением и получают прежние канонические defaults для обратной
   совместимости; это не ослабляет rejection непустого невалидного ввода.
9. Общие color/alignment controls и Start/Stop action удаляются из
   `metricsOverlayControls` страницы Panorama. Sampling status остаётся
   read-only. Наличие выбранных metrics в основном Apply включает sampling;
   очистка metrics и новый Apply останавливают sampling, не требуя удаления
   badges. Публичный API 8 `QueueMetricsConfig` не удаляется, но C1 UI его не
   использует как параллельный layout editor.
10. `1011` и `1021` сохраняют существующий backend capability gate. `2011`
    остаётся без display/overlay mutation. QML visibility не заменяет runtime
    validation по product profile, active session, device generation, busy и
    recovery state.

#### Граница verified Apply

Один пользовательский клик означает одну foreground operation, но не один USB
packet. Существующая безопасная последовательность сохраняется:

1. bounded fresh `UserConfiguration` preflight query;
2. одна single-shot запись изменённого `UserConfiguration`;
3. одна tracked activation полного `OverlayLayout` обеих активных областей;
4. bounded fresh `UserConfiguration` readback и exact проверка mode, media и
   явно изменённых display fields.

Успешно отправленный layout нельзя называть exact style readback. Потеря
подтверждения mutation, неполная activation или failure после возможной записи
сохраняют существующий `PartialOrUnknown`, закрытие session и запрет
automatic replay, rollback или compensating Apply. Draft не публикуется как
confirmed state до terminal success и нового `DisplayStateUpdated`.

#### Этапы реализации

1. RED Quick tests фиксируют шесть readback properties, exact Full request,
   разные Split style blocks, legacy shared-style gate, strict invalid input и
   одну queued operation на вызов.
2. RED daemon tests фиксируют rejection неизвестного alignment/position/color
   до worker dispatch для Full и Split и сохраняют product/session/generation
   gates.
3. Реализуются `RuntimeClient` properties, explicit request builders и
   двухуровневая validation без изменения runtime contract types.
4. Добавляется reusable QML editor, drafts и адаптивная компоновка: два Split
   editor располагаются в две колонки только при достаточной ширине, иначе в
   одну.
5. Удаляется конфликтующий layout-editing path Live metrics, обновляются QML
   resource/build manifests и русский translation catalog.
6. После узких GREEN tests выполняются полный Quick/QML/protocol gate,
   translation/catalog gate и fresh-binary `package-check`.

#### Результат реализации, 26 августа 2026 года

- Добавлены шесть read-only style properties, явные Full/Split request builders
  и legacy equality gate в `RuntimeClient`.
- Добавлен reusable `OverlayStyleEditor`: exact RGB24 input, color dialog,
  alignment, условный Full Waterfall placement, side-specific accessibility и
  одноколоночный reflow для узкого окна.
- Runtime boundary валидирует оба активных style blocks в Apply,
  upload/ensure и Replace до worker dispatch и USB write, сохраняя
  совместимость с отсутствующими style values старых API 8 callers.
- Один UI Apply формирует одну `QueueApplyWithMetrics` operation для
  printer-class либо один `SetScreenConfig` для legacy; `PanoramaPage` больше
  не использует `QueueMetricsConfig` для изменения layout.
- Полный software `package-check` прошёл на свежесобранных binaries: 778
  printer protocol cases, 17 replace journal, 39 QML, 83 Quick client, 37
  runtime bootstrap и 18 Linux tray tests; translation catalog и runtime
  refactor baseline также прошли. Fresh-binary offscreen smoke включён в этот
  gate и прошёл.
- Hardware visual smoke не выполнялся: software readback не доказывает
  фактический цвет, alignment или Full Waterfall placement на устройстве.

#### Acceptance

- Full показывает ровно один style editor и отправляет exact primary
  `#RRGGBB`, alignment и, для Waterfall, Top/Bottom;
- supported printer-class Split показывает два независимых editor и один Apply
  передаёт разные Left/Right color и alignment в одном request;
- обычный Full и любой Split не обещают несуществующую управляемую vertical
  geometry; Split placement определяется стороной;
- legacy Split явно использует один общий style и не теряет различающийся
  Right draft молча;
- invalid color, alignment или position блокирует dispatch и на client, и на
  runtime boundary;
- один click не вызывает `QueueMetricsConfig`, отдельный style Apply или
  повторную operation;
- success означает подтверждённый UserConfiguration readback и успешно
  активированный layout, но UI и документация не называют style exact hardware
  readback;
- QML controls доступны с клавиатуры, имеют side-specific accessible names и
  не выходят за ширину узкой одноколоночной страницы;
- API version остаётся 8, serialized tuple и protobuf schema не меняются,
  persistence migration отсутствует;
- software `package-check` проходит на свежесобранных binaries. Визуальная
  hardware-проверка exact RGB/alignment и Full Waterfall placement на `1021`
  выполняется только после отдельного явного разрешения на Apply и отмечается
  отдельно от software acceptance.

#### Риски и откат

- Layout activation не имеет device query. Visual hardware smoke может выявить
  renderer divergence, которую software readback доказать не способен.
- External successful display operation публикует новый confirmed state и
  синхронизирует draft. Защита несохранённого draft при route/close относится
  к C2 и не симулируется внутри C1.
- Legacy и printer-class имеют разную ширину контракта; client-side equality
  gate предотвращает silent data loss, runtime product gates остаются
  обязательными.
- Rollback удаляет C1 QML/editor arguments и возвращает сохранение
  подтверждённого style в request builders. Schema downgrade и data migration
  не нужны. Уже применённый device layout не переписывается автоматически и
  compensating USB Apply не отправляется.

**Out of scope:** новая Split vertical geometry, новый device-side overlay
query, D-Bus API 9, protobuf fields, изменение Manager1, C2 dirty-state guard,
C4 crop, C5 saved layouts, Turris display support, fonts, filters, FPS и
automatic replay после reconnect.

### C2. Dirty-state guard

**Статус:** реализовано 26 августа 2026 года. Software acceptance завершён;
hardware Apply не выполнялся и требует отдельного явного разрешения.

**Цель:** исключить silent loss несохранённого Display draft при route,
закрытии GUI и обновлении live state. Любой device write выполняется только
после явного `Apply changes`; close, `Discard changes` и `Stay` сами по себе не
отправляют USB mutation.

#### Подтверждённое текущее состояние

- `PanoramaPage` хранит QML drafts для media, Full/Split mode, play mode,
  metrics, badges, Full/Left/Right style, brightness, mirror и waterfall.
  Backlight не является draft: текущая кнопка отправляет mutation сразу.
- `synchronizeDisplayDraft()` безусловно копирует весь подтверждённый
  `DisplayState` в draft, а `onDisplayChanged()` вызывает её для каждого
  события. Поэтому draft может быть стёрт не только после будущего разрушения
  route, но и при refresh, внешнем display update, runtime invalidation или
  `RuntimeClient::retranslate()` при смене языка.
- Текущий `StackLayout` сохраняет экземпляр `PanoramaPage` при route, поэтому
  простое переключение Dashboard/Display/Settings пока не уничтожает draft.
  Это свойство реализации не заменяет явный leave contract: скрытая страница
  всё равно получает `displayChanged`, а будущая смена navigation layout не
  должна менять data-safety semantics.
- Sidebar и Dashboard меняют `currentPage` напрямую. Системный close, `Alt+F4`
  и кнопка окна сходятся в `ApplicationWindow.onClosing`, где применяется
  текущая Quit GUI или Hide to tray policy. При Hide QML остаётся живым, при
  Quit draft теряется.
- Explicit Tray Quit сейчас вызывает `QCoreApplication::exit(0)` напрямую и
  полностью обходит QML close handler. Этот путь обязан участвовать в C2.
- Printer-class Apply имеет caller-generated operation ID, operation lifecycle
  и новый `DisplayState`. Queue acknowledgement не доказывает успех. На
  success path подтверждённый state может прийти до terminal operation.
- Frozen Manager1 не имеет operation ID для `SetScreenConfig` и
  `SetBrightness`. Layout вместе с waterfall представимы одним
  `SetScreenConfig`, brightness использует отдельный `SetBrightness`, а mirror
  из текущего Quick UI для legacy не поддерживается. Поэтому произвольную
  комбинацию этих domains нельзя считать одной атомарной legacy-командой.
- Существующий `TryxRuntimeApplyRequest` уже содержит layout, metrics, badges,
  styles и `TryxRuntimeDisplayMutation`. Для printer-class C2 может объединить
  layout, brightness и orientation в одну существующую API 8 operation без
  нового D-Bus method, tuple field, protobuf field или persistent format.

#### Принятое решение

1. C2 охватывает весь основной Display draft:
   - выбранные media, Full/Split и play mode;
   - активные metrics, badges и Full либо Left/Right style;
   - ещё не применённые brightness, mirror и waterfall controls.
   Backlight, Media Editor transform draft, staged upload/replace и firmware
   dialogs не входят в этот guard: у них другой mutation и terminal contract.
2. `PanoramaPage` продолжает владеть editable values, но отделяет их от
   immutable canonical confirmed snapshot. Страница публикует в `Main.qml`
   только небольшой локальный contract: наличие изменений, валидность,
   возможность одного Apply, Discard и завершение submitted Apply.
3. `Main.qml` становится единственным владельцем route и leave intent. Все
   прямые присваивания `currentPage` проходят через один guard. Guard хранит
   только первый pending intent: `Route(target)`, `HideToTray`, `WindowQuit`
   или `ExplicitQuit`; повторный route/close во время prompt или Apply не
   заменяет его и не создаёт второй dispatch.
4. Любой dirty close gesture показывает guard, включая close при активной
   Hide to tray policy. Для clean state прежнее поведение сохраняется. После
   подтверждённого Discard или Apply intent Hide скрывает окно, WindowQuit
   повторяет обычный close path с one-shot approved-close bypass, ExplicitQuit
   завершает GUI напрямую и не попадает повторно в Hide policy.
5. Explicit Tray Quit больше не соединяется напрямую с `exit(0)`. Он передаёт
   intent в загруженный QML root; если окно скрыто, оно показывается для
   решения. Только отдельный approved continuation может завершить event loop.
6. `RuntimeClient` получает process-local submission identity и единый
   display-apply lifecycle. Это additive QML/client contract, а не изменение
   Manager1/Manager2 D-Bus API. Синхронный start закрывает окно double-click до
   асинхронного operation snapshot.
7. `Apply changes` фиксирует immutable submitted snapshot и делает ровно один
   client dispatch. Для printer-class это одна runtime Apply operation, внутри
   которой сохраняется существующая verified последовательность config write,
   activation и readback. Continuation разрешается только когда для той же
   submission получены и terminal `Succeeded`, и matching fresh
   `DisplayState`; порядок этих двух событий не важен. Request acceptance,
   progress или один readback без terminal success недостаточны.
8. Rejection, `Failed`, `Cancelled`, `RetryAvailable`, runtime invalidation,
   bounded wait без доказанного результата или mismatching readback отменяют
   pending leave, но сохраняют draft и текущую страницу. Автоматические retry,
   replay, rollback и compensating USB Apply запрещены. Partial или unknown
   outcome не называется failure либо success.
9. Для printer-class все изменённые Display domains объединяются в одну
   существующую API 8 operation. Для legacy Apply доступен только когда весь
   dirty payload представим одной поддерживаемой Manager1 mutation. Например,
   layout вместе с waterfall либо одна brightness mutation допустимы, а layout
   вместе с brightness, brightness вместе с waterfall либо изменённый mirror
   блокируют Apply с понятным объяснением. Пользователь может выбрать Stay и
   применить секции отдельно либо Discard. Последовательность legacy-команд,
   способная дать partial success, не отправляется.
10. Dialog является modal и не закрывается наружным click. Безопасный initial
    focus и действие Escape равны `Stay`; `Apply changes` отключён при invalid,
    disconnected, busy, conflicting, unresolved либо не представимом для
    legacy draft. `Discard changes` никогда не является rollback уже
    отправленной operation. Пока собственный Apply выполняется, Discard
    недоступен; после перехода в explicit `Unresolved` он снова доступен как
    сознательный отказ от локального draft без утверждения о device outcome.

#### Canonical draft и confirmed baseline

Process-local состояние C2 содержит:

- identity текущего display device и revision последнего принятого state;
- canonical confirmed payload;
- editable draft и domain-specific dirty flags;
- optional external-conflict flag;
- immutable submitted payload и его local/remote identity на время Apply.

Canonical comparison использует только payload, который реально будет
отправлен:

- Full сравнивает одну media, выбранный Full play mode, primary
  metrics/badges/style;
- Split сравнивает две media по порядку, всегда `Single`, обе стороны
  metrics/badges/style;
- неактивные Full или Split edit buffers не создают ложный dirty и после
  успешного Apply или Discard пересинхронизируются с новым baseline;
- списки сравниваются по длине и порядку, не как множества;
- valid `#RRGGBB` сравнивается после lowercase normalization;
- brightness bounded до `0..100`; mirror и waterfall сравниваются как exact
  booleans;
- legacy shared-style и capability gates остаются теми же, что в C1.

При новом confirmed state:

1. clean draft полностью синхронизируется;
2. идентичный state или `retranslate()` не меняет dirty state;
3. dirty draft сохраняется, а clean fields/domains получают новый baseline и
   синхронизируются с ним;
4. conflict возникает только если external writer изменил field/domain,
   пересекающийся с dirty payload. Изменение clean domain, например brightness
   при dirty layout, merge-ится без блокировки Apply;
5. matching state собственной submission учитывается, но leave всё равно ждёт
   её terminal result;
6. runtime invalidation не превращает пустой state в новый baseline;
7. reconnect с другой device identity не применяет старый draft. Apply остаётся
   запрещённым до явного Discard/rebase, а автоматического replay нет.

`Discard changes` всегда восстанавливает последний безопасный confirmed
baseline, а не snapshot, существовавший в момент первого edit.

#### Transition state machine

1. В `Idle/Clean` route или close выполняется сразу через существующую policy.
2. В `Idle/Dirty` первый leave request блокируется и открывает prompt.
3. `Stay` удаляет pending intent, сохраняет draft и оставляет Display видимым.
4. `Discard changes` пересинхронизирует draft без device call и ровно один раз
   продолжает сохранённый intent.
5. `Apply changes` фиксирует payload, блокирует повторный submit и переходит в
   ожидание коррелированного результата.
6. Только confirmed success и неизменившийся submitted payload очищают dirty и
   продолжают intent. Любой новый edit после submission, conflict или
   недоказанный outcome отменяют continuation.
7. Bounded wait без доказанного результата или runtime invalidation переводят
   submission в `Unresolved`: pending leave отменяется, Apply/replay запрещены,
   но пользователь может выбрать Stay либо явно Discard локальный draft и
   выполнить новый route/close. Этот Discard не заявляет, что device mutation
   не произошла, и не отправляет compensating write.
8. Посторонняя active operation отключает Apply, но не Discard и Stay. Guard не
   ставит mutation в скрытую очередь и не запускает её автоматически после
   освобождения runtime.

Close event сначала отклоняется, и только затем guard решает Hide, prompt или
approved Quit. Approved WindowQuit повторяет close с one-shot bypass, который
сбрасывается при первом следующем close event; он не может обойти более поздний
dirty state. Это не позволяет asynchronous dialog появиться после уже
принятого compositor close и не создаёт рекурсивный prompt.

#### Границы реализации

- `PanoramaPage` владеет draft, canonical snapshots, merge/conflict rules и
  формированием одного submitted payload.
- `Main.qml` и один reusable QML guard/dialog владеют navigation, pending intent,
  focus, continuation и responsive UI.
- `RuntimeClient` владеет validation, local submission token, modern operation
  correlation, legacy confirmation и runtime invalidation result. Daemon не
  получает GUI dirty-state.
- `WindowChromeController` продолжает владеть window actions и Hide policy, но
  не draft и не device Apply.
- `LinuxTrayController` публикует Show/Quit intents; решение о завершении GUI
  принимает общий guard.
- `AppSettingsController` продолжает владеть persisted close policy. C2 не
  сохраняет draft на диск и не добавляет настройки.

#### Этапы реализации

1. RED QML tests фиксируют canonical dirty comparison, active Full/Split
   payload, screen-control domains, сохранение dirty draft при
   `displayChanged`, conflict, Discard и immutable submission.
2. RED Quick client tests фиксируют exact modern submission identity, оба
   порядка terminal/readback, legacy local confirmation, rejection и
   invalidation без replay.
3. Реализуются canonical baseline и merge rules в `PanoramaPage`; brightness и
   orientation включаются в единственный `Apply to display`, а прямые
   section-dispatch paths удаляются без изменения wire contract.
4. Все routes и close paths переводятся на Main-owned guard. Tray Quit
   подключается к approved continuation вместо direct exit.
5. Добавляется modal responsive dialog, keyboard/focus/accessibility contract,
   error state, QML resource/build manifests и русский translation catalog.
6. После узких GREEN tests выполняются полные Quick/QML/tray/protocol gates,
   translation/catalog gate, runtime-refactor baseline и fresh-binary
   `package-check`.

#### Результат реализации, 26 августа 2026 года

- `PanoramaPage` хранит canonical baseline отдельно от editable Full/Split,
  brightness и orientation drafts. Для Manager2 это подтверждённый readback;
  для Manager1 начальный identity-bound bootstrap явно не считается remote
  readback. Идентичный `displayChanged` и runtime invalidation не стирают
  draft; clean domains merge-ятся, а пересекающееся внешнее изменение и смена
  device identity выставляют fail-closed conflict до явного Discard.
- Один `applyChanges()` фиксирует immutable payload и вызывает один additive
  `RuntimeClient` submission. Printer-class success требует одновременно
  terminal `Succeeded` и свежий matching `DisplayState` той же device
  identity, в любом порядке. Session loss, timeout и `PartialOrUnknown`
  сохраняют retained `Unresolved` без retry или replay.
- Legacy dispatch требует точный непустой serial и ограничен одной Manager1
  mutation: `SetScreenConfig` для полного layout с Waterfall либо изолированный
  `SetBrightness`. Manager1 не умеет запрашивать начальные layout/brightness,
  поэтому cached payload того же serial переживает disconnect/service loss
  только как quarantined UI baseline, а confirmation flags сбрасываются.
  Пустой serial не становится baseline, переход A -> B очищает payload, а
  отдельный Waterfall блокируется до подтверждения полного layout. Mirror и
  multi-command combinations отклоняются до dispatch с видимым объяснением.
- Все route, system/custom close, Hide to tray и Tray Quit проходят через один
  modal first-intent guard. `Stay` сохраняет draft, Discard не отправляет
  mutation, а explicit Quit достигает `exit(0)` только через отдельный
  approved continuation. Readback-before-terminal и повторный Alt+F4 не
  обходят guard.
- Dialog имеет безопасный initial focus на `Stay`, Escape, keyboard tab order,
  accessible names/descriptions, одноколоночный narrow reflow и разрешённый
  explicit Discard после unknown outcome без утверждения об итоговом device
  state.
- Полный software `package-check` прошёл на свежесобранных binaries: 778
  printer protocol cases, 17 replace journal, 70 QML, 94 Quick client, 37
  runtime bootstrap и 20 Linux tray tests. Каталог содержит 1576 завершённых
  переводов; translation/catalog gate, runtime-refactor baseline и
  fresh-binary offscreen smoke также прошли.
- D-Bus API остаётся 8; Manager1/Manager2 methods, serialized tuple, protobuf
  schema и persistent formats не менялись. Реальный USB/hardware Apply не
  выполнялся и не входит в подтверждённый результат C2.

#### Test-harness fidelity, 27 августа 2026 года

Штатный QML gate теперь явно использует `Material`, который production GUI
выбирает до создания `QGuiApplication`. Lifecycle tests ждут не только начала
exit transition (`Dialog.opened=false`), но и очистки first-intent state в
`onClosed`; фиксированного sleep и ослабления assertions нет. Два сценария
route-after-Stay и Explicit-Quit-after-Discard также выполняются как
именованный Material baseline, поэтому удаление production-style среды из
общего runner не скроет regression.

Fresh `package-check` прошёл: 778 printer/recovery cases, 17 replace journal,
84 Material QML, 95 Quick client/model, 37 runtime bootstrap и 20 tray.
Translation catalog и runtime baseline прошли. Production state machine, API 8,
wire, persistence и hardware/USB paths этим test-only follow-up не менялись.

#### Acceptance

- Любой route с dirty Display draft остаётся на странице до выбора Apply,
  Discard или Stay; click текущего route не открывает prompt.
- Dirty system close, `Alt+F4`, custom close button, Hide to tray и Tray Quit
  проходят через один guard. Clean close сохраняет существующую policy.
- Stay сохраняет draft; Discard продолжает intent без единого runtime/USB
  request; close никогда не выбирает Apply автоматически.
- Refresh, смена языка, identical state и runtime disconnect не стирают dirty
  draft. External update merge-ит clean domains, а пересечение с dirty payload
  сохраняет draft и выставляет conflict.
- Один Apply делает ровно один client dispatch и создаёт не более одной runtime
  Apply operation. Double-click и competing leave intents не создают вторую
  operation.
- Printer-class route/close продолжается только после exact submission
  terminal success и matching fresh DisplayState. Оба возможных порядка
  событий покрыты тестами.
- Legacy не выполняет последовательный composite Apply. Unsupported или
  multi-command draft ясно блокирует Apply; Stay и Discard остаются доступны.
- Failure, cancellation, retryable, mismatch и disconnect оставляют GUI
  открытым, draft сохранённым и не запускают retry/replay/rollback. После
  unknown outcome Apply остаётся заблокирован, но explicit Discard позволяет
  отказаться от локального draft и закрыть GUI без заявления об итоговом
  device state.
- Dialog доступен с клавиатуры, Escape равен Stay, безопасный initial focus не
  выбирает mutation, элементы имеют accessible names/descriptions и не выходят
  за узкую одноколоночную компоновку.
- API version остаётся 8; Manager1/Manager2 methods, serialized tuple, protobuf
  schema и persistent formats не меняются.
- Полный software `package-check` проходит на fresh binaries. Hardware Apply
  не выполняется без отдельного явного разрешения пользователя и не считается
  частью software acceptance C2.

#### Риски и откат

- Modern state и terminal signal приходят в разном порядке. Корреляция обязана
  хранить оба наблюдения независимо и никогда не продолжать intent по одному
  из них.
- Manager1 не даёт remote operation identity. Локальный token и exact matching
  confirmation закрывают обычный GUI path, но не превращают legacy в
  транзакционный protocol; при неизвестном исходе повторный Apply остаётся
  закрытым, а explicit Discard только отбрасывает локальный draft.
- External update, пересекающийся с dirty payload, создаёт конфликт. Clean
  domains merge-ятся, но автоматический merge конфликтующего domain либо replay
  может затереть изменения другого writer, поэтому C2 требует явный
  Discard/rebase.
- Прямой Tray Quit легко обойти отдельным `exit()` connection. Tests обязаны
  доказывать, что процесс не завершается до approved continuation.
- Crash, `SIGKILL`, power loss и compositor/session teardown не гарантируют
  interactive prompt. C2 не отправляет поздний USB Apply и не обещает
  persistence draft для этих случаев.
- Rollback удаляет QML guard, local RuntimeClient submission lifecycle и
  guarded tray connection. Schema downgrade и data migration не нужны. Уже
  применённый device state не переписывается, compensating USB Apply не
  отправляется.

**Out of scope:** persistent drafts между запусками, session-shutdown
inhibition, crash recovery UI, Media Editor/upload/replace drafts, изменение
немедленного backlight action, saved layouts C5, split-aware crop C4, API 9,
новые D-Bus methods или protobuf fields и automatic replay после reconnect.

### C3. API-8-safe media origin UI

**Статус:** реализовано 27 августа 2026 года. Software acceptance завершён;
hardware/USB smoke не выполнялся и для presentation-only C3 не требовался.

**Цель:** сделать уже существующий device media catalog честно читаемым в
общей `Media Library`: каждая карточка показывает подтверждённую категорию
источника, размер device-side файла и доступный thumbnail, не выдавая эти
данные за storage capacity, free space или свойства исходного локального
файла.

#### Подтверждённое текущее состояние

- API 8 `TryxRuntimeMediaEntry` уже содержит `name`, `size`, `source`,
  `readOnly`, `thumbnailKey` и `managedOrigin`. Manager2 дополнительно передаёт
  delete metadata и `mediaId`; positional D-Bus tuple не расширяется в C3.
- `MediaCatalogModel` уже публикует QML roles `mediaSize`, `mediaSource`,
  `readOnly`, `thumbnailUrl` и `managedOrigin`. Новый controller или model для
  C3 не нужен.
- Printer-class catalog формирует `source=1` только из firmware
  `media_file_list`, а `source=2` только из `preset_file_list`. Имя,
  `readOnly`, thumbnail и delete eligibility не используются как эвристика
  происхождения.
- `managedOrigin=true` означает наличие полной device-scoped local provenance
  записи для exact writable user tuple. `false` не доказывает внешний source,
  а само поле не является категорией `User media`/`Device preset`.
- `size` является размером подготовленного файла на устройстве. Original
  filename, source size, dimensions, duration, FPS и remote content hash через
  текущий catalog API не публикуются.
- Legacy serial/ADB path получает только список имён, синтетически выставляет
  `source=1` и оставляет `size=0`. Поэтому C3 не называет такие записи
  подтверждённым `User media` и не показывает ноль как реальный размер.
- Текущая карточка уже использует безопасно разрешённый `thumbnailUrl`, но не
  читает `mediaSource`, любой размер округляет до MiB и выводит для нуля
  `0.0 MiB`. Empty state `No uploaded media` неверен для общей библиотеки с
  device presets.

#### Выбранный UI и семантика

- C3 меняет только существующий delegate `Media Library` в `PanoramaPage`.
  Local file picker и `MediaEditor` не имеют authoritative device catalog
  identity и не получают origin badge.
- На thumbnail слева сверху размещается ограниченный по ширине текстовый badge,
  не пересекающий кнопку действий справа:
  - `source=1` на printer-class catalog: `User media`;
  - `source=2`: `Device preset`;
  - неизвестное значение либо legacy serial/ADB entry: `Unknown origin`.
- Цвет дополняет, но не заменяет видимый текст. Длинный перевод elide-ится
  только визуально; assistive technology получает полный label.
- `managedOrigin` не меняет source badge и не превращает отсутствие provenance
  в `External`. Отдельный технический badge provenance в C3 не добавляется.
- Размер форматируется бинарными единицами `B`, `KiB`, `MiB`, `GiB` с одним
  знаком после запятой после перехода от bytes. Нулевое, отрицательное,
  отсутствующее или non-finite значение показывает `Unknown size`.
- Thumbnail pipeline не меняется. Пустой или отклонённый URL продолжает
  показывать `No preview`; ради metadata не выполняются FilePull или USB
  requests.
- Empty state становится origin-neutral: `No media available`.
- Карточка сохраняет native `ItemDelegate` keyboard activation и получает
  полные `Accessible.name`, `Accessible.description`, selectable и selected
  state. Полное имя остаётся доступным при визуальном middle elide.
- Aggregate storage indicator не добавляется. Если сумма известных sizes
  появится в отдельном scope, она может называться только `Known listed size`,
  но не `Free space`, `Remaining` или `Capacity`.

#### Архитектурные границы

- `PanoramaPage` владеет presentation-only mapping source label и
  форматированием размера, переиспользуя существующие QML roles.
- `MediaCatalogModel` продолжает владеть stale revision и device identity
  fencing, raw role values и безопасным thumbnail URL.
- `RuntimeClient`, `DeviceManager`, `MediaCatalogStore`, artifact/lease paths,
  runtime contract, Manager1/Manager2, protobuf и persistent formats не
  меняются.
- C3 является пассивным UI: не запускает Apply, upload, delete, FilePull,
  refresh loop или иной device write.

#### Этапы реализации

1. RED Quick test фиксирует exact `mediaSize`, `mediaSource`, `readOnly` и
   `managedOrigin` role names/values и корректирует preset fixture с production
   `source=2`.
2. RED QML tests фиксируют user/preset/unknown/legacy labels, byte boundaries,
   neutral empty state, long-name/narrow bounds и accessibility/keyboard state.
3. В существующий media delegate добавляются source badge, общий локальный
   formatter, корректный empty state и accessibility metadata без нового QML
   component.
4. Добавляются законченные русские переводы и baseline pins для новых focused
   tests.
5. После focused GREEN выполняются весь QML runner, Quick client suite,
   `qmllint`, translation/catalog gate, runtime baseline и fresh-binary
   `package-check`.

#### Acceptance

- Printer-class user и preset entries различаются только по authoritative
  `mediaSource=1/2`; одинаковые имена и `readOnly` не меняют категорию.
- Unknown numeric source и legacy entry показывают `Unknown origin`, а не
  guessed user/preset label.
- Размеры `1`, `1023`, `1024`, `1536`, `1048576` и `1073741824` отображаются
  соответственно как `1 B`, `1023 B`, `1.0 KiB`, `1.5 KiB`, `1.0 MiB` и
  `1.0 GiB`; ноль показывает `Unknown size`.
- Валидный thumbnail виден, отсутствие thumbnail показывает `No preview`, и
  ни один из этих случаев не меняет origin label.
- Badge, filename, size и action button не пересекаются в narrow карточке;
  русский текст может визуально elide-иться без потери accessible description.
- Space/Enter сохраняют native выбор карточки; `Accessible.selected` совпадает
  с фактическим selection state.
- Пустая общая библиотека показывает `No media available` и не утверждает, что
  отсутствуют только uploads.
- UI нигде не называет сумму catalog sizes свободным или оставшимся местом.
- API остаётся 8; D-Bus tuple, protobuf, daemon state и persistent formats не
  меняются. Hardware/USB smoke для software acceptance C3 не требуется.

#### Результат реализации

Карточка `Media Library` показывает authoritative user/preset/unknown origin,
бинарный размер с честным unknown fallback, нейтральный empty state и полное
accessible metadata. Legacy `source=1` не выдаётся за подтверждённый user
origin; `readOnly` и `managedOrigin` не участвуют в классификации. Baseline
реально запускает пять именованных C3 QML tests, а не проверяет их имена как
произвольный текст.

Fresh `package-check` прошёл 27 августа 2026 года: 778 printer/recovery cases,
17 replace journal, 84 QML, 95 Quick client/model, 37 runtime bootstrap и 20
tray. `qmllint`, strict translation catalog, runtime characterization baseline
и GUI smoke также прошли. Два независимых read-only review не нашли C3
blockers. API 8, Manager1/Manager2 tuples, protobuf, persistence и USB paths в
C3 не менялись.

#### Риски и откат

- Required QML roles должны одновременно появиться во всех mock models, иначе
  component creation корректно завершится ошибкой. Tests обновляются в том же
  slice.
- Длинный русский badge может перекрыть action button без width clamp. Bounds
  проверяются на компактной карточке и полном переводе.
- `managedOrigin=false` легко ошибочно назвать external source. C3 намеренно не
  делает такого вывода и классифицирует только firmware `source`.
- Legacy `source=1` синтетический. Явная legacy boundary не позволяет выдать
  его за подтверждённый catalog origin.
- Rollback удаляет только presentation helpers, badge, accessibility metadata,
  focused tests и новые translations. Device/runtime rollback, migration и
  compensating USB actions не нужны.

**Out of scope:** source pathname, original filename/size/hash, remote content
hash, dimensions, duration, FPS, media filters, sorting, capacity/free-space
query, aggregate usage, FilePull, editor/picker metadata, cache cleanup C6,
split-aware crop C4, saved layouts C5, API 9/Manager3 и любые hardware writes.

### C4. Split-aware crop

**Статус:** реализовано 1 сентября 2026 года, 02:02:36 UTC+7. Формальный
проход начат 31 августа 2026 года, 23:35:36 UTC+7, и занял 2 часа 27 минут.
Software acceptance, fresh `package-check` и два независимых read-only review
завершены. Реализация сохраняет API `8` и все frozen D-Bus signatures; Split
добавлен только через additive Manager2 contract. Hardware visual acceptance
не подменяется software-проверками и остаётся отдельным pending gate.

**Пользовательская цель:** при открытии Media Editor пользователь явно
выбирает назначение новой копии: `Full screen` или `Split area`. Preview и
crop controls показывают фактическое соотношение выбранной области, поэтому
для PASE/PANORAMA split canvas равен `1120x1080`, а не полному
`2240x1080`. Один и тот же source с Full и Split transform получает разные
conversion identities и не может быть ошибочно переиспользован dedup.

#### Подтверждённые границы

- Локально установленный KANALI `2.4.0` проверен напрямую по
  `resources/app.asar`, SHA-256
  `27cd390005a92c70ecf62f93a76039775c42b761920ce062cd6ba5cacc567d69`.
  Для Full он передаёт `2240x1080`, для Split делит output width пополам и
  кодирует реальный H264 `1120x1080` с suffix `.h264_1120x1080`.
- Vendor cropper показывает для Split квадратный viewport `1:1`, а затем
  растягивает его в `1120x1080`. C4 не копирует это скрытое искажение:
  preview и production conversion используют честный canvas `1120x1080`
  (`28:27`, square pixels).
- Frozen `TryxRuntimeMediaTransform=(usuuuuu)` описывает только геометрию
  transform. Его `schemaVersion=1` и семантика остаются неизменными;
  destination нельзя кодировать новым значением этого поля.
- Legacy serial/ADB допускает только исходный neutral Full Fit. Turris 620
  не имеет display-configuration capability и не получает Split target.
- Выбор уже загруженного media в Full или Split остаётся без
  перекодирования. Target profile описывает только новую подготовленную копию
  и не заявляет, что firmware catalog хранит её предполагаемое назначение.

#### Versioned profile и conversion identity

- Новый frozen type
  `TryxRuntimeMediaPreparationProfileV1=(us(usuuuuu))` содержит
  `schemaVersion=1`, allowlisted target `FullFrame|SplitArea` и неизменённый
  `TryxRuntimeMediaTransform`. Runtime валидирует outer и nested schema
  независимо.
- Manager2 получает additive методы
  `QueueUploadWithPreparationProfileV1`,
  `QueueRecoveredMediaUploadWithPreparationProfileV1` и
  `QueueReplaceDeviceMediaWithPreparationProfileV1`. Старые transformed
  методы и tuple остаются frozen и означают только `FullFrame`.
- Runtime рекламирует token `runtime.media-preparation-profile.v1`. Quick
  показывает Split только после успешного runtime handshake и exact device
  capabilities `device.display-configuration.v1` и
  `device.media-split-area.v1`. При отсутствии runtime token
  Full использует старый метод; Split отклоняется. Если runtime уже заявил
  token, `UnknownMethod` считается нарушением контракта и не маскируется
  fallback.
- Full сохраняет существующие profiles v1/v2, output `2240x1080` и suffix.
  Split получает отдельный canonical profile v3 с `split-area`, реальной
  geometry `1120x1080` и собственным transform fingerprint. Managed-origin,
  retry identity и remote filename не могут пересекать Full и Split.
- Durable retry продолжает использовать текущий format version: обязательные
  conversion identity, conversion profile и exact remote suffix однозначно
  несут geometry. Validation, восстановление и генерация нового retry-name
  должны сохранить `1120x1080`, а не подставлять полную geometry устройства.
- Preview и production FFmpeg используют один canonical area transform.
  Split заканчивается в `1120x1080`; codec, 30 FPS, pixel format, bounded
  size/decode verification и USB write semantics остаются прежними.

#### UI, lifecycle и Replace

- Target selector расположен до sizing controls и явно подписан
  `Prepare for`. `Full screen` показывает полный canvas, `Split area`
  показывает одну реальную половину и поясняет, что одна подготовленная копия
  может использоваться слева или справа и ничего не применяет автоматически.
- Локальная новая editor session по умолчанию открывается в Full, сохраняя
  прежнее поведение. Для recovered copy target выводится из exact активного
  layout только когда этот remote media действительно в нём присутствует;
  иначе используется Full.
- Смена Full/Split создаёт новую transform domain и сбрасывает sizing,
  rotation, zoom, focus и background в neutral Fit. Интерфейс постоянно
  предупреждает об этом до выбора. `Reset` сбрасывает geometry внутри уже
  выбранного target и не меняет target скрыто.
- Потеря runtime/device capability во время открытого editor немедленно
  возвращает Full neutral transform. Pending submission не меняется задним
  числом; backend всё равно повторно проверяет target до принятия source. После
  отказа или ошибки такой submission editor синхронизируется с уже действующим
  Full neutral Fit, не оставляя визуально устаревший Split draft.
- Replace разрешает Split transform только для текущего exact Split layout,
  который ссылается на original media, и Full transform только для Full
  layout. Cached target или stale generation mismatch отклоняется до foreground
  operation. Drift полного `screenMode`, `playMode`, порядка media или exact
  Full/Left/Right slot обнаруживается внутри read-only foreground preflight;
  после него операция закрывается без FFmpeg, journal и mutating USB request.
  Save as new не меняет активный layout и допускает оба target на поддерживаемом
  устройстве.
- Target selector, описание и resolution доступны с клавиатуры и screen
  reader, не полагаются только на цвет, используют `Text.PlainText` и не
  выходят за узкий popup.

#### Явно подготовленный downgrade v11 -> released v10

- API остаётся равным `8`. Additive Manager2 method
  `PrepareRuntimeDowngradeV10()` доступен только вместе с token
  `runtime.downgrade-v10-preparation.v1` и возвращает только `Empty` или
  `FullFrame`.
- Runtime устанавливает общий mutation latch до проверки изменяемого состояния.
  Downgrade блокируют Split retry, in-flight dispatch, cleanup/transition,
  непроверенный или изменившийся retry store, firmware activity, активные
  операции и Delete/Replace recovery. Разрешён только exact empty store или
  один terminal Full retry с byte-identical canonical v11 и released-v10 shadow,
  точной geometry/origin и проверенной inode/hash/link topology.
- После проверки runtime сохраняет owner-only marker в `XDG_STATE_HOME`,
  привязанный к clean absolute path, device, inode, size и SHA-256 текущего
  executable. Тот же v11 executable завершает запуск до D-Bus ownership и
  `DeviceManager`; другой установленный executable, то есть released v10,
  marker игнорирует.
- `tryx prepare-downgrade-v10` принимает только exact capability, method
  signature и результат, а успех публикует лишь после стабильного исчезновения
  исходного unique owner и well-known name. Команда не меняет пакет.
- Если замена пакета отменена, `tryx abort-downgrade-v10` может удалить marker
  только при безопасном local user-session bus, отсутствии runtime owner и
  полном совпадении всё ещё установленного executable. Missing, changed,
  corrupt или unsafe state отклоняется. Прямой downgrade без успешной
  подготовки не поддерживается.

#### Этапы и Definition of Done

1. RED contract/backend tests фиксируют profile schema, frozen старую и новую
   D-Bus signatures, capability token, actual area filter/output, отдельный
   conversion profile и запрет cross-target origin reuse.
2. RED DeviceManager tests фиксируют product/capability gate, Full-only legacy
   и Turris, Replace target/layout match, read-only fresh-layout preflight и
   отсутствие hashing, FFmpeg, journal или mutating USB operation при reject.
3. RED/GREEN Quick tests фиксируют capability handshake, default/preselected
   target, reset при смене domain, честные dimensions и точный transform,
   который отправляется для Upload, Save as new и Replace.
4. RED/GREEN QML tests фиксируют selector, пояснения, `1120x1080` canvas,
   narrow layout, accessibility и полный русский перевод.
5. Шесть deterministic cancellation cases фиксируют финальную проверку сразу
   перед каждым media, thumbnail и frame-count `QProcess::start`; реальные
   Full/Split E2E проверяют H264, `yuv420p`, 30 FPS, длительность, exact
   `2240x1080|1120x1080`, suffix и строгий cross-target reject.
6. Downgrade matrix фиксирует `Empty|FullFrame`, запрет Split/in-flight/
   recovery, exact store topology, post-commit fail-closed latch, production
   startup marker и offline abort.
7. Выполняются focused suites, полный fresh `package-check`, diff review и
   независимые read-only reviews. Только после этого C4 получает статус
   `реализовано`, а `docs/todo-next.md` закрывает пункт.

**Hardware acceptance:** software проверяет exact FFmpeg geometry, output,
suffix и retry persistence, но визуальное качество Split crop на физическом
дисплее остаётся отдельным явным Apply-test. До него C4 не заявляет
hardware/visual approval и не меняет подтверждённые device defaults.

**Out of scope:** другие remote geometries и products, API 9, Manager3,
protobuf fields, автоматическое перекодирование уже
загруженной media library, сохранение editor target между запусками, новые
model geometries, vertical split, saved layouts C5, cache cleanup C6,
filters, trim, audio и automatic Apply/replay после reconnect.

### C5. Saved layouts

**Статус:** реализовано 1 сентября 2026 года, 12:04:10 UTC+7. Формальный
проход начат в 10:12:13 UTC+7 и занял 1 час 51 минуту 57 секунд. Software
acceptance, fresh `package-check` и три независимых read-only review завершены.
C5 сохраняет API `8`, frozen Manager1 и существующие Manager2 signatures;
новые контракты только additive и versioned. Runtime-owned store и fresh
catalog proof находятся в той же serialized foreground operation, что и
последующий explicit Apply, поэтому между GUI refresh и USB mutation не
остаётся C5 TOCTOU.

**Пользовательская цель:** на странице Display пользователь сохраняет текущий
полный editable draft под именем, позднее загружает его обратно в draft и
отдельно нажимает `Apply to display`. Save и Load никогда не отправляют USB
команду. Reconnect, запуск GUI, смена runtime owner или подключение устройства
никогда не загружают и не применяют layout автоматически.

#### Выбранная архитектура

- Runtime владеет versioned store и exact device/media proof. Quick показывает
  device-scoped snapshot и переиспользует существующий C2 draft lifecycle.
- Отклонён Quick-only store: refresh каталога и существующий Apply являются
  двумя независимыми действиями, поэтому media может измениться между ними.
- Отклонён hybrid с двумя независимыми schema в Quick и runtime: он дублирует
  validation и создаёт две конкурирующие revision boundaries.
- `391a:1011` и `391a:1021` допускаются только при подтверждённых capabilities
  `device.display-configuration.v1`, `device.media-catalog.v1` и
  `device.overlay-metrics.v1`. Split дополнительно требует
  `device.media-split-area.v1`.
- Legacy Manager1 не входит в C5 v1: его synthetic catalog не содержит
  надёжный `mediaId`. Turris `391a:2011` не входит: он не имеет catalog и
  display-configuration capability.

#### Versioned runtime contract

- Runtime рекламирует token `runtime.saved-layouts.v1`. Отсутствие token у
  старого API-8 runtime означает честное `Not supported`; только exact
  `UnknownMethod` до объявления capability допускает legacy absence.
- Новый frozen `TryxRuntimeSavedMediaRefV1` содержит `schemaVersion=1`,
  lowercase `mediaId`, authoritative remote name, size, source и `readOnly`.
  `mediaId` связывает device identity, name, size и source, а отдельный
  `readOnly` закрывает поле, которое не входит в текущий hash identity.
- Новый frozen `TryxRuntimeSavedLayoutV1` содержит `schemaVersion=1`, canonical
  UUID, monotonic record revision, exact device identity, product ID, bounded
  plain-text name, ordered media refs и canonical `TryxRuntimeApplyRequest`.
- В request разрешены только Full или Split, существующие play modes, ordered
  metrics/badges/styles, brightness и mirror/waterfall. `ratio` равен `2:1`,
  `replaceOverlay=true`. Standby, backlight, preset ID, filter opacity,
  operation IDs, retry/cache paths и C4 preparation target запрещены.
- `TryxRuntimeSavedLayoutsSnapshotV1` содержит `schemaVersion=1`, monotonic
  snapshot revision, `status`, bounded diagnostic, current exact device
  identity/product и layouts только этого устройства. Frozen status
  равен `Ready`, `Disconnected`, `Unsupported` или `Unavailable`.
  `Ready` с пустым list означает реально пустой writable store;
  unsafe, malformed, oversized и future-version store всегда даёт
  `Unavailable`, пустой list и non-empty diagnostic.
- Frozen D-Bus tuple order и wire signatures:
  `TryxRuntimeSavedMediaRefV1 = (schemaVersion:u, mediaId:s, name:s,
  size:t, source:u, readOnly:b)`, signature `(usstub)`;
  `TryxRuntimeSavedLayoutV1 = (schemaVersion:u, layoutId:s, revision:t,
  deviceIdentity:s, productId:s, name:s, media:a(usstub),
  request:TryxRuntimeApplyRequest)`, signature
  `(ustsssa(usstub)(assssassssasisasassssbb(bibbbbbbb)))`;
  `TryxRuntimeSavedLayoutsSnapshotV1 = (schemaVersion:u, revision:t,
  status:s, diagnostic:s, deviceIdentity:s, productId:s,
  layouts:aTryxRuntimeSavedLayoutV1)`, signature
  `(utssssa(ustsssa(usstub)(assssassssasisasassssbb(bibbbbbbb))))`.
- Exact additive Manager2 meta-signatures:
  `TryxRuntimeSavedLayoutsSnapshotV1 GetSavedLayoutsV1()`,
  `TryxRuntimeSavedLayoutsSnapshotV1 PutSavedLayoutV1(qulonglong,
  TryxRuntimeSavedLayoutV1)`,
  `TryxRuntimeSavedLayoutsSnapshotV1 DeleteSavedLayoutV1(qulonglong,
  QString)` и `QString QueueSavedLayoutApplyV1(QString,QString,qulonglong,
  TryxRuntimeApplyRequest)`. Их D-Bus input signatures равны
  `""`, `t(ustsssa(usstub)(assssassssasisasassssbb(bibbbbbbb)))`, `ts`
  и `sst(assssassssasisasassssbb(bibbbbbbb))`; Get/Put/Delete возвращают
  snapshot tuple, Queue возвращает `s`.
- Put/Delete domain failures используют exact D-Bus error names
  `org.tryx.Panorama.Error.InvalidSavedLayout`,
  `.SavedLayoutsRevisionConflict`, `.SavedLayoutNameConflict`,
  `.SavedLayoutsUnavailable`, `.UnsupportedProduct`, `.DowngradePrepared` и
  `.SavedLayoutsCommitUnknown`; после conflict клиент запрашивает
  fresh snapshot. Валидный Queue operation ID всегда возвращается:
  stale/missing/unsupported input становится обычной terminal
  `Rejected/NotStarted` operation с typed `errorCategory` и нулем worker/USB
  dispatch. Синтаксически invalid operation ID возвращает существующий
  `org.tryx.Panorama.Error.InvalidOperation`.
- UUID и revision authority только runtime: create принимает пустой
  `layoutId` и record revision `0`, генерирует UUID и новую
  revision; update требует exact existing UUID и record revision.
  Каждый successful Put/Delete повышает snapshot revision ровно
  на один; successful Put присваивает это же значение record.
  CAS mismatch ничего не перезаписывает.
- Все D-Bus inputs считаются недоверенными. Put до persist сверяет exact current
  device identity/product, canonical request и каждую media ref с текущим
  authoritative runtime catalog. Ordered `request.media` обязан exact
  совпадать с ordered authoritative names из media refs; mode и count тоже
  проверяются до commit. Queue повторно canonical-validates переданный
  current draft, разрешает его media names только через current catalog и сам
  строит immutable proof для worker; сохранённые клиентом size/source не
  становятся authority.
- Canonical saved full-state request не использует C2 delta semantics:
  `brightnessPresent=true`, `orientationPresent=true`,
  `standbyPresent=false`, `backlightPresent=false`, `presetId=""`,
  `filterOpacity=0`, `replaceOverlay=true`, а top-level `waterfallMode`
  exact равен `display.waterfallMode`. Значения brightness, mirror,
  waterfall, обе overlay areas, media, mode и play mode присутствуют
  даже при clean C2 domains. Save disabled, пока нет valid confirmed
  DisplayState, и строит этот full-state DTO из current draft явно.
- Exact canonical literals: `screenMode` только `Full Screen` или
  `Screen Splitting`; Full допускает `Single|Loop|Shuffle` и ровно
  одну media ref, Split только `Single` и ровно две. `ratio="2:1"`,
  brightness в `0..100`, `standbyEnabled=false`, `backlightEnabled=true`.
  Каждая active area имеет до трёх unique metrics из frozen catalog,
  до двух unique `CPU Badge|GPU Badge`, position `Top|Bottom`, alignment
  `Left|Center|Right` и lowercase color `#[0-9a-f]{6}`. Для Full все area-2
  lists и style strings exact пусты; для Split обе area style triples
  обязательны. Эти ignored/default fields тоже входят в canonical
  bytes и не нормализуются молча при load.
- Новые methods не меняют wire/protobuf, Manager1, API version или старые
  Manager2 tuples. Live multi-client signal и Manager3 не нужны в C5 v1.

#### Persistent format и CRUD

- Store расположен в owner-controlled XDG data namespace:
  `DXVSI/TRYX Panorama Manager/saved-layouts/index.json`.
- JSON имеет top-level format version `1`, monotonic revision и bounded array.
  Максимум 32 layouts на устройство, 256 всего, имя от 1 до 80 Unicode
  characters без control/bidi override, payload не более 1 MiB. Имена
  уникальны case-insensitive внутри exact device scope.
- Load использует `O_NOFOLLOW|O_NONBLOCK`, проверяет owner, regular file,
  single link, безопасный parent и exact allowlisted keys. Malformed, unsafe,
  oversized или future-version state публикует пустую read-only surface и
  запрещает writes, чтобы старая сборка не затёрла данные.
- Persist использует path-based `QSaveFile` на Linux path
  `/proc/self/fd/<opened-dirfd>/index.json`, `0600`, отключённый direct write
  fallback, atomic commit и post-commit identity checks открытого и live parent.
  Это не объявляется generic fd-constructor Qt. Ошибка до commit оставляет
  предыдущий snapshot. Неопределённый post-commit result блокирует дальнейшие
  writes до reload/restart и не публикуется как подтверждённый CRUD.
- `Save current` сохраняет текущий editable draft, включая brightness,
  mirror и waterfall, но не backlight. Save не применяет draft, не сбрасывает
  dirty-state и не меняет confirmed baseline.
- Новый layout получает UUID. Повторное имя требует явного overwrite dialog и
  обновляет существующий UUID/revision. Rename не входит в v1. Delete требует
  отдельного confirmation и удаляет только запись store, не device media.
- Без exact подключённого поддерживаемого устройства список скрыт/disabled.
  Записи другого устройства не показываются и не удаляются.

#### Load, dirty lifecycle и explicit Apply

- `Load into draft` сначала повторно проверяет record/device/product. Save и
  Load disabled при `applyPending`, `applyUnresolved`, external/device conflict
  или незавершённом device handshake. Если текущий draft dirty, пользователь
  должен подтвердить `Replace draft`.
- Load заменяет media, Full/Split, play mode, metrics, badges, styles,
  brightness и orientation только в локальном draft. Existing confirmed
  baseline не меняется, поэтому C2 guard видит обычный dirty draft.
- Discard всегда возвращает fresh confirmed device state, а не последний
  saved layout. Save и Load не закрывают unresolved Apply и не обходят conflict
  при external DisplayState или смене device identity.
- Загруженный draft сохраняет exact layout UUID/revision provenance при
  редактировании. Apply передаёт текущий canonical draft в
  `QueueSavedLayoutApplyV1`; runtime заново разрешает все его media через
  authoritative catalog и строит fresh worker proof. Поэтому изменение media,
  overlay или display fields не переводит persisted provenance на обычный C2
  path и не обходит C5 validation. Binding снимают только Discard, загрузка
  другого layout, удаление записи, device/owner change или explicit abandon.
  Automatic re-save отсутствует.
- Reconnect и owner change только инвалидируют snapshot/binding и запрашивают
  новый device-scoped list после handshake. Они не вызывают Load или Apply.

Queue reject contract frozen: `kind=SavedLayoutApply`, `state=Failed`, `stage=Rejected`,
`terminalOutcome=NotStarted`. Exact `errorCategory` равен
`SavedLayoutMissing`, `SavedLayoutRevisionConflict`,
`SavedLayoutsUnavailable`, `DeviceIdentityChanged`, `UnsupportedProduct`,
`DowngradePrepared`, `UnsupportedConfiguration`, либо уже существующему
`Busy|DeviceRecoveryRequired|SessionLost|FirmwareUpdateActive|RetryCacheConflict`.
Эта pre-dispatch group происходит до foreground/worker dispatch и даёт
ноль USB. После worker dispatch, но до mutating OUT, тот же
terminal contract использует `DeviceGenerationChanged`, если physical
generation сменился до или сразу после FileList; `FileListUnavailable`, если
bounded read-only FileList/session не дал proof; и `SavedLayoutMediaChanged`,
если successful FileList обнаружил missing, duplicate, changed или
ambiguous tuple. Первый случай даёт один worker dispatch и ноль USB;
второй даёт не более одной read-only FileList попытки; третий
даёт ровно один successful FileList. Generation change до FileList
даёт ноль FileList, а после proof даёт ровно один successful FileList;
оба исхода имеют `Failed/Rejected/NotStarted`,
`errorCategory=DeviceGenerationChanged`. Все proof-time rejects дают ноль mutating
OUT. Explicit user cancellation остаётся существующим
`state=Cancelled`, `stage=Cancelled`, `terminalOutcome=Cancelled`, а не reject.
После
`CommitUnknown` store блокирует writes, а Get в этом process lifetime
публикует `Unavailable` с diagnostic, не `Ready` со stale in-memory list.

#### Fresh catalog proof и mutation boundary

1. DeviceManager проверяет общий C4 downgrade mutation latch, затем фиксирует
   layout UUID/revision, store snapshot, canonical current draft, exact product,
   device identity и physical generation до foreground dispatch. Тот же latch
   блокирует Put/Delete и не позволяет менять C5 store после успешной
   подготовки downgrade.
2. Worker в существующей claimed session делает один bounded fresh FileList.
3. До первого mutating OUT он exact-сверяет ordered refs по mediaId,
   authoritative name, size, source и readOnly. Missing, duplicate, changed или
   ambiguous entry завершает operation как `Rejected/NotStarted`.
4. После proof worker повторно проверяет cancellation/generation и в той же
   foreground Apply operation выполняет существующую exact
   config-write/overlay-activation/readback последовательность.
5. Busy, firmware, retry-cache, recovery, session-loss и capability gates
   остаются теми же. Partial/unknown OUT закрывает session и никогда не
   запускает rollback, fallback или replay после reconnect.

Fresh FileList доказывает exact catalog tuple, но не bytes при замене файлом с
тем же name и size. FilePull/hash каждого media не добавляется скрыто и остаётся
отдельным возможным расширением контракта.

Released v10 не знает token/schema C5, игнорирует `saved-layouts/index.json` и
никогда не изменяет его. После возврата на C5-capable runtime valid v1 store
читается снова. Future-version/corrupt state остаётся read-only и не
перезаписывается старой реализацией.

#### UI и accessibility

- Компактная группа `Saved layouts` расположена перед `Display layout`.
  Она содержит bounded name field, `Save current`, empty/error state и список
  текущего устройства с действиями `Load into draft` и `Delete`.
- Load/overwrite/delete используют modal confirmation, Escape равен Cancel,
  после закрытия focus возвращается инициатору. Кнопки доступны с клавиатуры,
  имеют видимый текст и minimum hit target 44 px.
- При узкой ширине actions переходят в одну колонку, длинные имена elide/wrap
  без horizontal overflow. Missing media, stale revision, unsupported device и
  validation error сообщаются текстом и не только цветом.
- Все новые строки имеют полный русский перевод и QML baseline.

#### Этапы и Definition of Done

1. RED store/codec tests фиксируют strict v1 round-trip, device scope, bounds,
   CAS, canonical bytes, atomic CRUD, unsafe paths, malformed/future version,
   write/commit/race failure и сохранение предыдущего snapshot.
2. RED contract tests фиксируют новые exact D-Bus signatures/token, API `8` и
   byte-identical старые Manager1/Manager2 signatures, exact status/error mapping,
   runtime-generated UUID/revisions и distinction `Ready + empty` от
   `Unavailable + diagnostic`.
3. RED DeviceManager/worker tests фиксируют wrong identity/product/generation,
   stale revision, missing/changed/duplicate media, capability/recovery gates,
   ноль mutating dispatch при reject, downgrade-latch gate и ровно FileList +
   один verified Apply при success. Edited loaded draft сохраняет provenance и
   также проходит fresh proof. Invariant refs/request order нельзя
   разорвать; incomplete full-state presence отклоняется. Get/Put/Delete/Load
   дают ноль foreground/worker/USB dispatch. Unknown outcome не replay-ится.
4. RED/GREEN Quick/QML tests фиксируют Save current без Apply/dirty reset,
   device filtering, explicit overwrite/delete, Load только в draft, dirty
   replacement confirmation, Full/Split order, external conflict, reconnect
   invalidation, narrow layout, keyboard, accessibility и русский перевод.
5. Выполняются focused suites, fresh build, полный `package-check`, diff review
   и независимые read-only reviews. Только после этого C5 получает статус
   `реализовано`, а `docs/todo-next.md` закрывает пункт.

#### Фактическое закрытие

- Versioned owner-only store, device/product scope, bounded canonical schema,
  CAS CRUD, atomic persistence и fail-closed unsafe/future/commit-unknown
  состояния реализованы в runtime. Raw device identity проходит один общий
  canonical validator до любого `Ready`, Put, Delete или Apply dispatch.
- Additive Manager2 Get/Put/Delete/Queue contract и capability token проходят
  exact signature/API-8 baseline. Put/Delete проверяют текущего unique D-Bus
  owner непосредственно перед отправкой; timeout, disconnect и ambiguous
  transport сначала снимают `Ready`, затем допускают только read-only Get
  reconciliation без replay мутации.
- Quick публикует только подтверждённые snapshots. UI сохраняет current Full
  или Split draft без Apply и dirty reset, явно подтверждает overwrite/load/
  delete, загружает запись только в draft и инвалидирует pending confirmation
  при смене runtime/device/capability. Узкая раскладка, keyboard/focus,
  accessibility и русский каталог покрыты QML tests.
- Fresh `package-check` завершён с результатом `1538 passed, 0 failed,
  4 skipped` в 11 наборах. Все четыре skip являются ожидаемыми отдельными
  headless-тестами перевода; соответствующие RU suites прошли с загруженным
  `.qm`. Characterization baseline, translation completeness, `qmllint`,
  shell syntax и `git diff --check` прошли. Три независимых read-only review
  дали `APPROVE` без обязательных замечаний.

**Hardware acceptance:** software fixtures доказывают fresh FileList proof,
точный request, one mutation и readback, но визуальный Full/Split Apply на
физическом `1021` и отдельно community `1011` остаётся отдельным явным test.
C5 не заявляет hardware/visual approval без этого evidence.

**Out of scope:** vendor Presets, automatic restore последнего layout,
unfinished draft persistence, reconnect replay, cloud sync, import/export,
cross-device copy, auto-upload/Replace/re-encode отсутствующего media, backlight,
standby, rename, legacy Manager1, Turris, new product profiles, API 9, Manager3,
новые protobuf/USB commands, cache cleanup C6 и content hash каждого media.

### C6. Safe cache management

**Статус:** реализовано 1 сентября 2026 года, 17:25:38 UTC+7. Формальный
проход начат в 12:23:35 UTC+7 и занял 5 часов 2 минуты 3 секунды. Software
acceptance, fresh build, полный `package-check` и три раздельных read-only
review-прохода завершены. API остаётся равным `8`; USB device и установленный
runtime не использовались, поэтому это software/headless proof, а не hardware
acceptance.

**Пользовательская цель:** в Settings пользователь явно запускает узкую
очистку ненужных временных файлов и получает подтверждённый результат по
количеству и суммарному logical size удалённых файлов. Действие не удаляет
device media, видимые catalog thumbnails, saved layouts, recovery state или
незавершённую работу и не выдаёт logical size за гарантированно освобождённое
место на filesystem.

#### Подтверждённое текущее состояние

- `MediaCatalogStore` сохраняет accepted thumbnail bytes, hash/size и origin
  metadata, но не исходный local path или source bytes. Existing FilePull
  создаёт owner-scoped raw artifact для editor/export, но не вызывает
  `commitThumbnail()`. Поэтому indexed catalog thumbnail сейчас не имеет
  доказанного regeneration path и является protected data C6.
- Store уже выполняет best-effort `sweepThumbnailOrphans()`, но текущий helper
  не возвращает typed counts/bytes и не является C6 operation boundary.
  Unindexed direct-child `.jpg`, которого нет в accepted index, не виден UI и
  может быть удалён после повторной identity/index проверки.
- `MediaPreviewController` владеет Quick previews в private runtime directory.
  Startup sweep удаляет не более 64 файлов старше 24 часов. Current/pending
  preview и live helper output принадлежат Media Editor и не являются
  кандидатами C6.
- `DeviceMediaArtifactStore::release()` уже пытается сразу удалить artifact.
  Отдельного persisted состояния `released artifact` нет. Текущий
  `sweepExpired()` обходит все in-memory records и не публикует bounded C6
  assessment либо logical byte report.
- `prepared-media` одновременно содержит transient preparation, canonical
  `RetryCacheStore` v11, released-v10 shadow, `suspended-v10`, transition state
  и cleanup tombstones. Весь root является recovery boundary и исключён из
  C6 независимо от имени отдельного файла.
- Inbox/spool, Delete/Replace journals, downgrade marker, firmware state и
  saved layouts имеют собственные lifecycle. Quick уже single-instance, что
  позволяет одному controller удерживать local Media Editor interlock.

#### Выбранная архитектура и отклонённые варианты

- Runtime выполняет authoritative gate и runtime-owned часть очистки как одну
  serialized foreground operation. Quick-only вариант отклонён: GUI не видит
  полный retry, journal, firmware и artifact-lease state.
- Runtime рекламирует additive capability `runtime.cache-cleanup.v1` и новый
  Manager2 method `QueueCacheCleanupV1(QString operationId) -> QString` с
  D-Bus input/output signatures `s` и `s`. Operation kind равен
  `CacheCleanup`. API остаётся `8`; Manager1, существующие Manager2 methods и
  tuples, protobuf и USB contract не меняются.
- Новый getter/preflight snapshot отклонён: размер и blockers устаревают между
  Get и mutation. UI показывает только локально известную eligibility, а
  runtime повторяет authoritative проверку и атомарно берёт cleanup latch до
  построения candidate plan.
- `CacheManagementController(RuntimeClient*, MediaEditorController*)`
  передаётся через `main.cpp -> Main.qml -> SettingsPage`. QML не получает
  filesystem API. Controller до Queue атомарно получает Quick-local interlock;
  пока он удерживается, `begin`, `beginDropped` и новая preview generation
  запрещены. Если editor уже open/busy либо lease получить нельзя, runtime не
  вызывается.
- После exact-owner runtime `Succeeded` controller вызывает новый безопасный
  метод `MediaEditorController`, а тот единолично обращается к private
  `MediaPreviewController`. Interlock освобождается только после combined
  terminal state. Runtime и Quick части не объявляются одной межпроцессной
  транзакцией: local failure после подтверждённого runtime удаления даёт
  `Partial`, не rollback или automatic retry.
- Внутренний `PreviewCleanupReport` содержит `plannedFiles`, `removedFiles`,
  `removedLogicalBytes`, `complete` и typed error. Combined UI totals являются
  точной суммой подтверждённого runtime result и local report, при этом обе
  области также показываются раздельно.
- Accepted indexed thumbnails намеренно исключены. Автоматический FilePull или
  иная USB read для их регенерации отклонены: cleanup не должен неожиданно
  подключаться к device, требовать media session или создавать новый hardware
  operation.

#### Exact allowlist и filesystem boundary

1. `MediaCatalogStore` получает отдельный typed bounded cleanup только для
   unindexed thumbnail orphans. Candidate имеет canonical lowercase SHA-256
   `.jpg` name, является direct child принятого thumbnail directory и
   отсутствует в accepted loaded index при plan и непосредственно перед
   unlink. `index.json`, indexed thumbnail и origin metadata не изменяются;
   catalog revision/snapshot поэтому тоже не меняются.
2. `DeviceMediaArtifactStore` получает typed `cleanupAssessment()` и
   C6-specific bounded batch result с `plannedFiles`, `removedFiles`,
   `removedLogicalBytes`, `complete` и typed error. Кандидат должен быть
   expired или revoked, не иметь действующей lease/reservation/operation hold
   и повторно пройти store identity check. Existing periodic
   `sweepExpired()` не используется как C6 accounting boundary.
3. `MediaPreviewController` удаляет только inactive direct-child files с
   canonical preview UUID/generation name при удерживаемом Quick interlock.
   Current/pending path, live helper output, inbox source и `.part` другого
   ownership domain не выбираются. Interlock также блокирует отдельный
   `beginRecoveredVideo()` path наряду с `begin`, `beginDropped` и обычной
   preview generation.
4. Catalog parent обязан быть direct non-symlink directory, принадлежать
   effective UID и не быть writable для group/others. Любой такой mode,
   включая `0755`, допустим и не требует скрытой permission migration. Quick и
   outbox сохраняют existing private directory contract.
5. Catalog/Quick regular candidate перед unlink повторно проверяется через
   открытый parent: effective UID, regular type, single link, expected
   device/inode, bounded `st_size` и exact direct name. Symlink, hardlink,
   special, foreign-owned, changed-identity и outside-allowlist leaves
   сохраняются с typed failure.
6. Outbox сохраняет принятый A5 contract: store может удалить только exact
   tracked owner-owned regular file либо symlink leaf без перехода по ссылке;
   directories, special и foreign-owned leaves сохраняются. Logical size
   symlink leaf равен нулю. Recursive remove и общий storage glob запрещены;
   successful unlink сопровождается parent fsync.
7. Plan удерживает открытые parent и каждый candidate inode до окончания
   cleanup. Leaf закрепляется через `O_PATH | O_NOFOLLOW | O_CLOEXEC`, поэтому
   unlink/recreate не может пройти recheck за счёт повторного использования
   номера inode. Копии plan разделяют владение descriptor; terminal operation
   освобождает планы до публикации success/failure/cancel, включая reentrant
   cancellation. Пустой план тоже закрепляет parent. Лимит 4096 leaves на
   store сохраняется; нехватка descriptors прерывает весь plan без удаления
   файлов и без повышения `RLIMIT_NOFILE` или незакреплённого fallback.

`confirmedBytes` в public operation означает только сумму `st_size` успешно
unlink-нутых fenced regular files. Open descriptor, sparse file, reflink,
compression и filesystem accounting могут отличаться, поэтому UI пишет
`Размер удалённых файлов`, а не `Освобождено на диске`.

Никогда не удаляются indexed thumbnails, `prepared-media`, retry manifests,
artifacts или thumbnails, shadow/suspended-v10/transition files, cleanup
tombstones, inbox, spool, catalog index/origin, saved layouts, support reports,
Delete/Replace journals, downgrade marker, firmware files, Qt/QML caches и
media на устройстве.

#### Authoritative blockers и exclusive latch

До первой mutation runtime fail-closed проверяет:

- любой другой non-terminal operation или concurrent CacheCleanup;
- firmware exclusive/release-pending/interlock и prepared downgrade latch;
- printer recovery, session-loss и незавершённый startup outbox cleanup;
- retry load/validation failure или pending validation, а также любой
  `retryCandidate`, `inFlightDispatch`, `cleanupPending` или
  `candidateTransition`; обычный `RetryAvailable` тоже блокирует C6 и не
  полагается только на существующий `retryCacheMutationGateActive()`;
- любой pending, unsafe или неоднозначный Delete/Replace journal;
- любую active artifact reservation, действующую lease или operation hold;
- unavailable/unsafe catalog или artifact store.

До Queue Quick отдельно проверяет preview directory и получает editor
interlock. Unsafe/unavailable preview path либо занятый editor даёт combined
`Blocked` без создания runtime operation.

Blocker check и cleanup-exclusive latch выполняются атомарно на
`DeviceManager` thread. Пока latch активен, periodic artifact timer не
удаляет records, owner disconnect только revoke/defer-ит cleanup, а новые
reserve/claim/renew/hold и catalog mutations, способные изменить candidate
plan, отклоняются или откладываются. Runtime выполняет bounded
event-loop-stepped batches, чтобы D-Bus cancellation и owner change могли быть
обработаны между шагами без interleaving store mutations. Latch снимается
только после terminal operation publication.

Runtime сначала строит полный immutable candidate plan в пределах hard bound и
только затем начинает unlink batches. Превышение bound или незавершённый plan
даёт failure до первой mutation; после начала удаления `total` уже не меняется.

Чистый disconnect без recovery state не блокирует C6: cleanup не требует USB
device. Session-loss/recovery блокирует его, поскольку такой process может
содержать незавершённые ownership/reconciliation boundaries.

#### Exact operation outcomes

| Исход | `state` | `stage` | `terminalOutcome` | `errorCategory` | Counts |
|---|---|---|---|---|---|
| Domain blocker | `Failed` | `Rejected` | `NotStarted` | exact blocker | все `0` |
| Execution failure до первой mutation | `Failed` | `Failed` | `NotStarted` | `CacheCleanupFailed` | все `0` |
| Полный или пустой успех | `Succeeded` | `Succeeded` | `Succeeded` | пусто | exact final counts |
| Failure после mutation | `Failed` | `Failed` | `PartialCleanup` | `CacheCleanupFailed` | immutable `total`, confirmed `completed/bytes` |
| Cancel до mutation | `Cancelled` | `Cancelled` | `Cancelled` | `UserCancelled` | все `0` |
| Cancel после mutation | `Cancelled` | `Cancelled` | `PartialCleanup` | `UserCancelled` | immutable `total`, confirmed `completed/bytes` |

Exact blocker category равен `DowngradePrepared`, `FirmwareUpdateActive`,
`Busy`, `DeviceRecoveryRequired`, `SessionLost`,
`RetryCacheValidationPending`, `RetryCacheConflict`,
`RecoveryJournalPresent`, `ArtifactLeaseActive` или `CacheUnavailable`.
Malformed UUID и collision с другим kind используют существующий
`org.tryx.Panorama.Error.InvalidOperation`. Valid domain reject возвращает
operation ID и никогда не вызывает store unlink.

`completed` содержит число успешно удалённых leaves, `total` immutable
`plannedFiles`, `confirmedBytes` только logical regular-file size. Empty
success равен `0/0/0`. Cancel после mutation показывается combined UI как
`Partial`, а не как обычный безопасный `Cancelled`. Для `CacheCleanup`
`DeviceManager::cancelOperation` получает отдельную local branch: он выставляет
cancel flag и не отправляет USB worker cancellation.

Повтор того же UUID не выполняет cleanup второй раз только пока UUID присутствует
в bounded текущей runtime operation history. Cross-kind collision также
доказуема только внутри этой history. Persistent cleanup journal не добавляется;
после prune/restart старый UUID не является dedupe authority. Quick никогда
автоматически не переиспользует UUID. Новый явный пользовательский запуск
создаёт новый UUID и заново проходит все gates.

`NoReply` после Queue разрешает только один same-owner
`GetOperation(operationId)`. Cancel отправляется captured unique owner только
для exact tracked UUID/kind; `NoReply` не повторяет Cancel. Owner replacement
до подтверждённого runtime terminal result переводит UI в `Unresolved`,
освобождает local interlock без локальной очистки и ничего не отправляет новому
owner. После уже подтверждённого runtime `Succeeded` смена owner не отменяет
известный result: controller завершает local phase либо публикует `Partial`.

Same-owner Get может перевести `Unresolved` в подтверждённый terminal state.
Re-handshake replacement owner подтверждает только новый capability context и
не объявляет исход старой operation известным. Background reconnect не скрывает
`Unresolved`. После явного acknowledgment пользователь может вернуться в
`Ready`; новый ручной запуск получает новый UUID и заново проходит gates.

#### UI и accessibility

- Отдельная панель `Временные файлы` располагается в Settings после Device и
  перед Support report. Широкий термин `Очистить кэш` не используется.
- Текст сообщает, что удаляются только inactive previews, orphaned thumbnails
  и expired idle copies. Видимые thumbnails, layouts, recovery data,
  operation files и используемые файлы сохраняются. Internal paths, UUID и
  selectable category checkboxes не показываются.
- Кнопка `Очистить...` открывает confirmation
  `Удалить ненужные временные файлы?`; destructive action называется
  `Удалить временные файлы`, initial focus остаётся на Cancel. Escape/Cancel
  сразу возвращают focus инициатору.
- Accept синхронно повторно проверяет eligibility и получает local interlock,
  даже если queued invalidation signal ещё не обработан. После Accept focus
  следует фазовому contract ниже и не возвращается на disabled initiator.
- В runtime-фазе `Running` панель показывает `Отменить очистку`. После Accept
  focus переходит на эту кнопку. Controller отправляет Cancel только для exact
  tracked C6 UUID captured owner; общий `cancelActiveOperation()` без UUID/kind
  не используется. Повторный Enter не создаёт вторую operation.
- После runtime terminal начинается bounded Quick phase: D-Bus Cancel уже
  скрыт/disabled, focus переносится на status, local cleanup не объявляется
  cancellable. После combined terminal focus возвращается на `Очистить...`;
  если она недоступна, fallback получает `Обновить состояние`, затем status.
- Capability, owner, editor или controller state change закрывает открытое
  confirmation и сбрасывает pending intent. UI различает `NotSupported`,
  `Ready`, `Running`, `Blocked`, `Succeeded`, `Partial`, `Cancelled` и
  `Unresolved`. Нулевой успех сообщает, что безопасных временных файлов нет.
- `Обновить состояние` выполняет same-owner exact GetOperation либо owner-fenced
  re-handshake. Get может подтвердить старый outcome; replacement-owner
  handshake только обновляет capability и оставляет старый outcome
  `Unresolved` до explicit acknowledgment. Новый запуск до acknowledgment
  запрещён, а refresh не replay-ит Queue или Cancel. Предварительная оценка
  size не показывается.
- Заголовок панели имеет `Accessible.Heading`, routine dynamic state
  `Accessible.StatusBar`, а `Partial`, `Unresolved` и ошибки
  `Accessible.AlertMessage`; confirmation имеет accessible name. Смысл не
  передаётся только цветом. Кнопки имеют visible label, accessible name и
  description, keyboard focus и minimum height 44 px.
- При ширине до 430 px actions переходят в одну колонку. Dialog ограничен
  `min(440 px, overlay.width - 48 px)`; панель и открытый dialog не создают
  horizontal overflow на viewport 320 px.

#### Этапы и Definition of Done

1. RED catalog-store tests фиксируют byte-preservation accepted index,
   indexed thumbnail и origin records; typed bounded orphan cleanup, empty
   success, unsafe/malformed index blocker, symlink/hardlink/special/foreign/
   changed-identity preservation, bounds и exact logical byte report.
2. RED artifact-store tests фиксируют typed assessment и bounded batch report,
   exact IDs/counts/logical bytes, expiry boundary, revoked/expired idle,
   active reservation/valid lease/hold blocker, owner-disconnect defer,
   accepted symlink-leaf contract, unsafe startup и cleanup failure.
3. RED DeviceManager tests таблично фиксируют каждый blocker, atomic latch,
   timer/store mutation exclusion, event-loop batching, all outcome rows,
   empty success, partial failure, pre/post-mutation cancellation и bounded
   history idempotency. Negative assertions доказывают byte-preservation всего
   retry/recovery root, journals, inbox/spool, saved layouts, indexed
   thumbnails и device media.
4. RED D-Bus/Quick tests фиксируют exact token/additive signature, API `8`,
   byte-identical старые signatures, legacy `NotSupported`, exact UUID/kind,
   captured-owner Queue/Get/Cancel, NoReply/owner replacement без replay и
   отсутствие USB worker cancellation для C6.
5. RED Quick/controller tests фиксируют typed `PreviewCleanupReport`, local
   interlock до Queue, block `begin/beginDropped/beginRecoveredVideo` и preview
   generation, runtime-success-before-preview ordering, release на всех
   terminal paths, phase-aware app/owner loss и `Partial` при local failure без
   automatic retry.
6. RED/GREEN QML tests фиксируют confirmation без dispatch, synchronous Accept
   recheck, stale-confirmation invalidation, exact runtime-phase cancel,
   Cancel-to-status-to-initiator focus transitions, 320 px panel/dialog,
   keyboard/accessibility, refresh/acknowledgment без replay, все состояния и
   полный русский перевод.
7. Выполняются focused suites, fresh build, полный `package-check`,
   `git diff --check`, translation/catalog gate и три независимых read-only
   review. Только после подтверждённых результатов C6 получает статус
   `реализовано`, а `docs/todo-next.md` закрывает пункт.

#### Риски, rollback и out-of-scope

- Главный риск состоит в смешении disposable cache и recovery state. Store
  methods имеют positive allowlist, а negative-preservation tests делают
  indexed/recovery files невыбираемыми даже при похожем имени.
- Candidate scan и unlink не являются одной filesystem transaction. Exclusive
  latch, lifetime pins и per-leaf identity recheck защищают plan-to-batch
  boundary, но не делают пару pathname check/unlink атомарной против
  произвольного concurrent same-UID writer. Confirmed partial result не
  выдаётся за полный success.
- Runtime и Quick части не атомарны между процессами. Подтверждённая runtime
  часть не откатывается, local failure виден отдельно, automatic retry
  запрещён.
- Logical `st_size` не равен гарантированно освобождённым filesystem blocks.
  UI использует точную формулировку и не показывает free-space claim.
- Rollback удаляет capability, один additive Manager2 method, operation kind,
  store cleanup methods, Quick controller/interlock и panel. Persistent schema
  не меняется; уже удалённые disposable files не восстанавливаются. USB
  compensation и data migration не нужны.

**Out of scope:** удаление или автоматическая regeneration indexed thumbnails,
очистка retry/recovery state, перенос cache roots, общий cache browser,
automatic scheduled cleanup сверх существующих scheduled/startup sweeps,
disk-capacity/free-space API, очистка Qt/QML caches, CLI command, SSH/remote
operation, FilePull, device mutation, firmware, API 9, Manager3 и C7.

### C7. GIPHY integration (отменено)

**Статус:** отменено по решению пользователя 5 сентября 2026 года.
Реализация не начиналась; код, тесты, конфигурация, зависимости и runtime не
изменялись.

Ни встроенный GIPHY API/search, ни внешний browser handoff не входят в
планируемый scope. Существующий импорт локальных media-файлов остаётся
единственным таким пользовательским потоком. Причина решения: ожидаемая
пользовательская ценность мала, а стандартный API flow не соответствует
обязательной для Tryx цепочке local download, validation, transcoding и device
copy без отдельного разрешения сервиса. Возврат к C7 возможен только по новому
явному запросу и через новое предложение с повторной проверкой актуальных
условий сервиса.

### C12. Extended media metadata contract

**Статус:** реализовано 31 августа 2026 года. Software acceptance, fresh
`package-check` и три независимых read-only review завершены; API остаётся
равным `8`, новый USB protocol operation и device write не добавлялись.

**Пользовательская цель:** после явного `Edit` показывать в Media Editor
размеры, длительность и частоту кадров уже подготовленной device-копии. Export
использует тот же безопасный artifact pipeline, но не открывает Media Editor.
Поля не выдаются за свойства первоначального локального файла и не требуют
скрытого чтения всех файлов из Media Library.

#### Подтверждённые границы

- Firmware `MediaEntry` содержит только path, extension, size и read-only.
  Текущий catalog tuple не содержит geometry, duration, FPS или content hash.
- `mediaId` связывает device identity, remote name, size и source, но не bytes.
  Замена файла тем же именем и размером сохраняет `mediaId`, поэтому catalog
  metadata нельзя считать доказательством текущего remote content.
- Подтверждённый FilePull возвращает private owner-scoped raw Annex B H264 с
  exact decoded SHA-256. Уже существующая validation выполняет bounded local
  `ffprobe` и полный decode до публикации artifact.
- Raw H264 не имеет надёжного container duration и не восстанавливает original
  source FPS. Geometry можно прочитать из exact artifact. Constant 30 FPS и
  duration `frames / 30` публикуются только когда decoded artifact SHA-256
  совпал с `preparedSha256` точной managed-origin записи, созданной нашим
  30 FPS conversion profile. Для чужой или старой user media эти два поля
  остаются unavailable.

#### Выбранный versioned contract

- `GetRuntimeCapabilities()` получает additive token
  `runtime.device-media-metadata.v1`. API version остаётся `8`.
- Manager2 получает новый cache-only метод
  `GetDeviceMediaMetadataV1(artifactId, leaseId)` и отдельный wire type
  `TryxRuntimeDeviceMediaMetadataV1`. Старые `TryxRuntimeMediaEntry`,
  `TryxRuntimeMediaCatalogSnapshot` и `TryxRuntimeDeviceMediaArtifact` не
  расширяются и сохраняют exact positional signatures.
- Новый snapshot содержит `schemaVersion`, exact `operationId`, `artifactId`,
  `mediaId`, `deviceIdentity`, `decodedSha256`, `deviceGeneration`, typed
  `status`, битовую маску `availableFields` и значения `width/height`,
  `durationMilliseconds`, `frameRateNumerator/Denominator`. Биты `0x1`, `0x2`
  и `0x4` означают dimensions, duration и frame rate; остальные запрещены.
  Ноль при сброшенном бите является canonical empty value, а не измерением.
- Exact D-Bus signature заморожена как
  `TryxRuntimeDeviceMediaMetadataV1=(ussssstsuuutuu)`. Wire statuses допускают
  только `Ready` с mask `0x7`, `Partial` с mask `0x1`, `Unavailable` с mask
  `0` и `ProbeFailed` с mask `0`. `Loading` и `NotSupported` существуют только
  в Quick и не передаются по D-Bus.
- Getter доступен только D-Bus unique owner активного claimed artifact и exact
  lease. Он проверяет owner, lease, expiry и filesystem identity через уже
  существующий artifact store, но не запускает `ffprobe`, FilePull, USB request
  или другой subprocess.
- Metadata вычисляется в уже существующем bounded validation pass до atomic
  публикации artifact и хранится только в transient artifact record. Release,
  expiry, owner disconnect и runtime restart удаляют его вместе с artifact.
- Advertised capability не допускает fallback при `UnknownMethod`. Ошибка
  metadata остаётся presentation failure и не блокирует Preview, Export,
  Save as new или Replace уже корректно claimed artifact.

#### Probe и canonical validation

- `ffprobe` запускается без shell для `-f h264`, одного video stream,
  `-count_frames` и bounded diagnostic tail. Input `-framerate 30` не
  используется как доказательство FPS: raw H264 demuxer иначе подменил бы
  наблюдение заданным приложением значением. Один общий hard deadline охватывает
  ffprobe и последующий полный FFmpeg decode; cancellation по
  operation/generation сохраняется.
- Codec обязан быть H264, geometry ровно 2240x1080. Exact positive
  `nb_read_frames` сохраняется transient. Только совпадение pulled hash с
  persisted `preparedSha256` разрешает вывести duration и rational `30/1` FPS.
- Parser order-independent, отвергает duplicate/unknown keys, overflow и
  non-decimal dimensions/frame count. Canonical `30/1` берётся только из exact
  managed-origin conversion profile с совпавшим content hash. Отсутствующее или
  невалидное optional поле становится unavailable, но не превращается в ноль.
- Probe output/diagnostic ограничен 16 KiB, timeout использует bounded
  kill/drain. Frame count ограничен 18 144 000 кадрами, duration семью сутками.
  Checked half-up formula равна `(frames * 1000 + 15) / 30`; available FPS
  допускает только `30/1`. Любой overflow или выход за bounds не удаляет
  валидный artifact, а оставляет optional timing metadata недоступной.
- Timing hash proof требует одновременно exact `deviceIdentity`, exact writable
  user remote tuple name/size/source/read-only, exact `mediaId`, полную валидную
  managed-origin запись, canonical PASE 30 FPS conversion profile и равенство
  `decodedSha256 == preparedSha256`. Совпадение с другой записью, один только
  `managedOrigin=true`, wrong profile, malformed/unsupported store или hash
  mismatch не дают timing proof.
- Runtime и Quick повторно fail-closed проверяют schema, SHA-256, identity,
  canonical presence/value pairs и bounds. Ответ другого artifact, media,
  device, hash, owner epoch или handshake attempt отбрасывается.
- `deviceGeneration` означает provenance generation staging operation, а не
  текущий live connection gate. Reconnect или рост connection revision не
  инвалидирует immutable hash-bound artifact сам по себе.

#### UI и accessibility

- Блок `Device copy metadata` виден только для recovered device copy в Media
  Editor и имеет состояния `Loading`, `Ready`, `Partial`, `Unavailable` и
  `Not supported`.
- Все три строки остаются видимыми: `Dimensions`, `Duration`, `Frame rate`.
  Неизвестное значение показывает `Unavailable`, а не `0`.
- Ready geometry заменяет filename-suffix heuristic в предупреждении редактора.
  Если geometry ещё неизвестна, текст не подставляет target resolution как
  будто это измерение source.
- Текст использует `Text.PlainText`, переносится на узкой ширине, не полагается
  только на цвет и предоставляет полное accessible description.

#### Этапы и Definition of Done

1. RED tests фиксируют новый D-Bus type/signature, frozen API 8 tuples,
   capability token, strict probe parser и transient artifact binding.
2. GREEN backend расширяет validation result и artifact store, затем добавляет
   owner/lease-protected cache-only Manager2 getter.
3. RED/GREEN Quick tests фиксируют capability gating, malformed/stale response,
   editor reset и невлияние metadata failure на claimed artifact workflow.
4. RED/GREEN QML tests фиксируют все состояния, форматирование, узкую ширину,
   accessibility, отсутствие filename heuristic и законченный русский перевод.
5. Выполняются focused suites, полный fresh `package-check`, diff review и
   независимые read-only reviews. Только после этого C12 получает статус
   `реализовано`, а `docs/todo-next.md` закрывает пункт.

#### Результат реализации, 31 августа 2026 года

- Runtime публикует additive capability
  `runtime.device-media-metadata.v1` и самостоятельный frozen wire type
  `(ussssstsuuutuu)` через owner/lease-protected cache-only
  `GetDeviceMediaMetadataV1`. API version осталась `8`, а прежние Manager1,
  Manager2 и protobuf tuples не изменились.
- Один общий bounded validation pass проверяет exact H264 geometry, полный
  decode и optional frame count. Geometry сохраняется для любого валидного
  pulled artifact; duration и `30/1` FPS появляются только при exact
  device/remote/mediaId/profile/hash proof. Metadata остаётся transient и
  удаляется вместе с artifact при release, expiry, owner disconnect или
  restart.
- Media Editor показывает artifact-scoped `Loading`, `Ready`, `Partial`,
  `Unavailable` и `Not supported`, не выводит resolution из filename или
  target geometry и не блокирует Preview, Save as new или Replace при
  metadata-only failure. Capability handshake теперь откладывает ровно один
  запрос для текущего artifact; смена artifact, cancel и runtime invalidation
  гасят устаревший deferred request.
- Characterization baseline закрепляет C12 backend, RuntimeClient, controller,
  английские QML и отдельные русские QML regressions. Границы `18 144 000` и
  `18 144 001` кадров проверяются изолированно; production-path тест проходит
  через `DeviceManager` и доказывает как metadata forwarding, так и отказ от
  timing при wrong `mediaId` proof.
- Финальный fresh `package-check` прошёл: 38 NVIDIA provider, 833 printer и
  recovery, 17 replace journal, 70 CLI, 131 Material QML, 112 Quick
  client/model, 37 runtime bootstrap, 89 capability/handshake и 20 tray cases.
  Два translation-only теста ожидаемо пропускаются в общем QML запуске без
  каталога, а обязательные отдельные русские C12/B10 baseline-запуски проходят
  без skip. Собрано 1 704 завершённых русских перевода; `qmllint`, translation
  catalog, GUI smoke и расширенный runtime baseline прошли.
- Три независимых implementation review проверили backend proof и bounds,
  RuntimeClient owner/epoch/attempt fencing, UI lifecycle, accessibility и
  локализацию. После исправления найденной handshake race итоговый вердикт по
  всем трём направлениям `GO`, открытых C12 findings нет.
- Отдельный hardware/USB smoke для C12 software acceptance не требуется:
  feature не добавляет новый USB request или mutation, а анализирует только
  bytes уже явно полученного существующим FilePull artifact. Реальная ручная
  проверка Edit остаётся допустимой UX-проверкой, но не является незакрытым
  этапом C12.

#### Acceptance и rollback

- Один explicit FilePull остаётся единственной USB read operation. Открытие
  карточки, getter metadata и повторный показ editor не отправляют USB OUT.
- Готовый PASE artifact всегда показывает подтверждённую geometry. 30 FPS и
  duration из exact frame count показываются только при exact managed-origin
  hash proof; чужая или старая media честно остаётся `Partial`.
- Тот же `mediaId`, remote name и size с другим decoded hash остаётся
  `Partial`. Missing/N/A frame count, optional count timeout или origin-proof
  failure не меняют успешный FilePull, Claim, Edit/Export и artifact lease.
- Неверный codec, geometry или полный decode по-прежнему отклоняют artifact.
  Metadata-only failure не удаляет прошедшие основные validation gates bytes.
- Late response после release/close, runtime owner change или нового artifact
  не меняет editor state.
- API остаётся `8`, старые tuples и protobuf не меняются; persistent schema и
  package dependencies не добавляются.
- Getter является снимком metadata bytes, подтверждённых при atomic artifact
  finalization и названных echoed `decodedSha256`. Он повторно проверяет
  filesystem identity, но намеренно не хеширует заново до 500 MiB данных и не
  обещает новое content proof на момент каждого вызова.
- Rollback удаляет capability, method/type, transient fields, controller/QML
  presentation и focused tests. Artifact bytes, persistent data и device state
  не требуют migration или compensating USB mutation.

**Out of scope:** metadata всех catalog cards, automatic FilePull, presets,
original source/container properties, audio, remote content hash для catalog,
новая persistent provenance schema, API 9/Manager3, network и любые device
writes.

### C15. Extended telemetry and metric pages

**Статус:** первый software-only срез C15.1 реализован 31 августа 2026 года;
software acceptance, независимый read-only review и полный fresh
`package-check` прошли. Увеличение device-side лимита не одобрено до hardware
evidence.

**Пользовательская цель:** показывать больше полезных показателей системы и
сделать текущий предел очевидным. Full сейчас допускает максимум три метрики,
Split имеет независимые наборы по три на каждую сторону. При выборе четвёртой
метрики UI не должен молча вытеснять самую старую: он показывает `3/3` и
предлагает заменить конкретную позицию.

#### Подтверждённое текущее состояние

- Full, Left и Right хранят отдельные ordered списки максимум по три canonical
  metric token. Один и тот же предел fail-closed проверяется в QML,
  RuntimeClient, DeviceManager, persistence v2 и PASE formatter.
- Reusable `MetricSelector.qml` используется для всех трёх областей. Metric
  flow отделён от media helper: четвёртый выбор хранится только как pending и
  меняет ровно подтверждённую позицию, поэтому silent eviction отсутствует.
- Selector показывает полный сгруппированный catalog отдельно от live
  `availableMetrics`. Выбранный sensor остаётся видимым и удаляемым при потере
  availability, а новый unavailable token недоступен для выбора.
- RuntimeClient запрашивает `GetMetricsCapabilities()` у exact unique owner и
  fence-ит reply по service epoch и handshake attempt. Catalog валидируется по
  bounds, uniqueness и canonical allowlist; stale reply игнорируется. Только
  exact `UnknownMethod` включает bounded legacy fallback, остальные ошибки
  остаются fail-closed.
- API остаётся 8. Tuple `TryxRuntimeMetricsState`, существующие Manager methods
  и signals, protobuf, PASE group IDs и `pase-metrics.json` v2 не изменены;
  полный catalog и live subset остаются разными понятиями.

#### Выбранный selector contract

- Добавляется один reusable `MetricSelector.qml`, используемый для Full, Left
  и Right. Он группирует exact canonical tokens как CPU, GPU, Memory и System,
  ищет по локализованному label и не меняет token, отправляемый runtime.
- Selector показывает `N/3` для Full и отдельные `Left N/3`, `Right N/3` для
  Split. Лимит три остаётся видимым до и после поиска.
- Снятие выбранной метрики изменяет draft сразу. Выбор четвёртой сначала
  открывает явный replacement flow с тремя текущими позициями; до выбора
  позиции массив, порядок, dirty-state и apply request не меняются.
- Cancel, Escape и закрытие popup не меняют draft и возвращают focus на
  исходную metric action. Подтверждение заменяет только выбранный индекс и
  сохраняет порядок двух остальных элементов.
- Выбранная, но временно unavailable метрика остаётся видимой с текстовым и
  accessible unavailable status; её можно снять или заменить. Новую
  unavailable метрику выбрать нельзя. Availability update не редактирует draft
  автоматически. Уже выбранный unavailable token не блокирует Apply остальных
  изменений и передаётся как `--`. RuntimeClient валидирует max, uniqueness и
  подтверждённый catalog. Отсутствие live value разрешено только token, уже
  находившемуся в canonical list той же Full/Left/Right area последнего
  подтверждённого `TryxRuntimeDisplayState` для текущих service owner/epoch,
  display device identity и display revision. `TryxRuntimeMetricsState.revision`
  в grandfathering не участвует: его рост при availability update не отзывает
  подтверждённый layout. После owner/epoch/device replacement или нового
  display state без этого token allowance исчезает. Legacy fallback использует
  уже подтверждённый legacy layout текущей connection identity/revision. Новый
  unavailable token и перенос unavailable token в другую area отклоняются
  client и недоступны в UI.
- Полный catalog запрашивается асинхронно через существующий
  `GetMetricsCapabilities()` и привязывается к exact unique owner, service
  epoch и active handshake attempt. Reply валидируется по bounded count,
  length, uniqueness и известным tokens; stale reply отбрасывается.
- Старый совместимый API 8 runtime без catalog method не становится
  incompatible. При exact `UnknownMethod` selector использует bounded union
  live `availableMetrics` и уже выбранных tokens; другие ошибки оставляют
  catalog unavailable и не включают неподтверждённые choices.
- `availableMetrics` остаётся live subset. Для структурированных provider
  reasons, per-GPU descriptors или новых tokens в будущем потребуется новый
  additive versioned method/type; старый tuple не расширяется.

#### Первый software-only scope

C15.1 следует после provider foundation B9.1, но не отмечает B9 выполненным до
real NVIDIA smoke. Срез включает только:

1. owner-fenced получение полного catalog через существующий Manager2 method;
2. grouped searchable selector, counters и explicit positional replacement;
3. честное отображение selected unavailable без automatic draft mutation;
4. сохранение и regression-проверку реализованного B9.1 инварианта `--` вместо
   stale device value при потере выбранного sensor.

Локальное отображение GPU power/VRAM полностью принадлежит B9.1. C15.1 его не
реализует повторно и только прогоняет существующие B9.1 model/QML regressions.

Новые device labels для VRAM, disk, network, CPU cores или FPS не входят в
C15.1: они требуют новых PASE definitions, allowlist, persistence и rollback,
а не только более богатого host provider.

#### Этапы реализации и acceptance

1. RED RuntimeClient tests фиксируют successful catalog, exact
   `UnknownMethod` fallback, malformed reply, stale owner/epoch fencing и Apply
   с уже выбранным unavailable token без разрешения нового unavailable choice.
   Отдельно рост только `TryxRuntimeMetricsState.revision` с потерей availability
   сохраняет Apply, а owner/epoch/device replacement или подтверждённый display
   revision без token отклоняет его.
2. RED QML tests фиксируют Full `3/3`, независимые Split `3/3` и `3/3`, поиск,
   группы, selected unavailable и отсутствие mutation при четвёртом выборе до
   confirmation.
3. `MetricSelector.qml` заменяет три checkbox flow, а metric selection отделяется
   от общего media `toggled()` helper.
4. Replacement popup получает keyboard traversal, Escape/Cancel focus restore,
   screen-reader name/description/state и narrow-layout проверки на русском и
   английском.
5. После focused GREEN выполняются весь QML runner, Quick/handshake suites,
   существующие B9.1 protocol fixture для stale `--` и Dashboard model/QML
   regressions, `qmllint`, translation/catalog gate, frozen API baseline и
   fresh-binary `package-check`.

#### Результат реализации, 31 августа 2026 года

- Добавлен shared canonical catalog из 11 прежних tokens. Runtime возвращает
  его через существующий `GetMetricsCapabilities()`, а RuntimeClient хранит
  отдельные `metricsCatalog`/`metricsCatalogReady`, не смешивая их с live
  availability.
- Catalog handshake использует exact unique owner, service epoch и attempt.
  Malformed, duplicate, unknown, wrong-case и unbounded ответы отклоняются;
  delayed reply старого owner или superseded attempt не меняет состояние.
- `MetricSelector.qml` группирует CPU, GPU, Memory и System, ищет по
  локализованным labels, показывает Full/Left/Right counters и выполняет
  keyboard- и screen-reader-доступную точечную замену. Cancel, Escape, close и
  потеря availability не изменяют draft скрыто.
- Grandfathering unavailable token привязан к той же подтверждённой
  Full/Left/Right area, device identity и display revision. Новый unavailable
  token, перенос в другую area и allowance старого owner/device отклоняются.
- PASE formatter теперь повторно валидирует максимум три exact unique
  canonical token на область до layout, keepalive или metric-batch transport.
  Четвёртый, duplicate, whitespace, wrong-case и unknown token не приводят к
  частичной или молча обрезанной отправке.
- Полный fresh `package-check` прошёл: 38 NVIDIA provider, 814 printer
  protocol, 17 replace journal, 115 Material QML, 107 Quick client/model, 37
  runtime bootstrap, 49 capability/handshake и 20 tray cases. Собрано 1 672
  завершённых русских перевода, 0 `unfinished`; `qmllint`, translation catalog
  и расширенный runtime baseline, включая английский и русский C15 selector,
  прошли.
- Hardware capacity и visual smoke для 4/5/6 метрик не выполнялись. Старый
  максимум три остаётся обязательным, а B9 получает итоговый hardware-статус
  только после отдельного real NVIDIA smoke.

Acceptance требует одновременно:

- четвёртый выбор без подтверждения побитно сохраняет список, порядок,
  dirty-state и queued Apply request;
- явная замена изменяет ровно выбранный индекс, а Cancel/Escape не меняют ни
  один индекс;
- Full никогда не превышает три, Split имеет независимые максимум три на
  сторону на уровнях QML, client, runtime, persistence и formatter;
- selected unavailable остаётся видимой и удаляемой, unselected unavailable
  disabled при наличии полного catalog, а availability update не вызывает
  dirty-state и не блокирует Apply остальных изменений; legacy fallback
  показывает только bounded union live и уже выбранных tokens; grandfathering
  привязано к confirmed display layout/identity, а не metrics revision;
- search/grouping/counters/replacement полностью доступны с клавиатуры и для
  screen reader, включая long localized labels;
- stale catalog reply после owner replacement не применяется;
- API остаётся 8, старый tuple `(tsbbasassus)`, существующие methods/signals,
  protobuf, PASE group IDs и `pase-metrics.json` v2 не меняются;
- rollback к старому GUI/runtime читает все сохранённые layouts, потому что в
  них по-прежнему не больше трёх прежних canonical tokens.

Device capacity исследуется отдельно: сначала `1021`, затем `1011`, для Full,
Split и Waterfall с layout по 4, 5 и 6 групп. Нужны точный capture, readback,
визуальная читаемость, reboot/reconnect behavior и отсутствие regressions в
обычном layout из трёх групп. `repeated` в protobuf не считается
подтверждением firmware capacity.

Если capacity и читаемость подтверждены, runtime получает новый model-scoped
`maxMetricsPerArea` и отдельный versioned apply path. Старый API 8 и старые
клиенты сохраняют максимум три. Если плотный layout неприемлем, используются
явные pages по 1-3 метрики. Автоматическая rotation требует отдельного
protocol/lease proposal: replay последнего подтверждённого layout безопаснее,
чем новый foreground mutation без подтверждённого ACK.

#### Риски и откат

- Catalog и live subset легко ошибочно объединить. Отдельные client properties
  и tests запрещают объявлять unavailable sensor доступным к новому выбору.
- Popup может изменить draft до confirmation или потерять focus. Новый value
  хранится только как pending choice до явной positional replacement.
- Изменение availability во время открытого popup не применяет скрытую замену:
  confirmation повторно проверяет token и выбранную позицию либо отменяется.
- Rollback удаляет новый component/catalog request и возвращает прежнее
  представление уже сохранённых списков. Data migration, API downgrade и
  compensating USB Apply не нужны.

**Out of scope до hardware evidence:** generic max 4/5/6, скрытый override,
silent eviction, automatic rotation, изменение существующего protobuf только
по предположению, device write на неизвестной модели, FPS/frametime,
per-process telemetry, CPU/GPU voltage, Memory Frequency provider, новые
device tokens и обещание полной RivaTuner parity.

### C16. Пользовательский текст бейджей

**Статус:** software-реализация завершена 6 сентября 2026 года по запросу
«делай бейдж». Fresh `package-check`, translations, QML/baseline и итоговый
независимый read-only review прошли. Hardware smoke, USB queries/writes,
установка, перезапуск активного runtime и firmware в этот этап не входили.

Реализованы value/codec, serializer, versioned Apply, Saved V2, Retry v12,
согласованный display snapshot и EN/RU QML controls. Auto остаётся совместимым;
Custom включается только для `391a:1021` после exact capability handshake.

Проверка: все production binaries и test objects пересобраны с Qt 6.11.2 через
`make -B -j3 package-check` в изолированном root, с синтетическим `/dev`, пустым
`/sys`, отдельными XDG paths и D-Bus. Protocol: 936 pass; badge value/wire: 40;
overlay store: 20; Saved store: 57; Quick: 141; handshake: 126; CLI: 88.
Полный QML: 157 pass и 6 существующих translation-only skips; именованные RU
проверки в baseline прошли отдельно. GUI headless smoke и `git diff --check`
прошли. Итоговый review не обнаружил P0/P1/P2 в проверенном C16 scope.

Execution log: `/tmp/tryx-c16-package-lO1O2o/package-check-synthetic.log`;
review: `/tmp/c16-final-regression-review.md` (временные локальные evidence).
Первый CLI-прогон был некорректен из-за UID 65534 у bind-mounted `/` внутри
user namespace; исправлена только test isolation, существующий security guard
не ослаблен. Смешанные Qt 6.11.1/6.11.2 objects исключены полной пересборкой.
HostAccepted означает согласованную принятую host-конфигурацию, не firmware
glyph readback. Backup конфигурации не восстанавливает удалённые media artifacts.

#### Цель и подтверждённая база до C16

Пользователь может заменить автоматически определённое название CPU/GPU
своей короткой надписью в существующем бейдже. Это не редактор произвольного
числа текстовых объектов и не расширение числа метрик.

- [OverlayLabel](../protocol/wire-v1/overlay.proto) уже содержит `string text`;
  [configurePaseBadge](../src/paseconfigurationclient.cpp) передаёт строку,
  шрифт `roboto-regular`, размер и фон. Новый protobuf field или USB opcode для
  подстановки текста не требуется по текущему host-коду.
- [PrinterClassSession](../src/printerclasssession.cpp) заново вычисляет
  `cpuBadgeText`/`gpuBadgeText` из выбранных hardware models.
  [PaseMetricsConfigStore](../src/pasemetricsconfigstore.cpp) v2 намеренно
  очищает эти transient fields и не сериализует их.
- QML и runtime допускают только два identifiers: `CPU Badge`, `GPU Badge`.
  [ApplyRequest и DisplayState](../src/runtimecontract.h), C5 saved layouts
  и retry fingerprint не содержат пользовательского текста. Их старые
  positional D-Bus shapes нельзя расширить новым полем внутри API 8.
- Устройство не предоставляет подтверждённого query для `OverlayLayout`.
  ACK активации и существующий UserConfiguration readback не являются
  доказательством точного отображения glyphs или надписи.

#### Согласованный пользовательский контракт

1. Сохранить два существующих slots на область. У каждого включённого CPU/GPU
   бейджа появляется источник надписи: «Автоматически» или «Свой текст».
   Auto остаётся default и использует прежний выбор CPU/GPU model.
2. В Full одна область, в Split независимые Left и Right. Предлагается
   разрешить разные надписи одного slot на разных сторонах, в соответствии
   с существующим per-side styling C1. Третий бейдж не добавляется.
3. Custom представляет собой одну plain-text строку после удаления внешних
   пробелов: от 1 до 32 Unicode scalar values и не больше 128 UTF-8 bytes.
   Это предлагаемый **проектный лимит**, не измеренный лимит firmware и не
   гарантия размещения любой строки на экране. Запрещены malformed Unicode,
   NUL, переносы строк, управляющие и format characters, включая bidi controls.
   Пустой Custom является ошибкой, а не скрытым переключением в Auto.
4. Unicode допускается в software contract, но кириллица и другие glyphs
   остаются непроверенными на устройстве до отдельного smoke. Не обещать
   поддержку emoji, новых шрифтов или сложной письменности по одному факту
   наличия protobuf string.
5. Цвета Auto сохраняют текущую vendor-based логику. Custom использует
   существующий нейтральный фон, без угадывания бренда по пользовательскому
   тексту. Новые font/color/size controls и free-position layout вне scope.
6. Редактирование меняет только draft. Apply отправляет media, metrics, style
   и badge choices одной immutable foreground operation. Cancel, ошибка
   валидации, потеря capabilities и неподтверждённый outcome не сохраняют draft
   как успешно применённый. C2 dirty-state guard учитывает режим и текст.

#### Архитектура и совместимость

Рекомендуется явное расширение versioned request, а не второй независимый
SetBadgeText перед Apply. Два вызова создадут промежуточную конфигурацию и риск
применить текст к чужому layout. Подмена host model name тоже не подходит:
она изменит Auto и не позволит задать независимый текст на сторонах.

- Новый value type для slot: `mode = Auto|Custom`, `text`; у Auto text пустой.
  Новый overlay-badge type хранит primary CPU/GPU и secondary CPU/GPU choices.
  Disabled slot и неактивная secondary area канонизируются в Auto/empty;
  потерянный Custom не восстанавливается из названия hardware.
- Новый apply envelope содержит прежний `TryxRuntimeApplyRequest` неизменным
  и отдельный versioned badge payload. Существующие Manager1/Manager2 methods,
  signals и structs сохраняются. API major остаётся 8.
- Добавить versioned apply entry points для обычного Apply, upload+Apply и
  ensure-media+Apply, а также согласованный display snapshot с badge choices.
  Они используют существующий coordinator, operation ID, cancellation,
  owner/device/generation fences и единый worker-owned transport. Не вводить
  второй writer, network service или отдельную очередь бейджей.
- Feature включается через runtime/device capability handshake. Начальный
  implementation target: printer-class PASE `391a:1021`. PANORAMA `1011`
  требует отдельной qualification; TURRIS, legacy serial/ADB и неизвестные
  профили не получают generic custom-text write.
- Старый клиент/метод продолжает передавать только Auto semantics. Новая GUI
  не отправляет Custom через старый метод и не делает silent fallback к Auto.
  Вызов, заменяющий overlay старым request, имеет явную старую Auto-семантику;
  brightness-only и другие mutations без replaceOverlay сохраняют текущий
  custom overlay. Existing Auto-only workflows остаются без изменений.
- Пока capability handshake не завершён или завершился transport error,
  printer-class GUI не принимает старый display tuple как редактируемый
  baseline и не разрешает Apply. Legacy путь выбирается только после
  подтверждённого отсутствия нового token или предусмотренного UnknownMethod.
- Recovered-media Replace не получает новый wire/journal в C16. При Custom
  его кнопка и dispatch заблокированы с понятной причиной. Поддержанный путь:
  Save as new, выбор новой копии и общий Apply с сохранённым draft. Auto-only
  Replace не меняется; backend старого Replace сохраняет старую Auto-семантику.
- Внутренний normalized request, operation equality и retry fingerprint
  включают режим и текст каждой области. Retry восстанавливает именно
  подтверждённый payload, не перечитывает текущий GUI draft и не повторяет
  mutation автоматически после неизвестного результата.
- Hydration обновляет только Auto slots. Custom передаётся как данные через
  существующий serializer, не попадает в shell, rich text, файловые пути или
  имена media. User text не пишется в qInfo/error messages, lifecycle events
  или redacted support bundle; вместо содержимого допустимы режим и длина.

#### Сохранение и восстановление

- Хранить **пользовательский выбор** отдельно от transient resolved model text.
  Развить существующий PaseMetricsConfigStore до нового versioned payload,
  сохранив owner checks, atomic save, 64 KiB bound и device binding. Старые
  v1/v2 records читаются как Auto; чтение само по себе не переписывает файл.
  Будущий или malformed формат остаётся fail-closed.
- Новый versioned display snapshot возвращает принятые badge choices вместе
  с согласованными identity/revision. Это host-accepted configuration, не
  firmware glyph readback. Перезапуск GUI не теряет Custom; существующее
  восстановление подтверждённого overlay после bootstrap уважает Custom и
  не превращается в replay неизвестного или неуспешного Apply.
- C5 Save/Load/Apply должен сохранять эти choices. Нужны новые saved-layout
  wire types/methods и versioned store format, а не расширение V1 tuple.
  Существующие UUID, device binding и CAS revisions сохраняются; V1 records
  имеют Auto defaults. Старый V1 client не получает custom layout с молча
  выброшенным текстом и не может перезаписать такой record через V1.
- Retry/apply codecs и persistent formats тоже получают явную поддержку
  нового envelope. Старые записи читаются как Auto; новый формат не маскируется
  под старый и не подвергается lossy downgrade.
- Откат не удаляет настройки. До первого перехода формата нужен recoverable
  pre-migration snapshot; старый runtime не запускается поверх нового формата
  с надеждой на игнорирование fields. Возврат к Auto и старому формату является
  отдельным явным действием, не compensating USB write при uninstall.

#### Порядок реализации и acceptance

1. RED tests для value validation, canonical Auto/Custom, legacy Auto defaults,
   immutable frozen D-Bus signatures и нового typed round trip. Зафиксировать
   точные новые method/type signatures перед реализацией client/UI.
2. Реализовать normalized envelope, apply/coordinator/worker hydration и
   serializer; fixtures должны проверять точные bytes надписи, slots/IDs,
   Full/Split/Waterfall и отсутствие USB отправки при malformed input.
3. Добавить versioned persistence, saved layouts, retry fingerprint и
   display snapshot. Проверить restart/reconnect, старые records, rollback
   snapshot, unknown outcome, stale generation и разные тексты по сторонам.
4. Добавить EN/RU QML controls, validation, keyboard/accessibility, C2 guard
   и восстановление drafts. Ввод и Save layout не выполняют Apply; одна
   активация Apply создаёт ровно одну operation с точным payload.
5. Выполнить focused suites, полный fresh `package-check`, translation/QML
   baseline и независимый review перед отметкой software complete. B3 tests
   должны доказывать отсутствие sentinel custom text в support output/logs.
6. Отдельно, только после explicit consent, один bounded hardware smoke на
   `1021`: латиница, кириллица, длинная строка и два разных Split texts,
   размещение и возврат в Auto. Для `1011` отдельный capture и verdict.
   Software GREEN не считается подтверждением glyph coverage или новой модели.

**Out of scope:** третьи и дополнительные бейджи, произвольные overlay objects,
изменение числа метрик, загрузка шрифтов, HTML/markup, templates или команды
в тексте, анимация/бегущая строка, remote firmware update и автоматическая
аппаратная проверка. Изменения не должны включать посторонние A8/A9/B5 правки.

## Workstream D: hardware-backed развитие моделей

### D1. Общий hardware evidence kit

До новых моделей подготовить безопасный issue/report flow. Пассивная часть:

- retail model и фото label;
- `lsusb` USB ID и descriptors;
- Linux distribution, kernel, desktop и package version;
- bounded logs с redaction;

Немутирующие DeviceInfo/SysConfig queries всё равно отправляют USB OUT request.
Они допускаются только после подтверждения transport, с явным согласием на
query OUT и отдельно от любого mutating write smoke. Для mutating smoke нужно
ещё одно отдельное explicit consent.

Поддержка не включается только по `product_name`, marketing name или
внутреннему KANALI enum.

### D2. TURRIS 620 полный capability research

Текущий media upload остаётся рабочей базой. Отдельно и последовательно
исследуются catalog, brightness, metrics, presets, fonts и firmware. Каждая
возможность получает отдельный capture, capability flag, offline tests и один
bounded hardware smoke. PASE commands на TURRIS не переиспользуются по
предположению.

### D3. PANORAMA SE V2, PANORAMA V2 и PANORAMA WB V2

Сначала получить пассивную identity. `PASEV2` route в KANALI не является
доказательством USB ID или совместимости. Для каждого профиля последовательно:

- exact VID/PID и descriptors;
- подтвердить transport до немутирующих DeviceInfo/SysConfig query OUT и
  запросить явное согласие на такой query;
- создать versioned geometry/media profile только из подтверждённых данных;
- открывать preview, upload, readback, FilePull, Replace и recovery строго
  capability-by-capability; FilePull или Replace не требуются и не включаются,
  пока соответствующий wire path независимо не подтверждён;
- отдельный firmware eligibility decision.

### D4. HOLO, STAGE, ROTA и PANORAMA WB

Каждое семейство является отдельным feature track. Fan, pump, ARGB, ROTA
detection, multi-screen modes и fonts не объединяются с PASE backlog. Нужны
физический образец и независимый protocol capture.

### D5. Искомые retail variants

- PANORAMA 240 / 280 / 360;
- PANORAMA ARGB 240 / 280;
- PANORAMA SE ARGB 240;
- PANORAMA V2 и PANORAMA SE V2.

До получения identity они остаются tester requests, а не заявленной
совместимостью.

Для PANORAMA ARGB 360 identity `391a:1011` уже известна и community-tested,
но всё ещё требуется maintainer hardware. PANORAMA SE ARGB 360 `391a:1021`
уже подтверждена на maintainer hardware и не входит в unknown-identity список.

### D6. Brightness power-cycle acceptance

На `391a:1021` и отдельно `391a:1011` проверить:

1. установить значение штатным verified write;
2. физически перезапустить устройство;
3. после новой generation прочитать значение без replay;
4. зафиксировать, сохраняет ли firmware яркость самостоятельно.

Автоматический replay brightness при reconnect запрещён до результата этого
теста.

## Workstream E: отдельные security и product proposals

Следующие направления не входят в обычную parity-реализацию:

- cloud firmware updater: нужен официальный источник, authenticity/signature,
  compatibility manifest, power-loss contract и recovery/rollback; публичные
  источники проверены в [B6](#b6-remote-firmware-availability-research),
  execution gate остаётся закрытым;
- vendor preset/cloud library: vendor assets, encrypted materials и закрытые
  endpoints не используются;
- community media gallery или аналог TRYXZONE: отдельный product proposal с
  лицензиями контента, moderation, privacy, storage и abuse limits; это не
  даёт права подключаться к закрытой vendor library;
- fan, pump, ARGB и decoration controls: только для физически подтверждённой
  модели и отдельного protocol proposal;
- filters и Kaleidoscope: только после подтверждения активного wire path;
- device log pull: запрещён без доказанного bounded read-only contract;
- legacy serial FileTransport fallback при отсутствии ADB: отдельное protocol
  research, exact capture, bounded partial-write semantics и capability gate;
- CPU Voltage: добавлять только при точном labelled hwmon source и available
  flag, не угадывать sensor index и не показывать `0` как измерение;
- свободное место: нельзя вычислять из неподтверждённого фиксированного лимита;
- Electron stability change KANALI: к Qt Quick приложению неприменимо, отдельной
  функции не создаёт.

## Порядок milestones

С 6 сентября 2026 года очередь ниже описывает grouping backlog, а не указание
продолжать разработку до релиза. Текущая задача: зафиксированный UI/package
прогон 2.3.0; незакрытые новые возможности возвращаются после выпуска.

### M0. Architecture safety cleanup

- A1-A5.
- Никаких новых USB writes.
- Полный package gate после каждых двух задач.

### M1A. API-8-safe Linux usability

- B1, B4, B7, B8 и B11.
- C1-C3 в пределах уже существующих API 8 полей.
- D6 hardware acceptance отдельно от release build.

Реализованные пункты вошли в MINOR-кандидат 2.3.0; B11 остаётся после релиза.

### M1B. Versioned contracts и telemetry

- Сначала B0: capability handshake при неизменном API 8.
- Затем B2, B3, B10 и C12 отдельными feature changes.
- Новые поля публикуются только через новые versioned methods и types.
- API 9 и `Manager3` в этот milestone не входят.
- Для B9.1 выбран optional bounded `nvidia-smi` subprocess provider без
  D-Bus bump и hard NVIDIA package dependency. C15.1 использует существующий
  `GetMetricsCapabilities()` и сохраняет device limit три.

### M2. Media workflow improvements

- C4-C6.
- Saved layouts и safe cache.
- C16: пользовательский текст существующих бейджей; технический контракт
  подтверждён 6 сентября, software-реализация завершена; физическая проверка
  внесена в release checklist.

### M3. Network и desktop portals

- B5 завершён локально 6 сентября 2026 года с отдельной оговоркой о desktop
  notification delivery.
- B6 research завершён 6 сентября 2026 года. Official public sources найдены,
  но authenticity, точный compatibility manifest и power-loss/recovery contract
  не подтверждены; remote firmware updater остаётся NO-GO до отдельного proposal.
- C7 исключён из scope 5 сентября 2026 года.
- C8 отложен 5 сентября 2026 года из-за неоднородной доступности PipeWire и
  portal backends в поддерживаемых Linux-средах. C9 ожидает отдельного
  возврата к C8 и не реализуется самостоятельно.

### M4. Hardware expansion

- D1-D5, C13 и C14.
- Каждая модель получает собственный plan, tests и support statement.

### M5. Deep runtime decomposition

- A6-A9 по одному ownership boundary.
- Не совмещать с новой моделью или public API bump.

M0 и M1A могут частично выполняться параллельно только в непересекающихся
файлах. Изменения `runtimecontract.*`, `devicemanager.*` и D-Bus adaptors должны
быть последовательными.

## Команды проверки

Команды для ручного запуска приведены в синтаксисе fish.

### Полная сборка и package gate

```fish
qmake6 tryx-panorama-all.pro; and make -j(nproc); and dbus-run-session -- make package-check
```

### Focused protocol/runtime suite

```fish
cd tests; and qmake6 printerprotocol_tests.pro; and make -j(nproc); and ../build/tests/printerprotocol-tests
```

### Replace journal suite

```fish
cd tests; and qmake6 replacejournal_tests.pro; and make -j(nproc); and ../build/replacejournal-tests/replacejournal-tests
```

### Source hygiene

```fish
git diff --check; and git status --short
```

Hardware-write tests не входят в автоматическую команду. Они выполняются
только на точной модели, с явным разрешением и заранее описанным rollback или
recovery boundary.

## Definition of Done для каждой задачи

- Scope не смешивает refactor и новую функцию.
- Изменение затрагивает не более пяти основных файлов либо разбито дальше.
- Есть focused regression test и полный соответствующий suite.
- D-Bus API и package contract проверены, если они затронуты.
- Неподтверждённая hardware-функция остаётся capability-gated.
- Ошибка или потерянный ACK не превращаются в ложный success.
- Документация и support matrix обновлены в том же feature change.
- Hardware-tested статус используется только после реального smoke.
- Commit, push, PR, release и publication выполняются только по отдельному
  явному запросу.

## Границы

### Всегда

- переиспользовать Qt 6, QML, qmake и текущие D-Bus contracts;
- сохранять один hardware owner;
- валидировать все внешние файлы, paths и сетевые ответы;
- разделять feature, refactor, tests и release metadata на reviewable changes;
- сохранять dirty или пользовательские файлы вне scope.

### Сначала согласовать

- новую dependency;
- новый `Manager3` или иное несовместимое изменение API 8;
- изменение persistent format;
- сетевой сервис и credentials;
- новый USB write или firmware operation;
- изменение Git policy для локального `docs/`;
- публичное заявление о поддержке новой модели.

### Никогда

- не копировать vendor code, assets, keys или закрытые endpoints;
- не открывать capability только по marketing name;
- не повторять unknown mutation автоматически;
- не удалять recovery artifacts общим `Clear cache`;
- не обещать hardware verification без устройства;
- не смешивать архитектурный rewrite с feature release.

## Открытые решения перед реализацией

1. Определить project Privacy URL до включения сетевых функций.
2. Определить владельца hardware testing для TURRIS и V2.
Источник B9 больше не является открытым решением: 31 августа 2026 года выбран
optional bounded `nvidia-smi` subprocess provider. NVML допустим только как
будущий private helper process после отдельного review. Владелец и среда real
NVIDIA hardware acceptance пока не определены.
