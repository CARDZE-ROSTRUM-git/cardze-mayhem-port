# Mayhem Desktop — состояние порта (handoff)

Документ для продолжения работы в новой сессии. Обновлять на каждой границе этапа.
Дата: 2026-11-08. Автор предыдущей сессии: ассистент (DeepSeek V4.1 Flash Free).

Проект ведёт **[@CARDPUTER_ZERO](https://t.me/cardputer_zero)** (Telegram).

---

## 1. Цель проекта

Портировать прошивку PortaPack Mayhem (HackRF One + PortaPack H2) в десктопную
программу Linux:

* Реальный код UI Mayhem (меню, виджеты, шрифты, иконки, раскладка 240x320)
  используется БЕЗ ИЗМЕНЕНИЙ. Заменяются только нижние слои.
* Окно SDL2 240x320, масштаб x1/x2/x3, рамка в виде PortaPack H2: дисплей +
  экранные кнопки (энкодер + крестовина Up/Left/Select/Right/Down).
* Клавиатура работает как альтернатива экранным кнопкам. Тачскрин = мышь.
* ChibiOS заменяется на std::thread/mutex/condition_variable. Ядра LPC43xx
  M4/M0 -> потоки. SD-карта -> одна папка (по умолчанию `~/mayhem-sd/`,
  внутренняя структура как на карте).
* Портируются ВСЕ приложения, включая ниже 70 МГц. Приложения спрашивают у
  бэкенда диапазон/возможности, ничего не хардкодят.
* После нажатия «Старт»: никаких всплывающих окон/подтверждений. Частоты вне
  диапазона молча клампятся. Единственный предупреждающий текст — на лаунчере.
* Лаунчер содержит ТОЛЬКО: выпадающий список SDR + кнопка обновления, кнопка
  «Старт», один статический текст-предупреждение. Никаких тем/оформления.
* Порядок этапов:
  1. Скелет сборки + окно SDL + клавиатура/меню.
  2. Лаунчер.
  3. Папка вместо SD.
  4. SdrDevice + RTL-SDR + файловый TX-стаб + PlutoSDR + спектр/водопад.
  5. RX-приложения по одному.
  6. TX-приложения.
  Переход к следующему этапу — ТОЛЬКО после явной команды пользователя.
* Рабочий процесс: сначала показать дерево файлов проекта и список
  аппаратных/ChibiOS-зависимостей, ждать «ок». Код — целыми файлами, без
  фрагментов «...». Комментарии в коде — на английском. Ответы — на русском.
  GPL-заголовки сохранять. Никогда не утверждать, что проверено на железе.

Текущий этап: **ЭТАП 1** (скелет + окно SDL + клавиатура/меню). Этапы 2-6 не
начинать без команды.

---

## 2. Расположение и версия upstream

* Репозиторий: `<repo>`
* Upstream: `https://github.com/portapack-mayhem/mayhem-firmware.git`
* Путь: `<repo>/third_party/mayhem-firmware`
* Коммит: `77798cb09f227de5c58dea48899ddb7448b44a07` (7 ноя 2026), shallow clone.
* **Upstream никогда не патчится.** Все изменения — в `platform/` и `cmake/`.
  Адаптация — через forced-include (`platform/include/host_forced.hpp`) и
  shadowing include-путей (platform/include идёт первым).
* GPL: производная работа, заголовки upstream сохраняются. Новые файлы платформы
  помечены как часть GPL-производной.

### Дерево проекта сейчас

```
<repo>/
├── CMakeLists.txt                    # host-сборка (готов, см. §4)
├── cmake/
│   └── UpstreamSources.cmake         # 136 upstream .cpp (сгенерирован)
├── platform/
│   ├── include/
│   │   ├── ch.h                      # шим ChibiOS: потоки, события, мьютексы
│   │   ├── hal.h                     # типы HAL, SPI/I2C/SDC, редирект LPC_*
│   │   ├── nvic.h                    # NVIC/SCB/IRQn, __disable_irq и т.п.
│   │   ├── lpc43xx_m0.h              # CMSIS-заглушка M0
│   │   ├── pal_lld.h                 # пустой PAL
│   │   └── host_forced.hpp           # замена lpc43xx_cpp.hpp + std::max/min
│   └── src/
│       ├── chibios.cpp               # реализация шима (готово)
│       └── host_regs.cpp             # dummy-регистры + mmap backup/shared RAM
└── third_party/mayhem-firmware/      # upstream, не трогать
```

### Чего ещё НЕТ (CMake их уже требует — сборка не слинкуется)

```
platform/src/portapack.cpp
platform/src/lcd_sdl.cpp
platform/src/input_controls.cpp
platform/src/frame_sync.cpp
platform/src/rtc_time.cpp
platform/src/hardware_stubs.cpp
platform/src/ff_stub.cpp
platform/src/main.cpp
```
Плюс `platform/src/host_regs.cpp` НЕ добавлен в `PLATFORM_SOURCES` в CMake
(добавить!). В `host_regs.cpp` используется `errno` без `#include <cerrno>`.

---

## 3. Ключевая архитектура upstream (что и почему заменяется)

| Компонент upstream | Факт | Замена |
|---|---|---|
| `common/lcd_ili9341.*` | рисует напрямую по SPI, без фреймбуфера | реализовать те же методы `lcd::ILI9341` поверх RGB565-буфера + SDL-текстуры |
| `application/irq_controls.cpp` | опрос кнопок из GPT-ISR 1 кГц | заменить своим `input_controls.cpp` (SDL-события -> `control::Event`), сохранить `hw/debounce.*`, `hw/encoder.*` |
| `application/irq_lcd_frame.cpp` | кадры по IRQ LCD TE | заменить потоком 60 Гц (`frame_sync.cpp`) |
| `application/event_m0.cpp` | главный цикл UI, диспетчер событий | **используется без изменений**, компилируется |
| `application/ui_navigation.cpp` | `appList` инстанцирует фабрики ВСЕХ приложений | **без изменений**; поэтому все app-TU обязаны компилироваться/линковаться |
| `application/portapack.cpp` | инициализация порта | заменить своим: display/backlight/audio/pmem init |
| `application/rtc_time.cpp` | RTC LPC43xx | свой на `std::chrono` + локальное время |
| `common/portapack_persistent_memory.cpp` | `backup_ram` по адресу 0x40041000 | mmap MAP_FIXED по точному адресу (сделано в host_regs.cpp) |
| `common/portapack_shared_memory.cpp` | `shared_memory` по адресу 0x10016000 (8 КиБ) | mmap MAP_FIXED (сделано) |
| baseband-образы | у каждого `int main()` | на этапе 4 — `-Dmain=bb_<tag>` по файлу и запуск в потоке |
| SD/FatFs | FatFs | на этапе 3 — реализация `ff.h` API поверх POSIX, папка `~/mayhem-sd/` |

### Память / регистры (уже работает)

* `host_regs.cpp` делает `mmap(MAP_FIXED)`:
  * `shared_memory` = 0x10016000, 8 КиБ;
  * `backup_ram` = 0x40041000.
  Адреса берутся из `common/memory_map.hpp` (HACKRF_ONE).
* Все `LPC_*` макросы переопределены на объекты `host_regs::*` (в `hal.h`),
  чтобы записи в регистры не падали.
* `-D__LPC43XX_CPP_H__` + forced-include `host_forced.hpp` заменяют
  `common/lpc43xx_cpp.hpp`: определяют `RTCTime`, `lpc43xx::rtc::RTC`,
  `lpc43xx::creg::m4txevent`, `lpc43xx::cgu::CLK_SEL` и mixed-type
  `std::max/min` (иначе `std::max(1U, size_t)` неоднозначен на 64-бит).

### Сборка: состав

* `FLASH_TIER=0` — внешние приложения отложены upstream-логикой.
* `cmake/UpstreamSources.cmake` содержит 136 .cpp из блока `CPPSRC`
  `firmware/application/CMakeLists.txt`, из которых исключены 48
  аппаратных/платформенных файлов (usb_serial*, i2cdev_*, chibios-обёртки,
  audio/clock/rf_path/radio/sd_card/debug/shell, весь `application/hw/*`,
  `common/lcd_ili9341.cpp`, `common/spi_image.cpp`, `common/portapack_io.cpp`,
  `common/gpdma.cpp`, `common/adc.cpp`, `common/baseband_dma.cpp` и т.д.).
* `main.cpp` upstream исключён (свой `platform/src/main.cpp`).

---

## 4. Текущее состояние сборки (точка возобновления)

### Проверка синтаксиса

Скрипт: `/tmp/opencode/try.sh` (временный; продублировать в репозиторий как
`tools/syntax-check.sh`). Он делает `g++ -std=c++20 -fsyntax-only -fpermissive`
с теми же дефайнами/инклюдами, что и CMake.

**Последний результат:** `application/event_m0.cpp` компилируется без ошибок
(`exit=0`). Это была основная проверка.

`-fpermissive` добавлен и в `CMakeLists.txt`, и в `try.sh`. Причина:
`common/spi_image.hpp:172,173,178` делает `reinterpret_cast<uint32_t>(&_textend)`,
что на 64-бит теряет точность. Значения используются только при загрузке
baseband-образов из SPIFI (на десктопе не происходит), поэтому допустимо.
Альтернатива на будущее, если понадобится чистота: shadow-заголовок
`platform/include/spi_image.hpp` с `uintptr_t`.

### Что уже проверено компилятором

* `common/ui.cpp` — OK (после шима).
* `application/event_m0.cpp` — OK.

### Что НЕ проверено

* Остальные 134 upstream .cpp — не прогонялись. Ожидаются ошибки в местах,
  завязанных на HAL/железо. Прогонять по одному, исправлять шимом, не патча.
* Полный `cmake --build` не запускался: отсутствуют 8 platform/src файлов.

---

## 5. Следующие шаги (строго по порядку)

1. **Добавить `platform/src/host_regs.cpp` в `PLATFORM_SOURCES`** и
   `#include <cerrno>` в него.
2. **Прогнать все 136 TU через синтаксис-чек**, собрать список ошибок
   (`xargs -n1` по списку из UpstreamSources.cmake), исправлять в шиме.
3. **Написать 8 файлов платформы:**
   * `lcd_sdl.cpp` — реализация методов `lcd::ILI9341` (`init`, `sleep`,
     `wake`, `set_dirty`, `transfer`, `fill`, `paint`, `draw_pixel` и т.д. —
     см. `common/lcd_ili9341.hpp`) в RGB565-буфер 240x320; SDL-текстура;
     отрисовка рамки H2 и экранных кнопок; масштаб 1/2/3; мышь = тач.
   * `input_controls.cpp` — SDL keyboard/mouse -> `control::Event`
     (см. семантику `application/irq_controls.cpp` и `hw/debounce.*`,
     `hw/encoder.*`), очередь событий для `event_m0.cpp`.
   * `frame_sync.cpp` — поток 60 Гц, выставляет `EVT_MASK_LCD_FRAME_SYNC`
     (см. `irq_lcd_frame.cpp`), дергает перерисовку.
   * `portapack.cpp` — глобалы `portapack::display`, `backlight`, `audio`,
     `io`, `rtc`, `shutdown()`, `init()`, `persistent_memory::cache::init()`
     (вызов как в upstream `portapack.cpp:578`).
   * `rtc_time.cpp` — `rtc_time::now()/set()`, тик секунды, DST-функции,
     `signal_tick_second` (см. `application/rtc_time.hpp`).
   * `hardware_stubs.cpp` — radio/rf_path/audio/battery/i2cdev/max2837/
     rffc507x/si5351/usb_serial/gpio/power_control: пустые или
     возвращающие безопасные значения реализации символов.
   * `ff_stub.cpp` — линк-заглушки FatFs (`f_mount`, `f_open`, ...) до этапа 3
     (либо сразу минимальная POSIX-реализация).
   * `main.cpp` — `SDL_Init`, окно, главный поток SDL, поток UI
     (`event_m0.cpp`), корректный выход.
4. **Линковка**, затем запуск: окно, главное меню, навигация энкодером/
   стрелками/мышью. Честно задокументировать, что реально работает.
5. **HANDOFF.md обновлять** после каждого этапа.

---

## 6. Подводные камни (проверено)

* `ffconf.h`: `_FS_REENTRANT 1`, `_SYNC_t Semaphore*` — тип `Semaphore` уже
  определён в `ch.h`.
* `message.hpp:~920`: `std::max(1U, size_t)` — решено overload'ами в
  `host_forced.hpp`; не убирать.
* `ch.h` должен определять `CORTEX_M0`/`CORTEX_M4`, `chSysDisable`,
  `systick_stop` (используются в `event_m0.cpp`).
* `hal.h` должен включать `lpc43xx.inc`, затем `lpc43xx.h`, и только потом
  переопределять `LPC_*`. `__IO/__I/__O` — макросы (`volatile`), не typedef.
* `event_m0.cpp` при deep sleep пишет в LPC/NVIC — компилируется, на десктопе
  не исполняется.
* `ui_navigation.cpp` тянет все приложения: уменьшить сборку нельзя без
  патча upstream, поэтому линкуем всё.
* `chMtxUnlock()` в ChibiOS 2.6 освобождает последний взятый мьютекс — так и
  реализовано (стек мьютексов thread_local).
* baseband-файлы имеют `int main()`: на этапе 4 каждому дать
  `-Dmain=bb_<tag>`.
* Пользователь просил не утверждать о проверке на железе. Железа PlutoSDR нет;
  RTL-SDR только RX; TX проверяется файловым стабом.

---

## 7. Готовый промт для новой сессии (скопировать целиком)

```
Продолжаем порт PortaPack Mayhem в десктопное Linux-приложение.
Рабочая папка: <repo>
ПЕРВЫМ ДЕЛОМ прочитай <repo>/HANDOFF.md — там полное
состояние, архитектура, подводные камни и план. Затем проверь фактическое
состояние файлов (find platform cmake -type f; cat CMakeLists.txt).

Кратко: upstream Mayhem (shallow clone, коммит 77798cb0) лежит в
third_party/mayhem-firmware и НЕ ПАТЧИТСЯ. Порт — в platform/ (шим ChibiOS,
HAL, NVIC, SDL-дисплей/ввод) и cmake/. UI-код Mayhem используется как есть.
Сейчас идёт ЭТАП 1: скелет сборки + окно SDL 240x320 + клавиатура/меню.
Этапы 2-6 (лаунчер, папка вместо SD, SdrDevice+RTL+Pluto+спектр, RX, TX)
начинать ТОЛЬКО после явной команды пользователя.

Уже сделано: CMakeLists.txt, cmake/UpstreamSources.cmake (136 upstream .cpp,
FLASH_TIER=0), platform/include/{ch.h,hal.h,nvic.h,lpc43xx_m0.h,pal_lld.h,
host_forced.hpp}, platform/src/{chibios.cpp,host_regs.cpp}. event_m0.cpp и
common/ui.cpp проходят g++ -fsyntax-only -fpermissive без ошибок.

Ближайшая задача:
1) добавить platform/src/host_regs.cpp в PLATFORM_SOURCES и <cerrno>;
2) прогнать все 136 TU через /tmp/opencode/try.sh, починить шим;
3) написать 8 platform/src файлов: portapack.cpp, lcd_sdl.cpp,
   input_controls.cpp, frame_sync.cpp, rtc_time.cpp, hardware_stubs.cpp,
   ff_stub.cpp, main.cpp (см. §5 HANDOFF.md, там описано, что каждый делает);
4) слинковать, запустить, добиться окна и навигации по меню;
5) обновить HANDOFF.md.

Правила: upstream не патчить (только forced-include и shadow-заголовки);
файлы целиком, без «...»; комментарии в коде по-английски; ответы по-русски;
GPL-заголовки сохранять; не заявлять о проверке на реальном железе; перед
крупными изменениями показывать дерево/список зависимостей и ждать «ок».
Ограничения среды: x86_64 EndeavourOS, SDL2 2.32.74, libiio 0.26, librtlsdr
есть; PlutoSDR нет, RTL только RX.
```

---

## 8. Журнал

* 2026-11-07: клон upstream, анализ зависимостей, согласование дерева и
  дизайна H2-контролов, «ок» пользователя.
* 2026-11-08: создан CMake и шим; `event_m0.cpp` и `common/ui.cpp` компилируются;
  `spi_image.hpp` обойдён `-fpermissive`; зафиксировано состояние в этом файле.
* 2026-11-09 (сессия 2, этап 1 UI доводится):
  * Корпус H2: единый прибор — верхняя полоса с кнопками (квадрат ПОВОРОТ
    слева, DFU справа), под ней экран 240x320, снизу дек: круглый энкодер
    с LED-кольцом слева, крест из 5 кнопок (стрелки ^ v < > + центр) справа.
  * **Поворот окна на 90° по часовой** по клику на квадрат (или клавиша `R` /
    `Q`/`E` нет — энкодер; поворот только `R` и квадрат). Теперь крутится ВЕСЬ
    прибор вместе с кнопками ПОВОРОТ и DFU (раньше шапка была неподвижной).
    Реализация: весь прибор рисуется в off-screen текстуру `DEVICE_W×DEVICE_H`
    и блитится через `SDL_RenderCopyEx`; клики маппятся обратно
    `window_to_device()` (обратный поворот вокруг центра окна).
    Геометрия: 0°/180° → 544×952, 90°/270° → 952×544 при scale 2.
  * **Причина «половинки экрана» и неверного ресайза**: окно создавалось без
    `SDL_WINDOW_RESIZABLE`; тогда WM/niri то фиксировал его, то тайлил на
    пол-экрана. Исправлено: флаг `SDL_WINDOW_RESIZABLE` + правило в
    `~/.config/niri/config.kdl`:
    `window-rule { match app-id="^mayhem-desktop$"; open-floating true }`.
    Минимум окна — 64×64 (раньше пиннился стартовый размер → ресайз не работал).
  * **Энкодер**: клик ЛКМ по колесу без протяжки = SELECT; протяжка = вращение
    (в осях устройства через `device_delta`, накопление по 8 device-пикселей).
  * **Вход в приложение больше не падает**: `service_baseband_channel()`
    (забирает `shared_memory.baseband_message`), `set_baseband_ready()` в
    `m4_init`, валидный `FATFS` из `f_getfree` (шим `std::filesystem::space`).
    Проверено через `MAYHEM_SELFTEST=1`: Capture открывается, лог чист.
  * Дубль-клавиши: `R` — поворот, `1/2/3` — масштаб, `Q/E` — энкодер,
    стрелки/Enter/Esc — крестовина.
  * НЕ завершено на момент записи: полная ручная проверка тача (не залипает ли
    виджет) и энкодера в обе стороны на рантайме; полный цикл поворота
    подтверждён визуально по снимкам.
* 2026-11-09 (сессия 3, исправлены два вылета в File Manager):
  * **Вылет №1 `split_string()`** (`file_reader.cpp:45`). Строка модального
    сообщения заканчивается на `\n`, после последнего разделителя `start ==
    length()`, но проверка `start <= str.length()` пропускала это, и код делал
    `&str[start]` — на один символ за концом `string_view`. На устройстве
    (newlib) это молча давало пустой хвост, на хосте libstdc++ с
    `_GLIBCXX_ASSERTIONS` — аварийное завершение. Исправлено хост-заменой TU:
    `platform/src/host_file_reader.cpp` (поведение сохранено, семантика
    upstream-тестов `test_file_reader.cpp` воспроизведена, в т.ч. пустая
    последняя колонка при завершающем разделителе). Upstream
    `file_reader.cpp` убран из `cmake/UpstreamSources.cmake`.
  * **Вылет №2 `path::operator/=`** (`file.hpp:150`). Inline-метод в заголовке
    вызывает `_s.back()` / `p._s.front()` на пустой строке. При `current_path`
    = корень SD (пустой путь) `refresh_list()` строит `current_path / entry.path`
    и процесс падал. На устройстве reading-empty даёт «не разделитель», отсюда
    ведущий `/` при склейке (на это опирается `path_iequal(current_path,
    u"/" + dir)`). Исправлено shadow-заголовком: `platform/include/host_file.hpp`
    — дословная копия upstream с единственным изменением (guard'ы на пустоту);
    форс-инклюдится из `host_forced.hpp`, upstream-guard `__FILE_H__`
    предопределяется, так что все `#include "file.hpp"` становятся no-op.
    Два хостовых TU, использующих настоящий C++17 `<filesystem>`
    (`platform/src/ff_stub.cpp`, `platform/src/host_regs.cpp`), компилируются с
    `-DHOST_STD_FILESYSTEM` и shim пропускают.
  * **Проверено вживую (gdb, `MAYHEM_SD` = установленная псевдо-СД)**: вход
    Utilities → File Manager (модалка «Can't read external apps» рисуется и
    закрывается), корень SD, вход/выход в подкаталоги, возврат в корень, выход.
    Ноль assertion'ов за всю сессию.
  * Замечание на будущее: **Debug-сборка хоста включает `_GLIBCXX_ASSERTIONS`**
    (нет `__OPTIMIZE__`), поэтому UB, безопасное на bare-metal, на хосте
    аварийно завершает процесс. Это класс проблем, который ещё может
    всплывать — искать такие места и закрывать хост-заменой/shadow'ом, не
    патча upstream.

---

## 2. RX-тракт: реальный эфир работает (2026-11-09)

Приоритет этого этапа: чтобы **водопад и весь приём (RX) реально работали** на
живом железе (сейчас — RTL-SDR; цель — PlutoSDR/Zynq7010).

### Корень двух давних проблем

Всё упиралось в отсутствие ядра M4 на хосте. Раньше это лечили костылями на
M0-стороне. Сейчас на хосте играется роль M4 **настоящим DSP прошивки**.

### Что сделано

* **Хост-ядро M4** — `platform/src/host_baseband.cpp`. Продюсер-поток берёт IQ
  из `sdr::open_preferred()` (interleaved signed int8, ровно как из SGPIO),
  прогоняет через **те же** дециматоры прошивки
  (`dsp::decimate::FIRC8xR16x24FS4Decim4/8`, `FIRC16xR16x16Decim2`,
  `FIRC16xR16x32Decim8`, `NoopDecim`) и FFT (`fft_swap` + `fft_c_preswapped`,
  как в `SpectrumCollector::update()`). Заявки водопада/записи — из реального
  эфира, а не реимплементация.
* **Шим ARM-интринсиков** — `platform/include/host_simd.hpp` (форс-инклюд):
  `__SSAT`, `__PKHBT`/`__PKHTB`, `__SMLAD*`, `__SMLSLD`, `__SMUAD*`,
  `__SXTB16`, `__SMMULR`, `__QADD/__QSUB16`, `__BFI`, `__RBIT`, `__SMLALD`, …
  + `vec2_s16`/`vec4_s8`, `__SIMD32` (lvalue-алиас, как в `lpc43xx_m4.h`).
  DSP-исходники upstream компилируются как есть на x86-64.
* **Продюсер/потребитель.** `host_baseband::start()` поднимает поток (в
  `host_baseband_service_start()`); `handle_message()` вызывается потребителем
  канала (`service_baseband_channel`) и заводит дециматоры, `StreamInput`
  (для записи) и FIFO водопада.
* **Фолбэк устройства** — `sdr::open_preferred()` при пустом `g_pref_driver`
  (лаунчер пропущен) открывает первое устройство из `sdr::enumerate()`.
* **Размер блока** — `kBufferSamples = 2048`, а не 8192. `baseband_dma.cpp`
  делит буфер 8192 на `transfers_per_buffer = 4` LLI-трансфера, значит
  `execute()` получает 2048 `complex8_t`. Дециматоры рассчитаны ровно на это
  (Decim4 от 2048 → 512 = размер `dst`). Блок 8192 переполнял `dst` и портил
  соседнее состояние (`StreamInput*`) → SIGSEGV в `StreamInput::write`.
* **Жизненный цикл.** `start()` регистрирует `std::atexit(shutdown)`, чтобы
  джойнить продюсер-поток до статических деструкторов (иначе
  `terminate called without an active exception`).
* **Порядок include-путей.** `${FW}/baseband` идёт последним — иначе
  `baseband/debug.hpp` затеняет `application/debug.hpp` (ломало `ui_debug.cpp`).

### Проверено вживую (GUI, RTL-SDR Fitipower FC0013)

* Окно `mayhem-desktop` открывается и стабильно живёт (floating, ~548x956).
* Capture (автозапуск через `SETTINGS/nav.ini` → `autostart_app=capture`):
  * `CAPTURES/BBD_0000.C16` растёт (~4→8 МБ за 4 с), данные ненулевые;
  * `CAPTURES/BBD_0000.TXT`: `center_frequency=100000000`,
    `sample_rate=500000`;
  * **водопад течёт** (видно FM-станции на 100 МГц), `Rate:500k Format:C16`.
* RTL сам клампит 4 МГц → 3.2 МГц (FC0013 больше не тянет); лог печатает
  `Invalid sample rate: 4000000 Hz`, но приём идёт на 3.2 МГц.
* Headless (`SDL_VIDEODRIVER=dummy`) и GUI выходят с кодом 0.

### Осталось / следующий шаг

* Водопад использует хост-FFT хост-ядра, а не `SpectrumCollector` upstream
  напрямую (поведение совпадает; при желании можно свести к одному объекту).
* `SampleRateConfig`/`CaptureConfig`/`SpectrumStreamingConfig` уже
  маршрутизируются; остальные `radio::`/`baseband_api` заявки — по мере
  портирования приложений.
* PlutoSDR на железе не проверен (устройства нет: `iio_info -S` видит только
  local context).
* Открытые вопросы к пользователю: тач второго (HDMI) дисплея;
  `Escape` = Back вместо DFU.

---

## 3. Стабильность RX: дедлок RTL, звук и мьют (2026-11-09)

### Дедлок RTL при смене частоты/полосы (водопад «пропадал» и устройство висело)

Причина: **блокирующее ожидание внутри async-колбэка librtlsdr**.
`RtlDevice::on_samples()` (вызывается на USB-потоке из недр libusb, который
держит внутренний event-lock) ждал места в кольцевом буфере через
`space_cv_.wait()`. Продюсер в это время вызывал `set_center_freq()` →
`libusb_control_transfer()`, которому нужен тот же event-lock. Итог: колбэк
ждёт, пока продюсер сдренирует кольцо; продюсер ждёт lock, который держит
колбэк — взаимная блокировка, приём и водопад навсегда встают.

Фикс (`platform/src/sdr_rtlsdr.cpp`): колбэк **никогда не блокируется**. При
переполнении кольца отбрасываются самые старые сэмплы (`read_pos_` подтягивается
вперёд), `space_cv_` удалён. Стресс: 400 смен частоты при непрерывном чтении —
без зависания (раньше вешало намертво).

### Кнопка звука в верхней панели

`toggle_mute` в `ui_navigation.cpp` пишет `pmem::config_audio_mute()` и зовёт
`audio::output::update_audio_mute()` → `speaker_mute()/unmute()` →
`codec->headphone_enable()/disable()` + `i2s::tx_mute()`. На хосте i2s и кодек —
пустышки, поэтому кнопка только меняла иконку.

Фикс: шов — хост-мьют читается прямо из флага кнопки. Каждый хост-тик
(`host_controls_tick()`, `platform/src/input_controls.cpp`) делает
`host_audio::set_muted(portapack::persistent_memory::config_audio_mute())`;
SDL-колбэк (`platform/src/host_audio.cpp`, `g_muted`) при мьюте выдаёт тишину и
сливает очередь PCM. Так кнопка глушит и RX-звук, и бипы.

**Важная ловушка (первый вариант фикса был неверным).** Сначала мьют привязали к
кодековым линиям `HostAudioCodec::headphone_enable/disable()`. Но прошивка при
старте `SystemStatusView` зовёт `audio::output::stop()` → `mute()` →
`headphone_disable()`, а `app_requested_mute` остаётся `true`, поэтому
`speaker_unmute()` по кнопке больше не размьючивает (`unmute()` зовётся только
если `!app_requested_mute`). Мьют «залипает» навсегда, игры без `output::start()`
остаются без звука. Поэтому линии кодека пустышки, а мьют следует **только** за
флагом кнопки (`config_audio_mute`, по умолчанию `false`).

### Проверено вживую

* Capture, `Rate:16k` (16k×64 = 1.024 МГц — FC0013 тянет): водопад стабильно
  течёт; 400 смен частоты без зависания.
* Причина мёртвого водопада на `Rate:12K5`: baseband_fs = 12.5k×64 = **800 кГц**,
  а FC0013 принимает ~0.9–3.2 МГц → `Invalid sample rate: 800000 Hz`, потока нет.

---

## 4. Полноэкранный режим окна (2026-11-09)

Окно по-прежнему стартует **плавающим** (размер по масштабу x1/x2/x3). Добавлена
опция развернуть на весь экран:

* **Кнопка «FS»** в середине верхней планки корпуса (host-only, как и квадрат
  поворота; не firmware-кнопка). Рисуется в `draw_static_body()`.
* **Клавиши** `F` и `F11` (`toggle_fullscreen()`).
* Работает и на лаунчере (setup-экране) — те же горячие клавиши и клики по «FS».

Поведение (`platform/src/lcd_sdl.cpp`):

* `SDL_SetWindowFullscreen(SDL_WINDOW_FULLSCREEN_DESKTOP)` — borderless, во весь
  текущий вывод, без смены видеорежима.
* В fullscreen тело корпуса масштабируется **равномерно по обеим осям** (один
  коэффициент = `min(s_w, s_h)`, пропорции сохранены) и **центрируется**
  (letterbox), а не растягивается на весь экран. Это сделано в
  `current_screen_scale_xy()`: при `g_fullscreen` обе оси берут общий множитель.
  Обратное преобразование ввода (`window_to_device()`) использует тот же множитель,
  поэтому клики остаются точными.
* Пока fullscreen включён, вращение (R) и масштаб (1/2/3) **не** трогают размер
  окна (оно принадлежит композитору) — меняется только ориентация тела внутри
  экрана. Выход из fullscreen возвращает плавающий размер по текущему масштабу.

Проверено вживую (niri, outputs eDP-1 1920x1080 и HDMI-A-1 1024x600): «FS»/F11
разворачивают корпус по центру экрана с сохранением пропорций; повторное нажатие
возвращает плавающее окно. Ввод (клики/энкодер) корректен в обоих режимах.

