/*
 * ============================================================================
 *  wh-repeater - Web Management Application
 * ============================================================================
 *  Copyright (c) 2026 Phil Taylor (M0VSE)
 *
 *  Purpose:
 *    Implements the browser-side management UI, status polling,
 *    receiver/SD1/TX rendering, configuration editing, dirty-state
 *    handling, and API calls.
 *
 *  Project notes:
 *    wh-repeater is a fresh C++ daemon for a Winterhill-derived DVB repeater.
 *    It uses the original Winterhill application only as hardware reference and
 *    talks to the existing whdriver kernel module for board access.
 * ============================================================================
 */

const state = {
  config: null,
  status: null,
  dirty: false,
  sd1EnabledOverride: null,
};

const el = {
  apiState: document.querySelector("#api-state"),
  saveState: document.querySelector("#save-state"),
  statusGrid: document.querySelector("#status-grid"),
  txStatusPanel: document.querySelector("#tx-status-panel"),
  receivers: document.querySelector("#receivers"),
  refresh: document.querySelector("#refresh"),
  save: document.querySelector("#save"),
  serviceRestart: document.querySelector("#service-restart"),
  addReceiver: document.querySelector("#add-receiver"),
  statusInterval: document.querySelector("#status-interval"),
  minimumMer: document.querySelector("#minimum-mer"),
  minimumDNumber: document.querySelector("#minimum-dnumber"),
  plutoAddress: document.querySelector("#pluto-address"),
  plutoPort: document.querySelector("#pluto-port"),
  plutoMqttEnabled: document.querySelector("#pluto-mqtt-enabled"),
  plutoMqttHost: document.querySelector("#pluto-mqtt-host"),
  plutoMqttPort: document.querySelector("#pluto-mqtt-port"),
  plutoMqttProtocol: document.querySelector("#pluto-mqtt-protocol"),
  plutoMqttDeviceId: document.querySelector("#pluto-mqtt-device-id"),
  plutoCallsign: document.querySelector("#pluto-callsign"),
  plutoSystem: document.querySelector("#pluto-system"),
  plutoTxFrequency: document.querySelector("#pluto-tx-frequency"),
  plutoSymbolRate: document.querySelector("#pluto-symbol-rate"),
  plutoGain: document.querySelector("#pluto-gain"),
  plutoNco: document.querySelector("#pluto-nco"),
  plutoPilots: document.querySelector("#pluto-pilots"),
  plutoFrame: document.querySelector("#pluto-frame"),
  plutoFecMode: document.querySelector("#pluto-fec-mode"),
  plutoConstellation: document.querySelector("#pluto-constellation"),
  muxRate: document.querySelector("#mux-rate"),
  videoBitrate: document.querySelector("#video-bitrate"),
  audioBitrate: document.querySelector("#audio-bitrate"),
  outputWidth: document.querySelector("#output-width"),
  outputHeight: document.querySelector("#output-height"),
  outputFrameRate: document.querySelector("#output-frame-rate"),
  plutoFec: document.querySelector("#pluto-fec"),
  hardwarePttEnabled: document.querySelector("#hardware-ptt-enabled"),
  hardwarePttChip: document.querySelector("#hardware-ptt-chip"),
  hardwarePttLine: document.querySelector("#hardware-ptt-line"),
  hardwarePttActiveHigh: document.querySelector("#hardware-ptt-active-high"),
  watermarkText: document.querySelector("#watermark-text"),
  fallbackEnabled: document.querySelector("#fallback-enabled"),
  fallbackHardwareDecode: document.querySelector("#fallback-hardware-decode"),
  fallbackTimeout: document.querySelector("#fallback-timeout"),
  fallbackStill: document.querySelector("#fallback-still"),
  fallbackSlideDirectory: document.querySelector("#fallback-slide-directory"),
  fallbackXmasSlideDirectory: document.querySelector("#fallback-xmas-slide-directory"),
  fallbackSlideDuration: document.querySelector("#fallback-slide-duration"),
  fallbackVideos: document.querySelector("#fallback-videos"),
  fallbackPlayVideo: document.querySelector("#fallback-play-video"),
  fallbackStopVideo: document.querySelector("#fallback-stop-video"),
  rtmpEnabled: document.querySelector("#rtmp-enabled"),
  rtmpUrl: document.querySelector("#rtmp-url"),
  beaconScheduleEnabled: document.querySelector("#beacon-schedule-enabled"),
  beaconStartTime: document.querySelector("#beacon-start-time"),
  beaconEndTime: document.querySelector("#beacon-end-time"),
  analogueCaptureEnabled: document.querySelector("#analogue-capture-enabled"),
  analogueCaptureReceiverId: document.querySelector("#analogue-capture-receiver-id"),
  analogueCaptureDeviceId: document.querySelector("#analogue-capture-device-id"),
  analogueCaptureLabel: document.querySelector("#analogue-capture-label"),
  analogueCaptureDevice: document.querySelector("#analogue-capture-device"),
  analogueCaptureStandard: document.querySelector("#analogue-capture-standard"),
  analogueCaptureWidth: document.querySelector("#analogue-capture-width"),
  analogueCaptureHeight: document.querySelector("#analogue-capture-height"),
  analogueCaptureFrameRate: document.querySelector("#analogue-capture-frame-rate"),
  analogueCaptureFrameRateNumerator: document.querySelector("#analogue-capture-frame-rate-numerator"),
  analogueCaptureFrameRateDenominator: document.querySelector("#analogue-capture-frame-rate-denominator"),
  analogueCaptureLockMode: document.querySelector("#analogue-capture-lock-mode"),
  analogueCaptureGpioChip: document.querySelector("#analogue-capture-gpio-chip"),
  analogueCaptureGpioLine: document.querySelector("#analogue-capture-gpio-line"),
  analogueCaptureGpioActiveHigh: document.querySelector("#analogue-capture-gpio-active-high"),
  sd1Enabled: document.querySelector("#sd1-enabled"),
  sd1ReceiverId: document.querySelector("#sd1-receiver-id"),
  sd1DeviceId: document.querySelector("#sd1-device-id"),
  sd1I2cDevice: document.querySelector("#sd1-i2c-device"),
  sd1I2cAddress: document.querySelector("#sd1-i2c-address"),
  sd1Source: document.querySelector("#sd1-source"),
  sd1CaptureDevice: document.querySelector("#sd1-capture-device"),
  sd1CaptureWidth: document.querySelector("#sd1-capture-width"),
  sd1CaptureHeight: document.querySelector("#sd1-capture-height"),
  sd1CaptureFrameRate: document.querySelector("#sd1-capture-frame-rate"),
  identEnabled: document.querySelector("#ident-enabled"),
  serviceName: document.querySelector("#service-name"),
  identInterval: document.querySelector("#ident-interval"),
  identMorseTone: document.querySelector("#ident-morse-tone"),
  identMorseWpm: document.querySelector("#ident-morse-wpm"),
  receiverTemplate: document.querySelector("#receiver-template"),
  targetTemplate: document.querySelector("#target-template"),
};

function setMessage(message, ok = true) {
  el.apiState.textContent = message;
  el.apiState.style.color = ok ? "#647080" : "#b3261e";
}

function setSaveMessage(message, ok = true) {
  el.saveState.textContent = message;
  el.saveState.style.color = ok ? "#647080" : "#b3261e";
}

function numberValue(input, fallback = 0) {
  const value = Number(input.value);
  return Number.isFinite(value) ? value : fallback;
}

function clampedEvenValue(input, fallback, minimum, maximum) {
  const value = Math.max(minimum, Math.min(maximum, numberValue(input, fallback)));
  return Math.floor(value / 2) * 2;
}

function formatValue(value, suffix = "") {
  return value === null || value === undefined ? "-" : `${value}${suffix}`;
}

function fixedReceiverAntenna(receiverId) {
  return receiverId === 1 || receiverId === 3 ? "top" : "bottom";
}

function receiverNim(receiverId) {
  return receiverId <= 2 ? "A" : "B";
}

function receiverTuner(receiverId) {
  return receiverId === 1 || receiverId === 3 ? 1 : 2;
}

function receiverHardwareLabel(receiver) {
  if (receiver.type === "analogue") {
    return receiver.deviceId || "sd1";
  }
  return `NIM ${receiver.nim || receiverNim(receiver.id)} ${receiver.antenna || fixedReceiverAntenna(receiver.id)} antenna`;
}

function stateBadge(status) {
  const locked = status === "lockedDvbs" || status === "lockedDvbs2" || status === "lockedAnalogue";
  return `<span class="badge ${locked ? "locked" : ""}">${status || "idle"}</span>`;
}

function sd1Placeholder() {
  const sd1 = state.config?.analogue?.sd1 || {};
  const analogue = state.status?.analogue || {};
  return {
    id: sd1.receiverId ?? 5,
    name: "SD1",
    type: "analogue",
    deviceId: sd1.deviceId ?? "sd1",
    state: analogue.present ? "idle" : "fault",
    present: Boolean(analogue.present),
    ready: Boolean(analogue.ready),
    locked: Boolean(analogue.locked),
    cameraRunning: Boolean(analogue.cameraRunning),
    source: analogue.activeSource || analogue.selectedSource || "",
    firmwareVersion: analogue.firmwareVersion || "",
    hardwareId: analogue.hardwareId || "",
    rawLock: analogue.rawLock ?? 0,
    error: analogue.error || "Waiting for SD1 status",
  };
}

function renderStatus() {
  const sd1Enabled = state.sd1EnabledOverride
    ?? state.status?.analogue?.enabled
    ?? state.config?.analogue?.sd1?.enabled
    ?? false;
  const statuses = (state.status?.receivers || []).filter((receiver) => receiver.type !== "analogue" || sd1Enabled);
  if (sd1Enabled && !statuses.some((receiver) => receiver.type === "analogue")) {
    statuses.push(sd1Placeholder());
  }
  const activeReceiver = state.status?.activeReceiver;

  el.statusGrid.innerHTML = "";
  for (const receiver of statuses) {
    const card = document.createElement("article");
    card.className = `status-card ${receiver.id === activeReceiver ? "active" : ""}`;
    const target = receiver.target;
    const title = receiver.name || `RX${receiver.id}`;
    const isAnalogue = receiver.type === "analogue";
    const targetText = target ? `${target.frequencyKhz} kHz / ${target.symbolRateKs} kS` : "-";
    if (isAnalogue) {
      card.innerHTML = `
        <div class="status-title">
          <h2>${title}</h2>
          ${stateBadge(receiver.state)}
        </div>
        <dl>
          <dt>Detected</dt>
          <dd>${receiver.present ? "yes" : "no"}</dd>
          <dt>Ready</dt>
          <dd>${receiver.ready ? "yes" : "no"}</dd>
          <dt>Locked</dt>
          <dd>${receiver.locked ? "yes" : "no"}</dd>
          <dt>Raw lock</dt>
          <dd>${receiver.rawLock ?? "-"}</dd>
          <dt>Source</dt>
          <dd>${receiver.source || receiver.serviceName || "-"}</dd>
          <dt>CSI active</dt>
          <dd>${receiver.cameraRunning ? "yes" : "no"}</dd>
          <dt>Version</dt>
          <dd>${receiver.firmwareVersion || "-"}</dd>
          <dt>Device ID</dt>
          <dd>${receiver.deviceId || "-"}</dd>
          <dt>Hardware ID</dt>
          <dd>${receiver.hardwareId || "-"}</dd>
          <dt>Error</dt>
          <dd>${receiver.error || "-"}</dd>
        </dl>
      `;
      el.statusGrid.appendChild(card);
      continue;
    }

    card.innerHTML = `
      <div class="status-title">
        <h2>${title}</h2>
        ${stateBadge(receiver.state)}
      </div>
      <dl>
        <dt>${receiver.type === "analogue" ? "Source" : "Target"}</dt>
        <dd>${targetText}</dd>
        <dt>Hardware</dt>
        <dd>${receiverHardwareLabel(receiver)}</dd>
        <dt>FEC</dt>
        <dd>${target?.fec || "-"}</dd>
        <dt>MER</dt>
        <dd>${formatValue(receiver.merDb, " dB")}</dd>
        <dt>D-number</dt>
        <dd>${formatValue(receiver.dNumberDb, " dB")}</dd>
        <dt>TS packets</dt>
        <dd>${receiver.transportPackets ?? 0}</dd>
        <dt>CC errors</dt>
        <dd>${receiver.continuityErrors ?? 0}</dd>
      </dl>
    `;
    el.statusGrid.appendChild(card);
  }

  if (statuses.length === 0) {
    el.statusGrid.innerHTML = `<section class="panel">No receiver status yet.</section>`;
  }
  renderTxStatus();
}

function appendStatusRow(list, label, value) {
  const dt = document.createElement("dt");
  dt.textContent = label;
  const dd = document.createElement("dd");
  dd.textContent = value === null || value === undefined || value === "" ? "-" : String(value);
  list.appendChild(dt);
  list.appendChild(dd);
}

function txValue(values, key) {
  return values?.[key] ?? null;
}

function fecFraction(fec) {
  const [numerator, denominator] = String(fec || "1/2").split("/").map(Number);
  if (!Number.isFinite(numerator) || !Number.isFinite(denominator) || numerator <= 0 || denominator <= 0) {
    return [1, 2];
  }
  return [numerator, denominator];
}

function constellationBits(system, constellation) {
  if (system === "dvbs") {
    return 2;
  }
  return {
    qpsk: 2,
    "8psk": 3,
    "16apsk": 4,
    "32apsk": 5,
  }[constellation] ?? 2;
}

function calculatePlutoMuxRate() {
  return calculatePlutoMuxRateFromConfig({
    symbolRateS: numberValue(el.plutoSymbolRate, 333000),
    fec: el.plutoFec.value,
    system: el.plutoSystem.value,
    constellation: el.plutoConstellation.value,
  });
}

function calculatePlutoMuxRateFromConfig(pluto) {
  const symbolRate = Number(pluto?.symbolRateS ?? 333000);
  const [fecNumerator, fecDenominator] = fecFraction(pluto?.fec ?? "1/2");
  const bits = constellationBits(pluto?.system ?? "dvbs2", pluto?.constellation ?? "qpsk");
  let numerator = symbolRate * bits * fecNumerator;
  let denominator = fecDenominator;
  if (pluto?.system === "dvbs") {
    numerator *= 188;
    denominator *= 204;
  }
  return Math.floor(Math.floor(numerator / denominator) / 1000);
}

function updateCalculatedMediaRates() {
  const muxRate = calculatePlutoMuxRate();
  const audioRate = numberValue(el.audioBitrate, 96);
  el.muxRate.value = muxRate;
  el.videoBitrate.value = Math.max(1, muxRate - audioRate);
}

function renderTxStatus() {
  const pluto = state.status?.pluto;
  el.txStatusPanel.innerHTML = "";

  const head = document.createElement("div");
  head.className = "panel-head";
  const title = document.createElement("h2");
  title.textContent = "TX Status";
  const badge = document.createElement("span");
  badge.className = `badge ${pluto?.connected ? "locked" : ""}`;
  badge.textContent = pluto?.connected ? "connected" : "disconnected";
  head.appendChild(title);
  head.appendChild(badge);
  el.txStatusPanel.appendChild(head);

  const values = pluto?.values || {};
  const list = document.createElement("dl");
  list.className = "tx-status-grid";
  appendStatusRow(list, "MQTT", pluto?.enabled === false ? "disabled" : `${pluto?.host || "-"}:${pluto?.port || "-"}`);
  appendStatusRow(list, "Protocol", pluto?.protocol || "-");
  appendStatusRow(list, "Device ID", pluto?.deviceId || "-");
  appendStatusRow(list, "Callsign", pluto?.callsign);
  appendStatusRow(list, "Frequency", txValue(values, "tx/frequency"));
  appendStatusRow(list, "Gain", txValue(values, "tx/gain"));
  appendStatusRow(list, "Mute", txValue(values, "tx/mute"));
  appendStatusRow(list, "Stream mode", txValue(values, "tx/stream/mode"));
  appendStatusRow(list, "Symbol rate", txValue(values, "tx/dvbs2/sr"));
  appendStatusRow(list, "FEC", txValue(values, "tx/dvbs2/fec") || txValue(values, "tx/dvbs2/ts/fecvariable"));
  appendStatusRow(list, "TS bitrate", txValue(values, "tx/dvbs2/ts/bitrate"));
  appendStatusRow(list, "Queue", txValue(values, "tx/dvbs2/queue"));
  appendStatusRow(list, "Version", txValue(values, "system/version"));
  appendStatusRow(list, "Beacon schedule", state.status?.beaconSchedule?.enabled
    ? `${state.status.beaconSchedule.active ? "active" : "inactive"} ${state.status.beaconSchedule.startTime}-${state.status.beaconSchedule.endTime}`
    : "disabled");
  appendStatusRow(list, "Updated", pluto ? `${pluto.updatedMsAgo ?? 0} ms ago` : null);
  appendStatusRow(list, "Error", pluto?.error);
  el.txStatusPanel.appendChild(list);
}

function fillConfigForm() {
  const config = state.config;
  if (!config) {
    return;
  }

  el.statusInterval.value = config.statusIntervalMs ?? 500;
  el.minimumMer.value = config.selection?.minimumMerDb ?? 2;
  el.minimumDNumber.value = config.selection?.minimumDNumberDb ?? 0;
  el.plutoAddress.value = config.pluto?.address ?? "230.10.0.1";
  el.plutoPort.value = config.pluto?.port ?? 1234;
  el.plutoMqttEnabled.checked = config.pluto?.mqttEnabled ?? true;
  el.plutoMqttHost.value = config.pluto?.mqttHost ?? "192.168.2.1";
  el.plutoMqttPort.value = config.pluto?.mqttPort ?? 1883;
  el.plutoMqttProtocol.value = config.pluto?.mqttProtocol ?? "pluto-ori";
  el.plutoMqttDeviceId.value = config.pluto?.mqttDeviceId ?? "";
  el.plutoCallsign.value = config.pluto?.callsign ?? "GB3GV";
  el.plutoSystem.value = config.pluto?.system ?? "dvbs2";
  el.plutoTxFrequency.value = config.pluto?.txFrequencyHz ?? 2400000000;
  el.plutoSymbolRate.value = config.pluto?.symbolRateS ?? 333000;
  el.plutoGain.value = config.pluto?.txGainDb ?? -40;
  el.plutoNco.value = config.pluto?.ncoHz ?? 0;
  el.plutoPilots.checked = Boolean(config.pluto?.pilots);
  el.plutoFrame.value = config.pluto?.frame ?? "long";
  el.plutoFecMode.value = config.pluto?.fecMode ?? "fixed";
  el.plutoConstellation.value = config.pluto?.constellation ?? "qpsk";
  el.muxRate.value = config.pluto?.muxRateKbps ?? 1200;
  el.videoBitrate.value = config.pluto?.videoBitrateKbps ?? 900;
  el.audioBitrate.value = config.pluto?.audioBitrateKbps ?? 96;
  el.outputWidth.value = config.pluto?.outputWidth ?? 1280;
  el.outputHeight.value = config.pluto?.outputHeight ?? 720;
  el.outputFrameRate.value = config.pluto?.outputFrameRate ?? 25;
  el.plutoFec.value = config.pluto?.fec ?? "1/2";
  el.hardwarePttEnabled.checked = Boolean(config.hardwarePtt?.enabled);
  el.hardwarePttChip.value = config.hardwarePtt?.chip ?? "/dev/gpiochip0";
  el.hardwarePttLine.value = config.hardwarePtt?.line ?? 0;
  el.hardwarePttActiveHigh.checked = config.hardwarePtt?.activeHigh ?? true;
  el.watermarkText.value = config.pluto?.watermarkText ?? "WH Repeater";
  el.fallbackEnabled.checked = config.fallback?.enabled ?? true;
  el.fallbackHardwareDecode.checked = Boolean(config.fallback?.hardwareDecode);
  el.fallbackTimeout.value = config.fallback?.inputTimeoutMs ?? 1500;
  el.fallbackStill.value = config.fallback?.stillPath ?? "";
  el.fallbackSlideDirectory.value = config.fallback?.slideDirectory ?? "/var/lib/wh-repeater/slides";
  el.fallbackXmasSlideDirectory.value = config.fallback?.christmasSlideDirectory ?? "/var/lib/wh-repeater/slides/christmas";
  el.fallbackSlideDuration.value = config.fallback?.slideDurationSeconds ?? 10;
  el.fallbackVideos.value = (config.fallback?.videoPaths ?? []).join(",");
  el.rtmpEnabled.checked = Boolean(config.streaming?.rtmp?.enabled);
  el.rtmpUrl.value = config.streaming?.rtmp?.url ?? "";
  el.beaconScheduleEnabled.checked = Boolean(config.beaconSchedule?.enabled);
  el.beaconStartTime.value = config.beaconSchedule?.startTime ?? "09:00";
  el.beaconEndTime.value = config.beaconSchedule?.endTime ?? "23:00";
  el.analogueCaptureEnabled.checked = config.analogue?.capture?.enabled ?? false;
  el.analogueCaptureReceiverId.value = config.analogue?.capture?.receiverId ?? 5;
  el.analogueCaptureDeviceId.value = config.analogue?.capture?.deviceId ?? "usb-capture";
  el.analogueCaptureLabel.value = config.analogue?.capture?.label ?? "USB analogue";
  el.analogueCaptureDevice.value = config.analogue?.capture?.captureDevice ?? "/dev/video0";
  el.analogueCaptureStandard.value = config.analogue?.capture?.captureStandard ?? "pal";
  el.analogueCaptureWidth.value = config.analogue?.capture?.captureWidth ?? 720;
  el.analogueCaptureHeight.value = config.analogue?.capture?.captureHeight ?? 576;
  el.analogueCaptureFrameRate.value = config.analogue?.capture?.captureFrameRate ?? 25;
  el.analogueCaptureFrameRateNumerator.value = config.analogue?.capture?.captureFrameRateNumerator ?? config.analogue?.capture?.captureFrameRate ?? 25;
  el.analogueCaptureFrameRateDenominator.value = config.analogue?.capture?.captureFrameRateDenominator ?? 1;
  el.analogueCaptureLockMode.value = config.analogue?.capture?.lockMode ?? "v4l2-sync";
  el.analogueCaptureGpioChip.value = config.analogue?.capture?.gpioChip ?? "/dev/gpiochip0";
  el.analogueCaptureGpioLine.value = config.analogue?.capture?.gpioLine ?? 26;
  el.analogueCaptureGpioActiveHigh.checked = config.analogue?.capture?.gpioActiveHigh ?? true;
  el.sd1Enabled.checked = config.analogue?.sd1?.enabled ?? false;
  el.sd1ReceiverId.value = config.analogue?.sd1?.receiverId ?? 5;
  el.sd1DeviceId.value = config.analogue?.sd1?.deviceId ?? "sd1";
  el.sd1I2cDevice.value = config.analogue?.sd1?.i2cDevice ?? "/dev/i2c-0";
  el.sd1I2cAddress.value = config.analogue?.sd1?.i2cAddress ?? 64;
  el.sd1Source.value = config.analogue?.sd1?.source ?? "auto";
  el.sd1CaptureDevice.value = config.analogue?.sd1?.captureDevice ?? "/dev/video0";
  el.sd1CaptureWidth.value = config.analogue?.sd1?.captureWidth ?? 640;
  el.sd1CaptureHeight.value = config.analogue?.sd1?.captureHeight ?? 480;
  el.sd1CaptureFrameRate.value = config.analogue?.sd1?.captureFrameRate ?? 25;
  el.identEnabled.checked = Boolean(config.ident?.enabled);
  el.serviceName.value = config.ident?.serviceName ?? "WH Repeater";
  el.identInterval.value = config.ident?.intervalSeconds ?? 600;
  el.identMorseTone.value = config.ident?.morseToneHz ?? 650;
  el.identMorseWpm.value = config.ident?.morseWpm ?? 10;

  el.receivers.innerHTML = "";
  for (const receiver of config.receivers || []) {
    el.receivers.appendChild(receiverNode(receiver));
  }
  updatePlutoSystemFields();
  updateCalculatedMediaRates();
}

function receiverNode(receiver) {
  const node = el.receiverTemplate.content.firstElementChild.cloneNode(true);
  node.querySelector(".receiver-id").value = receiver.id;
  node.querySelector(".receiver-enabled").checked = Boolean(receiver.enabled);
  node.querySelector(".receiver-dwell").value = receiver.dwellMs ?? 1500;
  node.querySelector(".receiver-hang").value = receiver.hangMs ?? 5000;
  node.querySelector(".receiver-physical").textContent =
    `NIM ${receiverNim(receiver.id)} ${fixedReceiverAntenna(receiver.id)} antenna`;

  const targetList = node.querySelector(".targets");
  for (const target of receiver.targets || []) {
    targetList.appendChild(targetNode(target));
  }
  if ((receiver.targets || []).length === 0) {
    targetList.appendChild(emptyTargetMessage());
  }

  node.querySelector(".add-target").addEventListener("click", () => {
    const empty = targetList.querySelector(".empty");
    if (empty) {
      empty.remove();
    }
    targetList.appendChild(targetNode({
      frequencyKhz: 10491500,
      symbolRateKs: 1500,
      localOscillatorKhz: 9750000,
      antenna: fixedReceiverAntenna(receiver.id),
      system: "auto",
      fec: "auto",
      label: "",
    }));
    markDirty();
  });

  node.addEventListener("input", markDirty);
  node.addEventListener("change", markDirty);
  return node;
}

function emptyTargetMessage() {
  const item = document.createElement("div");
  item.className = "empty";
  item.textContent = "No scan targets configured.";
  return item;
}

function targetNode(target) {
  const node = el.targetTemplate.content.firstElementChild.cloneNode(true);
  node.querySelector(".target-label").value = target.label ?? "";
  node.querySelector(".target-frequency").value = target.frequencyKhz ?? 0;
  node.querySelector(".target-symbol-rate").value = target.symbolRateKs ?? 0;
  node.querySelector(".target-lo").value = target.localOscillatorKhz ?? 9750000;
  node.querySelector(".target-system").value = target.system ?? "auto";
  node.querySelector(".target-fec").value = target.fec ?? "auto";
  node.querySelector(".remove-target").addEventListener("click", () => {
    const parent = node.parentElement;
    node.remove();
    if (parent && parent.querySelectorAll(".target").length === 0) {
      parent.appendChild(emptyTargetMessage());
    }
    markDirty();
  });
  return node;
}

function readConfigForm() {
  updateCalculatedMediaRates();
  const receivers = [...el.receivers.querySelectorAll(".receiver")].map((receiver) => ({
    id: numberValue(receiver.querySelector(".receiver-id"), 1),
    enabled: receiver.querySelector(".receiver-enabled").checked,
    dwellMs: numberValue(receiver.querySelector(".receiver-dwell"), 1500),
    hangMs: numberValue(receiver.querySelector(".receiver-hang"), 5000),
    targets: [...receiver.querySelectorAll(".target")].map((target) => ({
      label: target.querySelector(".target-label").value,
      frequencyKhz: numberValue(target.querySelector(".target-frequency")),
      symbolRateKs: numberValue(target.querySelector(".target-symbol-rate")),
      localOscillatorKhz: numberValue(target.querySelector(".target-lo"), 9750000),
      antenna: fixedReceiverAntenna(numberValue(receiver.querySelector(".receiver-id"), 1)),
      system: target.querySelector(".target-system").value,
      fec: target.querySelector(".target-fec").value,
    })),
  }));

  return {
    statusIntervalMs: numberValue(el.statusInterval, 500),
    selection: {
      minimumMerDb: numberValue(el.minimumMer, 2),
      minimumDNumberDb: numberValue(el.minimumDNumber, 0),
    },
    pluto: {
      address: el.plutoAddress.value,
      port: numberValue(el.plutoPort, 1234),
      mqttEnabled: el.plutoMqttEnabled.checked,
      mqttHost: el.plutoMqttHost.value || "192.168.2.1",
      mqttPort: numberValue(el.plutoMqttPort, 1883),
      mqttProtocol: el.plutoMqttProtocol.value,
      mqttDeviceId: el.plutoMqttDeviceId.value.trim(),
      callsign: el.plutoCallsign.value || "GB3GV",
      system: el.plutoSystem.value,
      txFrequencyHz: numberValue(el.plutoTxFrequency, 2400000000),
      symbolRateS: numberValue(el.plutoSymbolRate, 333000),
      txGainDb: numberValue(el.plutoGain, -40),
      ncoHz: numberValue(el.plutoNco, 0),
      pilots: el.plutoPilots.checked,
      frame: el.plutoFrame.value,
      fecMode: el.plutoFecMode.value,
      constellation: el.plutoConstellation.value,
      muxRateKbps: numberValue(el.muxRate, 1200),
      videoBitrateKbps: numberValue(el.videoBitrate, 900),
      audioBitrateKbps: numberValue(el.audioBitrate, 96),
      outputWidth: clampedEvenValue(el.outputWidth, 1280, 320, 1920),
      outputHeight: clampedEvenValue(el.outputHeight, 720, 240, 1080),
      outputFrameRate: Math.max(1, Math.min(50, numberValue(el.outputFrameRate, 25))),
      fec: el.plutoFec.value,
      watermarkText: el.watermarkText.value,
    },
    fallback: {
      enabled: el.fallbackEnabled.checked,
      hardwareDecode: el.fallbackHardwareDecode.checked,
      inputTimeoutMs: numberValue(el.fallbackTimeout, 1500),
      staticFrameRate: state.config?.fallback?.staticFrameRate ?? 2,
      stillPath: el.fallbackStill.value,
      slideDirectory: el.fallbackSlideDirectory.value || "/var/lib/wh-repeater/slides",
      christmasSlideDirectory: el.fallbackXmasSlideDirectory.value || "/var/lib/wh-repeater/slides/christmas",
      slideDurationSeconds: numberValue(el.fallbackSlideDuration, 10),
      videoPaths: el.fallbackVideos.value.split(",").map((value) => value.trim()).filter(Boolean),
    },
    streaming: {
      rtmp: {
        enabled: el.rtmpEnabled.checked,
        url: el.rtmpUrl.value.trim(),
      },
    },
    hardwarePtt: {
      enabled: el.hardwarePttEnabled.checked,
      chip: el.hardwarePttChip.value || "/dev/gpiochip0",
      line: numberValue(el.hardwarePttLine, 0),
      activeHigh: el.hardwarePttActiveHigh.checked,
    },
    beaconSchedule: {
      enabled: el.beaconScheduleEnabled.checked,
      startTime: el.beaconStartTime.value || "09:00",
      endTime: el.beaconEndTime.value || "23:00",
    },
    analogue: {
      capture: {
        enabled: el.analogueCaptureEnabled.checked,
        receiverId: numberValue(el.analogueCaptureReceiverId, 5),
        deviceId: el.analogueCaptureDeviceId.value || "usb-capture",
        label: el.analogueCaptureLabel.value || "USB analogue",
        captureDevice: el.analogueCaptureDevice.value || "/dev/video0",
        captureStandard: el.analogueCaptureStandard.value || "pal",
        captureWidth: numberValue(el.analogueCaptureWidth, 720),
        captureHeight: numberValue(el.analogueCaptureHeight, 576),
        captureFrameRate: numberValue(el.analogueCaptureFrameRate, 25),
        captureFrameRateNumerator: numberValue(el.analogueCaptureFrameRateNumerator, numberValue(el.analogueCaptureFrameRate, 25)),
        captureFrameRateDenominator: numberValue(el.analogueCaptureFrameRateDenominator, 1),
        lockMode: el.analogueCaptureLockMode.value || "v4l2-sync",
        gpioChip: el.analogueCaptureGpioChip.value || "/dev/gpiochip0",
        gpioLine: numberValue(el.analogueCaptureGpioLine, 26),
        gpioActiveHigh: el.analogueCaptureGpioActiveHigh.checked,
      },
      sd1: {
        enabled: el.sd1Enabled.checked,
        receiverId: numberValue(el.sd1ReceiverId, 5),
        deviceId: el.sd1DeviceId.value || "sd1",
        i2cDevice: el.sd1I2cDevice.value || "/dev/i2c-0",
        i2cAddress: numberValue(el.sd1I2cAddress, 64),
        source: el.sd1Source.value,
        captureDevice: el.sd1CaptureDevice.value || "/dev/video0",
        captureWidth: numberValue(el.sd1CaptureWidth, 640),
        captureHeight: numberValue(el.sd1CaptureHeight, 480),
        captureFrameRate: numberValue(el.sd1CaptureFrameRate, 25),
      },
    },
    ident: {
      enabled: el.identEnabled.checked,
      serviceName: el.serviceName.value,
      intervalSeconds: numberValue(el.identInterval, 600),
      morseToneHz: numberValue(el.identMorseTone, 650),
      morseWpm: numberValue(el.identMorseWpm, 10),
    },
    receivers,
  };
}

function updatePlutoSystemFields() {
  const s2Only = [
    el.plutoPilots,
    el.plutoFrame,
    el.plutoFecMode,
    el.plutoConstellation,
  ];
  const dvbs = el.plutoSystem.value === "dvbs";
  for (const input of s2Only) {
    input.disabled = dvbs;
    const label = input.closest("label");
    if (label) {
      label.classList.toggle("disabled", dvbs);
    }
  }
  updateCalculatedMediaRates();
}

function markDirty() {
  state.dirty = true;
  setSaveMessage("Unsaved changes");
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function canonicalConfig(value) {
  if (Array.isArray(value)) {
    return value.map(canonicalConfig);
  }
  if (value && typeof value === "object") {
    return Object.fromEntries(
      Object.keys(value).sort().map((key) => [key, canonicalConfig(value[key])]),
    );
  }
  return value;
}

function comparableConfig(config) {
  const copy = structuredClone(config);
  if (copy.pluto) {
    copy.pluto.muxRateKbps = calculatePlutoMuxRateFromConfig(copy.pluto);
    copy.pluto.videoBitrateKbps = Math.max(1, copy.pluto.muxRateKbps - Number(copy.pluto.audioBitrateKbps ?? 96));
  }
  for (const receiver of copy.receivers || []) {
    for (const target of receiver.targets || []) {
      target.antenna = fixedReceiverAntenna(receiver.id);
    }
  }
  return canonicalConfig(copy);
}

function configMatches(left, right) {
  return JSON.stringify(comparableConfig(left)) === JSON.stringify(comparableConfig(right));
}

async function loadStatus() {
  const response = await fetch("/api/status", { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`status ${response.status}`);
  }
  state.status = await response.json();
  renderStatus();
}

async function loadConfig() {
  const response = await fetch("/api/config", { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`config ${response.status}`);
  }
  state.config = await response.json();
  if (!state.dirty) {
    fillConfigForm();
  }
}

async function fetchConfig() {
  const response = await fetch("/api/config", { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`config ${response.status}`);
  }
  return response.json();
}

async function fetchAppliedConfig(expected) {
  let latest = null;
  for (let attempt = 0; attempt < 24; attempt += 1) {
    await delay(250);
    latest = await fetchConfig();
    if (configMatches(latest, expected)) {
      return latest;
    }
  }
  return null;
}

async function refreshAll() {
  try {
    await Promise.all([loadStatus(), loadConfig()]);
    setMessage("Connected");
  } catch (error) {
    setMessage(`API unavailable: ${error.message}`, false);
  }
}

async function saveConfig() {
  const config = readConfigForm();
  setSaveMessage("Saving");
  const response = await fetch("/api/config", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(config),
  });
  if (!response.ok) {
    const body = await response.json().catch(() => ({ error: `HTTP ${response.status}` }));
    throw new Error(body.error || `HTTP ${response.status}`);
  }
  state.config = config;
  state.sd1EnabledOverride = config.analogue?.sd1?.enabled ?? null;
  state.dirty = false;
  fillConfigForm();
  renderStatus();
  setSaveMessage("Saved, applying");

  const applied = await fetchAppliedConfig(config);
  if (applied) {
    state.config = applied;
    state.sd1EnabledOverride = null;
    fillConfigForm();
    setSaveMessage("Saved");
  } else {
    setSaveMessage("Saved");
  }
  await loadStatus();
  setMessage("Connected");
}

async function playFallbackVideo() {
  const path = el.fallbackVideos.value.split(",").map((value) => value.trim()).find(Boolean) ?? "";
  const response = await fetch("/api/fallback/play", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path }),
  });
  if (!response.ok) {
    const body = await response.json().catch(() => ({ error: `HTTP ${response.status}` }));
    throw new Error(body.error || `HTTP ${response.status}`);
  }
  setSaveMessage("Fallback video triggered");
  await loadStatus();
}

async function stopFallbackVideo() {
  const response = await fetch("/api/fallback/stop", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
  });
  if (!response.ok) {
    const body = await response.json().catch(() => ({ error: `HTTP ${response.status}` }));
    throw new Error(body.error || `HTTP ${response.status}`);
  }
  setSaveMessage("Fallback video stopped");
  await loadStatus();
}

async function requestServiceAction(action) {
  const response = await fetch(`/api/service/${action}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
  });
  if (!response.ok) {
    const body = await response.json().catch(() => ({ error: `HTTP ${response.status}` }));
    throw new Error(body.error || `HTTP ${response.status}`);
  }
  setSaveMessage(`Service ${action} requested`);
  if (action === "restart" || action === "stop") {
    setMessage(action === "restart" ? "Restarting" : "Stopping");
    return;
  }
  await loadStatus();
}

el.refresh.addEventListener("click", refreshAll);
el.save.addEventListener("click", () => {
  saveConfig().catch((error) => setSaveMessage(`Save failed: ${error.message}`, false));
});
if (el.fallbackPlayVideo) {
  el.fallbackPlayVideo.addEventListener("click", () => {
    playFallbackVideo().catch((error) => setSaveMessage(`Fallback video failed: ${error.message}`, false));
  });
}
if (el.fallbackStopVideo) {
  el.fallbackStopVideo.addEventListener("click", () => {
    stopFallbackVideo().catch((error) => setSaveMessage(`Stop fallback video failed: ${error.message}`, false));
  });
}
if (el.serviceRestart) {
  el.serviceRestart.addEventListener("click", () => {
    if (!window.confirm("Restart wh-repeater service now?")) {
      return;
    }
    requestServiceAction("restart").catch((error) => setSaveMessage(`Restart failed: ${error.message}`, false));
  });
}
if (el.addReceiver) {
  el.addReceiver.addEventListener("click", () => {
    const nextId = el.receivers.querySelectorAll(".receiver").length + 1;
    el.receivers.appendChild(receiverNode({ id: nextId, enabled: true, dwellMs: 1500, hangMs: 5000, targets: [] }));
    markDirty();
  });
}

for (const input of [
  el.statusInterval,
  el.minimumMer,
  el.minimumDNumber,
  el.plutoAddress,
  el.plutoPort,
  el.plutoMqttEnabled,
  el.plutoMqttHost,
  el.plutoMqttPort,
  el.plutoMqttProtocol,
  el.plutoMqttDeviceId,
  el.plutoCallsign,
  el.plutoSystem,
  el.plutoTxFrequency,
  el.plutoSymbolRate,
  el.plutoGain,
  el.plutoNco,
  el.plutoPilots,
  el.plutoFrame,
  el.plutoFecMode,
  el.plutoConstellation,
  el.audioBitrate,
  el.outputWidth,
  el.outputHeight,
  el.outputFrameRate,
  el.plutoFec,
  el.hardwarePttEnabled,
  el.hardwarePttChip,
  el.hardwarePttLine,
  el.hardwarePttActiveHigh,
  el.watermarkText,
  el.fallbackEnabled,
  el.fallbackHardwareDecode,
  el.fallbackTimeout,
  el.fallbackStill,
  el.fallbackSlideDirectory,
  el.fallbackXmasSlideDirectory,
  el.fallbackSlideDuration,
  el.fallbackVideos,
  el.rtmpEnabled,
  el.rtmpUrl,
  el.beaconScheduleEnabled,
  el.beaconStartTime,
  el.beaconEndTime,
  el.analogueCaptureEnabled,
  el.analogueCaptureReceiverId,
  el.analogueCaptureDeviceId,
  el.analogueCaptureLabel,
  el.analogueCaptureDevice,
  el.analogueCaptureWidth,
  el.analogueCaptureHeight,
  el.analogueCaptureFrameRate,
  el.analogueCaptureLockMode,
  el.analogueCaptureGpioChip,
  el.analogueCaptureGpioLine,
  el.analogueCaptureGpioActiveHigh,
  el.sd1Enabled,
  el.sd1ReceiverId,
  el.sd1DeviceId,
  el.sd1I2cDevice,
  el.sd1I2cAddress,
  el.sd1Source,
  el.sd1CaptureDevice,
  el.sd1CaptureWidth,
  el.sd1CaptureHeight,
  el.sd1CaptureFrameRate,
  el.identEnabled,
  el.serviceName,
  el.identInterval,
  el.identMorseTone,
  el.identMorseWpm,
]) {
  input.addEventListener("input", () => {
    if (input === el.muxRate) {
      updateCalculatedMediaRates();
    }
    if (input === el.plutoSymbolRate || input === el.audioBitrate) {
      updateCalculatedMediaRates();
    }
    markDirty();
  });
  input.addEventListener("change", () => {
    if (input === el.plutoSystem) {
      updatePlutoSystemFields();
    }
    if (input === el.plutoFec || input === el.plutoConstellation) {
      updateCalculatedMediaRates();
    }
    markDirty();
  });
}

refreshAll();
setInterval(loadStatus, 2000);
