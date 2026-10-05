// Lightweight internationalisation (i18n) for the installer page.
// Supports English (default) and Russian, with auto-detection and persistence.

export const TRANSLATIONS = {
  en: {
    hero_eyebrow: 'NEBADASSWIFTY / NINTENDO SWITCH',
    hero_title: 'Mass Effect',
    hero_subtitle: 'Your disc. Your galaxy.',
    hero_description: 'Prepare the port on your computer, directly in your browser.',
    releases_link: 'Releases ↗',
    lead_text: 'Make the SD card package of the Mass Effect port from <strong>your own copy</strong> of the Xbox 360 game. Everything runs in this browser tab: <strong>your game files never leave your computer.</strong>',

    step_1_title: 'Choose your game',
    format_iso: 'Disc image (.iso)',
    format_folder: 'XEX format (folder)',
    format_hint_iso: 'The disc image of the Xbox 360 game (single or two discs). It is read in place and never loaded as a whole.',
    format_hint_folder: 'The extracted disc: the folder that contains default.xex next to the game\'s folders (Layer0, Layer1...). It is read in place.',
    pick_iso: 'Choose .iso file(s)',
    pick_folder: 'Choose folder',
    drop_or: 'or drop it here',

    status_reading: 'Reading the disc...',
    status_checking: 'Checking default.xex...',
    status_folder_listing: 'Listing the folder: {count} files...',

    step_2_title: 'Detected edition',
    edition_label: 'Edition',
    badge_supported: 'supported',
    badge_unverified: 'unverified variant',
    badge_not_supported: 'not supported',
    game_files_label: 'Game files',
    files_summary: '{count} files, {bytes} go into game_root',
    left_out_label: 'Left out',
    left_out_summary: '{count} files, {bytes} that the game never reads ({prefixes})',
    unverified_notice_title: 'Notice: unverified variant of {name}.',
    unverified_notice_body: 'The SHA-256 hash ({hash}) is not in the list of verified builds, but the XEX2 header signature matches this edition. You may proceed to install at your own risk.',
    not_supported_msg: 'This default.xex is not one of the editions the port is built for.',
    invalid_xex_msg: 'Invalid default.xex: {error}.',
    supported_editions_intro: 'The port is the recompiled program of one exact executable, so it cannot run with another. Supported editions:',
    check_original_hint: 'Check that you picked an original, unmodified dump of the game (not a title-update patched, trimmed or re-authored one, and not another region). If you believe your edition should work, open an issue and include the SHA-256 above.',
    open_issue: 'Open an issue',

    step_3_title: 'Create the package',
    create_full_btn: 'Create masseffect-nx.zip',
    create_update_btn: 'Create masseffect-nx-update.zip',
    create_hint: '<strong>First install:</strong> the full zip with the game files. <strong>Update</strong> (a new release): only the program, the settings file and the shaders, to extract over the existing folder. It needs the same disc selected, because the shaders are made from it. The update overwrites <code>masseffect.toml</code>: keep a copy if you changed it.',
    create_estimate: 'The full zip will be about <strong>{full}</strong>; the update zip about <strong>{upd}</strong>. Making the shaders is the long part (30,000 shaders are translated on your computer): expect a long wait, and keep this tab open and in the foreground.',
    memory_warning: 'Warning: this browser can only build the zip in memory and {size} will very likely not fit. Use Chrome or Edge, or make only the update zip.',
    cancel_btn: 'Cancel',
    details_summary: 'Details',

    stage_manifest: 'Checking build manifest',
    stage_download: 'Downloading build',
    stage_scan: 'Scanning packages for shaders',
    stage_translate: 'Translating shaders to SPIR-V',
    stage_pack: 'Packing shader library',
    stage_zip: 'Writing the zip',

    how_title: 'After it finishes',
    how_desc: 'Extract the zip into <code>sdmc:/switch/</code> on the SD card. You get:',
    download_nsp_btn: 'Download the NSP launcher',
    nsp_instruction: 'Install the NSP with your CFW title installer to launch Mass Effect from the HOME menu. It uses 39-bit application mode and needs the files above on the SD card. You can also launch the NRO with a title override (hold R while starting an installed game).',

    privacy_title: 'Privacy',
    privacy_desc: 'The page reads your disc with your browser\'s own file access, makes the shaders on your computer with WebAssembly and writes the zip to your disk. The only network traffic is loading this page and downloading the build (<code>masseffect-nx.nro</code>) and <code>masseffect.toml</code> from this site. No game data is uploaded, and there is no tracking. Nothing from the game is distributed with the port.',

    credits_title: 'Credits & Acknowledgments',
    credits_bioware: '<strong>BioWare & Electronic Arts</strong> — Original creators of Mass Effect. This is an unofficial, non-commercial fan project.',
    credits_stevens: '<strong><a href="https://github.com/StevensND" target="_blank" rel="noopener">StevensND</a></strong> — Creator of <a href="https://github.com/StevensND/NFSMW-NX" target="_blank" rel="noopener">NFSMW-NX</a>, whose pioneering work on running statically recompiled Xbox 360 games on Nintendo Switch and client-side browser shader packaging served as the foundational inspiration for this installer. <em>(Note: StevensND is not affiliated with or responsible for this Mass Effect project.)</em>',
    credits_rexglue: '<strong><a href="https://github.com/rexglue/rexglue-sdk" target="_blank" rel="noopener">Tom Clay & ReXGlue SDK</a></strong> — The static binary translation framework for Xbox 360 executables.',
    credits_xenos: '<strong><a href="https://github.com/hedge-dev/XenosRecomp" target="_blank" rel="noopener">hedge-dev</a></strong> — XenosRecomp, the Xbox 360 Xenos shader translator.',
    credits_mesa: '<strong><a href="https://github.com/danfromtico/mesa-switch" target="_blank" rel="noopener">danfromtico & NaGaa95</a></strong> — mesa-switch: Vulkan/NVK implementation on Horizon OS.',
    credits_devkit: '<strong><a href="https://devkitpro.org" target="_blank" rel="noopener">devkitPro</a> & <a href="https://github.com/switchbrew/libnx" target="_blank" rel="noopener">switchbrew</a></strong> — Nintendo Switch homebrew toolchain and runtime libraries.',

    footer_source: 'Source code and releases',
    footer_by: 'by',
    footer_credits: 'Credits',
    footer_notices: 'Third-party notices',
    footer_disclaimer: 'Not affiliated with BioWare, Electronic Arts or Nintendo.',
  },

  ru: {
    hero_eyebrow: 'NEBADASSWIFTY / NINTENDO SWITCH',
    hero_title: 'Mass Effect',
    hero_subtitle: 'Ваш диск. Ваша галактика.',
    hero_description: 'Подготовьте порт на вашем компьютере прямо в браузере.',
    releases_link: 'Релизы ↗',
    lead_text: 'Соберите пакет для SD-карты с портом Mass Effect из <strong>вашей собственной копии</strong> игры для Xbox 360. Всё работает прямо в этой вкладке браузера: <strong>файлы вашей игры никогда не покидают ваш компьютер.</strong>',

    step_1_title: 'Выберите вашу игру',
    format_iso: 'Образ диска (.iso)',
    format_folder: 'Папка XEX (распакованный диск)',
    format_hint_iso: 'Образ диска игры для Xbox 360 (один или два файла .iso). Читается напрямую с диска и не загружается в память целиком.',
    format_hint_folder: 'Распакованный диск: папка, содержащая default.xex рядом с игровыми папками (Layer0, Layer1...). Читается напрямую.',
    pick_iso: 'Выбрать файл(ы) .iso',
    pick_folder: 'Выбрать папку',
    drop_or: 'или перетащите сюда',

    status_reading: 'Чтение диска...',
    status_checking: 'Проверка default.xex...',
    status_folder_listing: 'Чтение папки: {count} файлов...',

    step_2_title: 'Распознанное издание',
    edition_label: 'Издание',
    badge_supported: 'поддерживается',
    badge_unverified: 'непроверенный вариант',
    badge_not_supported: 'не поддерживается',
    game_files_label: 'Файлы игры',
    files_summary: '{count} файлов, {bytes} идут в game_root',
    left_out_label: 'Исключено',
    left_out_summary: '{count} файлов, {bytes}, которые игра никогда не читает ({prefixes})',
    unverified_notice_title: 'Внимание: непроверенный вариант {name}.',
    unverified_notice_body: 'Хэш SHA-256 ({hash}) отсутствует в списке проверенных релизов, но сигнатура заголовка XEX2 соответствует этому изданию. Вы можете продолжить установку под свою ответственность.',
    not_supported_msg: 'Этот default.xex не относится к поддерживаемым изданиям порта.',
    invalid_xex_msg: 'Некорректный default.xex: {error}.',
    supported_editions_intro: 'Порт представляет собой статически перекомпилированную программу конкретного исполняемого файла. Поддерживаемые издания:',
    check_original_hint: 'Убедитесь, что выбран оригинальный дамп игры (не патченный обновлениями, не урезанный и не из другого региона). Если вы считаете, что ваше издание должно работать, создайте issue с указанием SHA-256 выше.',
    open_issue: 'Открыть issue',

    step_3_title: 'Создание пакета',
    create_full_btn: 'Создать masseffect-nx.zip',
    create_update_btn: 'Создать masseffect-nx-update.zip',
    create_hint: '<strong>Первая установка:</strong> полный zip-архив с файлами игры. <strong>Обновление</strong> (новый релиз): только исполняемый файл, настройки и шейдеры для распаковки поверх существующей папки. Для обновления также требуется диск, так как шейдеры собираются из него. Обновление перезаписывает <code>masseffect.toml</code>: сохраните копию, если меняли настройки.',
    create_estimate: 'Полный zip будет весить около <strong>{full}</strong>; zip обновления около <strong>{upd}</strong>. Компиляция шейдеров занимает больше всего времени (30,000 шейдеров транслируются на вашем ПК): держите вкладку открытой на переднем плане.',
    memory_warning: 'Предупреждение: этот браузер может собирать zip только в оперативной памяти, и {size} почти наверняка не поместятся. Используйте Chrome или Edge, либо создавайте только zip обновления.',
    cancel_btn: 'Отмена',
    details_summary: 'Подробности',

    stage_manifest: 'Проверка манифеста сборки',
    stage_download: 'Скачивание сборки',
    stage_scan: 'Сканирование пакетов на наличие шейдеров',
    stage_translate: 'Трансляция шейдеров в SPIR-V',
    stage_pack: 'Упаковка библиотеки шейдеров',
    stage_zip: 'Запись zip-архива',

    how_title: 'После завершения',
    how_desc: 'Распакуйте полученный zip-архив в <code>sdmc:/switch/</code> на SD-карте. В итоге получится:',
    download_nsp_btn: 'Скачать NSP-форвардер',
    nsp_instruction: 'Установите NSP через ваш установщик тайтлов в CFW, чтобы запускать Mass Effect прямо из главного меню HOME. Форвардер использует 39-битное адресное пространство и требует наличия указанных файлов на SD-карте. Вы также можете запускать NRO через Title Override (зажав R при запуске любой установленной игры).',

    privacy_title: 'Конфиденциальность',
    privacy_desc: 'Страница читает ваш диск через стандартный доступ к файлам браузера, компилирует шейдеры на вашем компьютере с помощью WebAssembly и записывает zip на ваш диск. Единственный сетевой трафик — загрузка этой страницы и скачивание сборки (<code>masseffect-nx.nro</code>) и <code>masseffect.toml</code> с этого сайта. Данные игры никуда не отправляются. Игра не распространяется вместе с портом.',

    credits_title: 'Благодарности',
    credits_bioware: '<strong>BioWare & Electronic Arts</strong> — Оригинальные создатели Mass Effect. Это неофициальный некоммерческий фанатский проект.',
    credits_stevens: '<strong><a href="https://github.com/StevensND" target="_blank" rel="noopener">StevensND</a></strong> — Создатель <a href="https://github.com/StevensND/NFSMW-NX" target="_blank" rel="noopener">NFSMW-NX</a>, чья новаторская работа над статически рекомпилированными играми Xbox 360 для Nintendo Switch и браузерной сборкой шейдеров послужила вдохновением для этого установщика.',
    credits_rexglue: '<strong><a href="https://github.com/rexglue/rexglue-sdk" target="_blank" rel="noopener">Tom Clay & ReXGlue SDK</a></strong> — Фреймворк статической бинарной трансляции исполняемых файлов Xbox 360.',
    credits_xenos: '<strong><a href="https://github.com/hedge-dev/XenosRecomp" target="_blank" rel="noopener">hedge-dev</a></strong> — XenosRecomp, транслятор шейдеров Xbox 360 Xenos.',
    credits_mesa: '<strong><a href="https://github.com/danfromtico/mesa-switch" target="_blank" rel="noopener">danfromtico & NaGaa95</a></strong> — mesa-switch: реализация Vulkan/NVK под Horizon OS.',
    credits_devkit: '<strong><a href="https://devkitpro.org" target="_blank" rel="noopener">devkitPro</a> & <a href="https://github.com/switchbrew/libnx" target="_blank" rel="noopener">switchbrew</a></strong> — Тулчейн и рантайм-библиотеки для homebrew на Nintendo Switch.',

    footer_source: 'Исходный код и релизы',
    footer_by: 'автор:',
    footer_credits: 'Благодарности',
    footer_notices: 'Уведомления сторонних библиотек',
    footer_disclaimer: 'Не связано с BioWare, Electronic Arts или Nintendo.',
  },
};

let currentLang = 'en';

export function getLanguage() {
  return currentLang;
}

export function initLanguage() {
  const saved = localStorage.getItem('masseffect_lang');
  if (saved && (saved === 'ru' || saved === 'en')) {
    currentLang = saved;
  } else if (navigator.language && navigator.language.toLowerCase().startsWith('ru')) {
    currentLang = 'ru';
  } else {
    currentLang = 'en';
  }
  return currentLang;
}

export function setLanguage(lang) {
  if (lang !== 'en' && lang !== 'ru') return;
  currentLang = lang;
  try {
    localStorage.setItem('masseffect_lang', lang);
  } catch (_) {}
}

export function t(key, params = {}) {
  const dict = TRANSLATIONS[currentLang] || TRANSLATIONS.en;
  let str = dict[key] || TRANSLATIONS.en[key] || key;
  for (const [k, v] of Object.entries(params)) {
    str = str.replaceAll(`{${k}}`, v);
  }
  return str;
}
