# Шпаргалка команд

Все команды Windows — в обычной командной строке (Пуск → `cmd`), если не сказано «от администратора».
Команды Linux — в WSL, в каталоге проекта `~/projects/esp`.

## Сеть ПК (Windows)

| Что | Команда |
|---|---|
| Адреса всех подключений | `ipconfig` |
| Только адреса разъёма Ethernet (PowerShell) | `Get-NetIPAddress -InterfaceAlias Ethernet -AddressFamily IPv4` |
| Имена подключений | `netsh interface ipv4 show interfaces` |
| Задать один адрес (от администратора) | `netsh interface ipv4 set address name="Ethernet" source=static address=192.168.144.10 mask=255.255.255.0` |
| Добавить второй адрес к уже заданному (от администратора) | `netsh interface ipv4 add address name="Ethernet" address=192.168.1.10 mask=255.255.255.0` |
| Удалить адрес (от администратора) | `netsh interface ipv4 delete address name="Ethernet" address=192.168.1.10` |
| Вернуть автоматические настройки (от администратора) | `netsh interface ipv4 set address name="Ethernet" source=dhcp` |

На ПК разработчика у разъёма `Ethernet` два адреса: **192.168.144.10** (рабочая сеть стенда) и **192.168.1.10**
(сеть модуля с заводскими настройками 192.168.1.254, нужна только при сбросе модуля). В «Параметрах» Windows виден
только один из них, `ipconfig` показывает оба. Интернет идёт по Wi-Fi и этими адресами не затрагивается.

## Стенд по сети (Windows)

| Что | Команда |
|---|---|
| Отвечает ли стенд | `ping 192.168.144.36` |
| Страница стенда | браузер: `http://192.168.144.36/` |
| Настройки модуля Ethernet | браузер: `http://192.168.144.36:8000/`, пароль пустой |
| Состояние стенда (JSON) | `curl http://192.168.144.36/api/state` |
| Найти модуль в сети 192.168.144.x | `for /L %i in (1,1,254) do @start /b ping -n 1 -w 300 192.168.144.%i >nul` затем `arp -a` (MAC модуля `28-80-e4-…`) |
| Найти модуль программой | `C:\Users\user\Downloads\VirCom\VirCom_en.exe` → поиск устройств |

Команды API (для проверок, `curl` есть в Windows 10/11):

| Что | Команда |
|---|---|
| Нуль BMP585 / BMP581 | `curl -X POST -d "ch=0" http://192.168.144.36/api/zero` (`ch=1` — BMP581) |
| Сбросить нуль | `curl -X POST -d "ch=0" http://192.168.144.36/api/zero/reset` |
| Журнал: включить, интервал 5 с | `curl -X POST -d "enabled=1&interval=5" http://192.168.144.36/api/log/config` |
| Журнал: последние записи | `curl "http://192.168.144.36/api/log?limit=20"` |
| Журнал: очистить | `curl -X POST http://192.168.144.36/api/log/clear` |

Одновременно со стендом должна работать одна страница или одна команда: модуль рассылает ответы всем соединениям.

## Плата ESP32 по USB (Windows → WSL)

| Что | Команда |
|---|---|
| Список USB-устройств | `usbipd list` (плата: `USB-SERIAL CH340`, BUSID `4-9`) |
| Разрешить проброс (один раз, от администратора) | `usbipd bind --busid 4-9` |
| Пробросить в WSL (после каждого переподключения кабеля) | `usbipd attach --wsl --busid 4-9` |
| Права на порт в WSL (до перезахода в WSL) | `wsl -d DDEV -u root -- chmod a+rw /dev/ttyUSB0` |

После `attach` плата исчезает из Windows как COM7 и появляется в WSL как `/dev/ttyUSB0`.

## Сборка и прошивка (WSL)

| Что | Команда |
|---|---|
| Окружение ESP-IDF (в каждом новом терминале) | `. ~/esp/esp-idf/export.sh` |
| Собрать | `idf.py build` |
| Прошить | `idf.py -p /dev/ttyUSB0 -b 460800 flash` |
| Лог платы (выход: `Ctrl+]`) | `idf.py -p /dev/ttyUSB0 monitor` |
| Тестовая прошивка первого запуска | `cd tools/bringup && idf.py build && idf.py -p /dev/ttyUSB0 flash` |
| Тесты ядра на ПК | `make -C host test` |
| Эмулятор стенда | `make -C host emu && ./host/build/emu --port 8080` → `http://127.0.0.1:8080/` |

**Перед каждой прошивкой** сверить `docs/connections.md`: какие выводы прошивка делает выходами и что к ним подключено.

## Из WSL к стенду

WSL сам до 192.168.144.36 не достаёт (берёт адрес 192.168.1.10). Запросы делать через Windows:
`/mnt/c/Windows/System32/curl.exe http://192.168.144.36/api/state`.
