(function () {
  "use strict";

  var STATUS_ENDPOINT = "/api/v1/status";
  var CHALLENGE_ENDPOINT = "/api/v1/auth/challenge";
  var VERIFY_ENDPOINT = "/api/v1/auth/verify";
  var STATUS_TIMEOUT_MS = 5000;
  var STATUS_POLL_INTERVAL_MS = 750;
  var LIVE_STALL_NOTICE_MS = 1500;
  /* Progress callbacks are not a clock: browsers may coalesce several
   * callbacks and deliver them in the same event-loop turn.  Measuring a
   * rate from one callback delta therefore produces impossible values (for
   * example 279 MB/s after a few megabytes arrive in a burst).  Keep the
   * instantaneous figure on a real-time window instead. */
  var LIVE_RATE_WINDOW_MS = 1000;
  var LIVE_RATE_SMOOTHING = 0.35;
  var DEFAULT_CHUNK_BYTES = 8 * 1024 * 1024;
  var DEFAULT_CHUNK_PARALLELISM = 6;
  var PARALLEL_MIN_BYTES = 16 * 1024 * 1024;
  var SESSION_STORAGE_KEY = "ps5localsend.session.v1";
  var language = "en";
  var TRANSLATIONS = {
    en: {
      skip: "Skip to main content", checkingServer: "Checking server",
      localReceiver: "Local receiver",
      heroCopy: "No cloud or app installation. Open this page on a phone or computer connected to the same network and confirm the PIN shown by the PS5.",
      trustedLan: "Trusted local network only.",
      lanWarning: "The connection uses unencrypted HTTP. Do not transfer files over guest or public Wi-Fi.",
      serverStatus: "Server status", waitingConnection: "PS5 is waiting for a connection",
      loadingReceiver: "Loading receiver information…", receivingFile: "Receiving file",
      preparing: "Preparing…", receiving: "Receiving…", receiveProgress: "File receive progress",
      receiverDetails: "Receiver details", address: "Address", version: "Version",
      protection: "Protection", fileLimit: "File limit", connection: "Connection",
      twoSteps: "2 steps", available: "Available", confirmPin: "Confirm PIN",
      pinInstructions: "Request a PIN from your phone or computer. Enter the six digits shown in the PS5 notification to authorize this browser.",
      requestPin: "Request PIN on PS5", sixDigitPin: "Six-digit PIN",
      pinWillAppear: "The PIN will appear in a PS5 notification.", pinRequired: "PIN required",
      chooseFiles: "Choose files",
      filesInstructions: "Select files after authorization. They are sent sequentially and directly from the browser without packaging.",
      storage: "Storage", selectDestination: "Select destination folder",
      destinationFolder: "Destination folder",
      foldersConfigured: "Available folders are configured in config.ini on the PS5.",
      selectOrDrop: "Select or drop files", filesQueued: "Files will be added to a sequential queue.",
      queueEmptyAria: "File queue is empty", queueEmpty: "Queue is empty",
      queueHint: "Added files will appear here.", uploadQueue: "Upload queue",
      internalStorage: "Internal storage", usbDrive: "USB drive",
      pinRequiredState: "PIN required", requestPinAgain: "Request PIN again",
      sessionExpired: "Session expired. Request a new PIN.", sessionRestored: "Session restored",
      sessionRestoredMessage: "This browser session was restored after the page reload.",
      noPin: "No PIN", serverAvailable: "Server available", localAddress: "local address",
      ps5Ready: "PS5 is ready to receive files",
      noPinReady: "PIN is disabled in config.ini. Choose files to send to the PS5.",
      pinReady: "PIN confirmed. Choose files to send to the PS5.",
      ps5ShowsPin: "PS5 is showing a PIN", checkPin: "Check the PS5 notification and enter the PIN here.",
      receiverPreparing: "PS5 is preparing the receiver", receiverStillPreparing: "The receiver is responding but is still preparing.",
      requestPinForSession: "The local HTTP server is available. Request a PIN for a protected session.",
      ps5PreparingFile: "PS5 is preparing the file", ps5ReceivingFile: "PS5 is receiving the file",
      receivingNamed: "Receiving “{name}” directly into PS5 storage.", receivingGeneric: "Receiving a file directly into PS5 storage.",
      lastTransfer: "Last transfer: ", timerUnavailable: "PS5 timer unavailable",
      write: "write", confirmPinForStorage: "Confirm the PIN to change the destination.",
      savingDestination: "Saving destination…", storageBusy: "The destination cannot be changed during an upload.",
      storageConfigured: "The folder list is configured in config.ini on the PS5.",
      readyToSend: "Ready to send", total: "Total", waiting: "waiting", uploading: "uploading",
      done: "done", errors: "errors", waitingState: "Waiting", cancel: "Cancel",
      cancelling: "Cancelling…", retry: "Retry", preparingSpace: "Preparing storage on PS5…",
      savedAs: "Saved as", browser: "browser", ps5TimingUnavailable: "PS5 timing unavailable",
      sent: "sent", cancelled: "Cancelled", checking: "Checking server",
      loadingStatus: "Loading receiver information…", offline: "Offline",
      ps5Unavailable: "PS5 unavailable",
      ps5NoResponse: "PS5 did not respond. Check the payload and ensure both devices are on the same network.",
      deviceOffline: "This device is not connected to a network.", requestingPin: "Requesting a new PIN…",
      pinSent: "PIN sent", enterPin: "Enter the six digits shown in the PS5 notification. The PIN expires shortly.",
      pinRequestFailed: "Could not display a PIN. Check the connection and try again.",
      enterSixDigits: "Enter all six PIN digits.", checkingPin: "Checking PIN…", confirmed: "Confirmed",
      pinConfirmedSession: "PIN confirmed. This browser session remains active until it expires or the tab is closed.",
      authenticated: "Authentication complete.", attemptsExceeded: "Attempt limit reached. Request a new PIN.",
      pinInvalid: "The PIN is invalid or expired. Try again.", selectedFolder: "Files will be saved to “{name}”.",
      receivedByPs5: "PS5 received", awaitingData: "waiting for data", browserPause: "browser pause",
      pause: "pause", now: "now", of: "of",
      file_too_large: "The file exceeds the server limit.", insufficient_storage: "Not enough free space on the PS5.",
      upload_busy: "The server is receiving another file.", size_mismatch: "The received size does not match the file.",
      hash_mismatch: "The file checksum does not match.", unauthorized: "The session expired. Confirm a new PIN.",
      upload_not_found: "The upload no longer exists on the PS5.", upload_forbidden: "The upload belongs to another session.",
      file_limit_reached: "The session file limit was reached.", length_required: "The PS5 did not receive the file size.",
      invalid_range: "The PS5 rejected a file range.", unsupported_media_type: "The browser used an unsupported upload format.",
      invalid_upload: "The PS5 rejected the file metadata.", storage_error: "The PS5 could not write the file.",
      storage_busy: "Finish or cancel the current upload first.", storage_unavailable: "The selected storage is unavailable or not mounted.",
      storage_config_error: "Could not save the storage setting.", invalid_storage: "Choose an available folder.",
      network_error: "The connection to the PS5 was interrupted.", request_failed: "The PS5 did not respond.",
      invalid_response: "The PS5 returned an invalid response.", genericError: "Could not send the file (code: {code}).",
      fileTooLarge: "The file exceeds the server limit.", home: "PS5 LocalSend home",
      footer: "Device ↔ PS5 · local network · v1.0"
    },
    ru: {
      skip: "К основному содержимому", checkingServer: "Проверяем сервер", localReceiver: "Локальный приёмник",
      heroCopy: "Без облака и установки приложения. Откройте эту страницу на телефоне или ПК в той же сети и подтвердите отправку PIN-кодом с экрана PS5.",
      trustedLan: "Только доверенная локальная сеть.", lanWarning: "Соединение работает по HTTP без шифрования. Не передавайте файлы через гостевой или публичный Wi-Fi.",
      serverStatus: "Состояние сервера", waitingConnection: "PS5 ожидает подключение", loadingReceiver: "Получаем сведения от локального приёмника…",
      receivingFile: "Получение файла", preparing: "Подготовка…", receiving: "Приём…", receiveProgress: "Прогресс получения файла",
      receiverDetails: "Параметры приёмника", address: "Адрес", version: "Версия", protection: "Защита", fileLimit: "Лимит файла",
      connection: "Подключение", twoSteps: "2 шага", available: "Доступно", confirmPin: "Подтвердите PIN",
      pinInstructions: "Запросите PIN с телефона или ПК. Введите шесть цифр из уведомления PS5, чтобы разрешить отправку этому браузеру.",
      requestPin: "Запросить PIN на PS5", sixDigitPin: "Шестизначный PIN", pinWillAppear: "PIN появится в уведомлении PS5.",
      pinRequired: "Требуется PIN", chooseFiles: "Выберите файлы", filesInstructions: "После подтверждения выберите файлы. Они отправляются последовательно и напрямую из браузера без упаковки.",
      storage: "Хранилище", selectDestination: "Выбрать каталог сохранения", destinationFolder: "Каталог сохранения",
      foldersConfigured: "Доступные каталоги задаются в config.ini на PS5.", selectOrDrop: "Выберите или перетащите файлы",
      filesQueued: "Файлы будут добавлены в последовательную очередь.", queueEmptyAria: "Очередь файлов пуста", queueEmpty: "Очередь пуста",
      queueHint: "Добавленные файлы появятся здесь.", uploadQueue: "Очередь отправки", internalStorage: "Внутренняя память", usbDrive: "USB-накопитель",
      pinRequiredState: "Требуется PIN", requestPinAgain: "Запросить новый PIN", sessionExpired: "Сессия истекла. Запросите новый PIN.",
      sessionRestored: "Сессия восстановлена", sessionRestoredMessage: "Сессия этого браузера восстановлена после обновления страницы.",
      noPin: "Без PIN", serverAvailable: "Сервер доступен", localAddress: "локальный адрес", ps5Ready: "PS5 готова принимать файлы",
      noPinReady: "PIN отключён в config.ini. Выберите файлы для отправки на PS5.", pinReady: "PIN подтверждён. Выберите файлы для отправки на PS5.",
      ps5ShowsPin: "PS5 показывает PIN", checkPin: "Проверьте уведомление PS5 и введите PIN в этой форме.", receiverPreparing: "PS5 готовит приёмник",
      receiverStillPreparing: "Приёмник отвечает, но ещё завершает подготовку.", requestPinForSession: "Локальный HTTP-сервер отвечает. Запросите PIN для защищённой сессии.",
      ps5PreparingFile: "PS5 готовит файл", ps5ReceivingFile: "PS5 принимает файл", receivingNamed: "Получение «{name}» прямо в хранилище PS5.",
      receivingGeneric: "Получение файла прямо в хранилище PS5.", lastTransfer: "Последняя передача: ", timerUnavailable: "таймер PS5 недоступен",
      write: "запись", confirmPinForStorage: "Подтвердите PIN, чтобы менять место сохранения файлов.", savingDestination: "Сохраняем выбранное место…",
      storageBusy: "Нельзя менять хранилище во время отправки файла.", storageConfigured: "Список каталогов задаётся в config.ini на PS5.",
      readyToSend: "Готово к отправке", total: "Всего", waiting: "ожидают", uploading: "отправляется", done: "готово", errors: "ошибки",
      waitingState: "Ожидает", cancel: "Отменить", cancelling: "Отменяем…", retry: "Повторить", preparingSpace: "Подготавливаем место на PS5…",
      savedAs: "Сохранено как", browser: "браузер", ps5TimingUnavailable: "PS5: замер времени недоступен", sent: "отправлен", cancelled: "Отменено",
      checking: "Проверяем сервер", loadingStatus: "Получаем сведения от локального приёмника…", offline: "Нет связи с сервером", ps5Unavailable: "PS5 недоступна",
      ps5NoResponse: "PS5 не ответила. Проверьте payload и подключение к той же сети.", deviceOffline: "Это устройство сейчас не подключено к сети.",
      requestingPin: "Запрашиваем новый PIN…", pinSent: "PIN отправлен", enterPin: "Введите шесть цифр из уведомления PS5. PIN действует недолго.",
      pinRequestFailed: "Не удалось показать PIN. Проверьте соединение и повторите запрос.", enterSixDigits: "Введите все шесть цифр PIN.",
      checkingPin: "Проверяем PIN…", confirmed: "Подтверждено", pinConfirmedSession: "PIN подтверждён. Сессия браузера действует до истечения срока или закрытия вкладки.",
      authenticated: "Аутентификация выполнена.", attemptsExceeded: "Лимит попыток исчерпан. Запросите новый PIN.", pinInvalid: "PIN неверен или уже недействителен. Попробуйте ещё раз.",
      selectedFolder: "Файлы будут сохраняться в «{name}».", receivedByPs5: "PS5 приняла", awaitingData: "ожидаем данные", browserPause: "пауза браузера",
      pause: "пауза", now: "сейчас", of: "из",
      file_too_large: "Файл превышает лимит сервера.", insufficient_storage: "На PS5 недостаточно свободного места.", upload_busy: "Сервер занят другой загрузкой.",
      size_mismatch: "Размер переданных данных не совпал.", hash_mismatch: "Контрольная сумма файла не совпала.", unauthorized: "Сессия истекла. Снова подтвердите PIN.",
      upload_not_found: "Загрузка больше не найдена на PS5.", upload_forbidden: "Загрузка принадлежит другой сессии.", file_limit_reached: "Достигнут лимит файлов для этой сессии.",
      length_required: "PS5 не получила размер файла.", invalid_range: "PS5 отклонила диапазон части файла.", unsupported_media_type: "Браузер отправил файл в неподдерживаемом формате.",
      invalid_upload: "PS5 отклонила параметры файла.", storage_error: "PS5 не смогла записать файл в хранилище.", storage_busy: "Сначала завершите или отмените текущую загрузку.",
      storage_unavailable: "Выбранное хранилище недоступно или не подключено.", storage_config_error: "Не удалось сохранить настройку хранилища.",
      invalid_storage: "Выберите доступный каталог из списка.", network_error: "Соединение с PS5 прервано.", request_failed: "PS5 не ответила на запрос.",
      invalid_response: "PS5 вернула некорректный ответ.", genericError: "Не удалось отправить файл (код: {code}).", fileTooLarge: "Файл превышает лимит сервера.",
      home: "Главная PS5 LocalSend", footer: "Устройство ↔ PS5 · локальная сеть · v1.0"
    }
  };

  function t(key, values) {
    var table = TRANSLATIONS[language] || TRANSLATIONS.en;
    var text = table[key] !== undefined ? table[key] : TRANSLATIONS.en[key];
    Object.keys(values || {}).forEach(function (name) {
      text = text.replace("{" + name + "}", String(values[name]));
    });
    return text || key;
  }

  function applyLanguage(value) {
    var next = value === "ru" ? "ru" : "en";
    if (next === language) return false;
    language = next;
    document.documentElement.lang = language;
    Array.prototype.forEach.call(document.querySelectorAll("[data-i18n]"), function (node) {
      node.textContent = t(node.getAttribute("data-i18n"));
    });
    Array.prototype.forEach.call(document.querySelectorAll("[data-i18n-aria-label]"), function (node) {
      node.setAttribute("aria-label", t(node.getAttribute("data-i18n-aria-label")));
    });
    return true;
  }
  var serverAuthMode = "pin";
  var challengeId = null;
  var bearerToken = null;
  var maxFileBytes = null;
  var maxFilesPerSession = null;
  var chunkUploadSupported = false;
  var chunkUploadBytes = DEFAULT_CHUNK_BYTES;
  var chunkUploadParallelism = DEFAULT_CHUNK_PARALLELISM;
  var fastUploadBase = "";
  var uploadQueue = [];
  var nextQueueSequence = 0;
  var currentXhr = null;
  var activeXhrs = [];
  var uploadTicker = null;
  var uploadTickerItem = null;
  var running = false;
  var statusPollTimer = null;
  var statusRequestInFlight = false;
  var statusLoaded = false;
  var pageActive = true;
  var lastProgressRenderAt = 0;
  var storageTarget = "internal";
  var storageTargets = [
    { id: "internal", label: "Internal storage", path: "/data/ps5localsend/inbox" },
    { id: "usb", label: "USB drive", path: "/mnt/usb0/ps5localsend/inbox" }
  ];
  var storageTargetsSignature = "";
  var storageRequestInFlight = false;

  var elements = {
    connectionPill: document.getElementById("connection-pill"),
    connectionLabel: document.getElementById("connection-label"),
    receiverPanel: document.querySelector(".receiver-panel"),
    receiverTitle: document.getElementById("receiver-title"),
    serverSummary: document.getElementById("server-summary"),
    receiverTransfer: document.getElementById("receiver-transfer"),
    receiverTransferName: document.getElementById("receiver-transfer-name"),
    receiverTransferState: document.getElementById("receiver-transfer-state"),
    receiverTransferProgress: document.getElementById("receiver-transfer-progress"),
    receiverTransferMeta: document.getElementById("receiver-transfer-meta"),
    serverAddress: document.getElementById("server-address"),
    serverVersion: document.getElementById("server-version"),
    serverAuth: document.getElementById("server-auth"),
    serverLimit: document.getElementById("server-limit"),
    pinCard: document.getElementById("pin-card"),
    pinStateLabel: document.getElementById("pin-state-label"),
    requestPin: document.getElementById("request-pin"),
    pinForm: document.getElementById("pin-form"),
    pinInput: document.getElementById("pin-code"),
    verifyPin: document.getElementById("verify-pin"),
    pinMessage: document.getElementById("pin-message"),
    interactionMessage: document.getElementById("interaction-message"),
    filesCard: document.getElementById("files-card"),
    filesStateLabel: document.getElementById("files-state-label"),
    fileInput: document.getElementById("file-input"),
    dropZone: document.getElementById("drop-zone"),
    storageControl: document.getElementById("storage-control"),
    storageSelect: document.getElementById("storage-select"),
    storageLabel: document.getElementById("storage-label"),
    storagePath: document.getElementById("storage-path"),
    storageMessage: document.getElementById("storage-message"),
    queuePlaceholder: document.getElementById("queue-placeholder"),
    queueSummary: document.getElementById("queue-summary"),
    uploadQueue: document.getElementById("upload-queue")
  };

  function setConnectionState(state, label) {
    elements.connectionPill.dataset.state = state;
    elements.connectionLabel.textContent = label;
  }

  function sessionStore() {
    try {
      return window.sessionStorage;
    } catch (_) {
      return null;
    }
  }

  function clearStoredSession() {
    var store = sessionStore();
    if (!store) return;
    try {
      store.removeItem(SESSION_STORAGE_KEY);
    } catch (_) {}
  }

  function persistSession(token, expiresIn) {
    var store = sessionStore();
    var seconds = Number(expiresIn);
    if (!store || !/^[0-9a-f]{64}$/.test(token)) return;
    if (!Number.isFinite(seconds) || seconds <= 0) seconds = 900;
    try {
      store.setItem(SESSION_STORAGE_KEY, JSON.stringify({
        token: token,
        expiresAt: Date.now() + Math.floor(seconds * 1000)
      }));
    } catch (_) {}
  }

  function forgetSession(message) {
    bearerToken = null;
    challengeId = null;
    clearStoredSession();
    disableFiles();
    elements.pinForm.hidden = true;
    elements.pinCard.dataset.state = "locked";
    elements.pinStateLabel.textContent = t("pinRequiredState");
    elements.requestPin.textContent = t("requestPin");
    elements.pinMessage.textContent = message || t("sessionExpired");
  }

  function restoreSession() {
    var store = sessionStore();
    var saved;
    if (!store) return;
    try {
      saved = JSON.parse(store.getItem(SESSION_STORAGE_KEY) || "null");
    } catch (_) {
      saved = null;
    }
    if (!saved || !/^[0-9a-f]{64}$/.test(saved.token) ||
        !Number.isFinite(Number(saved.expiresAt)) ||
        Number(saved.expiresAt) <= Date.now()) {
      clearStoredSession();
      return;
    }
    bearerToken = saved.token;
    elements.pinForm.hidden = true;
    elements.pinCard.dataset.state = "authenticated";
    elements.pinStateLabel.textContent = t("sessionRestored");
    elements.pinMessage.textContent = t("sessionRestoredMessage");
    enableFiles();
  }

  function firstDefined(object, keys) {
    var index;
    for (index = 0; index < keys.length; index += 1) {
      if (object[keys[index]] !== undefined && object[keys[index]] !== null) {
        return object[keys[index]];
      }
    }
    return null;
  }

  function formatBytes(value) {
    var bytes = Number(value);
    var units = language === "ru"
      ? ["Б", "КиБ", "МиБ", "ГиБ", "ТиБ"]
      : ["B", "KiB", "MiB", "GiB", "TiB"];
    var unitIndex = 0;
    if (!Number.isFinite(bytes) || bytes < 0) {
      return "—";
    }
    while (bytes >= 1024 && unitIndex < units.length - 1) {
      bytes /= 1024;
      unitIndex += 1;
    }
    return (unitIndex === 0 ? bytes.toFixed(0) : bytes.toFixed(bytes >= 10 ? 0 : 1)) + " " + units[unitIndex];
  }

  function nowMilliseconds() {
    if (window.performance && typeof window.performance.now === "function") {
      return window.performance.now();
    }
    return Date.now();
  }

  function formatSeconds(milliseconds) {
    var seconds = Number(milliseconds) / 1000;
    if (!Number.isFinite(seconds) || seconds < 0) return "—";
    return seconds.toFixed(seconds >= 10 ? 1 : 2).replace(".", language === "ru" ? "," : ".") +
      (language === "ru" ? " с" : " s");
  }

  function formatDuration(milliseconds) {
    var duration = Number(milliseconds);
    if (!Number.isFinite(duration) || duration < 0) return "—";
    return duration >= 1000
      ? formatSeconds(duration)
      : Math.round(duration) + (language === "ru" ? " мс" : " ms");
  }

  function formatRate(value) {
    var speed = Number(value);
    if (!Number.isFinite(speed) || speed <= 0) return language === "ru" ? "0,0 МБ/с" : "0.0 MB/s";
    return speed.toFixed(1).replace(".", language === "ru" ? "," : ".") +
      (language === "ru" ? " МБ/с" : " MB/s");
  }

  function liveUploadMessage(item) {
    var elapsed = Number(item.liveElapsedMs);
    var loaded = Number(item.lastProgressLoaded);
    var averageSpeed = Number(item.liveSpeed);
    var instantSpeed = Number(item.instantSpeed);
    var stallMs = Number(item.stallMs);
    var serverReceived = Number(item.serverReceived);
    var serverStallMs = Number(item.serverStallMs);
    var message = formatBytes(loaded) + " " + t("of") + " " + formatBytes(item.file.size);
    if (Number.isFinite(averageSpeed) && averageSpeed > 0) {
      message += " · " + t("browser") + ": " + formatRate(averageSpeed);
    } else {
      message += " · " + t("awaitingData");
    }
    if (Number.isFinite(stallMs) && stallMs >= LIVE_STALL_NOTICE_MS) {
      message += item.serverStatusSeen && Number.isFinite(serverStallMs) &&
        serverStallMs < LIVE_STALL_NOTICE_MS
        ? " · " + t("browserPause") + ": " + formatSeconds(stallMs)
        : " · " + t("pause") + ": " + formatSeconds(stallMs);
    } else if (Number.isFinite(instantSpeed) && instantSpeed > 0) {
      message += " · " + t("now") + ": " + formatRate(instantSpeed);
    }
    if (item.serverStatusSeen && Number.isFinite(serverReceived) &&
        serverReceived >= 0) {
      message += " · " + t("receivedByPs5") + ": " + formatBytes(serverReceived);
    }
    return message + " · " + formatSeconds(elapsed);
  }

  function stopUploadTicker() {
    if (uploadTicker !== null) {
      window.clearInterval(uploadTicker);
      uploadTicker = null;
    }
    uploadTickerItem = null;
  }

  function startUploadTicker(item) {
    stopUploadTicker();
    uploadTickerItem = item;
    uploadTicker = window.setInterval(function () {
      var elapsed;
      var now;
      if (uploadTickerItem !== item || item.state !== "uploading") {
        stopUploadTicker();
        return;
      }
      now = nowMilliseconds();
      elapsed = Math.max(0, now - item.transferStartedAt);
      item.liveElapsedMs = elapsed;
      if (item.lastProgressAt !== null) {
        item.stallMs = Math.max(0, now - item.lastProgressAt);
      }
      if (item.serverStatusSeen && item.serverLastProgressAt !== null) {
        item.serverStallMs = Math.max(0, now - item.serverLastProgressAt);
      }
      if (item.stallMs >= LIVE_STALL_NOTICE_MS) {
        item.instantSpeed = 0;
      }
      if (item.lastProgressLoaded > 0 && elapsed > 0) {
        item.liveSpeed = Number(item.lastProgressLoaded) / 1048576 /
          (elapsed / 1000);
      }
      updateInstantSpeed(item, now);
      item.message = liveUploadMessage(item);
      renderQueue();
    }, 500);
  }

  function formatAuthMode(value) {
    return typeof value === "string" && value.toLowerCase() === "none"
      ? t("noPin") : "PIN";
  }

  function setReceiverState(state, title, summary) {
    elements.receiverPanel.dataset.state = state;
    elements.receiverTitle.textContent = title;
    elements.serverSummary.textContent = summary;
  }

  function renderTransfer(transfer) {
    var state = transfer && typeof transfer.state === "string"
      ? transfer.state : "idle";
    var active = transfer && (transfer.active === true || state !== "idle");
    var name = transfer && typeof transfer.name === "string" ? transfer.name : "";
    var received = transfer ? Number(transfer.receivedBytes) : 0;
    var expected = transfer ? Number(transfer.expectedBytes) : 0;
    var progress = expected > 0 && Number.isFinite(received)
      ? Math.max(0, Math.min(100, received * 100 / expected)) : 0;
    if (!Number.isFinite(received) || received < 0) received = 0;
    if (!Number.isFinite(expected) || expected < 0) expected = 0;
    elements.receiverTransfer.hidden = !active;
    if (!active) return;
    elements.receiverTransferName.textContent = name || t("receivingFile");
    elements.receiverTransferState.textContent = state === "preparing"
      ? t("preparing") : t("receiving");
    elements.receiverTransferProgress.value = progress;
    elements.receiverTransferMeta.textContent = expected > 0
      ? formatBytes(received) + " " + t("of") + " " + formatBytes(expected)
      : formatBytes(received);
    setReceiverState(
      "receiving",
      state === "preparing" ? t("ps5PreparingFile") : t("ps5ReceivingFile"),
      name ? t("receivingNamed", { name: name }) : t("receivingGeneric")
    );
  }

  function renderLastUpload(lastUpload, transfer) {
    var timing = lastUpload && lastUpload.timing;
    var bytes = lastUpload && Number(lastUpload.bytes);
    var transferMs = timing && Number(timing.transferMs);
    var writeMs = timing && Number(timing.writeMs);
    var summary;
    var speed;
    if (!lastUpload || typeof lastUpload !== "object" ||
        (transfer && transfer.active === true) ||
        (challengeId && !bearerToken) || !timing || typeof timing !== "object") {
      return;
    }
    summary = t("lastTransfer");
    if (Number.isFinite(bytes) && bytes >= 0 &&
        Number.isFinite(transferMs) && transferMs > 0) {
      speed = bytes / 1048576 / (transferMs / 1000);
      summary += formatRate(speed) + " · " +
        formatSeconds(transferMs);
    } else {
      summary += t("timerUnavailable");
    }
    if (Number.isFinite(writeMs) && writeMs >= 0) {
      summary += " · " + t("write") + " " + formatDuration(writeMs);
    }
    setReceiverState(
      bearerToken ? "authenticated" : "waiting",
      bearerToken ? t("ps5Ready") : t("waitingConnection"),
      summary
    );
  }

  function renderStatus(status) {
    var limits = status.limits && typeof status.limits === "object" ? status.limits : {};
    var ready = firstDefined(status, ["ready", "isReady", "is_ready"]);
    var version = firstDefined(status, ["version", "serverVersion", "server_version"]);
    var authMode = firstDefined(status, ["authMode", "auth_mode"]);
    var languageChanged = applyLanguage(status.language === "ru" ? "ru" : "en");
    if (languageChanged) {
      storageTargetsSignature = "";
      renderQueue();
    }
    var capabilities = status.capabilities && typeof status.capabilities === "object"
      ? status.capabilities : {};
    var advertisedChunkBytes = Number(capabilities.chunkSize);
    var advertisedParallelism = Number(capabilities.chunkParallelism);
    setFastUploadPort(Number(capabilities.fastUploadPort));
    chunkUploadSupported = capabilities.chunkUpload === true &&
      Number.isFinite(advertisedChunkBytes) && advertisedChunkBytes >= 1024 * 1024 &&
      Number.isFinite(advertisedParallelism) && advertisedParallelism >= 2;
    if (chunkUploadSupported) {
      chunkUploadBytes = Math.floor(advertisedChunkBytes);
      chunkUploadParallelism = Math.min(8, Math.max(2, Math.floor(advertisedParallelism)));
    }
    maxFileBytes = firstDefined(status, ["maxFileBytes", "max_file_bytes"]);
    if (maxFileBytes === null) {
      maxFileBytes = firstDefined(limits, ["maxFileBytes", "max_file_bytes"]);
    }
    maxFilesPerSession = firstDefined(limits, ["maxFilesPerSession", "max_files_per_session"]);
    setConnectionState("online", t("serverAvailable"));
    elements.serverAddress.textContent = window.location.host || t("localAddress");
    elements.serverVersion.textContent = version === null ? "1.0" : String(version);
    elements.serverAuth.textContent = formatAuthMode(authMode);
    serverAuthMode = typeof authMode === "string" && authMode.toLowerCase() === "none"
      ? "none" : "pin";
    if (serverAuthMode === "none") {
      bearerToken = "none";
      challengeId = null;
      clearStoredSession();
      elements.pinForm.hidden = true;
      elements.pinCard.hidden = true;
      enableFiles();
    } else {
      elements.pinCard.hidden = false;
      if (bearerToken === "none") bearerToken = null;
      if (bearerToken) {
        elements.pinStateLabel.textContent = t("confirmed");
        elements.pinMessage.textContent = t("pinConfirmedSession");
      }
    }
    elements.serverLimit.textContent = formatBytes(maxFileBytes);
    if (serverAuthMode === "none") {
      setReceiverState(
        "authenticated",
        t("ps5Ready"),
        t("noPinReady")
      );
    } else if (bearerToken) {
      setReceiverState(
        "authenticated",
        t("ps5Ready"),
        t("pinReady")
      );
    } else if (challengeId) {
      setReceiverState(
        "pending",
        t("ps5ShowsPin"),
        t("checkPin")
      );
    } else {
      setReceiverState(
        ready === false ? "preparing" : "waiting",
        ready === false ? t("receiverPreparing") : t("waitingConnection"),
        ready === false
          ? t("receiverStillPreparing")
        : t("requestPinForSession")
      );
    }
    renderStorage(status.storage, status.transfer);
    renderTransfer(status.transfer);
    renderLastUpload(status.lastUpload, status.transfer);
    if (updateServerTransferTelemetry(status.transfer)) {
      renderQueue();
    }
  }

  function setFastUploadPort(port) {
    var hostname;
    if (!Number.isFinite(port) || port < 1 || port > 65535) {
      fastUploadBase = "";
      return;
    }
    hostname = window.location.hostname || "127.0.0.1";
    if (hostname.indexOf(":") >= 0 && hostname.charAt(0) !== "[") {
      hostname = "[" + hostname + "]";
    }
    fastUploadBase = "http://" + hostname + ":" + Math.floor(port);
  }

  function uploadUrl(uploadId) {
    return fastUploadBase + "/api/v1/uploads/" + uploadId;
  }

  function updateServerTransferTelemetry(transfer) {
    var item = uploadQueue.find(function (entry) {
      return entry.state === "uploading";
    });
    var expected;
    var received;
    var now;
    var previousReceived;
    if (!item || !transfer || transfer.active !== true) return false;
    expected = Number(transfer.expectedBytes);
    received = Number(transfer.receivedBytes);
    if (!Number.isFinite(expected) || expected !== Number(item.file.size) ||
        !Number.isFinite(received) || received < 0) {
      return false;
    }
    now = nowMilliseconds();
    previousReceived = Number(item.serverReceived);
    if (!item.serverStatusSeen || !Number.isFinite(previousReceived) ||
        received < previousReceived || received > previousReceived) {
      item.serverLastProgressAt = now;
    }
    if (item.serverLastProgressAt === null) {
      item.serverLastProgressAt = now;
    }
    item.serverReceived = received;
    item.serverExpected = expected;
    item.serverStatusSeen = true;
    item.serverStallMs = Math.max(0, now - item.serverLastProgressAt);
    item.message = liveUploadMessage(item);
    return true;
  }

  function storagePath(target) {
    var entry = storageTargets.find(function (candidate) {
      return candidate.id === target;
    });
    return entry ? entry.path : "—";
  }

  function storageLabel(target) {
    var entry = storageTargets.find(function (candidate) {
      return candidate.id === target;
    });
    if (!entry) return target;
    if (entry.id === "internal" && entry.label === "Internal storage") return t("internalStorage");
    if (entry.id === "usb" && entry.label === "USB drive") return t("usbDrive");
    return entry.label;
  }

  function setStorageTargets(targets) {
    var valid = Array.isArray(targets) ? targets.filter(function (entry) {
      return entry && typeof entry.id === "string" && entry.id &&
        typeof entry.label === "string" && entry.label &&
        typeof entry.path === "string" && entry.path;
    }) : [];
    var signature;
    if (!valid.length) return;
    signature = valid.map(function (entry) {
      return entry.id + "\u0000" + entry.label + "\u0000" + entry.path;
    }).join("\u0001");
    if (signature === storageTargetsSignature) return;
    storageTargetsSignature = signature;
    storageTargets = valid;
    while (elements.storageSelect.firstChild) {
      elements.storageSelect.removeChild(elements.storageSelect.firstChild);
    }
    storageTargets.forEach(function (entry) {
      var option = document.createElement("option");
      option.value = entry.id;
      option.textContent = storageLabel(entry.id);
      elements.storageSelect.appendChild(option);
    });
  }

  function renderStorage(storage, transfer) {
    var target;
    var active = transfer && transfer.active === true;
    if (storage && Array.isArray(storage.targets)) setStorageTargets(storage.targets);
    target = storage && typeof storage.target === "string"
      ? storage.target : storageTargets[0].id;
    if (!storageTargets.some(function (entry) { return entry.id === target; })) {
      target = storageTargets[0].id;
    }
    storageTarget = target;
    elements.storageControl.dataset.state = target;
    elements.storageSelect.value = target;
    elements.storageLabel.textContent = storageLabel(target);
    elements.storagePath.textContent = storagePath(target);
    elements.storageControl.setAttribute("aria-disabled", bearerToken ? "false" : "true");
    elements.storageSelect.disabled = !bearerToken || storageRequestInFlight || active;
    if (!bearerToken) {
      elements.storageMessage.textContent = t("confirmPinForStorage");
    } else if (storageRequestInFlight) {
      elements.storageMessage.textContent = t("savingDestination");
    } else if (active) {
      elements.storageMessage.textContent = t("storageBusy");
    } else if (!elements.storageMessage.dataset.notice) {
      elements.storageMessage.textContent = t("storageConfigured");
    }
  }

  function disableFiles() {
    elements.filesCard.setAttribute("aria-disabled", "true");
    elements.filesCard.dataset.state = "locked";
    elements.filesStateLabel.textContent = t("pinRequiredState");
    elements.fileInput.disabled = true;
    elements.dropZone.setAttribute("aria-disabled", "true");
    elements.dropZone.tabIndex = -1;
    elements.storageControl.setAttribute("aria-disabled", "true");
    elements.storageSelect.disabled = true;
  }

  function enableFiles() {
    elements.filesCard.setAttribute("aria-disabled", "false");
    elements.filesCard.dataset.state = "authenticated";
    elements.filesStateLabel.textContent = t("readyToSend");
    elements.fileInput.disabled = false;
    elements.dropZone.setAttribute("aria-disabled", "false");
    elements.dropZone.tabIndex = 0;
    elements.storageControl.setAttribute("aria-disabled", "false");
    elements.storageSelect.disabled = storageRequestInFlight;
    setReceiverState(
      "authenticated",
      t("ps5Ready"),
      serverAuthMode === "none"
        ? t("noPinReady")
        : t("pinReady")
    );
  }

  function errorMessage(code) {
    var table = TRANSLATIONS[language] || TRANSLATIONS.en;
    return table[code] || t("genericError", { code: code || "unknown" });
  }

  function describeError(error) {
    var code = error && error.code;
    var message = errorMessage(code);
    if (error && error.status && (!code || code === "request_failed")) {
      message += " HTTP " + error.status + ".";
    }
    if (error && error.apiMessage && (!code || code === "request_failed" || code === "storage_error")) {
      message += " " + error.apiMessage + ".";
    }
    return message;
  }

  function renderQueue() {
    var counts = { waiting: 0, uploading: 0, done: 0, error: 0 };
    var displayQueue;
    uploadQueue.forEach(function (item) {
      if (item.state === "queued" || item.state === "preparing") counts.waiting += 1;
      else if (item.state === "uploading") counts.uploading += 1;
      else if (item.state === "done") counts.done += 1;
      else if (item.state === "error" || item.state === "cancelled") counts.error += 1;
    });
    elements.queueSummary.textContent = t("total") + ": " + uploadQueue.length +
      " · " + t("waiting") + ": " + counts.waiting + " · " + t("uploading") + ": " + counts.uploading +
      " · " + t("done") + ": " + counts.done + " · " + t("errors") + ": " + counts.error;
    elements.queuePlaceholder.hidden = uploadQueue.length !== 0;
    elements.uploadQueue.textContent = "";
    /* Keep processing order stable, but show the most recently added file
     * first. Action buttons retain the backing-array index for event handling. */
    displayQueue = uploadQueue.map(function (item, index) {
      return { item: item, index: index };
    });
    displayQueue.sort(function (left, right) {
      return right.item.queueSequence - left.item.queueSequence;
    });
    displayQueue.forEach(function (entry) {
      var item = entry.item;
      var index = entry.index;
      var row = document.createElement("li");
      var info = document.createElement("div");
      var title = document.createElement("strong");
      var state = document.createElement("span");
      var progress = document.createElement("progress");
      var action = null;
      title.textContent = item.file.name;
      row.dataset.state = item.state;
      if (item.state === "uploading" && Number(item.stallMs) >= LIVE_STALL_NOTICE_MS) {
        row.dataset.stall = "true";
      }
      state.textContent = item.message || (formatBytes(item.file.size) + " · " + t("waitingState"));
      progress.max = 100; progress.value = item.progress || 0;
      progress.setAttribute("aria-label", t("receiveProgress") + ": " + item.file.name);
      if (item.state === "uploading" || item.state === "error" ||
          item.state === "cancelled") {
        action = document.createElement("button");
        action.type = "button";
        action.dataset.index = String(index);
        action.textContent = item.state === "uploading"
          ? (item.cancelRequested ? t("cancelling") : t("cancel"))
          : t("retry");
        action.disabled = item.state === "uploading" && item.cancelRequested;
      }
      info.appendChild(title); info.appendChild(state); info.appendChild(progress);
      row.appendChild(info);
      if (action) row.appendChild(action);
      elements.uploadQueue.appendChild(row);
    });
  }

  function jsonRequest(url, options) {
    options.cache = "no-store"; options.credentials = "omit";
    options.headers = options.headers || {};
    options.headers.Accept = "application/json";
    options.headers.Authorization = "Bearer " + bearerToken;
    return window.fetch(url, options).then(function (response) {
      return response.ok ? response.json() : apiError(response);
    });
  }

  function resetUploadTelemetry(item, startedAt) {
    item.transferStartedAt = startedAt;
    item.lastProgressLoaded = 0;
    item.lastProgressAt = startedAt;
    item.liveElapsedMs = 0;
    item.liveSpeed = 0;
    item.instantSpeed = 0;
    item.instantWindowAt = startedAt;
    item.instantWindowLoaded = 0;
    item.stallMs = 0;
    item.serverReceived = null;
    item.serverExpected = null;
    item.serverStatusSeen = false;
    item.serverLastProgressAt = null;
    item.serverStallMs = 0;
  }

  function updateInstantSpeed(item, now) {
    var windowAt = Number(item.instantWindowAt);
    var windowLoaded = Number(item.instantWindowLoaded);
    var loaded = Number(item.lastProgressLoaded);
    var elapsed;
    var delta;
    var sampleSpeed;
    var previousSpeed;
    if (!Number.isFinite(windowAt) || !Number.isFinite(windowLoaded)) {
      item.instantWindowAt = now;
      item.instantWindowLoaded = Number.isFinite(loaded) && loaded >= 0 ? loaded : 0;
      item.instantSpeed = 0;
      return;
    }
    if (!Number.isFinite(loaded) || loaded < windowLoaded) {
      loaded = windowLoaded;
    }
    elapsed = now - windowAt;
    if (!Number.isFinite(elapsed) || elapsed < LIVE_RATE_WINDOW_MS) return;
    delta = loaded - windowLoaded;
    sampleSpeed = delta > 0
      ? delta / 1048576 / (elapsed / 1000)
      : 0;
    previousSpeed = Number(item.instantSpeed);
    item.instantSpeed = sampleSpeed > 0
      ? previousSpeed > 0
        ? previousSpeed * (1 - LIVE_RATE_SMOOTHING) +
          sampleSpeed * LIVE_RATE_SMOOTHING
        : sampleSpeed
      : 0;
    /* Do not advance the anchor during an empty window.  A browser can stop
     * emitting progress events while data is buffered, then report a large
     * jump later; retaining the older anchor keeps that jump from looking
     * like a real wire-rate burst. */
    if (delta > 0) {
      item.instantWindowAt = now;
      item.instantWindowLoaded = loaded;
    }
  }

  function updateUploadProgress(item, loaded, total, now) {
    var previousLoaded = Number(item.lastProgressLoaded);
    var elapsed;
    if (!Number.isFinite(loaded) || loaded < previousLoaded) {
      loaded = previousLoaded;
    }
    if (loaded > previousLoaded) {
      item.lastProgressAt = now;
      item.stallMs = 0;
    }
    elapsed = Math.max(0, now - item.transferStartedAt);
    item.lastProgressLoaded = loaded;
    item.progress = total ? Math.round(loaded * 100 / total) : 0;
    item.liveElapsedMs = elapsed;
    if (item.lastProgressLoaded > 0 && elapsed > 0) {
      item.liveSpeed = Number(item.lastProgressLoaded) / 1048576 /
        (elapsed / 1000);
    }
    updateInstantSpeed(item, now);
    item.message = liveUploadMessage(item);
    if (Date.now() - lastProgressRenderAt >= 100) {
      lastProgressRenderAt = Date.now();
      renderQueue();
    }
  }

  function trackUploadXhr(xhr) {
    activeXhrs.push(xhr);
  }

  function untrackUploadXhr(xhr) {
    var index = activeXhrs.indexOf(xhr);
    if (index >= 0) activeXhrs.splice(index, 1);
    if (currentXhr === xhr) currentXhr = null;
  }

  function uploadBody(item, uploadId) {
    return new Promise(function (resolve, reject) {
      var xhr = new XMLHttpRequest();
      var startedAt = null;
      currentXhr = xhr; trackUploadXhr(xhr);
      item.state = "uploading"; item.uploadId = uploadId; renderQueue();
      xhr.open("PUT", uploadUrl(uploadId), true);
      xhr.setRequestHeader("Accept", "application/json");
      xhr.setRequestHeader("Authorization", "Bearer " + bearerToken);
      xhr.setRequestHeader("Content-Type", "application/octet-stream");
      xhr.upload.onprogress = function (event) {
        var now = nowMilliseconds();
        updateUploadProgress(item, Number(event.loaded),
          event.lengthComputable && event.total ? Number(event.total) :
            Number(item.file.size), now);
      };
      xhr.onload = function () {
        var payload;
        untrackUploadXhr(xhr);
        stopUploadTicker();
        try { payload = JSON.parse(xhr.responseText); } catch (_) { payload = null; }
        if (xhr.status >= 200 && xhr.status < 300) {
          if (payload && typeof payload === "object") {
            payload.clientTransferMs = startedAt !== null
              ? Math.max(0, Math.round(nowMilliseconds() - startedAt)) : 0;
            resolve(payload);
          } else {
            reject({ code: "invalid_response", status: xhr.status });
          }
          return;
        }
        reject({
          code: payload && payload.error && payload.error.code
            ? payload.error.code
            : "request_failed",
          apiMessage: payload && payload.error && payload.error.message
            ? payload.error.message
            : null,
          status: xhr.status
        });
      };
      xhr.onerror = function () { untrackUploadXhr(xhr); stopUploadTicker(); reject({ code: "network_error" }); };
      xhr.onabort = function () { untrackUploadXhr(xhr); stopUploadTicker(); reject({ code: "cancelled" }); };
      startedAt = nowMilliseconds();
      resetUploadTelemetry(item, startedAt);
      startUploadTicker(item);
      xhr.send(item.file);
    });
  }

  /* A prepared upload can be split into independent, aligned extents.  The
   * browser keeps at most the advertised number of XHRs in flight; the PS5
   * stages those extents and drains them through one sequential writer.  The
   * legacy single PUT remains the fallback for small files or older pages. */
  function uploadChunks(item, uploadId, chunkSize, parallelism) {
    return new Promise(function (resolve, reject) {
      var total = Number(item.file.size);
      var count = Math.ceil(total / chunkSize);
      var loaded = new Array(count);
      var retries = new Array(count);
      var nextIndex = 0;
      var completed = 0;
      var active = 0;
      var settled = false;
      var startedAt = nowMilliseconds();
      var index;
      for (index = 0; index < count; index += 1) {
        loaded[index] = 0;
        retries[index] = 0;
      }
      item.state = "uploading";
      item.uploadId = uploadId;
      resetUploadTelemetry(item, startedAt);
      renderQueue();
      startUploadTicker(item);

      function fail(error) {
        if (settled) return;
        settled = true;
        activeXhrs.slice().forEach(function (xhr) {
          try { xhr.abort(); } catch (_) {}
        });
        stopUploadTicker();
        reject(error || { code: "network_error" });
      }

      function finish() {
        if (settled) return;
        jsonRequest("/api/v1/uploads/" + uploadId + "/complete", {
          method: "POST"
        }).then(function (payload) {
          if (!payload || typeof payload !== "object") {
            fail({ code: "invalid_response" });
            return;
          }
          settled = true;
          stopUploadTicker();
          item.lastProgressLoaded = total;
          item.progress = 100;
          payload.clientTransferMs = Math.max(0,
            Math.round(nowMilliseconds() - startedAt));
          resolve(payload);
        }).catch(fail);
      }

      function schedule() {
        if (settled) return;
        while (active < parallelism && nextIndex < count) {
          start(nextIndex);
          nextIndex += 1;
        }
        if (completed === count && active === 0) finish();
      }

      function start(chunkIndex, replacingActiveRequest) {
        var xhr = new XMLHttpRequest();
        var start = chunkIndex * chunkSize;
        var end = Math.min(total, start + chunkSize);
        var chunkLength = end - start;
        if (!replacingActiveRequest) active += 1;
        trackUploadXhr(xhr);
        currentXhr = xhr;
        xhr.open("PUT", uploadUrl(uploadId), true);
        xhr.setRequestHeader("Accept", "application/json");
        xhr.setRequestHeader("Authorization", "Bearer " + bearerToken);
        xhr.setRequestHeader("Content-Type", "application/octet-stream");
        xhr.setRequestHeader("Content-Range", "bytes " + start + "-" +
          (end - 1) + "/" + total);
        xhr.upload.onprogress = function (event) {
          var value = Number(event.loaded);
          var aggregate;
          var i;
          loaded[chunkIndex] = Number.isFinite(value)
            ? Math.max(loaded[chunkIndex], Math.min(chunkLength, value))
            : loaded[chunkIndex];
          aggregate = 0;
          for (i = 0; i < loaded.length; i += 1) aggregate += loaded[i];
          updateUploadProgress(item, aggregate, total, nowMilliseconds());
        };
        xhr.onload = function () {
          var payload;
          var code;
          untrackUploadXhr(xhr);
          try { payload = JSON.parse(xhr.responseText); } catch (_) { payload = null; }
          code = payload && payload.error && payload.error.code
            ? payload.error.code : "request_failed";
          if (xhr.status < 200 || xhr.status >= 300) {
            if (code === "upload_backpressure" && !item.cancelRequested) {
              var delay = Math.min(500, 40 * Math.pow(2, retries[chunkIndex]));
              retries[chunkIndex] += 1;
              loaded[chunkIndex] = 0;
              window.setTimeout(function () {
                if (settled) return;
                if (item.cancelRequested) {
                  active = Math.max(0, active - 1);
                  fail({ code: "cancelled" });
                  return;
                }
                start(chunkIndex, true);
              }, delay);
              return;
            }
            active -= 1;
            fail({
              code: code,
              apiMessage: payload && payload.error && payload.error.message
                ? payload.error.message : null,
              status: xhr.status
            });
            return;
          }
          active -= 1;
          loaded[chunkIndex] = chunkLength;
          completed += 1;
          updateUploadProgress(item, loaded.reduce(function (sum, value) {
            return sum + value;
          }, 0), total, nowMilliseconds());
          schedule();
        };
        xhr.onerror = function () {
          untrackUploadXhr(xhr);
          active -= 1;
          fail({ code: "network_error" });
        };
        xhr.onabort = function () {
          untrackUploadXhr(xhr);
          active = Math.max(0, active - 1);
          fail({ code: "cancelled" });
        };
        xhr.send(item.file.slice(start, end));
      }

      if (!Number.isFinite(total) || total <= 0 || !Number.isFinite(chunkSize) ||
          chunkSize <= 0 || !Number.isFinite(parallelism) || parallelism < 2) {
        fail({ code: "invalid_response" });
        return;
      }
      schedule();
    });
  }

  function runQueue() {
    var item;
    if (running || !bearerToken) return;
    item = uploadQueue.find(function (entry) { return entry.state === "queued"; });
    if (!item) return;
    running = true; item.state = "preparing";
    item.message = t("preparingSpace"); renderQueue();
    jsonRequest("/api/v1/uploads", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name: item.file.name, size: item.file.size, type: item.file.type || "application/octet-stream" })
    }).then(function (prepared) {
      var advertisedChunkSize = Number(prepared && prepared.chunkSize);
      var advertisedParallelism = Number(prepared && prepared.chunkParallelism);
      if (prepared && prepared.fastUploadPort !== undefined) {
        setFastUploadPort(Number(prepared.fastUploadPort));
      }
      if (!Number.isFinite(advertisedChunkSize) && chunkUploadSupported) {
        advertisedChunkSize = chunkUploadBytes;
      }
      if (!Number.isFinite(advertisedParallelism) && chunkUploadSupported) {
        advertisedParallelism = chunkUploadParallelism;
      }
      /* The dedicated listener is intentionally FTP-like: one TCP stream is
       * faster on PS5 than several writers contending for the upload mutex.
       * Keep ranged XHRs only as a compatibility fallback when fastUploadPort
       * is not advertised. */
      var canUseChunks = fastUploadBase === "" && chunkUploadSupported &&
        Number.isFinite(advertisedChunkSize) && advertisedChunkSize >= 1024 * 1024 &&
        Number.isFinite(advertisedParallelism) && advertisedParallelism >= 2 &&
        item.file.size >= Math.max(PARALLEL_MIN_BYTES, advertisedChunkSize * 2);
      item.savedName = prepared.name;
      item.uploadId = prepared.uploadId;
      if (canUseChunks) {
        return uploadChunks(item, prepared.uploadId, Math.floor(advertisedChunkSize),
          Math.min(8, Math.max(2, Math.floor(advertisedParallelism))));
      }
      return uploadBody(item, prepared.uploadId);
    }).then(function (completed) {
      var timing = completed && completed.timing;
      var timingAvailable = timing && typeof timing === "object";
      var transferMs = timing && Number(timing.transferMs);
      var writeMs = timing && Number(timing.writeMs);
      var clientTransferMs = completed && Number(completed.clientTransferMs);
      var measuredMs = isFinite(transferMs) && transferMs > 0
        ? transferMs : clientTransferMs;
      var speed;
      item.state = "done"; item.progress = 100;
      item.message = t("savedAs") + " " + item.savedName;
      if (isFinite(measuredMs) && measuredMs > 0) {
        speed = Number(item.file.size) / 1048576 / (measuredMs / 1000);
        item.message += " · " + (isFinite(transferMs) && transferMs > 0 ? "PS5" : t("browser")) +
          ": " + formatRate(speed) + " · " +
          formatSeconds(measuredMs);
      } else if (timingAvailable) {
        item.message += " · " + t("ps5TimingUnavailable");
      }
      if (timingAvailable && isFinite(writeMs) && writeMs >= 0) {
        item.message += " · " + t("write") + " " + formatDuration(writeMs);
      }
      elements.interactionMessage.textContent = item.file.name + " " + t("sent") + ".";
    }).catch(function (error) {
      error = error || {};
      if (!error.code) error.code = "request_failed";
      if (error.code === "unauthorized") {
        forgetSession(t("sessionExpired"));
      }
      if (item.uploadId && !item.cancelRequested && error.code !== "cancelled") {
        jsonRequest("/api/v1/uploads/" + item.uploadId, { method: "DELETE" }).catch(function () {});
      }
      item.state = item.cancelRequested || error.code === "cancelled" ? "cancelled" : "error";
      item.message = item.state === "cancelled" ? t("cancelled") : describeError(error);
      elements.interactionMessage.textContent = item.file.name + ": " + item.message;
    }).then(function () { item.uploadId = null; running = false; renderQueue(); runQueue(); });
  }

  function cancelUpload(item) {
    if (item.cancelRequested) return;
    item.cancelRequested = true;
    item.message = t("cancelling");
    renderQueue();
    activeXhrs.slice().forEach(function (xhr) {
      try { xhr.abort(); } catch (_) {}
    });
    currentXhr = null;
    if (item.uploadId) {
      jsonRequest("/api/v1/uploads/" + item.uploadId, { method: "DELETE" }).catch(function () {});
    }
  }

  function addFiles(files) {
    Array.prototype.forEach.call(files, function (file) {
      if (maxFilesPerSession !== null && uploadQueue.length >= Number(maxFilesPerSession)) return;
      uploadQueue.push({ file: file, queueSequence: nextQueueSequence++, state: maxFileBytes !== null && file.size > Number(maxFileBytes) ? "error" : "queued", progress: 0,
        cancelRequested: false,
        message: maxFileBytes !== null && file.size > Number(maxFileBytes) ? t("fileTooLarge") : "" });
    });
    elements.fileInput.value = ""; renderQueue(); runQueue();
  }

  function renderOffline(message) {
    elements.receiverTransfer.hidden = true;
    setConnectionState("offline", t("offline"));
    elements.serverAddress.textContent = window.location.host || "—";
    elements.serverVersion.textContent = "—";
    elements.serverLimit.textContent = "—";
    if (!bearerToken) {
      setReceiverState("offline", t("ps5Unavailable"), message);
    } else {
      elements.serverSummary.textContent = message;
    }
  }

  function apiError(response) {
    return response.json().catch(function () {
      return null;
    }).then(function (payload) {
      var error = new Error("HTTP " + response.status);
      error.status = response.status;
      error.code = payload && payload.error ? payload.error.code : "request_failed";
      error.apiMessage = payload && payload.error && typeof payload.error.message === "string"
        ? payload.error.message
        : null;
      throw error;
    });
  }

  function changeStorage() {
    var previous = storageTarget;
    var target;
    if (!bearerToken) {
      elements.storageSelect.value = previous;
      return;
    }
    target = elements.storageSelect.value;
    storageRequestInFlight = true;
    delete elements.storageMessage.dataset.notice;
    renderStorage({ target: target }, { active: false });
    jsonRequest("/api/v1/storage", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ target: target })
    }).then(function (payload) {
      if (!payload || payload.target !== target) {
        throw { code: "invalid_response" };
      }
      storageTarget = target;
      elements.storageMessage.dataset.notice = "true";
      elements.storageMessage.textContent = t("selectedFolder", {
        name: storageLabel(target)
      });
    }).catch(function (error) {
      storageTarget = previous;
      elements.storageSelect.value = previous;
      elements.storageMessage.dataset.notice = "true";
      elements.storageMessage.textContent = describeError(error || {});
      if (error && error.code === "unauthorized") {
        forgetSession(t("sessionExpired"));
      }
    }).then(function () {
      storageRequestInFlight = false;
      renderStorage({ target: storageTarget }, { active: false });
    });
  }

  function scheduleStatusPoll() {
    if (!pageActive || statusPollTimer !== null ||
        document.visibilityState === "hidden") return;
    statusPollTimer = window.setTimeout(function () {
      statusPollTimer = null;
      loadStatus();
    }, STATUS_POLL_INTERVAL_MS);
  }

  function loadStatus() {
    if (!pageActive || statusRequestInFlight) return;
    var controller = typeof AbortController === "function" ? new AbortController() : null;
    var timeoutId = controller ? window.setTimeout(function () { controller.abort(); }, STATUS_TIMEOUT_MS) : null;
    var requestOptions = {
      method: "GET",
      cache: "no-store",
      credentials: "omit",
      headers: { "Accept": "application/json" }
    };
    statusRequestInFlight = true;
    if (statusPollTimer !== null) {
      window.clearTimeout(statusPollTimer);
      statusPollTimer = null;
    }
    if (!statusLoaded) {
      setConnectionState("checking", t("checking"));
      elements.serverSummary.textContent = t("loadingStatus");
    }
    if (controller) {
      requestOptions.signal = controller.signal;
    }
    window.fetch(STATUS_ENDPOINT, requestOptions)
      .then(function (response) {
        if (!response.ok) {
          throw new Error("status endpoint returned " + response.status);
        }
        return response.json();
      })
      .then(function (status) {
        if (!status || typeof status !== "object" || Array.isArray(status)) {
          throw new Error("invalid status response");
        }
        statusLoaded = true;
        renderStatus(status);
      })
      .catch(function () {
        renderOffline(navigator.onLine
          ? t("ps5NoResponse")
          : t("deviceOffline"));
      })
      .then(function () {
        if (timeoutId !== null) {
          window.clearTimeout(timeoutId);
        }
        statusRequestInFlight = false;
        scheduleStatusPoll();
      });
  }

  function setPinBusy(busy) {
    elements.requestPin.disabled = busy;
    elements.verifyPin.disabled = busy;
    elements.pinInput.disabled = busy;
  }

  function clearPin() {
    elements.pinInput.value = "";
  }

  function currentPin() {
    return elements.pinInput.value;
  }

  function requestChallenge() {
    bearerToken = null;
    challengeId = null;
    clearStoredSession();
    disableFiles();
    setPinBusy(true);
    elements.pinMessage.textContent = t("requestingPin");
    window.fetch(CHALLENGE_ENDPOINT, {
      method: "POST",
      cache: "no-store",
      credentials: "omit",
      headers: { "Accept": "application/json" }
    }).then(function (response) {
      return response.ok ? response.json() : apiError(response);
    }).then(function (payload) {
      if (!payload || typeof payload.challengeId !== "string") {
        throw new Error("invalid challenge response");
      }
      challengeId = payload.challengeId;
      bearerToken = null;
      clearPin();
      elements.pinForm.hidden = false;
      elements.requestPin.textContent = t("requestPinAgain");
      elements.pinStateLabel.textContent = t("pinSent");
      elements.pinMessage.textContent = t("enterPin");
      setReceiverState(
        "pending",
        t("ps5ShowsPin"),
        t("checkPin")
      );
      elements.pinInput.focus();
    }).catch(function () {
      challengeId = null;
      elements.pinMessage.textContent = t("pinRequestFailed");
      setReceiverState(
        "waiting",
        t("waitingConnection"),
        t("pinRequestFailed")
      );
    }).then(function () {
      setPinBusy(false);
    });
  }

  function verifyChallenge(event) {
    var pin;
    event.preventDefault();
    pin = currentPin();
    if (!challengeId || !/^\d{6}$/.test(pin)) {
      elements.pinMessage.textContent = t("enterSixDigits");
      elements.pinInput.focus();
      return;
    }
    setPinBusy(true);
    elements.pinMessage.textContent = t("checkingPin");
    window.fetch(VERIFY_ENDPOINT, {
      method: "POST",
      cache: "no-store",
      credentials: "omit",
      headers: {
        "Accept": "application/json",
        "Content-Type": "application/json"
      },
      body: JSON.stringify({ challengeId: challengeId, pin: pin })
    }).then(function (response) {
      return response.ok ? response.json() : apiError(response);
    }).then(function (payload) {
      if (!payload || typeof payload.token !== "string") {
        throw new Error("invalid verification response");
      }
      bearerToken = payload.token;
      persistSession(payload.token, payload.expiresIn);
      challengeId = null;
      clearPin();
      elements.pinForm.hidden = true;
      elements.pinCard.dataset.state = "authenticated";
      elements.pinStateLabel.textContent = t("confirmed");
      elements.pinMessage.textContent = t("pinConfirmedSession");
      elements.interactionMessage.textContent = t("authenticated");
      enableFiles();
    }).catch(function (error) {
      clearPin();
      elements.pinMessage.textContent = error.status === 429
        ? t("attemptsExceeded")
        : t("pinInvalid");
      if (error.status === 429) {
        challengeId = null;
        elements.pinForm.hidden = true;
      } else {
        elements.pinInput.focus();
      }
    }).then(function () {
      setPinBusy(false);
    });
  }

  elements.pinInput.addEventListener("input", function () {
    var digits = elements.pinInput.value.replace(/\D/g, "").slice(0, 6);
    if (elements.pinInput.value !== digits) {
      elements.pinInput.value = digits;
    }
  });

  elements.requestPin.addEventListener("click", requestChallenge);
  elements.pinForm.addEventListener("submit", verifyChallenge);
  elements.storageSelect.addEventListener("change", changeStorage);
  elements.fileInput.addEventListener("change", function () { addFiles(elements.fileInput.files); });
  elements.dropZone.addEventListener("keydown", function (event) {
    if ((event.key === "Enter" || event.key === " ") && !elements.fileInput.disabled) { event.preventDefault(); elements.fileInput.click(); }
  });
  ["dragenter", "dragover"].forEach(function (name) { elements.dropZone.addEventListener(name, function (event) { if (!elements.fileInput.disabled) { event.preventDefault(); elements.dropZone.dataset.drag = "true"; } }); });
  ["dragleave", "drop"].forEach(function (name) { elements.dropZone.addEventListener(name, function (event) { if (!elements.fileInput.disabled) { event.preventDefault(); delete elements.dropZone.dataset.drag; if (name === "drop") addFiles(event.dataTransfer.files); } }); });
  function queueButtonFromEvent(event) {
    var node = event.target;
    while (node && node !== elements.uploadQueue) {
      if (node.tagName && node.tagName.toLowerCase() === "button" &&
          node.dataset && node.dataset.index !== undefined) {
        return node;
      }
      node = node.parentNode;
    }
    return null;
  }

  function handleQueueAction(event) {
    var button = queueButtonFromEvent(event); var item; var index;
    if (!button) return;
    /* Progress updates rebuild the queue rows. Handle a pointer/touch down
     * before a slow PS5 browser can replace the pressed button, while the
     * normal click path keeps keyboard activation accessible. */
    if (event.type !== "click" && event.preventDefault) event.preventDefault();
    index = Number(button.dataset.index); item = uploadQueue[index]; if (!item) return;
    if (item.state === "uploading") {
      cancelUpload(item);
    } else if (item.state === "error" || item.state === "cancelled") {
      item.cancelRequested = false;
      item.state = "queued"; item.progress = 0; item.message = ""; renderQueue(); runQueue();
    }
  }
  ["pointerdown", "touchstart", "mousedown", "click"].forEach(function (eventName) {
    elements.uploadQueue.addEventListener(eventName, handleQueueAction);
  });
  window.addEventListener("online", loadStatus);
  window.addEventListener("offline", function () {
    renderOffline(t("deviceOffline"));
  });
  window.addEventListener("pagehide", function () {
    pageActive = false;
    if (statusPollTimer !== null) {
      window.clearTimeout(statusPollTimer);
      statusPollTimer = null;
    }
    challengeId = null;
  });
  window.addEventListener("pageshow", function () {
    pageActive = true;
    loadStatus();
  });
  document.addEventListener("visibilitychange", function () {
    if (document.visibilityState === "hidden") {
      if (statusPollTimer !== null) {
        window.clearTimeout(statusPollTimer);
        statusPollTimer = null;
      }
    } else {
      loadStatus();
    }
  });
  restoreSession();
  loadStatus();
}());
