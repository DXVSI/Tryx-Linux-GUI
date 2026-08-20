# Feature proposal: декомпозиция runtime и развитие функций

## Статус

- Статус: active, выполняется небольшими последовательными изменениями.
- Дата фиксации: 9 августа 2026 года; обновлено 20 августа 2026 года.
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
- Полный AMD telemetry path и vendor-neutral GPU model/badge resolution,
  включая NVIDIA и Intel. Приложение запускается на NVIDIA-системах, но
  полноценные NVIDIA temperature, usage и frequency ещё не реализованы.
- Native Linux tray, notifications, language и user-systemd autostart.
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
        |       +-- OperationRecoveryStore
        |
        +-- FirmwareBridge / firmware-exclusive gate
```

`DeviceManager` публикует стабильный façade и связывает компоненты. Session
controller владеет generation/reconnect state. Operation coordinator владеет
operation lifecycle. Stores выполняют только bounded atomic persistence и не
эмитят UI-сигналы. Protocol слой не владеет GUI или persistent operation state.

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

**Зависимости:** A1.

**Scope:** перенести уже самостоятельный QObject в
`printermediapreparer.{h,cpp}` без изменения сигналов, queue order или
thread affinity.

**Acceptance:** все preparation, thumbnail, Turris MXHD, cancellation и retry
validation tests проходят без изменения observable behavior.

### A4. Вынести чистые helpers

**Зависимости:** A3.

Последовательно, отдельными изменениями:

- `PrinterMediaValidator`;
- `TurrisMediaWriter` и MXHD validation;
- overlay/config serializers;
- safe path, fingerprint и bounded-process helpers.

Каждая задача должна затрагивать не более пяти файлов и сохранять старые test
fixtures.

### A5. Выделить persistence stores

**Зависимости:** A2, A4.

Каждый store выполняется отдельной задачей:

1. `MediaCatalogStore` для index, thumbnails и origin metadata.
2. `PaseMetricsConfigStore` для versioned per-device overlay settings.
3. `DeviceMediaArtifactStore` для ownership, leases и outbox cleanup.
4. `OperationRecoveryStore` для retry cache, delete intent и replace journal
   coordination.

**Acceptance:**

- atomic `QSaveFile` boundaries сохранены;
- corrupt, stale, oversized и cross-device records отклоняются;
- абсолютные пользовательские paths и secrets не попадают в persistent data;
- stores возвращают typed results и не меняют operation state самостоятельно.

### A6. Выделить PrinterOperationCoordinator

**Зависимости:** A5.

**Scope:** перенести queue/retry/cancel и четыре крупных completion workflow
целиком, не дробя recovery branch между владельцами.

**Acceptance:**

- только coordinator владеет active operation и operation history;
- upload, pull, apply, delete, replace, metrics и retry сохраняют exact
  terminal outcomes;
- сначала переносятся существующие string contracts без изменения D-Bus;
- typed internal enum/variant вводятся только отдельным последующим изменением.

### A7. Выделить PrinterSessionController

**Зависимости:** A6.

**Scope:** передать controller состояние discovery snapshot, product identity,
generation, reconnect, keepalive, overlay restoration, recovery и firmware
quiesce.

**Acceptance:**

- один physical remove/add создаёт ровно одну новую generation;
- stale operation не может продолжиться на новом endpoint;
- session recovery не повторяет mutation;
- firmware fence сериализован с USB worker queue.

### A8. Разделить legacy и printer-class worker policy

**Зависимости:** A7.

Legacy serial/ADB и printer-class code получают отдельные policy/session
объекты, но продолжают исполняться через один контролируемый worker context.
Создание двух независимых USB writers запрещено.

### A9. Разделить PrinterProtocol по слоям

**Зависимости:** A7. Выполнять последним из архитектурных этапов.

Отдельными задачами:

- `UsbPrinterTransport`;
- frame/transaction channel;
- discovery и `PrinterDeviceMonitor`;
- PASE config/media client;
- Turris container и media client.

Wire fixtures, response matching, drain/cancel и ambiguous-outcome semantics
должны оставаться побитно совместимыми.

## Workstream B: Linux Settings и диагностика

| ID | Функция | Зависимости | Acceptance |
|---|---|---|---|
| B0 | API 8 capability handshake | A1 | Зафиксированы API 8 wire и behavioral baseline; старый API 8 без `GetRuntimeCapabilities()` сохраняет baseline-функции и получает пустой набор только при `UnknownMethod`; остальные ошибки fail closed; device capabilities привязаны к identity, connection revision и physical generation; неизвестные и malformed tokens не включают функции; API остаётся 8 |
| B1 | Реальная модель и live version | A5 | Без изменения API 8 Settings публикует уже доступные product ID, mapped model, firmware и app version; serial/chip ID не экспортируются открыто |
| B2 | °C/°F и 12/24H | B0, B1 | Dashboard и runtime overlay используют одну persisted setting; timezone остаётся системным; runtime работает после закрытия GUI |
| B3 | Redacted support bundle | A5, B1 | bounded host logs, app/runtime versions, USB/session state и operation summary; удалены usernames, home paths, serial, chip ID, environment и media |
| B4 | Close behavior | нет | Пользователь выбирает Hide to tray или Quit GUI; Hide недоступен без StatusNotifier host; runtime не останавливается |
| B5 | GitHub release notification | нет | Проверка выполняется явно или с opt-in interval; только уведомление и ссылка; без self-update и без restart active runtime |
| B6 | Remote firmware availability research | B1 | Сначала определить официальный source, authenticity/signature, compatibility manifest и rollback contract; до отдельного proposal нет remote download, update badge или flash |
| B7 | Legal/About links | нет | Показываются только существующие project License, Privacy или User Agreement URLs; отсутствующая политика не выдумывается |
| B8 | GUI autostart option | B4 | Фоновый systemd service и запуск GUI разделены; состояние доступно и обратимо через user session |
| B9 | Полный NVIDIA telemetry backend | A1 | Stable per-GPU identity, temperature, usage, frequency, power и VRAM публикуются только при реальном источнике; bounded timeout; отсутствие driver/tool даёт unavailable, а не ноль; dual-GPU ordering тестируется |
| B10 | Versioned Device Specifications contract | B0, A1, B1 | Для известных `1011/1021` декодируется и кэшируется уже полученный bootstrap SysConfig response без второго USB query; geometry и подтверждённые fields публикуются новым versioned method и новым wire type; дополнительный query OUT допустим только для исследуемого профиля после D1, подтверждения transport и явного согласия |
| B11 | Дополнительные локализации | нет | Каждый язык имеет отдельный Qt translation catalog, native-speaker review основных workflow и fallback на English; machine-only перевод не объявляется полноценной локализацией |

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

## Workstream C: Display и media workflow

| ID | Функция | Зависимости | Acceptance |
|---|---|---|---|
| C1 | Per-side styling | B1 | Full использует один style block, Split отдельные Left/Right color, alignment и Top/Bottom; отправляется один verified Apply |
| C2 | Dirty-state guard | C1 | При route/close предлагаются Apply, Discard и Stay; закрытие не применяет USB mutation автоматически |
| C3 | API-8-safe media origin UI | A5 | Уже доступные source, size и thumbnail показываются как User media или Device preset; сумма sizes не называется free space |
| C4 | Split-aware crop | B0, C3 | отдельный versioned transform profile и честный area canvas; full origin не переиспользуется как split transform |
| C5 | Saved layouts | C1, C3 | versioned, device-scoped layouts; fresh catalog validation; только explicit Apply; никакого replay после reconnect |
| C6 | Safe cache management | A5 | очищаются только regeneratable previews и expired released artifacts; active operation, retry, journal или lease блокируют cleanup |
| C7 | GIPHY integration | C3 | только публичный API, project credentials, opt-in network, privacy/TOS attribution, bounded download и существующий media validation pipeline |
| C8 | Wayland screen recorder | C3 | XDG ScreenCast Portal, PipeWire, системный permission dialog, bounded owner-only temp file и передача результата в Media Editor |
| C9 | Global shortcuts | C8 | XDG GlobalShortcuts portal при наличии; отсутствие portal не ломает GUI; shortcut не обходит recorder permission |
| C10 | Device-side fonts | B0, D2 или D4 | allowlisted enum и exact readback только на модели, где selector и protocol подтверждены; для PANORAMA/PASE не включать по данным KANALI 2.4.0 |
| C11 | Passive `play_finished` diagnostics | D1 | сначала read-only trace; событие не запускает automatic Apply или replay |
| C12 | Extended media metadata contract | B0, C3 | Dimensions, duration и FPS получают bounded `ffprobe` и публикуются только новым versioned method и новым wire type; старый API 8 tuple не меняется |
| C13 | Display frame-rate control | B0, D1 | только allowlisted значения и exact readback на отдельно подтверждённом product profile; неподдерживаемая модель не получает generic write |
| C14 | Linux display sleep policy | B4, D1 | отдельно исследуются GUI Quit, runtime stop, suspend и shutdown; политика opt-in и model-gated, не переписывает standby media и не обещает USB mutation после начала poweroff; resume не повторяет предыдущую mutation |

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
  compatibility manifest, power-loss contract и recovery/rollback;
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

### M0. Architecture safety cleanup

- A1-A5.
- Никаких новых USB writes.
- Полный package gate после каждых двух задач.

### M1A. API-8-safe Linux usability

- B1, B4, B7, B8 и B11.
- C1-C3 в пределах уже существующих API 8 полей.
- D6 hardware acceptance отдельно от release build.

Это рекомендуемый следующий пользовательский MINOR scope.

### M1B. Versioned contracts и telemetry

- Сначала B0: capability handshake при неизменном API 8.
- Затем B2, B3, B10 и C12 отдельными feature changes.
- Новые поля публикуются только через новые versioned methods и types.
- API 9 и `Manager3` в этот milestone не входят.
- B9 не зависит от D-Bus bump, но начинается только после выбора и упаковки
  NVIDIA backend.

### M2. Media workflow improvements

- C4-C6.
- Saved layouts и safe cache.

### M3. Network и desktop portals

- B5, B6 research и C7-C9.
- GIPHY, recorder и shortcuts разными feature changes.

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

1. Решить, нужен ли GIPHY непосредственно в приложении или достаточно
   безопасного browser-assisted import без API credentials.
2. Определить project Privacy URL до включения сетевых функций и recorder.
3. Определить владельца hardware testing для TURRIS и V2.
4. Решить, должен ли этот локальный план стать tracked public roadmap. Сейчас
   `docs/` намеренно игнорируется `.gitignore`.
5. Выбрать источник NVIDIA telemetry: bounded `nvidia-smi` integration или
   NVML с явным dependency/package contract. Факт запуска GUI на NVIDIA не
   является acceptance telemetry backend.
