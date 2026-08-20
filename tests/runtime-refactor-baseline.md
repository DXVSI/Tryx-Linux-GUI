# Baseline декомпозиции runtime

Этот перечень фиксирует минимальную страховочную сетку перед переносом кода из
`DeviceManager`. Полные test suites остаются главным критерием корректности.
Скрипт `check_runtime_refactor_baseline.sh` дополнительно не позволяет незаметно
удалить перечисленные ниже защитные сценарии из собранных Qt Test binaries.

## Автоматические проверки

- `printer-protocol`: стабильные Manager1/Manager2 D-Bus контракты и API 8,
  точные product capabilities, generation fencing, отсутствие автоматического
  replay после неизвестного результата, firmware-exclusive ownership и
  fail-closed восстановление transport;
- `quick-client`: совместимость Manager1, защита от stale revisions и replies,
  ownership device-media workflow и безопасное закрытие окна через tray;
- `runtime-bootstrap`: fail-closed выбор владельца D-Bus, несовместимый runtime
  и запрет автоматического restart при активной операции;
- `linux-tray`: lifecycle watcher, явный Quit и безопасная работа без watcher;
- `replace-journal`: fail-closed разбор, безопасная файловая граница и
  монотонные переходы crash-recovery journal.

Baseline выполняется после полных suites внутри `package-check`. Он проверяет
наличие критических тестов, но не заменяет их запуск и не разрешает ослаблять
остальные проверки.

## Только на реальном железе

Эти проверки намеренно не входят в автоматический CI и выполняются отдельно
для точного USB product ID и модели:

- три последовательных physical remove/add цикла без роста `NRestarts`, со
  сменой generation и без replay последней mutation;
- 24-часовая qualification активной PASE-сессии с keepalive и метриками;
- upload, Apply, Delete и display configuration с точным readback каталога и
  конфигурации на PANORAMA/PASE;
- power-cycle проверка сохранения brightness и backlight state;
- Turris 620 upload изображения, GIF и видео с немедленной активацией после
  подтверждённого финального ACK;
- firmware validation и read-only recovery на поддерживаемой модели;
- live firmware flashing только по отдельному явному разрешению, с заранее
  проверенными package identity, rollback и recovery boundary.

Успех офлайн-тестов не повышает модель до статуса `Tested on real hardware`.
