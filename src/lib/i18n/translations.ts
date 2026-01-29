import type { Language } from "@/lib/stores/types";

export type TranslationValue =
  | string
  | {
      zero?: string;
      one?: string;
      few?: string;
      many?: string;
      other?: string;
    };

export type TranslationDictionary = Record<string, TranslationValue>;

export const supportedLanguages: Language[] = ["en", "ru"];
export const fallbackLanguage: Language = "en";

export const translations: Record<Language, TranslationDictionary> = {
  en: {
    "common.on": "On",
    "common.off": "Off",
    "common.yes": "Yes",
    "common.no": "No",
    "common.enabled": "Enabled",
    "common.disabled": "Disabled",
    "common.cancel": "Cancel",
    "common.create": "Create",
    "common.save": "Save",
    "common.reset": "Reset",
    "common.change": "Change",
    "common.choose": "Choose",
    "language.en": "English",
    "language.ru": "Russian",
    "settings.title": "Settings",
    "settings.changeRoot": "Change root",
    "settings.decoyGallery": "Decoy gallery",
    "settings.pickDecoy": "Pick decoy root",
    "settings.changeDecoy": "Change decoy root",
    "settings.preventScreenshots": "Prevent screenshots",
    "settings.screenshotHint":
      "Enable privacy features to turn on screenshot protection.",
    "settings.advanced": "Advanced settings",
    "settings.language": "Language",
    "directory.title": "Choose root directory for your gallery",
    "directory.subtitle":
      "All HEIC files in it will be converted to PNG. The metadata for files and albums will appear in the hidden files.",
    "directory.choose": "Choose directory",
    "lock.title": "FBI locked us out",
    "lock.subtitle":
      "Unfortunately we cannot risk you seeing the media files in this gallery.",
    "albums.hashing": "Hashing images...",
    "album.new": "New album",
    "album.placeholder": "Album name",
    "album.create": "Create",
    "album.cancel": "Cancel",
    "hotkeys.selectPhoto": "select photo",
    "hotkeys.selectAll": "select all",
    "hotkeys.clearSelection": "clear selection",
    "hotkeys.copyToClipboard": "copy to clipboard",
    "hotkeys.pasteFromClipboard": "paste from clipboard",
    "hotkeys.deleteSelection": "delete selection",
    "hotkeys.moreActions": "context menu",
    "hotkeys.show": "Show hotkeys",
    "hotkeys.hide": "Hide hotkeys",
    "media.selectAlbum": "Select an album to view",
    "media.empty.title": "This album is empty",
    "media.empty.subtitle": "Drop a photo here to upload",
    "preload.title": "Indexing your library",
    "preload.conversion": "HEIC to JPEG Conversion",
    "preload.thumbnails": "Thumbnails",
    "preload.metadata": "Metadata",
    "layout.grid": "Grid",
    "layout.masonry": "Masonry",
    "layout.apple": "Apple-Style",
    "duplicates.loading": "Looking for duplicates...",
    "duplicates.show": "Show Duplicates",
    "duplicates.hide": "Hide Duplicates",
    "duplicates.markNonDuplicates": "Mark as non-duplicates",
    "duplicates.none": "No duplicates found.",
    "album.deleteTitle": "Delete album",
    "album.deleteConfirm":
      "Are you sure you want to delete the album <b>{{album}}</b>?",
    "album.deleteWarning": "This action cannot be undone.",
    "favorites.show": "Show favorites",
    "favorites.showAll": "Show all",
    "media.unknownDate": "Unknown Date",
    "media.copyFailed": "Failed to copy image",
    "contextMenu.copyFile": "Copy file",
    "contextMenu.copyFiles": {
      one: "Copy {{count}} file",
      few: "Copy {{count}} files",
      many: "Copy {{count}} files",
      other: "Copy {{count}} files",
    },
    "contextMenu.copyImage": "Copy image",
    "contextMenu.copyPath": "Copy path",
    "contextMenu.revealIn": "Reveal in {{fileManager}}",
    "contextMenu.changeDate": "Change date",
    "contextMenu.changeDateFor": {
      one: "Change date for {{count}} item",
      few: "Change date for {{count}} items",
      many: "Change date for {{count}} items",
      other: "Change date for {{count}} items",
    },
    "contextMenu.moveToAlbum": "Move to album",
    "contextMenu.moveItems": {
      one: "Move {{count}} item to...",
      few: "Move {{count}} items to...",
      many: "Move {{count}} items to...",
      other: "Move {{count}} items to...",
    },
    "contextMenu.delete": "Delete",
    "contextMenu.deleteItems": {
      one: "Delete {{count}} item?",
      few: "Delete {{count}} items?",
      many: "Delete {{count}} items?",
      other: "Delete {{count}} items?",
    },
    "contextMenu.noOtherAlbums": "No other albums",
    "contextMenu.cancel": "Cancel",
    "contextMenu.save": "Save",
    "sort.shoot": "EXIF Date",
    "sort.added": "Added Date",
    "sort.name": "Name",
    "sort.random": "Random",
    "toast.addingToAlbum": {
      one: "Uploading {{count}} file...",
      few: "Uploading {{count}} files...",
      many: "Uploading {{count}} files...",
      other: "Uploading {{count}} files...",
    },
    "toast.addedFiles": {
      one: "Added {{count}} file",
      few: "Added {{count}} files",
      many: "Added {{count}} files",
      other: "Added {{count}} files",
    },
    "toast.noFilesAdded": "No files were added",
    "toast.failedAdd": "Failed to add files",
    "toast.failedDate": "Failed to update date",
    "toast.moveMedia.loading": {
      one: "Moving {{count}} media file...",
      few: "Moving {{count}} media files...",
      many: "Moving {{count}} media files...",
      other: "Moving {{count}} media files...",
    },
    "toast.moveMedia.failed": {
      one: "Failed to move {{count}} media file. Check the console for details.",
      few: "Failed to move {{count}} media files. Check the console for details.",
      many: "Failed to move {{count}} media files. Check the console for details.",
      other:
        "Failed to move {{count}} media files. Check the console for details.",
    },
    "toast.moveMedia.success": {
      one: "Moved {{count}} media file successfully.",
      few: "Moved {{count}} media files successfully.",
      many: "Moved {{count}} media files successfully.",
      other: "Moved {{count}} media files successfully.",
    },
    "toast.deleteMedia.loading": {
      one: "Deleting {{count}} media file...",
      few: "Deleting {{count}} media files...",
      many: "Deleting {{count}} media files...",
      other: "Deleting {{count}} media files...",
    },
    "toast.deleteMedia.failed": {
      one: "Failed to delete {{count}} media file. Check the console for details.",
      few: "Failed to delete {{count}} media files. Check the console for details.",
      many: "Failed to delete {{count}} media files. Check the console for details.",
      other:
        "Failed to delete {{count}} media files. Check the console for details.",
    },
    "toast.deleteMedia.success": {
      one: "Deleted {{count}} media file.",
      few: "Deleted {{count}} media files.",
      many: "Deleted {{count}} media files.",
      other: "Deleted {{count}} media files.",
    },
    "confirmOpen.title": "We found your library",
    "confirmOpen.subtitle":
      "Just making sure you want to open <b>{{folder}}</b>. It might contain sensitive media files.",
    "confirmOpen.open": "Open",
    "confirmOpen.pickAnother": "Pick another root",
    "advanced.trigger": "Advanced settings",
    "advanced.title": "Advanced Settings",
    "advanced.disclaimer":
      "Some of these settings are intended for advanced users. Changing them may affect application performance or behavior. Please proceed with caution.",
    "advanced.saveSuccess": "Advanced settings saved",
    "advanced.saveError": "Failed to save settings",
    "advanced.resetAll": "Reset all",
    "advanced.cancel": "Cancel",
    "advanced.save": "Save",
    "advanced.resetSection": "Reset section",
    "advanced.resetDefaults": "Settings reset to defaults",
    "advanced.section.privacy": "Privacy",
    "advanced.section.duplicates": "Duplicates & Hashing",
    "advanced.section.thumbnails": "Thumbnails",
    "advanced.section.ffmpeg": "FFmpeg",
    "advanced.section.preload": "Preload",
    "advanced.section.metadata": "Metadata",
    "advanced.section.album": "Albums",
    "advanced.field.privacyFeatures": "Privacy features",
    "advanced.field.privacyFeatures.helper":
      "Adds privacy-focused UI and protections like decoy galleries and blurs.",
    "advanced.field.lockscreen": "Lockscreen",
    "advanced.field.lockscreen.helper":
      "Allow lock/unlock shortcuts and auto-lock even when privacy features are off. Always on when privacy is enabled.",
    "advanced.field.confirmOpen": "Confirm before opening",
    "advanced.field.confirmOpen.helper":
      "Require confirmation before loading your library. Always on when privacy is enabled.",
    "advanced.field.duplicates.threshold": "Threshold",
    "advanced.field.duplicates.threshold.helper":
      "Lower value means that less duplicates found.",
    "advanced.field.duplicates.hashSize": "Hash size",
    "advanced.field.duplicates.hashSize.helper":
      "Dimensions of the perceptual hash.",
    "advanced.field.duplicates.algorithm": "Algorithm",
    "advanced.field.duplicates.algorithm.helper":
      "Used to compute image hashes to find duplicates.",
    "advanced.field.duplicates.resizeFilter": "Resize filter",
    "advanced.field.duplicates.resizeFilter.helper":
      "Filter used when resizing images for hashing.",
    "advanced.field.duplicates.useThumbsFirst": "Use thumbnails first",
    "advanced.field.duplicates.useThumbsFirst.helper":
      "Hash thumbnails first to speed up duplicate detection.",
    "advanced.field.duplicates.maxPerAlbum": "Max files per album",
    "advanced.field.duplicates.maxPerAlbum.helper":
      "Max number of duplicate files to list per album.",
    "advanced.field.thumbs.maxDim": "Max dimension (px)",
    "advanced.field.thumbs.maxDim.helper":
      "Maximum width or height of generated thumbnails.",
    "advanced.field.thumbs.webpQuality": "WebP quality",
    "advanced.field.thumbs.webpQuality.helper":
      "Quality of generated WebP thumbnails.",
    "advanced.field.thumbs.webpCompression": "WebP compression level",
    "advanced.field.thumbs.webpCompression.helper":
      "Compression level for thumbnails (0=fastest, 9=slowest).",
    "advanced.field.thumbs.videoSeek": "Video seek (s)",
    "advanced.field.thumbs.videoSeek.helper":
      "Time position for video thumbnails.",
    "advanced.field.thumbs.lockPoll": "Lock poll (ms)",
    "advanced.field.thumbs.lockPoll.helper":
      "Interval to check for thumbnail lock release.",
    "advanced.field.ffmpeg.threads": "Threads",
    "advanced.field.ffmpeg.threads.helper": "Number of threads FFmpeg can use.",
    "advanced.field.ffmpeg.timeout": "Timeout (s)",
    "advanced.field.ffmpeg.timeout.helper":
      "Timeout for FFmpeg operations in seconds.",
    "advanced.field.ffmpeg.hwaccel": "Hardware acceleration",
    "advanced.field.ffmpeg.hwaccel.helper":
      "Hardware acceleration method for FFmpeg.",
    "advanced.field.ffmpeg.processPoll": "Process wait poll (ms)",
    "advanced.field.ffmpeg.processPoll.helper":
      "Interval to poll FFmpeg process status.",
    "advanced.field.preload.thumbWorkers": "Thumb workers",
    "advanced.field.preload.thumbWorkers.helper":
      "Number of concurrent thumbnail preload workers.",
    "advanced.field.preload.metaWorkers": "Metadata workers",
    "advanced.field.preload.metaWorkers.helper":
      "Number of concurrent metadata preload workers.",
    "advanced.field.preload.hashWorkers": "Hash workers",
    "advanced.field.preload.hashWorkers.helper":
      "Number of concurrent hashing preload workers.",
    "advanced.field.preload.progressEmit": "Progress emit (ms)",
    "advanced.field.preload.progressEmit.helper":
      "Interval to emit preload progress updates.",
    "advanced.field.preload.hashDelay": "Hash queue delay (ms)",
    "advanced.field.preload.hashDelay.helper":
      "Delay between processing items in the hash queue.",
    "advanced.field.preload.hashAfter": "Hash only after preload",
    "advanced.field.preload.hashAfter.helper":
      "Only compute thumb hashes after preloading.",
    "advanced.field.preload.hashRetry": "Retry on thumb change",
    "advanced.field.preload.hashRetry.helper":
      "Retry computing hash if thumbnail changes during preload.",
    "advanced.field.metadata.probeTimeout": "Probe timeout (s)",
    "advanced.field.metadata.probeTimeout.helper":
      "Timeout for probing media metadata.",
    "advanced.field.metadata.parseCreation": "Parse creation_time",
    "advanced.field.metadata.parseCreation.helper":
      "Extract creation time from media metadata when available.",
    "advanced.field.album.renameDelay": "Rename cleanup delay (s)",
    "advanced.field.album.renameDelay.helper":
      "Delay before cleaning up after renaming media.",
    "advanced.field.album.moveThumbs": "Move thumbs/meta with media",
    "advanced.field.album.moveThumbs.helper":
      "Move associated thumbnails and metadata.",
  },
  ru: {
    "common.on": "Вкл",
    "common.off": "Выкл",
    "common.yes": "Да",
    "common.no": "Нет",
    "common.enabled": "Включено",
    "common.disabled": "Выключено",
    "common.cancel": "Отмена",
    "common.create": "Создать",
    "common.save": "Сохранить",
    "common.reset": "Сбросить",
    "common.change": "Сменить",
    "common.choose": "Выбрать",
    "language.en": "Английский",
    "language.ru": "Русский",
    "settings.title": "Настройки",
    "settings.changeRoot": "Сменить корень",
    "settings.decoyGallery": "Фальшивая галерея",
    "settings.pickDecoy": "Выбрать фальш-галерею",
    "settings.changeDecoy": "Сменить фальш-галерею",
    "settings.preventScreenshots": "Защита от скриншотов",
    "settings.screenshotHint":
      "Включи режим приватности, чтобы активировать защиту от скриншотов.",
    "settings.advanced": "Расширенные настройки",
    "settings.language": "Язык",
    "directory.title": "Выбери корень для галереи",
    "directory.subtitle":
      "Все HEIC-файлы в ней будут конвертированы в PNG. Метаданные файлов и альбомов сохранятся в скрытых файлах.",
    "directory.choose": "Выбрать папку",
    "lock.title": "КГБ закрыл нам доступ",
    "lock.subtitle":
      "К сожалению, мы не можем рисковать показом файлов в этой галерее.",
    "albums.hashing": "Считаем хэши изображений...",
    "album.new": "Новый альбом",
    "album.placeholder": "Название альбома",
    "album.create": "Создать",
    "album.cancel": "Отмена",
    "hotkeys.selectPhoto": "выбрать фото",
    "hotkeys.selectAll": "выбрать всё",
    "hotkeys.clearSelection": "снять выделение",
    "hotkeys.copyToClipboard": "скопировать в буфер",
    "hotkeys.pasteFromClipboard": "вставить из буфера",
    "hotkeys.deleteSelection": "удалить выделенное",
    "hotkeys.moreActions": "контекстное меню",
    "hotkeys.show": "Показать хоткеи",
    "hotkeys.hide": "Скрыть хоткеи",
    "media.selectAlbum": "Выбери альбом",
    "media.empty.title": "Альбом пуст",
    "media.empty.subtitle": "Перетащи фото сюда, чтобы загрузить",
    "preload.title": "Индексируем библиотеку",
    "preload.conversion": "Конвертация HEIC в JPEG",
    "preload.thumbnails": "Превью",
    "preload.metadata": "Метаданные",
    "sort.shoot": "Дата EXIF",
    "sort.added": "Дата добавления",
    "sort.name": "Имя",
    "sort.random": "Случайно",
    "toast.addingToAlbum": {
      one: "Загружаем {{count}} файл...",
      few: "Загружаем {{count}} файла...",
      many: "Загружаем {{count}} файлов...",
      other: "Загружаем {{count}} файла...",
    },
    "toast.addedFiles": {
      one: "Добавлен {{count}} файл",
      few: "Добавлено {{count}} файла",
      many: "Добавлено {{count}} файлов",
      other: "Добавлено {{count}} файла",
    },
    "toast.noFilesAdded": "Не удалось добавить файлы",
    "toast.failedAdd": "Не удалось добавить файлы",
    "toast.failedDate": "Не удалось обновить дату",
    "toast.moveMedia.loading": {
      one: "Перемещаем {{count}} файл...",
      few: "Перемещаем {{count}} файла...",
      many: "Перемещаем {{count}} файлов...",
      other: "Перемещаем {{count}} файла...",
    },
    "toast.moveMedia.failed": {
      one: "Не удалось переместить {{count}} файл. Подробности в консоли.",
      few: "Не удалось переместить {{count}} файла. Подробности в консоли.",
      many: "Не удалось переместить {{count}} файлов. Подробности в консоли.",
      other: "Не удалось переместить {{count}} файла. Подробности в консоли.",
    },
    "toast.moveMedia.success": {
      one: "Перемещён {{count}} файл.",
      few: "Перемещено {{count}} файла.",
      many: "Перемещено {{count}} файлов.",
      other: "Перемещено {{count}} файла.",
    },
    "toast.deleteMedia.loading": {
      one: "Удаляем {{count}} файл...",
      few: "Удаляем {{count}} файла...",
      many: "Удаляем {{count}} файлов...",
      other: "Удаляем {{count}} файла...",
    },
    "toast.deleteMedia.failed": {
      one: "Не удалось удалить {{count}} файл. Подробности в консоли.",
      few: "Не удалось удалить {{count}} файла. Подробности в консоли.",
      many: "Не удалось удалить {{count}} файлов. Подробности в консоли.",
      other: "Не удалось удалить {{count}} файла. Подробности в консоли.",
    },
    "toast.deleteMedia.success": {
      one: "Удалён {{count}} файл.",
      few: "Удалено {{count}} файла.",
      many: "Удалено {{count}} файлов.",
      other: "Удалено {{count}} файла.",
    },
    "layout.grid": "Квадратики",
    "layout.masonry": "Пинтерест",
    "layout.apple": "Стиль Apple",
    "duplicates.loading": "Ищем дубликаты...",
    "duplicates.show": "Показать дубликаты",
    "duplicates.hide": "Скрыть дубликаты",
    "duplicates.markNonDuplicates": "Отметить как разные",
    "duplicates.none": "Дубликатов не найдено.",
    "album.deleteTitle": "Удалить альбом",
    "album.deleteConfirm": "Точно удалить альбом <b>{{album}}</b>?",
    "album.deleteWarning": "Это действие нельзя отменить.",
    "favorites.show": "Показывать избранное",
    "favorites.showAll": "Показать все",
    "media.unknownDate": "Неизвестная дата",
    "media.copyFailed": "Не удалось скопировать изображение",
    "contextMenu.copyFile": "Скопировать файл",
    "contextMenu.copyFiles": {
      one: "Скопировать {{count}} файл",
      few: "Скопировать {{count}} файла",
      many: "Скопировать {{count}} файлов",
      other: "Скопировать {{count}} файла",
    },
    "contextMenu.copyImage": "Скопировать изображение",
    "contextMenu.copyPath": "Скопировать путь",
    "contextMenu.revealIn": "Показать в {{fileManager}}",
    "contextMenu.changeDate": "Изменить дату",
    "contextMenu.changeDateFor": {
      one: "Изменить дату для {{count}} элемента",
      few: "Изменить дату для {{count}} элементов",
      many: "Изменить дату для {{count}} элементов",
      other: "Изменить дату для {{count}} элементов",
    },
    "contextMenu.moveToAlbum": "Переместить в альбом",
    "contextMenu.moveItems": {
      one: "Переместить {{count}} элемент в...",
      few: "Переместить {{count}} элемента в...",
      many: "Переместить {{count}} элементов в...",
      other: "Переместить {{count}} элемента в...",
    },
    "contextMenu.delete": "Удалить",
    "contextMenu.deleteItems": {
      one: "Удалить {{count}} элемент?",
      few: "Удалить {{count}} элемента?",
      many: "Удалить {{count}} элементов?",
      other: "Удалить {{count}} элемента?",
    },
    "contextMenu.noOtherAlbums": "Нет других альбомов",
    "contextMenu.cancel": "Отмена",
    "contextMenu.save": "Сохранить",
    "confirmOpen.title": "Мы нашли твою библиотеку",
    "confirmOpen.subtitle":
      "Убедимся, что ты хочешь открыть <b>{{folder}}</b>. Там могут быть личные файлы.",
    "confirmOpen.open": "Открыть",
    "confirmOpen.pickAnother": "Выбрать другой корень",
    "advanced.trigger": "Расширенные настройки",
    "advanced.title": "Расширенные настройки",
    "advanced.disclaimer":
      "Некоторые параметры рассчитаны на опытных пользователей. Изменения могут повлиять на производительность или поведение приложения. Действуй осторожно.",
    "advanced.saveSuccess": "Расширенные настройки сохранены",
    "advanced.saveError": "Не удалось сохранить настройки",
    "advanced.resetAll": "Сбросить все",
    "advanced.cancel": "Отмена",
    "advanced.save": "Сохранить",
    "advanced.resetSection": "Сбросить раздел",
    "advanced.resetDefaults": "Настройки сброшены к значениям по умолчанию",
    "advanced.section.privacy": "Приватность",
    "advanced.section.duplicates": "Дубликаты и хэши",
    "advanced.section.thumbnails": "Превью",
    "advanced.section.ffmpeg": "FFmpeg",
    "advanced.section.preload": "Предзагрузка",
    "advanced.section.metadata": "Метаданные",
    "advanced.section.album": "Альбомы",
    "advanced.field.privacyFeatures": "Режим приватности",
    "advanced.field.privacyFeatures.helper":
      "Добавляет приватные элементы интерфейса и защиту (фальш-галерея, блюр и т.п.).",
    "advanced.field.lockscreen": "Экран блокировки",
    "advanced.field.lockscreen.helper":
      "Включает блокировку/разблокировку и авто-блокировку даже без режима приватности. Всегда активен, если приватность включена.",
    "advanced.field.confirmOpen": "Подтверждать открытие",
    "advanced.field.confirmOpen.helper":
      "Требовать подтверждение перед загрузкой библиотеки. Всегда активно при включенной приватности.",
    "advanced.field.duplicates.threshold": "Порог",
    "advanced.field.duplicates.threshold.helper":
      "Чем меньше значение, тем меньше дубликатов будет найдено.",
    "advanced.field.duplicates.hashSize": "Размер хэша",
    "advanced.field.duplicates.hashSize.helper": "Размер перцептивного хэша.",
    "advanced.field.duplicates.algorithm": "Алгоритм",
    "advanced.field.duplicates.algorithm.helper":
      "Используется для подсчёта хэшей изображений при поиске дубликатов.",
    "advanced.field.duplicates.resizeFilter": "Фильтр изменения размера",
    "advanced.field.duplicates.resizeFilter.helper":
      "Фильтр для ресайза изображений при хэшировании.",
    "advanced.field.duplicates.useThumbsFirst": "Сначала превью",
    "advanced.field.duplicates.useThumbsFirst.helper":
      "Сначала хэшировать превью, чтобы ускорить поиск дубликатов.",
    "advanced.field.duplicates.maxPerAlbum": "Максимум файлов на альбом",
    "advanced.field.duplicates.maxPerAlbum.helper":
      "Максимальное число найденных дубликатов на один альбом.",
    "advanced.field.thumbs.maxDim": "Максимальная сторона (px)",
    "advanced.field.thumbs.maxDim.helper":
      "Максимальная ширина или высота создаваемых превью.",
    "advanced.field.thumbs.webpQuality": "Качество WebP",
    "advanced.field.thumbs.webpQuality.helper":
      "Качество создаваемых превью в WebP.",
    "advanced.field.thumbs.webpCompression": "Уровень сжатия WebP",
    "advanced.field.thumbs.webpCompression.helper":
      "Уровень сжатия превью (0 - быстрее всего, 9 - медленнее всего).",
    "advanced.field.thumbs.videoSeek": "Позиция видео (с)",
    "advanced.field.thumbs.videoSeek.helper":
      "Момент времени для превью видео.",
    "advanced.field.thumbs.lockPoll": "Период блокировки (мс)",
    "advanced.field.thumbs.lockPoll.helper":
      "Интервал проверки снятия блокировки превью.",
    "advanced.field.ffmpeg.threads": "Потоки",
    "advanced.field.ffmpeg.threads.helper":
      "Число потоков, которое может использовать FFmpeg.",
    "advanced.field.ffmpeg.timeout": "Таймаут (с)",
    "advanced.field.ffmpeg.timeout.helper":
      "Таймаут операций FFmpeg в секундах.",
    "advanced.field.ffmpeg.hwaccel": "Аппаратное ускорение",
    "advanced.field.ffmpeg.hwaccel.helper":
      "Метод аппаратного ускорения для FFmpeg.",
    "advanced.field.ffmpeg.processPoll": "Период опроса процесса (мс)",
    "advanced.field.ffmpeg.processPoll.helper":
      "Интервал опроса статуса процесса FFmpeg.",
    "advanced.field.preload.thumbWorkers": "Потоки превью",
    "advanced.field.preload.thumbWorkers.helper":
      "Количество параллельных воркеров предзагрузки превью.",
    "advanced.field.preload.metaWorkers": "Потоки метаданных",
    "advanced.field.preload.metaWorkers.helper":
      "Количество параллельных воркеров предзагрузки метаданных.",
    "advanced.field.preload.hashWorkers": "Потоки хэшей",
    "advanced.field.preload.hashWorkers.helper":
      "Количество параллельных воркеров для подсчёта хэшей.",
    "advanced.field.preload.progressEmit": "Частота прогресса (мс)",
    "advanced.field.preload.progressEmit.helper":
      "Интервал отправки обновлений прогресса предзагрузки.",
    "advanced.field.preload.hashDelay": "Задержка очереди хэшей (мс)",
    "advanced.field.preload.hashDelay.helper":
      "Пауза между задачами очереди хэшей.",
    "advanced.field.preload.hashAfter": "Хэшировать после предзагрузки",
    "advanced.field.preload.hashAfter.helper":
      "Считать хэши превью только после завершения предзагрузки.",
    "advanced.field.preload.hashRetry": "Повтор при изменении превью",
    "advanced.field.preload.hashRetry.helper":
      "Повторять подсчёт хэша, если превью изменилось во время предзагрузки.",
    "advanced.field.metadata.probeTimeout": "Таймаут опроса (с)",
    "advanced.field.metadata.probeTimeout.helper":
      "Таймаут извлечения метаданных файла.",
    "advanced.field.metadata.parseCreation": "Парсить creation_time",
    "advanced.field.metadata.parseCreation.helper":
      "Извлекать creation_time из метаданных, если доступно.",
    "advanced.field.album.renameDelay":
      "Задержка очистки после переименования (с)",
    "advanced.field.album.renameDelay.helper":
      "Пауза перед очисткой после переименования медиа.",
    "advanced.field.album.moveThumbs": "Перенос превью/метаданных с медиа",
    "advanced.field.album.moveThumbs.helper":
      "Переносить связанные превью и метаданные.",
  },
};
