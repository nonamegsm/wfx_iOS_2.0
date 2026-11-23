/*
 * lang.c - all texts shown to the user, in English, Ukrainian, Russian and Polish.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This file is UTF-8. Every text has an identifier (see wfx_ios.h) and one
 * entry per language. tr(id) returns the text in the current language.
 *
 * Rules when editing:
 *  - the placeholders (%s, %hs, %d, ...) must be the same, in the same order,
 *    in all four languages; tools/check_lang.py verifies this
 *  - folder names such as "User" or "Photos" are not translated: they are part
 *    of the paths Total Commander remembers
 *
 * The language is taken from the setting Language= (en, uk, ru, pl); without
 * it, from the Windows display language.
 */
#include "wfx_ios.h"

enum { LANG_EN, LANG_UK, LANG_RU, LANG_PL, LANG_COUNT };

static int g_language = LANG_EN;

/* Order of the columns: English, Ukrainian, Russian, Polish. */
static const WCHAR* const g_text[S_COUNT][LANG_COUNT] = {

    /* ---- no device / pairing ---- */
    [S_ERR_NO_SERVICE] = {
        L"Cannot reach the Apple Mobile Device Service (usbmuxd, 127.0.0.1:27015), error %d.\nInstall Apple Mobile Device Support and make sure the \"Apple Mobile Device Service\" is running.",
        L"Немає зв'язку зі службою Apple Mobile Device Service (usbmuxd, 127.0.0.1:27015), помилка %d.\nВстановіть Apple Mobile Device Support і перевірте, що служба \"Apple Mobile Device Service\" запущена.",
        L"Нет связи со службой Apple Mobile Device Service (usbmuxd, 127.0.0.1:27015), ошибка %d.\nУстановите Apple Mobile Device Support и проверьте, что служба \"Apple Mobile Device Service\" запущена.",
        L"Brak połączenia z usługą Apple Mobile Device Service (usbmuxd, 127.0.0.1:27015), błąd %d.\nZainstaluj Apple Mobile Device Support i sprawdź, czy usługa \"Apple Mobile Device Service\" jest uruchomiona." },
    [S_ERR_NO_DEVICE] = {
        L"No iOS device is connected.\nConnect and unlock the device. (The Apple Mobile Device Service is running but reports no device.)",
        L"Жоден пристрій iOS не підключено.\nПідключіть і розблокуйте пристрій. (Служба Apple Mobile Device Service працює, але не бачить пристроїв.)",
        L"Ни одно устройство iOS не подключено.\nПодключите и разблокируйте устройство. (Служба Apple Mobile Device Service работает, но не видит устройств.)",
        L"Żadne urządzenie iOS nie jest podłączone.\nPodłącz i odblokuj urządzenie. (Usługa Apple Mobile Device Service działa, ale nie widzi żadnego urządzenia.)" },
    [S_ERR_DEVICE_NOT_CONNECTED] = {
        L"Device %hs is not connected (idevice error %d).",
        L"Пристрій %hs не підключено (помилка idevice %d).",
        L"Устройство %hs не подключено (ошибка idevice %d).",
        L"Urządzenie %hs nie jest podłączone (błąd idevice %d)." },
    [S_ERR_OPEN_DEVICE] = {
        L"Could not open the device (idevice error %d).",
        L"Не вдалося відкрити пристрій (помилка idevice %d).",
        L"Не удалось открыть устройство (ошибка idevice %d).",
        L"Nie udało się otworzyć urządzenia (błąd idevice %d)." },
    [S_ERR_DEVICES_REPORTED] = {
        L"\nDevices reported by the service:",
        L"\nПристрої, які бачить служба:",
        L"\nУстройства, которые видит служба:",
        L"\nUrządzenia widziane przez usługę:" },
    [S_ERR_PASSCODE] = {
        L"The device is locked with a passcode.\nUnlock it and try again.",
        L"Пристрій заблоковано код-паролем.\nРозблокуйте його і спробуйте ще раз.",
        L"Устройство заблокировано код-паролем.\nРазблокируйте его и попробуйте ещё раз.",
        L"Urządzenie jest zablokowane kodem.\nOdblokuj je i spróbuj ponownie." },
    [S_ERR_TRUST_PENDING] = {
        L"Tap \"Trust\" on the device, then try again.",
        L"Натисніть \"Довіряти\" на пристрої і спробуйте ще раз.",
        L"Нажмите \"Доверять\" на устройстве и попробуйте ещё раз.",
        L"Stuknij \"Zaufaj\" na urządzeniu i spróbuj ponownie." },
    [S_ERR_TRUST_DENIED] = {
        L"Pairing was declined on the device (\"Don't Trust\").\nReconnect the device and tap \"Trust\".",
        L"На пристрої відхилено довіру (\"Не довіряти\").\nПідключіть пристрій знову і натисніть \"Довіряти\".",
        L"На устройстве отклонено доверие (\"Не доверять\").\nПодключите устройство заново и нажмите \"Доверять\".",
        L"Na urządzeniu odrzucono zaufanie (\"Nie ufaj\").\nPodłącz urządzenie ponownie i stuknij \"Zaufaj\"." },
    [S_ERR_NOT_PAIRED] = {
        L"The device is not paired with this computer.\nReconnect it and tap \"Trust\".",
        L"Пристрій не спарено з цим комп'ютером.\nПідключіть його знову і натисніть \"Довіряти\".",
        L"Устройство не сопряжено с этим компьютером.\nПодключите его заново и нажмите \"Доверять\".",
        L"Urządzenie nie jest sparowane z tym komputerem.\nPodłącz je ponownie i stuknij \"Zaufaj\"." },
    [S_ERR_LOCKDOWN] = {
        L"Could not connect to lockdownd: %hs (%d)",
        L"Не вдалося з'єднатися з lockdownd: %hs (%d)",
        L"Не удалось соединиться с lockdownd: %hs (%d)",
        L"Nie udało się połączyć z lockdownd: %hs (%d)" },

    /* ---- services ---- */
    [S_ERR_START_SERVICE] = {
        L"Could not start service %hs: %hs",
        L"Не вдалося запустити службу %hs: %hs",
        L"Не удалось запустить службу %hs: %hs",
        L"Nie udało się uruchomić usługi %hs: %hs" },
    [S_ERR_HOUSE_ARREST] = {
        L"Could not connect to house_arrest.",
        L"Не вдалося з'єднатися з house_arrest.",
        L"Не удалось соединиться с house_arrest.",
        L"Nie udało się połączyć z house_arrest." },
    [S_ERR_APP_NOT_SHARED] = {
        L"The application %s does not share its files (%hs).",
        L"Програма %s не надає доступу до своїх файлів (%hs).",
        L"Приложение %s не даёт доступа к своим файлам (%hs).",
        L"Aplikacja %s nie udostępnia swoich plików (%hs)." },
    [S_ERR_AFC] = {
        L"Could not connect to the AFC service.",
        L"Не вдалося з'єднатися зі службою AFC.",
        L"Не удалось соединиться со службой AFC.",
        L"Nie udało się połączyć z usługą AFC." },
    [S_ERR_INSTPROXY] = {
        L"Could not connect to installation_proxy.",
        L"Не вдалося з'єднатися з installation_proxy.",
        L"Не удалось соединиться с installation_proxy.",
        L"Nie udało się połączyć z installation_proxy." },
    [S_ERR_APP_LIST] = {
        L"Could not read the list of applications.",
        L"Не вдалося прочитати список програм.",
        L"Не удалось прочитать список приложений.",
        L"Nie udało się odczytać listy aplikacji." },
    [S_ERR_READ_DIR] = {
        L"Could not read the directory (AFC error %d).",
        L"Не вдалося прочитати каталог (помилка AFC %d).",
        L"Не удалось прочитать каталог (ошибка AFC %d).",
        L"Nie udało się odczytać katalogu (błąd AFC %d)." },
    [S_ERR_FILE_INFO] = {
        L"Could not read file information.",
        L"Не вдалося прочитати відомості про файл.",
        L"Не удалось прочитать сведения о файле.",
        L"Nie udało się odczytać informacji o pliku." },

    /* ---- applications ---- */
    [S_UNINSTALL_QUESTION] = {
        L"Uninstall \"%s\" from the device?\n\n%s\n\nThis removes the application and ALL its documents and data. It cannot be undone.",
        L"Видалити \"%s\" з пристрою?\n\n%s\n\nБуде видалено програму і ВСІ її документи та дані. Скасувати це неможливо.",
        L"Удалить \"%s\" с устройства?\n\n%s\n\nБудет удалено приложение и ВСЕ его документы и данные. Отменить это невозможно.",
        L"Odinstalować \"%s\" z urządzenia?\n\n%s\n\nZostanie usunięta aplikacja oraz WSZYSTKIE jej dokumenty i dane. Tej operacji nie można cofnąć." },
    [S_UNINSTALL_TITLE] = {
        L"Uninstall application", L"Видалення програми", L"Удаление приложения", L"Odinstalowanie aplikacji" },
    [S_ERR_UNINSTALL] = {
        L"The device refused to remove the application (error %d).",
        L"Пристрій відмовився видалити програму (помилка %d).",
        L"Устройство отказалось удалить приложение (ошибка %d).",
        L"Urządzenie odmówiło usunięcia aplikacji (błąd %d)." },
    [S_ERR_INSTALL] = {
        L"The device did not install the package (error %d).\nThe package must be signed for this device.",
        L"Пристрій не встановив пакет (помилка %d).\nПакет має бути підписаний для цього пристрою.",
        L"Устройство не установило пакет (ошибка %d).\nПакет должен быть подписан для этого устройства.",
        L"Urządzenie nie zainstalowało pakietu (błąd %d).\nPakiet musi być podpisany dla tego urządzenia." },

    /* ---- photos by month ---- */
    [S_ERR_PHOTO_DB_MISSING] = {
        L"The photo database could not be found on the device.",
        L"На пристрої не знайдено базу даних фотографій.",
        L"На устройстве не найдена база данных фотографий.",
        L"Na urządzeniu nie znaleziono bazy danych zdjęć." },
    [S_ERR_PHOTO_DB_READ] = {
        L"The photo database could not be read from the device.",
        L"Не вдалося прочитати базу даних фотографій з пристрою.",
        L"Не удалось прочитать базу данных фотографий с устройства.",
        L"Nie udało się odczytać bazy danych zdjęć z urządzenia." },
    [S_ERR_PHOTO_DB_LAYOUT] = {
        L"The photo database of this iOS version has a layout the plugin does not know.\nPlease report the iOS version; the log has the details.",
        L"База даних фотографій цієї версії iOS має структуру, невідому плагіну.\nПовідомте, будь ласка, версію iOS; подробиці є в журналі.",
        L"База данных фотографий этой версии iOS имеет структуру, неизвестную плагину.\nСообщите, пожалуйста, версию iOS; подробности есть в журнале.",
        L"Baza danych zdjęć tej wersji iOS ma układ nieznany wtyczce.\nZgłoś proszę wersję iOS; szczegóły są w dzienniku." },

    /* ---- settings dialog ---- */
    [S_DLG_SETTINGS] = {
        L"iOS plugin settings", L"Налаштування плагіна iOS", L"Настройки плагина iOS", L"Ustawienia wtyczki iOS" },
    [S_GRP_DEVICE]  = { L"Device", L"Пристрій", L"Устройство", L"Urządzenie" },
    [S_LBL_USE]     = { L"&Use:", L"&Обрати:", L"&Выбрать:", L"&Użyj:" },
    [S_BTN_REFRESH] = { L"&Refresh", L"О&новити", L"О&бновить", L"O&dśwież" },
    [S_GRP_OPTIONS] = { L"Options", L"Параметри", L"Параметры", L"Opcje" },
    [S_CHK_APPNAMES] = {
        L"Show applications by &name instead of bundle id",
        L"Показувати програми за &назвою, а не за ідентифікатором",
        L"Показывать приложения по &названию, а не по идентификатору",
        L"Pokazuj aplikacje według &nazwy zamiast identyfikatora" },
    [S_CHK_ALLAPPS] = {
        L"List &all applications (most will not open)",
        L"Показувати &всі програми (більшість не відкриється)",
        L"Показывать вс&е приложения (большинство не откроется)",
        L"Pokazuj &wszystkie aplikacje (większość się nie otworzy)" },
    [S_CHK_WIFI] = {
        L"Use devices over &Wi-Fi when none is on USB (can be slow)",
        L"Використовувати пристрої через &Wi-Fi, якщо немає USB (може бути повільно)",
        L"Использовать устройства по &Wi-Fi, если нет USB (может быть медленно)",
        L"Używaj urządzeń przez &Wi-Fi, gdy żadne nie jest na USB (może być wolno)" },
    [S_CHK_LOG] = {
        L"Write a &log file", L"Вести &журнал", L"Вести &журнал", L"Zapisuj &dziennik" },
    [S_CHK_DEBUG] = {
        L"Include the full protocol &trace in the log (large, slow)",
        L"Додавати до журналу повне &трасування протоколу (великий, повільно)",
        L"Добавлять в журнал полную &трассировку протокола (большой, медленно)",
        L"Dołączaj do dziennika pełny ślad pro&tokołu (duży, wolno)" },
    [S_LBL_LANGUAGE] = { L"Lan&guage:", L"&Мова:", L"&Язык:", L"&Język:" },
    [S_LANG_AUTO]    = { L"Same as Windows", L"Як у Windows", L"Как в Windows", L"Jak w Windows" },
    [S_BTN_OPENLOG]  = { L"Open lo&g", L"Відкрити жу&рнал", L"Открыть жу&рнал", L"Otwórz dzie&nnik" },
    [S_BTN_DEVINFO]  = { L"Device &information...", L"&Інформація про пристрій...", L"&Информация об устройстве...", L"&Informacje o urządzeniu..." },
    [S_DLG_INFO]     = { L"Device information", L"Інформація про пристрій", L"Информация об устройстве", L"Informacje o urządzeniu" },
    [S_COL_PROPERTY] = { L"Property", L"Властивість", L"Свойство", L"Właściwość" },
    [S_COL_VALUE]    = { L"Value", L"Значення", L"Значение", L"Wartość" },
    [S_BTN_COPY_VALUE] = { L"&Copy value", L"&Копіювати значення", L"&Копировать значение", L"&Kopiuj wartość" },
    [S_BTN_COPY_ALL] = { L"Copy &all", L"Копіювати &все", L"Копировать &всё", L"Kopiuj &wszystko" },
    [S_BTN_SAVE]     = { L"&Save to file...", L"&Зберегти у файл...", L"&Сохранить в файл...", L"&Zapisz do pliku..." },
    [S_BTN_CLOSE]    = { L"Close", L"Закрити", L"Закрыть", L"Zamknij" },
    [S_SEC_MAIN]     = { L"Device", L"Пристрій", L"Устройство", L"Urządzenie" },
    [S_SEC_IDS]      = { L"Identifiers", L"Ідентифікатори", L"Идентификаторы", L"Identyfikatory" },
    [S_SEC_STORAGE]  = { L"Storage", L"Сховище", L"Хранилище", L"Pamięć" },
    [S_SEC_BATTERY]  = { L"Battery", L"Акумулятор", L"Аккумулятор", L"Bateria" },
    [S_SEC_REGION]   = { L"Language and region", L"Мова та регіон", L"Язык и регион", L"Język i region" },
    [S_SEC_OTHER]    = { L"Everything else", L"Усе інше", L"Всё остальное", L"Wszystko inne" },
    [S_INFO_LIMITED] = {
        L"Not trusted: only basic values. Unlock the device and tap Trust.",
        L"Немає довіри: лише основні значення. Розблокуйте пристрій і натисніть «Довіряти».",
        L"Нет доверия: только основные значения. Разблокируйте устройство и нажмите «Доверять».",
        L"Brak zaufania: tylko podstawowe wartości. Odblokuj urządzenie i stuknij Zaufaj." },
    [S_YES]          = { L"Yes", L"Так", L"Да", L"Tak" },
    [S_NO]           = { L"No", L"Ні", L"Нет", L"Nie" },
    [S_ERR_SAVE]     = { L"Could not write the file.", L"Не вдалося записати файл.", L"Не удалось записать файл.", L"Nie można zapisać pliku." },
    [S_ERR_HOME_READ] = {
        L"Could not read the home screen layout (error %d).",
        L"Не вдалося прочитати розташування іконок (помилка %d).",
        L"Не удалось прочитать расположение значков (ошибка %d).",
        L"Nie można odczytać układu ekranu początkowego (błąd %d)." },
    [S_ERR_HOME_WRITE] = {
        L"The device did not accept the new home screen layout (error %d).\nThe layout from before the change is saved in:\n%s",
        L"Пристрій не прийняв нове розташування іконок (помилка %d).\nРозташування до зміни збережено у файлі:\n%s",
        L"Устройство не приняло новое расположение значков (ошибка %d).\nРасположение до изменения сохранено в файле:\n%s",
        L"Urządzenie nie przyjęło nowego układu ekranu początkowego (błąd %d).\nUkład sprzed zmiany zapisano w pliku:\n%s" },
    [S_ERR_HOME_FILE] = {
        L"This file is not a home screen layout.",
        L"Цей файл не є розташуванням іконок.",
        L"Этот файл не является расположением значков.",
        L"Ten plik nie jest układem ekranu początkowego." },
    [S_DLG_ARRANGE]  = { L"Arrange home screen", L"Упорядкування екрана", L"Упорядочивание экрана", L"Układ ekranu początkowego" },
    [S_A_NEWFOLDER]  = { L"New &folder", L"Нова &тека", L"Новая &папка", L"Nowy &folder" },
    [S_A_NEWPAGE]    = { L"New &page", L"Нова &сторінка", L"Новая &страница", L"Nowa &strona" },
    [S_A_UNGROUP]    = { L"&Ungroup folder", L"&Розформувати", L"&Расформировать", L"&Rozgrupuj folder" },
    [S_A_RENAME]     = { L"Re&name folder", L"Пере&йменувати", L"Пере&именовать", L"Zmień &nazwę" },
    [S_A_RELOAD]     = { L"&Read from device", L"&Зчитати з пристрою", L"&Считать с устройства", L"&Odczytaj z urządzenia" },
    [S_A_LOAD]       = { L"&Open file...", L"&Відкрити файл...", L"&Открыть файл...", L"O&twórz plik..." },
    [S_A_SAVE]       = { L"&Save to file...", L"З&берегти у файл...", L"Со&хранить в файл...", L"&Zapisz do pliku..." },
    [S_A_APPLY]      = { L"&Apply to device", L"З&астосувати", L"&Применить", L"Z&astosuj" },
    [S_A_HINT] = {
        L"Drag icons onto a page or folder on the left, or within the list to change their order. Nothing changes on the device until Apply.",
        L"Перетягуйте іконки на сторінку чи теку ліворуч або в межах списку, щоб змінити порядок. На пристрої нічого не зміниться до натискання «Застосувати».",
        L"Перетаскивайте значки на страницу или папку слева либо внутри списка, чтобы изменить порядок. На устройстве ничего не изменится до нажатия «Применить».",
        L"Przeciągaj ikony na stronę lub folder po lewej albo w obrębie listy, aby zmienić kolejność. Na urządzeniu nic się nie zmieni do kliknięcia Zastosuj." },
    [S_A_CHANGED]    = { L"%s  (not applied yet)", L"%s  (ще не застосовано)", L"%s  (ещё не применено)", L"%s  (jeszcze nie zastosowano)" },
    [S_A_DOCK]       = { L"Dock", L"Dock", L"Dock", L"Dock" },
    [S_A_PAGE]       = { L"Page %d", L"Сторінка %d", L"Страница %d", L"Strona %d" },
    [S_A_FOLDER_NAME] = { L"Folder", L"Тека", L"Папка", L"Folder" },
    [S_A_SELECT_FIRST] = {
        L"Open a page and select the icons that go into the new folder.",
        L"Відкрийте сторінку та виділіть іконки, які потраплять до нової теки.",
        L"Откройте страницу и выделите значки, которые попадут в новую папку.",
        L"Otwórz stronę i zaznacz ikony, które mają trafić do nowego folderu." },
    [S_A_DISCARD] = {
        L"The changes are not on the device yet. Close without applying them?",
        L"Зміни ще не застосовано на пристрої. Закрити без застосування?",
        L"Изменения ещё не применены на устройстве. Закрыть без применения?",
        L"Zmiany nie zostały jeszcze zastosowane na urządzeniu. Zamknąć bez zastosowania?" },
    [S_A_APPLY_QUESTION] = {
        L"Send this arrangement to the device?\nThe present one is saved to a file first.",
        L"Надіслати це розташування на пристрій?\nПоточне спершу буде збережено у файл.",
        L"Отправить это расположение на устройство?\nТекущее сначала будет сохранено в файл.",
        L"Wysłać ten układ do urządzenia?\nObecny zostanie najpierw zapisany w pliku." },
    [S_A_APPLIED] = {
        L"Applied. The window now shows what the device made of it.",
        L"Застосовано. Вікно показує результат на пристрої.",
        L"Применено. Окно показывает результат на устройстве.",
        L"Zastosowano. Okno pokazuje wynik na urządzeniu." },
    [S_BTN_OK]       = { L"OK", L"OK", L"OK", L"OK" },
    [S_BTN_CANCEL]   = { L"Cancel", L"Скасувати", L"Отмена", L"Anuluj" },
    [S_ALL_DEVICES] = {
        L"<All connected devices>", L"<Усі підключені пристрої>", L"<Все подключённые устройства>", L"<Wszystkie podłączone urządzenia>" },
    [S_NOT_CONNECTED] = { L"Not connected.", L"Не підключено.", L"Не подключено.", L"Nie podłączono." },
    [S_NOT_CONNECTED_SUFFIX] = {
        L"%s  (not connected)", L"%s  (не підключено)", L"%s  (не подключено)", L"%s  (nie podłączono)" },
    [S_INFO_FORMAT] = {
        L"Name:\t%s\r\nModel:\t%s\r\niOS:\t%s (%s)",
        L"Назва:\t%s\r\nМодель:\t%s\r\niOS:\t%s (%s)",
        L"Имя:\t%s\r\nМодель:\t%s\r\niOS:\t%s (%s)",
        L"Nazwa:\t%s\r\nModel:\t%s\r\niOS:\t%s (%s)" },
    [S_FREE_FORMAT] = {
        L"\r\nFree:\t%.1f GB of %.1f GB",
        L"\r\nВільно:\t%.1f ГБ з %.1f ГБ",
        L"\r\nСвободно:\t%.1f ГБ из %.1f ГБ",
        L"\r\nWolne:\t%.1f GB z %.1f GB" },
    [S_ABOUT] = {
        L"wfx_iOS %hs \x2013 update of wfx_iOS 1.3 by Domo, the original author",
        L"wfx_iOS %hs \x2013 оновлення wfx_iOS 1.3 від Domo, автора оригіналу",
        L"wfx_iOS %hs \x2013 обновление wfx_iOS 1.3 от Domo, автора оригинала",
        L"wfx_iOS %hs \x2013 aktualizacja wfx_iOS 1.3 autorstwa Domo, autora oryginału" },
    [S_RESTART_NEEDED] = {
        L"The log file and language settings take full effect after Total Commander is restarted.",
        L"Налаштування журналу і мови повністю набудуть чинності після перезапуску Total Commander.",
        L"Настройки журнала и языка полностью вступят в силу после перезапуска Total Commander.",
        L"Ustawienia dziennika i języka zaczną w pełni obowiązywać po ponownym uruchomieniu Total Commandera." },

    /* ---- file properties ---- */
    [S_PROP_TITLE]    = { L"Properties of %s", L"Властивості: %s", L"Свойства: %s", L"Właściwości: %s" },
    [S_PROP_NAME]     = { L"Name:", L"Назва:", L"Имя:", L"Nazwa:" },
    [S_PROP_PATH]     = { L"On device:", L"На пристрої:", L"На устройстве:", L"Na urządzeniu:" },
    [S_PROP_TYPE]     = { L"Type:", L"Тип:", L"Тип:", L"Typ:" },
    [S_PROP_SIZE]     = { L"Size:", L"Розмір:", L"Размер:", L"Rozmiar:" },
    [S_PROP_CREATED]  = { L"Created:", L"Створено:", L"Создан:", L"Utworzono:" },
    [S_PROP_MODIFIED] = { L"Modified:", L"Змінено:", L"Изменён:", L"Zmodyfikowano:" },
    [S_PROP_LINKS]    = { L"Hard links:", L"Посилань:", L"Ссылок:", L"Dowiązania:" },
    [S_PROP_BLOCKS]   = { L"Blocks:", L"Блоків:", L"Блоков:", L"Bloki:" },
    [S_PROP_TARGET]   = { L"Link target:", L"Ціль посилання:", L"Цель ссылки:", L"Cel dowiązania:" },
    [S_ENTRIES]       = { L"%d entries", L"елементів: %d", L"элементов: %d", L"elementów: %d" },
    [S_SIZE_FORMAT]   = { L"%s (%I64u bytes)", L"%s (%I64u байт)", L"%s (%I64u байт)", L"%s (%I64u bajtów)" },
    [S_TYPE_FILE]     = { L"File", L"Файл", L"Файл", L"Plik" },
    [S_TYPE_DIR]      = { L"Directory", L"Каталог", L"Каталог", L"Katalog" },
    [S_TYPE_LINK]     = { L"Symbolic link", L"Символьне посилання", L"Символическая ссылка", L"Dowiązanie symboliczne" },
    [S_TYPE_BLOCK]    = { L"Block device", L"Блоковий пристрій", L"Блочное устройство", L"Urządzenie blokowe" },
    [S_TYPE_DEVICE]   = { L"Device", L"Пристрій", L"Устройство", L"Urządzenie" },
};

/* Language code from the ini file -> index, or -1 for "same as Windows". */
static int language_from_code(const char* code)
{
    if (_stricmp(code, "en") == 0) return LANG_EN;
    if (_stricmp(code, "uk") == 0) return LANG_UK;
    if (_stricmp(code, "ru") == 0) return LANG_RU;
    if (_stricmp(code, "pl") == 0) return LANG_PL;
    return -1;
}

/* Chooses the language: the Language= setting, else the Windows display language. */
void lang_load(void)
{
    char code[16];
    int language;

    ini_get_text("Language", code, sizeof(code));
    language = language_from_code(code);
    if (language < 0) {
        switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_UKRAINIAN: language = LANG_UK; break;
        case LANG_RUSSIAN:   language = LANG_RU; break;
        case LANG_POLISH:    language = LANG_PL; break;
        default:             language = LANG_EN; break;
        }
    }
    g_language = language;
}

/* The text for 'id' in the current language; English when a translation is missing. */
const WCHAR* tr(StringId id)
{
    const WCHAR* text;

    if (id < 0 || id >= S_COUNT) return L"";
    text = g_text[id][g_language];
    if (!text) text = g_text[id][LANG_EN];
    return text ? text : L"";
}
