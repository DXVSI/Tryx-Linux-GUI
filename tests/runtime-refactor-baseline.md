# Baseline декомпозиции runtime

Этот перечень фиксирует минимальную страховочную сетку перед переносом кода из
`DeviceManager`. Полные test suites остаются главным критерием корректности.
Скрипт `check_runtime_refactor_baseline.sh` дополнительно не позволяет незаметно
удалить перечисленные ниже защитные сценарии из собранных Qt Test binaries, а
критические C2/C3/C4 QML-сценарии запускает как именованные тесты через
`qmltestrunner`. Полный QML gate и именованные проверки используют тот же
`Material` style, который production GUI выбирает до создания приложения.

## Автоматические проверки

- `C16 custom badges`: строгий Unicode/JSON codec, frozen API 8 tuples и
  additive Apply/Saved V2/coherent snapshot round trips. Проверяются независимые
  Full/Left/Right choices, neutral Custom background, immutable operation/retry
  identity, persistence-before-publication, owner/generation/revision fences,
  pending/error handshake без Auto fallback и запрет lossy Replace. Рост
  connection revision между чтениями capabilities и coherent snapshot запускает
  не более трёх дополнительных чтений контекста без device mutations до
  coherent HostAccepted либо нового явного refresh/handshake; Pending не
  сбрасывает лимит. Ответы старого owner/handshake отбрасываются, восстановление
  очищает только собственную диагностику. Непрерывный рост оставляет Apply
  заблокированным. Ввод с клавиатуры меняет только draft, ошибка блокирует Apply, выключение slot
  сбрасывает его в Auto. Overlay store v3, Saved store v2 и Retry v12 читают
  legacy Auto без записи; version upgrade сохраняет точный private backup,
  несовместимый downgrade не удаляет настройки. Retry после restart остаётся
  manual-only. Проверка support snapshot и логов не допускает custom text;
- `B5 release updates`: offline Qt Network fixture проверяет fixed endpoint,
  request headers, строгий stable version parser, ETag/snapshot binding, 304,
  silent failures, monotonic startup/hourly schedule без catch-up, один request,
  абсолютный timeout, streaming cap, cancel/destructor и rate-limit cooldown.
  Структурный guard удерживает checker после single-instance/QML startup, вне
  smoke/runtime/CLI. EN/RU Settings tests проверяют условную строку, keyboard
  activation и возврат фокуса после исчезновения обновления. Изолированный
  notification service проверяет отказ без очистки availability; fresh-client
  и slow-service processes защищают от блокирующего D-Bus Introspect и обхода
  regression через Qt interface cache. Ни один fixture не обращается к GitHub;
  реальная доставка desktop popup проверяется отдельно;
- `A9 protocol layers`: façade сохраняет публичные типы и API 8; один
  `PrinterTransactionChannel` владеет frame buffer, track IDs и outbound clock,
  `UsbPrinterTransport` владеет native/scripted I/O и fd test seam. PASE
  configuration/media и Turris clients заимствуют channel и не копируются;
  общий upload helper сохраняет единственную ACK-последовательность. Прямые
  owner-тесты проверяют close/destructor без закрытия borrowed cancellation fd,
  persistent failure latch после close, сохранение уже прочитанного второго
  кадра и track numbering после уничтожения clients. Structural guard требует
  qmake wiring всех слоёв и запрещает второй transport/buffer/upload loop;
- `A8 worker policies`: существующий QObject façade сохраняет slots/signals,
  немедленные generation/cancellation gates и два разных eventfd; legacy и
  printer sessions владеют своим transport/FSM/timers в одном I/O context.
  PTY/socket fixtures проверяют legacy handshake и команды, закрытие обоих
  transport до firmware ACK, отсутствие I/O из отложенного callback после
  quiesce, один SystemMonitor, перенос всех QObject children вместе с worker
  и уничтожение sessions до закрытия gates. Отмена из другого потока прерывает
  уже ожидающий ответ foreground request; same-path generation и late
  completion не возобновляют timers. Прежние cancel/drain, recovery, result
  ordering и overlay checks сохранены;
- `printer-protocol`: стабильные Manager1/Manager2 D-Bus контракты и API 8,
  точные product capabilities, generation fencing, отсутствие автоматического
  replay после неизвестного результата, firmware-exclusive ownership и
  fail-closed восстановление transport, persistent media catalog и PASE
  overlay stores с device scoping, v1 migration и atomic persistence
  boundaries, а также transient device-media artifact ownership с точной
  D-Bus caller identity, UTC lease/hold lifecycle, bounded startup cleanup и
  inode/hash validation; typed delete-intent v2 с product identity,
  совместимым чтением legacy v1, монотонными crash-recovery transitions и
  запретом USB replay после неизвестного результата; canonical
  `RetryCacheStore` v12 в прежнем canonical каталоге v11; compatibility root
  shadow v12 не интерпретируется выпущенным v10 как Auto; legacy v10/v11 читается
  новым runtime как Auto,
  двумя durable barriers перед единственным USB emit, fail-closed миграцией
  v1-v10, A+B/retry lineage, shadow-missing fences, exact cleanup tombstones,
  сохранением независимого retry candidate при успешной новой загрузке и
  upload-only restart-нормализацией legacy continuation;
- `C4 split-aware crop`: frozen API 8 transform tuple не меняется, а additive
  profile `(us(usuuuuu))` и отдельные runtime/device capability tokens
  разрешают Split только для поддерживаемого PASE/PANORAMA. Full сохраняет
  прежние 2240x1080 и Turris 1280x720 paths, Split получает честные
  1120x1080 preview, FFmpeg output, suffix, conversion profile и durable retry
  identity. Произвольный downgrade в released v10 не разрешён: explicit
  Manager2/CLI preparation принимает только `Empty` или exact terminal
  `FullFrame`, а Split, in-flight dispatch, cleanup/transition, journals и
  непроверенный store блокируют переход. Owner-only marker в
  `XDG_STATE_HOME` связан с полной identity executable; тот же v11 runtime
  останавливается до D-Bus ownership и `DeviceManager`, другой executable его
  игнорирует, а несостоявшийся package replacement отменяется только offline
  через `tryx abort-downgrade-v10`. Released-v10 cleanup не удаляет canonical
  v11, но это сохранение само по себе не является разрешением на downgrade.
  FilePull связывает фактический H264 размер с suffix. Replace повторно
  сравнивает свежие `screenMode`, `playMode`, порядок media и exact
  Full/Left/Right slot до FFmpeg и journal. Шесть final cancellation gates
  стоят непосредственно перед каждым subprocess start, а production E2E
  проверяет реальные Full/Split H264, `yuv420p`, 30 FPS и exact geometry;
- `C5 saved layouts`: runtime-owned store v2 с чтением v1 сохраняет полный canonical
  Full/Split draft только для exact устройства, использует snapshot CAS,
  runtime-generated UUID/revision, owner-only atomic persistence и fail-closed
  read-only состояние после unsafe, malformed, future-version или
  post-commit unknown результата. Save, Load и Delete не создают foreground
  device operation; загруженный draft сохраняет UUID/revision provenance после
  редактирования и применяется через `QueueSavedLayoutApplyV2`; старый
  `QueueSavedLayoutApplyV1` остаётся Auto-only и не перезаписывает Custom UUID.
  DeviceManager повторно разрешает current draft через authoritative catalog,
  а worker выполняет один fresh FileList и exact tuple proof до первого
  mutating OUT. Quick принимает snapshot только после exact capability,
  owner/device/product fencing, не делает reconnect Load/Apply и сохраняет
  общий C2 dirty/conflict/unresolved lifecycle. Отдельные QML проверки
  фиксируют explicit overwrite/delete/dirty-replace dialogs, порядок полей,
  narrow reflow, keyboard focus, accessibility и русский перевод;
- `C6 safe cache management`: additive API-8 capability и Manager2 Queue
  создают serialized foreground operation без USB mutation. `DeviceManager`
  атомарно проверяет exact recovery/operation/lease blockers, удерживает
  exclusive latch от immutable bounded plan до terminal publication и
  публикует точные success, partial и pre/post-mutation cancellation counts.
  Catalog и artifact stores удаляют только повторно проверенные allowlisted
  leaves, сохраняют indexed thumbnails и весь retry/recovery state и fsync-ят
  parent после каждого unlink. RuntimeClient привязывает Queue/Get/Cancel к
  exact UUID, kind и captured owner без replay; Quick interlock защищает
  current/pending preview и editor lifecycle. Settings повторно проверяет
  eligibility перед confirmation, различает blocked/partial/unresolved,
  сохраняет keyboard focus и accessibility contract на английском и русском;
- `B2 presentation preferences`: frozen `(utss)` tuple и additive API 8
  manifest, отдельный fail-closed runtime store, CAS с persistence-before-
  publish, единый conversion-before-rounding formatter, одинаковая пара
  °C/°F и 12/24H для initial/periodic PASE paths и byte-identical default
  wire fixture;
- `B3 support report`: additive API 8 capability и snapshot method работают
  только через unique-owner/epoch fencing; runtime сериализует cached state и
  bounded typed lifecycle ring по точному allowlist без USB query, journal,
  environment, identifiers и free-form text; GUI валидирует размер, schema и
  тип каждого поля, а host-only статусы различают старый runtime
  `unsupported` и недоступный runtime `unavailable`; folder-only экспорт
  создаёт новый owner-only файл через покомпонентный descriptor walk с
  `O_NOFOLLOW`, pinned directory descriptor и `RENAME_NOREPLACE`, не
  перезаписывая symlink, FIFO, hardlink или raced target и не следуя
  заменённому symlink-предку;
- `B10 device specifications`: обычный PASE bootstrap сохраняет только
  presence-aware проекцию уже полученного SysConfig response без второго USB
  query; cache публикуется перед session-ready только после успешной activation
  и привязан к exact path, product, identity и physical generation. Additive
  API 8 tuple `(usttssuusb)` читается только из cache, а RuntimeClient повторно
  проверяет schema, status, безопасную product string, geometry, screen type,
  exact owner, revision и generation. Ошибки остаются fail closed, legacy и
  Turris не получают ложный `Ready`, delayed replies старой revision,
  cancellation или owner не могут восстановить устаревшие значения;
- `B9 NVIDIA telemetry`: необязательный provider запускает только доверенный
  `/usr/bin/nvidia-smi` через изолированный bounded subprocess, строго разбирает
  и ограничивает output, применяет TTL/backoff/breaker и синхронно инвалидирует
  числовые данные в состоянии `Off`; единый GPU inventory сохраняет stable
  PCI identity, детерминированный ordering и primary pin, обогащает его UUID и
  разрешает rebind только по exact UUID без fallback на другую карту; runtime
  использует `Discovery` для overlay-capable session до выбора GPU metric и для
  badge-only конфигурации, `Active` только для committed GPU metric и `Off` вне
  demand, публикуя `--` вместо stale значения; локальный Dashboard опрашивает
  GPU только на видимой Home page и честно показывает power и отдельную
  доступность VRAM;
- `B12 headless CLI packaging`: пакет сохраняет имя
  `tryx-panorama-manager`, дополнительно устанавливает отдельные
  `/usr/bin/tryx` и `tryx(1)` без alias; source archive явно содержит CLI
  project, runner, process tests и man page, а Fedora, Debian и Arch выполняют
  version, headless help и `ldd` smoke как в package-build, так и в clean
  runtime jobs. Package verifier требует executable и ровно одну man page,
  проверяет exact version и запрещает Qt Widgets, QML и Quick в dependency
  closure CLI; отдельный process-level suite фиксирует headless argv/JSON,
  API-8 legacy/error/timeout paths, B3 redaction, capability context, exact
  owner и отсутствие report при stale owner. До Qt session-bus connection
  принимается только один существующий local `unix:path` socket текущего UID с
  безопасной цепочкой каталогов; unset, autolaunch, TCP, abstract/system bus,
  fallback list и небезопасный socket отклоняются без подключения. Output path
  также отклоняет terminal/bidi controls, а B3 writer проверяет guard до publish
  и безопасный rollback после publish. Inspection-команды остаются read-only;
  явно названные `prepare-downgrade-v10` и `abort-downgrade-v10` меняют только
  owner-only local downgrade state, не устанавливают пакет и не выполняют
  device mutation;
- `C15.1 metric selector`: полный canonical catalog запрашивается отдельно от
  live availability и привязан к exact unique owner, service epoch и handshake
  attempt; только exact `UnknownMethod` включает bounded legacy fallback, а
  malformed reply и остальные D-Bus ошибки остаются fail closed. Full, Left и
  Right используют общий grouped/searchable selector с явным `N/3`, selected-
  unavailable состоянием и подтверждаемой заменой точной позиции без silent
  eviction. Лимит три независимо проверяется QML, RuntimeClient,
  DeviceManager, persistence v2 и PASE formatter; formatter отклоняет 4+,
  duplicate и неканонические tokens до USB payload, а frozen API 8 tuple,
  protobuf и PASE group/label IDs не меняются. Keyboard, screen-reader и narrow
  layout проверки выполняются на английском и со свежим русским каталогом;
- `runtime-capability-handshake`: API 8 probe и capability query адресуются
  одному unique owner; настоящий legacy `UnknownMethod` сохраняет baseline и
  даёт готовый пустой набор, тогда как `UnknownInterface`, access denied и
  invalid signature остаются fail closed; runtime/device tokens фильтруются,
  device reply привязывается к exact identity, connection revision и
  ненулевой physical generation; owner/revision invalidation отбрасывает
  устаревшие ответы, generation-only cancellation немедленно закрывает
  presentation gates, а повторная registration во время API probe запускает
  новую fenced handshake-попытку; B2 getter, setter и optional signal также
  привязаны к unique owner и owner-local revision, не делают optimistic
  update и после неизвестного mutation outcome выполняют только read-only
  reconciliation;
- `quick-client`: совместимость Manager1, защита от stale revisions и replies,
  точное сохранение API 8 media origin metadata roles для user media и device
  presets,
  exact modern display submission с terminal/readback correlation, retained
  unknown outcome без replay, legacy preflight classification и exact-serial
  fencing одной Manager1 mutation,
  exact claim identity, release lease после Save as new, invalidation
  device-media workflow, безопасное закрытие окна через tray и owner-managed
  XDG GUI autostart без вызовов `systemctl`, с pinned directory descriptor,
  conditional `renameat2` mutation, single-racer сохранением raced/foreign
  leaf, exact-current-executable state, unsafe-path rejection и корректным
  system-wide `Hidden=true` override;
- `C4 Quick/QML`: Full остаётся default и использует старые API 8 методы при
  отсутствии нового runtime token; Split никогда не делает fallback и
  доступен только после exact runtime/device handshake. Смена target или
  потеря capability сбрасывает transform domain в neutral Fit, recovered copy
  получает target только из exact активного layout. UI показывает Full/Split
  до sizing controls, фактические 2240x1080 или 1120x1080 canvas, видимую
  причину disabled состояния, keyboard/screen-reader contract, narrow reflow
  и полный русский перевод;
- `QML/guard wiring`: отсутствие прямых brightness/orientation dispatch paths
  в `PanoramaPage`, route/close continuation через production `Main.qml`,
  защищённый повторный close и Tray Quit через тот же first-intent guard, а
  также ожидание полного завершения Material popup transition перед следующим
  one-shot intent. Дополнительно проверяются честные source/size fallbacks,
  legacy boundary и keyboard-accessible
  metadata карточек media library;
- `B2 QML`: Dashboard делегирует C/F formatting общему runtime helper,
  Settings показывает подтверждённые runtime-owned controls с accessibility
  и disabled fallback, а Panorama использует канонический token
  `Date&Time`;
- `B3 QML`: Settings показывает ready, collecting, host-only, success и error
  состояния с keyboard/accessibility contract; отдельный folder-only picker
  не принимает имя файла, отключает повторный запуск во время export и
  возвращает focus после Save или Escape;
- `B10 QML`: Settings честно различает отсутствие runtime capability,
  непроверенный runtime response, disconnect, unsupported, unavailable и ready,
  показывает четыре plain-text значения, не меняет media profile при geometry
  mismatch и удерживает 128-символьное product name внутри narrow content;
- `runtime-bootstrap`: fail-closed выбор владельца D-Bus, несовместимый runtime,
  запрет автоматического restart при активной операции и строгий
  single-instance launch intent с bounded framed ACK, pinned owner-only runtime
  directory, native descriptor listener и atomic listening-socket replacement
  для stale recovery, включая обратный exchange при позднем появлении живого
  владельца, где ручной запуск показывает окно, а повторный
  `--autostart` не раскрывает скрытый экземпляр;
- `linux-tray`: lifecycle watcher, явный Quit, безопасная работа без watcher и
  production `StartupVisibilityController` с bounded ожиданием StatusNotifier
  host, скрытым login-start только при Hide to tray, ручным восстановлением
  окна и fail-visible поведением при timeout или потере host;
- `replace-journal`: fail-closed разбор, безопасная файловая граница,
  монотонные переходы crash-recovery journal, internal v2 с exact PANORAMA/PASE
  product identity, reject duplicate JSON keys, одностороннее повышение legacy
  v1 только как `0x1021` при реальном progress transition и общий
  Replace/Delete restart gate по operation, product, serial, generation и media
  identity.

Baseline выполняется после полных suites внутри `package-check`. Он проверяет
наличие критических тестов, но не заменяет их запуск и не разрешает ослаблять
остальные проверки.

Перед baseline `check_translation_catalog.sh` строго собирает tracked русский
каталог, восстанавливает из `.qm` только реально поставляемые переводы и через
`lupdate` сверяет их с полным текущим набором runtime и Quick. Актуальная строка
не может отсутствовать, оставаться `unfinished` или быть помечена `vanished`;
отдельный отрицательный сценарий проверяет эту границу. Динамические сообщения
`DeviceManager` оформляются только как извлекаемые literal-вызовы, чтобы новые
строки не обходили стандартный Qt catalog workflow.

## Только на реальном железе

Эти проверки намеренно не входят в автоматический CI и выполняются отдельно
для точного USB product ID, модели устройства или host GPU:

- три последовательных physical remove/add цикла без роста `NRestarts`, со
  сменой generation и без replay последней mutation;
- 24-часовая qualification активной PASE-сессии с keepalive и метриками;
- upload, Apply, Delete и display configuration с точным readback каталога и
  конфигурации на PANORAMA/PASE;
- power-cycle проверка сохранения brightness и backlight state;
- Turris 620 upload изображения, GIF и видео с немедленной активацией после
  подтверждённого финального ACK;
- real NVIDIA GeForce smoke фактических temperature, usage, graphics frequency,
  power и VRAM; mixed/dual ordering дополнительно проверяется, если такой host
  доступен;
- firmware validation и read-only recovery на поддерживаемой модели;
- обычный bootstrap `391a:1021` и отдельный community/maintainer smoke
  `391a:1011` подтверждают реальные SysConfig field numbers, четыре безопасных
  значения и совпадение geometry без дополнительного SysConfig query;
- live firmware flashing только по отдельному явному разрешению, с заранее
  проверенными package identity, rollback и recovery boundary.
- C5 Full и Split saved-layout Apply на физическом `391a:1021`, а также
  отдельный community smoke на `391a:1011`, подтверждают визуальный результат,
  fresh FileList boundary и отсутствие мутации при stale/missing media.

Успех офлайн-тестов не повышает модель до статуса `Tested on real hardware`.
