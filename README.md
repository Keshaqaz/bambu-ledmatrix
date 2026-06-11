# LED Matrix

Умная LED-матрица 16×16 на ESP32-C3: часы + уличная температура и live-статус
3D-принтера Bambu Lab полосой прогресса внизу экрана.

Система из двух частей:

- **Прошивка ESP32** (`esp32_matrix_controller22/`) — рендер матрицы, веб-интерфейс
  настроек, Wi-Fi, погода, OTA-обновления с авто-откатом при неудачной прошивке.
  Матрица живёт на `192.168.1.65`.
- **Bambu relay** (`esp32_matrix_controller22/tools/bambu_relay/`) — Python-сервис на
  OpenWrt-роутере (`192.168.1.1:8787`): подписывается на MQTT принтера/облака и отдаёт
  статус печати прошивке по HTTP.

## Документация

| Что нужно | Куда смотреть |
|---|---|
| Собрать/прошить плату, OTA, веб-превью | [`esp32_matrix_controller22/README.md`](esp32_matrix_controller22/README.md) |
| Запустить/задеплоить relay, его API и конфиг | [`esp32_matrix_controller22/tools/bambu_relay/README.md`](esp32_matrix_controller22/tools/bambu_relay/README.md) |
| Рабочий контекст: сеть, тулчейн, принятые решения, грабли | [`CLAUDE.md`](CLAUDE.md) |
