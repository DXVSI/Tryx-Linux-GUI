# TODO - актуальный roadmap

Подробная спецификация, зависимости, acceptance criteria и команды проверки:
[2026-08-09-runtime-decomposition-and-feature-roadmap.md](2026-08-09-runtime-decomposition-and-feature-roadmap.md).

Этот файл оставляет только верхнеуровневую очередь. При конфликте источником
истины является полный feature proposal.

## M0: Architecture safety cleanup

- [x] A1. Зафиксировать characterization baseline.
- [x] A2. Удалить неиспользуемый remote-mode из `DeviceManager`.
- [ ] A3. Вынести `PrinterMediaPreparer`.
- [ ] A4. Вынести чистые media, path и serialization helpers.
- [ ] A5. Выделить catalog, metrics, artifact и recovery stores.

## M1A: API-8-safe Linux usability

- [ ] B1. Реальная модель, product ID, live firmware и app version из уже
  доступного API 8 snapshot.
- [ ] B4. Close behavior.
- [ ] B7. Только существующие legal/about links.
- [ ] B8. Отдельный GUI autostart без смешивания с background service.
- [ ] B11. Дополнительные локализации с native-speaker review и English
  fallback.
- [ ] C1. Независимое Left/Right styling.
- [ ] C2. Dirty-state guard.
- [ ] C3. API-8-safe media source, size и origin badges.
- [ ] D6. Brightness power-cycle test на `1021` и `1011`.

## M1B: Versioned contracts и telemetry

- [ ] B0. Добавить capability handshake при неизменном API 8. `Manager1` и
  опубликованный `Manager2` остаются замороженными; новые поля получают новые
  versioned methods/types, а настоящий breaking change требует `Manager3`.
- [ ] B2. Runtime-owned °C/°F и 12/24H.
- [ ] B3. Redacted support bundle и bounded log export.
- [ ] B9. Полный NVIDIA temperature/usage/frequency/power/VRAM backend после
  выбора packaged backend.
- [ ] B10. Device Specifications: для `1011/1021` декодировать существующий
  bootstrap response без повторного USB query и опубликовать поля через
  versioned contract.
- [ ] C12. Dimensions, duration и FPS без изменения старого API 8 tuple.

## M2: Media workflow

- [ ] C4. Split-aware crop.
- [ ] C5. Saved layouts.
- [ ] C6. Safe cache management.

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

- [ ] A6. `PrinterOperationCoordinator`.
- [ ] A7. `PrinterSessionController`.
- [ ] A8. Разделить legacy и printer-class worker policy при одном I/O owner.
- [ ] A9. Разделить transport, framing, discovery и model clients в
  `PrinterProtocol`.

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

Полный NVIDIA telemetry, CPU Voltage и legacy serial FileTransport fallback
остаются отдельными задачами. Запуск GUI на NVIDIA не доказывает доступность
NVIDIA temperature, usage или frequency.
