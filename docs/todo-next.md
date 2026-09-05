# TODO - актуальный roadmap

Подробная спецификация, зависимости, acceptance criteria и команды проверки:
[2026-08-09-runtime-decomposition-and-feature-roadmap.md](2026-08-09-runtime-decomposition-and-feature-roadmap.md).

Этот файл оставляет только верхнеуровневую очередь. При конфликте источником
истины является полный feature proposal.

## M0: Architecture safety cleanup

- [x] A1. Зафиксировать characterization baseline.
- [x] A2. Удалить неиспользуемый remote-mode из `DeviceManager`.
- [x] A3. Вынести `PrinterMediaPreparer`.
- [x] A4. Вынести чистые media, path и serialization helpers.
- [x] A5. Выделить catalog, metrics, artifact и recovery stores, включая
  product-bound `TryxReplaceJournal` v2 с legacy v1 только для `0x1021`.

## M1A: API-8-safe Linux usability

- [x] B1. Реальная модель, product ID, live firmware и app version из уже
  доступного API 8 snapshot.
- [x] B4. Close behavior.
- [x] B7. Только существующие legal/about links.
- [x] B8. Отдельный GUI autostart без смешивания с background service.
- [ ] B11. Дополнительные локализации с native-speaker review и English
  fallback.
- [x] C1. Независимое Left/Right styling. Software acceptance завершён;
  hardware visual smoke остаётся отдельной проверкой с явным разрешением.
- [x] C2. Dirty-state guard, включая production Material QML gate и
  детерминированное ожидание popup lifecycle.
- [x] C3. API-8-safe media source, size и origin badges. Software acceptance
  завершён; hardware/USB smoke для presentation-only среза не требовался.
- [ ] D6. Brightness power-cycle test на `1021` и `1011`.

## M1B: Versioned contracts и telemetry

- [x] B0. Добавить capability handshake при неизменном API 8. `Manager1` и
  опубликованный `Manager2` остаются замороженными; новые поля получают новые
  versioned methods/types, а настоящий breaking change требует `Manager3`.
  Выполнено 28 августа 2026 года; полный `package-check` и независимое review
  прошли.
- [x] B2. Runtime-owned °C/°F и 12/24H. Выполнено 28 августа 2026 года;
  software `package-check` и три независимых read-only review прошли.
- [x] B3. Redacted support report и bounded typed lifecycle ring. Выполнено
  30 августа 2026 года; полный software `package-check` и три независимых
  read-only review прошли, raw journal/log export не добавлялся.
- [x] B9.1 software. Реализован 31 августа 2026 года optional bounded
  `nvidia-smi` provider: generation-scoped PCI `entityKey` с UUID enrichment,
  единый badge/value ordering, temperature/usage/frequency/power/VRAM,
  timeout/TTL/breaker и unavailable вместо нуля. API остался равным `8`, hard
  NVIDIA package dependency не добавлена, полный fresh `package-check` прошёл.
- [ ] B9 NVIDIA hardware acceptance. На реальной NVIDIA GeForce подтвердить
  actual temperature, usage, graphics frequency, power и VRAM; mixed/dual GPU
  ordering дополнительно проверить, если такой host доступен.
- [x] B10. Device Specifications software implementation завершена 31 августа
  2026 года. Для `1011/1021` декодируется только существующий bootstrap response
  без повторного USB query; добавлены generation-fenced cache, additive
  Manager2 getter и честные Settings states. API остаётся `8`; fresh
  `package-check` и независимые read-only review прошли.
- [ ] B10. Device Specifications hardware acceptance. На реальном `1021`
  сохранить обычный bootstrap capture и сверить четыре безопасных значения без
  дополнительного SysConfig query; для `1011` выполнить отдельный
  community/maintainer smoke. До этого новая geometry/model support не
  заявляется.
- [x] C12. Extended media metadata contract реализован 31 августа 2026 года:
  artifact-scoped dimensions, duration и FPS публикуются additive Manager2
  getter после existing explicit FilePull; exact managed-origin hash proof,
  owner/lease и stale-response fences сохранены. API остался `8`, новый USB
  operation и device write не добавлены; fresh `package-check`, обязательный
  английский/русский baseline и три независимых read-only review прошли.
- [x] C15.1. Реализован 31 августа 2026 года: owner-fenced полный catalog через
  `GetMetricsCapabilities()`, grouped/searchable selector с явным Full `3/3`,
  независимым Split `3+3`, selected-unavailable state и positional replacement
  без silent eviction. Полный fresh `package-check`, независимый read-only
  review, русский/английский QML baseline и frozen API/wire/persistence
  regressions прошли. 4+ метрик, pages и rotation остаются после D1 и hardware
  captures/readback.
- [x] B12. Local headless CLI: реализован 31 августа 2026 года. Новый one-shot
  binary `tryx` использует только существующий exact-owner user-session D-Bus
  runtime для read-only status/capabilities/redacted operations и B3 export;
  позднее C4 добавляет только явно названные local-state prepare/abort команды.
  CLI не запускает runtime, не добавляет network listener/remote D-Bus и не
  обещает GUI forwarding. Process-level CLI suite, fresh `package-check`, staged install
  verifier и независимые read-only reviews прошли. Remote terminal qualification
  не входит в B12 и перенесена в backlog. Clean-HEAD source archive gate пройден
  после создания локального Git snapshot: archive script принял чистое дерево,
  собрал committed `HEAD` и подтвердил обязательные CLI members.

## M2: Media workflow

- [x] C4. Split-aware crop. Реализован 1 сентября 2026 года, 02:02:36 UTC+7:
  Full и Split используют отдельные versioned preparation profiles, честные
  `2240x1080` и `1120x1080` canvas и раздельные conversion identities;
  Replace fail-closed проверяет exact live layout, а подготовленный downgrade
  v11 -> released v10 атомарно останавливает runtime mutations. Fresh
  `package-check` и два независимых read-only review прошли. Визуальный Apply
  на физическом Split-дисплее остаётся отдельной hardware acceptance в D1.
- [x] C5. Saved layouts. Реализован 1 сентября 2026 года, 12:04:10 UTC+7;
  формальный проход занял 1 час 51 минуту 57 секунд. Добавлены runtime-owned
  versioned device-scoped store, additive Manager2 Get/Put/Delete/Queue,
  strict raw identity/owner fencing, fresh FileList proof в одной serialized
  foreground Apply operation и Quick/QML UI с explicit confirmations.
  Fresh `package-check` завершён: 1538 passed, 0 failed, 4 ожидаемых skip;
  три независимых read-only review дали `APPROVE`. Физический Full/Split Apply
  на `1021` и community `1011` остаётся отдельной hardware acceptance.
- [x] C6. Safe cache management. Реализован 1 сентября 2026 года,
  17:25:38 UTC+7; формальный проход занял 5 часов 2 минуты 3 секунды.
  `DeviceManager` сериализует additive API-8 cleanup operation через exact
  blockers и exclusive latch; store-методы удаляют только unindexed thumbnail
  orphans и expired/revoked idle artifacts, а Quick interlock защищает active
  previews. Indexed thumbnails, весь retry/recovery state, journals, leases и
  active operations остаются protected. Fresh build, полный `package-check`,
  translation/baseline gates и три раздельных read-only review-прохода прошли.
  Открытых задач внутри C6 нет; C7 остаётся следующим, но не начат.

## M3: Network и portals

- [ ] B5. GitHub release notification без self-update.
- [ ] B6. Только research официального firmware source, signatures,
  compatibility и rollback, без remote update до отдельного proposal.
- [ ] C7. Clean public-API GIPHY integration.
- [ ] C8. Wayland screen recorder через XDG Portal и PipeWire.
- [ ] C9. Global shortcuts через portal.

## M4: Hardware tracks

- [ ] D1. Hardware evidence kit для community issues.
- [ ] D2. TURRIS catalog, brightness, metrics, presets, fonts и firmware по
  отдельным captures.
- [ ] D3. PANORAMA SE V2, PANORAMA V2 и WB V2.
- [ ] D4. HOLO, STAGE, ROTA и PANORAMA WB.
- [ ] D5. Получить identities/testers для unknown variants из D5:
  PANORAMA 240/280/360, PANORAMA ARGB 240/280 и PANORAMA SE ARGB 240;
  отдельно найти maintainer hardware для известного PANORAMA ARGB 360
  `391a:1011`.
- [ ] C10. Device-side fonts только после model-specific capture и exact
  readback.
- [ ] C13. Frame-rate control только для подтверждённого product profile.
- [ ] C14. Linux display sleep policy отдельно для GUI close, runtime stop,
  suspend, shutdown и resume.
- [ ] Fan, pump и ARGB controls только как отдельные hardware-backed
  proposals.

## M5: Deep runtime decomposition

- [x] A6. `PrinterOperationCoordinator`: реализован 5 сентября 2026 года;
  coordinator единолично владеет operation lifecycle, а `DeviceManager`
  остался public/session façade. Чистые сборки, 875 protocol tests, полный
  `package-check`, translation и structural baseline прошли; hardware smoke
  для software-only refactor не выполнялся.
- [x] A7. `PrinterSessionController`: реализован 5 сентября 2026 года;
  session state, recovery/firmware gates и projections вынесены из manager.
  Сохранены generation/event contract и единственный worker I/O owner;
  закрыты reentrant dispatch/teardown и firmware release-order сценарии.
  Чистые сборки, 893 protocol tests, итоговый `package-check`, translation и
  structural baseline прошли. В QML-тесте стабилизирована готовность к клику
  через bounded render check; production UI не менялся. Hardware smoke
  не выполнялся.
- [ ] A8. Разделить legacy и printer-class worker policy при одном I/O owner.
- [ ] A9. Разделить transport, framing, discovery и model clients в
  `PrinterProtocol`.

## Backlog

- [ ] Квалификация `tryx` для SSH и VS Code Remote SSH. Это отдельный будущий
  support/test track без встроенного SSH server, remote D-Bus или GUI
  forwarding; текущий contract ограничен локальным terminal.

## Уже подтверждено и не должно возвращаться в TODO

- [x] AMD telemetry и vendor-neutral NVIDIA/Intel model/badge resolution.
- [x] Full и Split display modes, Mirror и Waterfall.
- [x] До трёх метрик и CPU/GPU badges на каждую область.
- [x] Per-device overlay persistence version 2.
- [x] Device media catalog, FilePull, Export, Edit, Save as new, Replace и
  Delete для catalog-capable профилей.
- [x] Turris `1280x720` image/GIF/video upload и immediate activation.
- [x] Local gated firmware workflows для уже разрешённых моделей.

`Memory Frequency` остаётся unavailable по текущему portable Linux contract и
не должна показываться как нулевое измерение. Новые hardware counters требуют
отдельного источника и проверки, а не только нового label.

Software B9.1 реализован. Отдельный real NVIDIA hardware acceptance, CPU
Voltage и legacy serial FileTransport fallback остаются задачами. Запуск GUI на
NVIDIA сам по себе не доказывает доступность temperature, usage, frequency,
power или VRAM.
