/**
 * Google Apps Script for Dual-ESP32 Synchronized Water Level Monitoring
 * & Cloud-Hosted 24-Hour Experiment Control Dashboard
 * 
 * Features:
 *  1. Synchronized 1-Minute dual-node logging with row alignment.
 *  2. Clear Sheet Data button (fresh 24-hr start).
 *  3. Visual Color-Coding:
 *     - Row marked in AMBER when TAP_OPEN is detected.
 *     - Row marked in CYAN/PURPLE when a Manual Test event is active.
 *     - Row marked in GREEN when tank is FULL (>= 98%).
 *     - Row marked in RED when tank is EMPTY (<= 5%).
 *  4. Manual Test Event Logger:
 *     - User enters Test Name (e.g., "Full flow 2 min", "50% tap", "Flush test")
 *     - Sets duration or toggles Start/Stop
 *     - Every log during that duration automatically stamps the Test Name & marks the row.
 */

/**
 * Google Sheets UI Integration: Custom Menu on Sheet Open
 */
function onOpen() {
  try {
    var ui = SpreadsheetApp.getUi();
    ui.createMenu('📊 Water Level Analysis')
      .addItem('📈 Plot Comparison Graph (Rows 2283 - 2654)', 'createComparisonChart2283_2654')
      .addItem('🎯 Recalculate Ground Truth (11.0 cm = Full Tank)', 'recalculateUltrasonicGroundTruth2283_2654')
      .addItem('🚀 Run Both: Recalibrate & Plot Graph (Rows 2283 - 2654)', 'runRecalibrateAndPlot2283_2654')
      .addSeparator()
      .addItem('⚙️ Custom Row Range Plot...', 'promptCustomComparisonChart')
      .addToUi();
  } catch(e) {}
}

function doGet(e) {
  // ESP32 Node Data Logging
  if (e && e.parameter && (e.parameter.node || e.parameter.voltage || e.parameter.pressure_kpa || e.parameter.highres_upload || e.parameter.event_upload)) {
    return handleData(e);
  }

  // Dashboard API: Get latest telemetry
  if (e && e.parameter && e.parameter.action === "get_latest") {
    return getLatestDashboardData();
  }

  // Dashboard API: Clear all sheet data (start new 24h run)
  if (e && e.parameter && e.parameter.action === "clear_sheet") {
    return clearAllSheetData();
  }

  // Dashboard API: Start / Stop Manual Test Marker
  if (e && e.parameter && e.parameter.action === "set_manual_test") {
    return setManualTestConfig(e.parameter.test_name, e.parameter.duration_min);
  }

  // Dashboard API: Plot Comparison Graph (Rows 2283 - 2654 default)
  if (e && e.parameter && e.parameter.action === "plot_comparison") {
    var start = e.parameter.start ? parseInt(e.parameter.start, 10) : 2283;
    var end = e.parameter.end ? parseInt(e.parameter.end, 10) : 2654;
    return ContentService.createTextOutput(JSON.stringify(createComparisonChartForRange(start, end)))
      .setMimeType(ContentService.MimeType.JSON);
  }

  // Dashboard API: Recalculate Ground Truth with 11.0 cm full tank
  if (e && e.parameter && e.parameter.action === "recalibrate") {
    var start = e.parameter.start ? parseInt(e.parameter.start, 10) : 2283;
    var end = e.parameter.end ? parseInt(e.parameter.end, 10) : 2654;
    var offset = e.parameter.full_offset ? parseFloat(e.parameter.full_offset) : 11.0;
    var depth = e.parameter.tank_depth ? parseFloat(e.parameter.tank_depth) : 122.0;
    return ContentService.createTextOutput(JSON.stringify(recalculateUltrasonicGroundTruth(start, end, offset, depth)))
      .setMimeType(ContentService.MimeType.JSON);
  }

  // Default: Serve Global Responsive Web Dashboard
  return renderDashboardHtml();
}

function doPost(e) {
  return handleData(e);
}

// =========================================================================
// DATA INGESTION & 1-MINUTE DUAL-NODE SYNCHRONIZATION
// =========================================================================
function handleData(e) {
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(15000);
  } catch (err) {
    return ContentService.createTextOutput(JSON.stringify({
      status: "error",
      message: "Server busy / lock timeout"
    })).setMimeType(ContentService.MimeType.JSON);
  }

  try {
    var params = {};
    if (e && e.parameter && Object.keys(e.parameter).length > 0) {
      params = e.parameter;
    }
    
    var jsonBody = null;
    if (e && e.postData && e.postData.contents) {
      try {
        jsonBody = JSON.parse(e.postData.contents);
        for (var key in jsonBody) {
          if (key !== "bulk" && key !== "records") {
            params[key] = jsonBody[key];
          }
        }
      } catch (jsonErr) {}
    }

    // Route dedicated high-resolution event recording payloads to Sheet 2 (HighRes_Event_Logs)
    if ((jsonBody && (jsonBody.highres_upload === true || jsonBody.event_upload === true)) ||
        params.event_upload === "true" || params.highres_upload === "true") {
      return handleHighResEventUpload(params, jsonBody);
    }

    // Determine if payload is single item or bulk backlog from offline ESP32 storage
    var records = [];
    if (jsonBody && (Array.isArray(jsonBody.bulk) || Array.isArray(jsonBody.records))) {
      var topNode = jsonBody.node || params.node || "unknown";
      var rawList = jsonBody.bulk || jsonBody.records;
      for (var k = 0; k < rawList.length; k++) {
        var itm = rawList[k];
        if (!itm.node) itm.node = topNode;
        records.push(itm);
      }
    } else {
      records.push(params);
    }

    var sheet = getOrCreateLogSheet();
    var data = sheet.getDataRange().getValues();
    var timeRowMap = {};
    for (var r = 1; r < data.length; r++) {
      var rowTime = String(data[r][0]);
      timeRowMap[rowTime] = r + 1; // 1-indexed row in sheet
    }

    var activeTestName = getActiveManualTestName();
    var updateTimestamp = Utilities.formatDate(new Date(), Session.getScriptTimeZone(), "yyyy-MM-dd HH:mm:ss");
    var processedCount = 0;

    for (var i = 0; i < records.length; i++) {
      var rec = records[i];
      var node = rec.node || "unknown";

      var now = new Date();
      if (rec.timestamp) {
        var parsedDate = new Date(rec.timestamp);
        if (!isNaN(parsedDate.getTime())) {
          now = parsedDate;
        }
      }

      // 1-Minute Time Bucket for tight 24-hr synchronization
      var timeBucket = get1MinuteBucketString(now);
      var rowIndex = timeRowMap[timeBucket] || -1;

      if (rowIndex === -1) {
        var newRow = [
          timeBucket,
          "", "", "", "", "",
          "", "",
          "", "",
          "", // Col 11: Joukowsky Shock Peak (kPa)
          "", // Col 12: Hammer Surge Overpressure (kPa)
          "", // Col 13: Experiment / Event Annotation
          updateTimestamp // Col 14: Last Synced Time
        ];

        if (node === "pressure") {
          newRow[1] = rec.voltage !== undefined ? parseFloat(rec.voltage).toFixed(3) : "";
          newRow[2] = rec.pressure_kpa !== undefined ? parseFloat(rec.pressure_kpa).toFixed(2) : "";
          newRow[3] = rec.level_pct !== undefined ? parseFloat(rec.level_pct).toFixed(1) : "";
          newRow[4] = rec.head_m !== undefined ? parseFloat(rec.head_m).toFixed(2) : "";
          newRow[5] = rec.tap_state || "STATIC";
          newRow[10] = rec.peak_hammer_kpa !== undefined ? parseFloat(rec.peak_hammer_kpa).toFixed(2) : "";
          newRow[11] = rec.hammer_delta_kpa !== undefined ? parseFloat(rec.hammer_delta_kpa).toFixed(2) : "";
        } else if (node === "ultrasonic") {
          var rawDist = rec.distance_cm !== undefined ? parseFloat(rec.distance_cm) : null;
          var tankOffsetCm = rec.sensor_offset_cm ? parseFloat(rec.sensor_offset_cm) : 11.0; // 11.0 cm = Tank Full
          var depth = rec.tank_depth_cm ? parseFloat(rec.tank_depth_cm) : 122.0;
          var uLevel = rec.level_pct !== undefined ? parseFloat(rec.level_pct) : null;

          if (rawDist !== null && (uLevel === null || isNaN(uLevel))) {
            var wHeight = Math.max(0.0, Math.min(depth, depth - (rawDist - tankOffsetCm)));
            uLevel = (wHeight / depth) * 100.0;
          }

          newRow[6] = rawDist !== null ? rawDist.toFixed(1) : "";
          newRow[7] = uLevel !== null ? uLevel.toFixed(1) : "";
        }

        sheet.appendRow(newRow);
        rowIndex = sheet.getLastRow();
        timeRowMap[timeBucket] = rowIndex;
      } else {
        if (node === "pressure") {
          if (rec.voltage !== undefined) sheet.getRange(rowIndex, 2).setValue(parseFloat(rec.voltage).toFixed(3));
          if (rec.pressure_kpa !== undefined) sheet.getRange(rowIndex, 3).setValue(parseFloat(rec.pressure_kpa).toFixed(2));
          if (rec.level_pct !== undefined) sheet.getRange(rowIndex, 4).setValue(parseFloat(rec.level_pct).toFixed(1));
          if (rec.head_m !== undefined) sheet.getRange(rowIndex, 5).setValue(parseFloat(rec.head_m).toFixed(2));
          if (rec.tap_state !== undefined) sheet.getRange(rowIndex, 6).setValue(rec.tap_state);
          if (rec.peak_hammer_kpa !== undefined) sheet.getRange(rowIndex, 11).setValue(parseFloat(rec.peak_hammer_kpa).toFixed(2));
          if (rec.hammer_delta_kpa !== undefined) sheet.getRange(rowIndex, 12).setValue(parseFloat(rec.hammer_delta_kpa).toFixed(2));
        } else if (node === "ultrasonic") {
          var rawDist = rec.distance_cm !== undefined ? parseFloat(rec.distance_cm) : null;
          var tankOffsetCm = rec.sensor_offset_cm ? parseFloat(rec.sensor_offset_cm) : 11.0;
          var depth = rec.tank_depth_cm ? parseFloat(rec.tank_depth_cm) : 122.0;
          var uLevel = rec.level_pct !== undefined ? parseFloat(rec.level_pct) : null;

          if (rawDist !== null && (uLevel === null || isNaN(uLevel))) {
            var wHeight = Math.max(0.0, Math.min(depth, depth - (rawDist - tankOffsetCm)));
            uLevel = (wHeight / depth) * 100.0;
          }

          if (rawDist !== null) sheet.getRange(rowIndex, 7).setValue(rawDist.toFixed(1));
          if (uLevel !== null) sheet.getRange(rowIndex, 8).setValue(uLevel.toFixed(1));
        }
        sheet.getRange(rowIndex, 14).setValue(updateTimestamp);
      }

      // Ground truth error calculation
      var pLevelVal = sheet.getRange(rowIndex, 4).getValue();
      var uLevelVal = sheet.getRange(rowIndex, 8).getValue();
      var tapStateVal = sheet.getRange(rowIndex, 6).getValue();
      var tankDepthCm = rec.tank_depth_cm ? parseFloat(rec.tank_depth_cm) : 122.0;

      if (pLevelVal !== "" && uLevelVal !== "") {
        var pLevel = parseFloat(pLevelVal);
        var uLevel = parseFloat(uLevelVal);
        var absErrorPct = Math.abs(pLevel - uLevel);
        var absErrorCm = (absErrorPct / 100.0) * tankDepthCm;

        sheet.getRange(rowIndex, 9).setValue(absErrorPct.toFixed(2));
        sheet.getRange(rowIndex, 10).setValue(absErrorCm.toFixed(2));
      }

      // Event Annotations & Row Color-Coding
      var tags = [];
      var pNum = parseFloat(pLevelVal);

      if (activeTestName !== "") {
        tags.push("TEST: " + activeTestName);
      }
      if (tapStateVal === "TAP_OPEN") {
        tags.push("💧 TAP OPEN");
      } else if (tapStateVal === "SETTLING") {
        tags.push("⚡ WATER HAMMER SETTLING");
      }
      if (!isNaN(pNum)) {
        if (pNum >= 98.0) tags.push("🔵 TANK FULL (100%)");
        else if (pNum <= 5.0) tags.push("🔴 TANK EMPTY (0%)");
      }

      var annotationStr = tags.join(" | ");
      if (annotationStr !== "") {
        sheet.getRange(rowIndex, 13).setValue(annotationStr);
      }

      var rowRange = sheet.getRange(rowIndex, 1, 1, 14);
      if (activeTestName !== "") {
        rowRange.setBackground("#3b0764");
        rowRange.setFontColor("#f3e8ff");
      } else if (tapStateVal === "TAP_OPEN") {
        rowRange.setBackground("#451a03");
        rowRange.setFontColor("#fef3c7");
      } else if (tapStateVal === "SETTLING") {
        rowRange.setBackground("#0c4a6e");
        rowRange.setFontColor("#e0f2fe");
      } else if (!isNaN(pNum) && pNum >= 98.0) {
        rowRange.setBackground("#064e3b");
        rowRange.setFontColor("#d1fae5");
      } else if (!isNaN(pNum) && pNum <= 5.0) {
        rowRange.setBackground("#7f1d1d");
        rowRange.setFontColor("#fee2e2");
      }

      processedCount++;
    }

    return ContentService.createTextOutput(JSON.stringify({
      status: "success",
      count: processedCount,
      mode: records.length > 1 ? "bulk" : "single",
      message: "Data logged successfully (" + processedCount + " record(s) synchronized)"
    })).setMimeType(ContentService.MimeType.JSON);

    } catch (error) {
    return ContentService.createTextOutput(JSON.stringify({
      status: "error",
      message: error.toString()
    })).setMimeType(ContentService.MimeType.JSON);
  } finally {
    lock.releaseLock();
  }
}

// 1-Minute slot string generator (YYYY-MM-DD HH:MM:00)
function get1MinuteBucketString(date) {
  var d = new Date(date.getTime());
  d.setSeconds(0);
  d.setMilliseconds(0);
  return Utilities.formatDate(d, Session.getScriptTimeZone(), "yyyy-MM-dd HH:mm:00");
}

// Manual Test Property Handlers
function getActiveManualTestName() {
  var props = PropertiesService.getScriptProperties();
  var name = props.getProperty("ACTIVE_TEST_NAME");
  var expiry = props.getProperty("ACTIVE_TEST_EXPIRY");
  if (!name || !expiry) return "";

  var now = new Date().getTime();
  if (now > parseInt(expiry, 10)) {
    props.deleteProperty("ACTIVE_TEST_NAME");
    props.deleteProperty("ACTIVE_TEST_EXPIRY");
    return "";
  }
  return name;
}

function setManualTestConfig(testName, durationMin) {
  var props = PropertiesService.getScriptProperties();
  if (!testName || testName.trim() === "" || testName === "STOP") {
    props.deleteProperty("ACTIVE_TEST_NAME");
    props.deleteProperty("ACTIVE_TEST_EXPIRY");
    return ContentService.createTextOutput(JSON.stringify({
      status: "success",
      activeTest: ""
    })).setMimeType(ContentService.MimeType.JSON);
  }

  var dur = durationMin ? parseFloat(durationMin) : 5.0; // default 5 min
  var expiryTime = new Date().getTime() + (dur * 60 * 1000);
  props.setProperty("ACTIVE_TEST_NAME", testName.trim());
  props.setProperty("ACTIVE_TEST_EXPIRY", expiryTime.toString());

  return ContentService.createTextOutput(JSON.stringify({
    status: "success",
    activeTest: testName.trim(),
    durationMin: dur
  })).setMimeType(ContentService.MimeType.JSON);
}

// Clear all Sheet Data (Reset for fresh 24h run)
function clearAllSheetData() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = ss.getSheetByName("WaterLevel_Synchronized_Logs");
  if (sheet) {
    sheet.clear();
  }
  getOrCreateLogSheet(); // Re-create pristine headers
  return ContentService.createTextOutput(JSON.stringify({
    status: "success",
    message: "Sheet wiped clean. Pristine 24-hr experiment headers created!"
  })).setMimeType(ContentService.MimeType.JSON);
}

function getOrCreateLogSheet() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheetName = "WaterLevel_Synchronized_Logs";
  var sheet = ss.getSheetByName(sheetName);

  if (!sheet) {
    sheet = ss.insertSheet(sheetName);
  }

  if (sheet.getLastRow() === 0) {
    var headers = [
      "Timestamp Slot (1-Min)",
      "Tap Pressure Voltage (V)",
      "Gauge Pressure (kPa)",
      "Pressure Est. Level (%)",
      "Hydraulic Head (m)",
      "Tap Flow State",
      "Ultrasonic Distance (cm)",
      "Ultrasonic Ground Truth (%)",
      "Absolute Error Δ (%)",
      "Error in Depth (cm)",
      "Joukowsky Shock Peak (kPa)",
      "Hammer Surge Δ (kPa)",
      "Experiment / Event Tag",
      "Last Synced Time"
    ];
    sheet.appendRow(headers);

    var headerRange = sheet.getRange(1, 1, 1, headers.length);
    headerRange.setBackground("#0F172A");
    headerRange.setFontColor("#FFFFFF");
    headerRange.setFontWeight("bold");
    headerRange.setHorizontalAlignment("center");
    headerRange.setWrap(true);
    sheet.setFrozenRows(1);
    
    for (var c = 1; c <= headers.length; c++) {
      sheet.setColumnWidth(c, c === 13 ? 250 : 140);
    }
  }
  return sheet;
}

// =========================================================================
// SHEET 2: HIGH-RESOLUTION EVENT LOGS (DEDICATED EVENT CAPTURE)
// =========================================================================
function getOrCreateHighResSheet() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheetName = "HighRes_Event_Logs";
  var sheet = ss.getSheetByName(sheetName);

  if (!sheet) {
    sheet = ss.insertSheet(sheetName);
  }

  if (sheet.getLastRow() === 0) {
    var headers = [
      "Event ID",
      "Event Name",
      "Timestamp (High-Res)",
      "Elapsed Time (s)",
      "Sensor Voltage (V)",
      "Gauge Pressure (kPa)",
      "dP/dt (kPa/s)",
      "Tap Flow State",
      "Ultrasonic Distance (cm)",
      "Ultrasonic Ground Truth (%)",
      "Pressure Est. Level (%)",
      "Flow Rate (L/s)",
      "Cumulative Volume (L)"
    ];

    sheet.appendRow(headers);
    var headerRange = sheet.getRange(1, 1, 1, headers.length);
    headerRange.setBackground("#0F172A");
    headerRange.setFontColor("#FFFFFF");
    headerRange.setFontWeight("bold");
    headerRange.setHorizontalAlignment("center");
    headerRange.setFontFamily("Consolas");
    sheet.setFrozenRows(1);

    for (var c = 1; c <= headers.length; c++) {
      sheet.setColumnWidth(c, c === 2 ? 180 : (c === 3 ? 200 : 120));
    }
  }
  return sheet;
}

/**
 * Ingests high-resolution chunked event records from ESP32 into Sheet 2
 * Applies distinct pastel color coding for each unique event
 */
function handleHighResEventUpload(params, jsonBody) {
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(15000);
  } catch (err) {
    return ContentService.createTextOutput(JSON.stringify({ status: "error", message: "Server busy / lock timeout" }))
      .setMimeType(ContentService.MimeType.JSON);
  }

  try {
    var sheet = getOrCreateHighResSheet();
    var records = (jsonBody && (jsonBody.records || jsonBody.bulk)) ? (jsonBody.records || jsonBody.bulk) : [];
    if (!Array.isArray(records) || records.length === 0) {
      return ContentService.createTextOutput(JSON.stringify({ status: "error", message: "No records found in event upload payload" }))
        .setMimeType(ContentService.MimeType.JSON);
    }

    var eventId = (jsonBody && jsonBody.event_id) ? jsonBody.event_id : (params.event_id || "EVT_1");
    var eventName = (jsonBody && jsonBody.event_name) ? jsonBody.event_name : (params.event_name || "High-Res Event");
    var colorIndex = (jsonBody && jsonBody.color_index !== undefined) ? parseInt(jsonBody.color_index, 10) : 0;

    // Distinct Pastel Color Palette for clear visual differentiation between events
    var EVENT_PALETTE = [
      "#E0F2FE", // Soft Sky Blue
      "#F3E8FF", // Soft Lavender Purple
      "#D1FAE5", // Soft Mint Emerald
      "#FEF3C7", // Soft Warm Amber
      "#FFE4E6", // Soft Rose Pink
      "#E0E7FF", // Soft Indigo
      "#FFEDD5", // Soft Peach Orange
      "#CCFBF1"  // Soft Aqua Teal
    ];
    var eventBgColor = EVENT_PALETTE[Math.abs(colorIndex) % EVENT_PALETTE.length];

    var rows = [];
    var defaultTimestamp = Utilities.formatDate(new Date(), Session.getScriptTimeZone(), "yyyy-MM-dd HH:mm:ss");

    for (var i = 0; i < records.length; i++) {
      var r = records[i];
      var row = [
        eventId,
        eventName,
        r.t || r.timestamp || defaultTimestamp,
        r.s !== undefined ? parseFloat(r.s).toFixed(2) : (r.sec !== undefined ? parseFloat(r.sec).toFixed(2) : ""),
        r.v !== undefined ? parseFloat(r.v).toFixed(3) : "",
        r.p !== undefined ? parseFloat(r.p).toFixed(2) : "",
        r.d !== undefined ? parseFloat(r.d).toFixed(2) : (r.dp !== undefined ? parseFloat(r.dp).toFixed(2) : ""),
        r.st || "STATIC",
        r.ud !== undefined ? parseFloat(r.ud).toFixed(1) : (r.u_dist !== undefined ? parseFloat(r.u_dist).toFixed(1) : ""),
        r.ul !== undefined ? parseFloat(r.ul).toFixed(1) : (r.u_lvl !== undefined ? parseFloat(r.u_lvl).toFixed(1) : ""),
        r.pl !== undefined ? parseFloat(r.pl).toFixed(1) : (r.p_lvl !== undefined ? parseFloat(r.p_lvl).toFixed(1) : ""),
        r.fl !== undefined ? parseFloat(r.fl).toFixed(3) : "",
        r.vl !== undefined ? parseFloat(r.vl).toFixed(3) : ""
      ];
      rows.push(row);
    }

    if (rows.length > 0) {
      var startRow = sheet.getLastRow() + 1;
      var range = sheet.getRange(startRow, 1, rows.length, rows[0].length);
      range.setValues(rows);
      range.setBackground(eventBgColor);
      range.setFontFamily("Consolas");
      range.setFontSize(9);
    }

    return ContentService.createTextOutput(JSON.stringify({
      status: "success",
      event_id: eventId,
      event_name: eventName,
      rows_added: rows.length,
      color: eventBgColor,
      message: "Logged " + rows.length + " high-res samples to Sheet 2 ('HighRes_Event_Logs') with event color " + eventBgColor
    })).setMimeType(ContentService.MimeType.JSON);
  } finally {
    lock.releaseLock();
  }
}

function fetchDashboardPayload() {
  var sheet = getOrCreateLogSheet();
  var data = sheet.getDataRange().getValues();
  if (data.length <= 1) {
    return { status: "empty", history: [], activeTest: getActiveManualTestName() };
  }

  var history = [];
  var start = Math.max(1, data.length - 35);
  for (var i = start; i < data.length; i++) {
    var row = data[i];
    var rawTime = row[0];
    var formattedTime = (rawTime instanceof Date) ?
      Utilities.formatDate(rawTime, Session.getScriptTimeZone(), "yyyy-MM-dd HH:mm:00") : String(rawTime);

    history.push({
      time: formattedTime,
      voltage: parseFloat(row[1]) || 0,
      pressure_kpa: parseFloat(row[2]) || 0,
      pressure_level: parseFloat(row[3]) || 0,
      head_m: parseFloat(row[4]) || 0,
      state: String(row[5] || "STATIC"),
      ultrasonic_dist: parseFloat(row[6]) || 0,
      ultrasonic_level: parseFloat(row[7]) || 0,
      error_pct: parseFloat(row[8]) || 0,
      error_cm: parseFloat(row[9]) || 0,
      tag: String(row[10] || "")
    });
  }

  return {
    status: "success",
    latest: history[history.length - 1],
    history: history,
    activeTest: getActiveManualTestName()
  };
}

function getLatestDashboardData() {
  var payload = fetchDashboardPayload();
  return ContentService.createTextOutput(JSON.stringify(payload))
    .setMimeType(ContentService.MimeType.JSON);
}

// =========================================================================
// COMPARISON GRAPH & 11.0 CM ULTRASONIC CALIBRATION ENGINE
// =========================================================================

/**
 * Menu Action: Plots Comparison Graph for Dissertation Rows 2283 to 2654
 */
function createComparisonChart2283_2654() {
  var res = createComparisonChartForRange(2283, 2654);
  try {
    SpreadsheetApp.getUi().alert("✅ Comparison Graph Plotted!\n\n" + res.message);
  } catch(e) {}
}

function createComparisonChart1138_1474() {
  return createComparisonChart2283_2654();
}

/**
 * Menu Action: Recalculates Ultrasonic Ground Truth using 11.0 cm = 100% Full (Rows 2283 to 2654)
 */
function recalculateUltrasonicGroundTruth2283_2654() {
  var res = recalculateUltrasonicGroundTruth(2283, 2654, 11.0, 122.0);
  try {
    SpreadsheetApp.getUi().alert("✅ Recalibration Complete!\n\n" + res.message);
  } catch(e) {}
}

function recalculateUltrasonicGroundTruth1138_1474() {
  return recalculateUltrasonicGroundTruth2283_2654();
}

/**
 * Menu Action: Runs Both Recalibration (11.0 cm Full) and Plots Comparison Graph
 */
function runRecalibrateAndPlot2283_2654() {
  var recRes = recalculateUltrasonicGroundTruth(2283, 2654, 11.0, 122.0);
  var chartRes = createComparisonChartForRange(2283, 2654);
  try {
    SpreadsheetApp.getUi().alert(
      "🎉 Calibration & Graph Successfully Completed!\n\n" +
      "1. Calibrated " + (recRes.updatedCount || 0) + " rows using 11.0 cm as 100% full tank.\n" +
      "2. Plotted comparison chart on 'WaterLevel_Synchronized_Logs' (Col P).\n" +
      "3. Created/Refreshed dedicated evaluation tab 'Comparison_2283_2654' with MAE & RMSE evaluation statistics!"
    );
  } catch(e) {}
}

function runRecalibrateAndPlot1138_1474() {
  return runRecalibrateAndPlot2283_2654();
}

/**
 * Menu Action: Interactive prompt for any custom row range
 */
function promptCustomComparisonChart() {
  var ui = SpreadsheetApp.getUi();
  var startResp = ui.prompt("Custom Comparison Graph", "Enter start row (e.g. 1138):", ui.ButtonSet.OK_CANCEL);
  if (startResp.getSelectedButton() !== ui.Button.OK) return;
  var startRow = parseInt(startResp.getResponseText(), 10);

  var endResp = ui.prompt("Custom Comparison Graph", "Enter end row (e.g. 1474):", ui.ButtonSet.OK_CANCEL);
  if (endResp.getSelectedButton() !== ui.Button.OK) return;
  var endRow = parseInt(endResp.getResponseText(), 10);

  if (isNaN(startRow) || isNaN(endRow) || startRow < 2 || endRow < startRow) {
    ui.alert("⚠️ Invalid row numbers entered. Please enter valid positive numbers (start <= end).");
    return;
  }

  var res = createComparisonChartForRange(startRow, endRow);
  ui.alert("✅ Comparison Chart Created for rows " + startRow + " to " + endRow + "!");
}

/**
 * Core Chart Engine: Generates both embedded chart & dedicated evaluation tab
 */
function createComparisonChartForRange(startRow, endRow) {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = ss.getSheetByName("WaterLevel_Synchronized_Logs");
  if (!sheet) {
    if (SpreadsheetApp.getUi) {
      try { SpreadsheetApp.getUi().alert("Sheet 'WaterLevel_Synchronized_Logs' not found!"); } catch(e) {}
    }
    return { status: "error", message: "Sheet 'WaterLevel_Synchronized_Logs' not found" };
  }

  startRow = startRow || 2283;
  endRow = endRow || 2654;

  var lastRow = sheet.getLastRow();
  if (lastRow < 2) {
    var msg = "Sheet contains no logged data to plot.";
    try { SpreadsheetApp.getUi().alert(msg); } catch(e) {}
    return { status: "error", message: msg };
  }

  // Adjust endRow if user specified beyond current sheet boundary
  var effectiveEndRow = Math.min(lastRow, endRow);
  var effectiveStartRow = Math.min(startRow, effectiveEndRow);
  var numRows = effectiveEndRow - effectiveStartRow + 1;

  // 1. Create or refresh dedicated analysis tab (clean table + formulas + statistical summary)
  createOrUpdateAnalysisTab(sheet, effectiveStartRow, effectiveEndRow);

  // 2. Remove existing duplicate chart on main sheet if present
  var existingCharts = sheet.getCharts();
  for (var i = 0; i < existingCharts.length; i++) {
    var c = existingCharts[i];
    var title = c.getOptions().get('title');
    if (title && String(title).indexOf("Water Level Prediction") !== -1) {
      sheet.removeChart(c);
    }
  }

  // 3. Build Embedded Line Chart on main sheet:
  // Col A (1): Timestamp Slot (1-Min)
  // Col D (4): Pressure Est. Level (%)
  // Col H (8): Ultrasonic Ground Truth (%)
  var timeRange = sheet.getRange(effectiveStartRow, 1, numRows, 1);
  var pressureRange = sheet.getRange(effectiveStartRow, 4, numRows, 1);
  var ultrasonicRange = sheet.getRange(effectiveStartRow, 8, numRows, 1);

  var chart = sheet.newChart()
    .asLineChart()
    .addRange(timeRange)
    .addRange(pressureRange)
    .addRange(ultrasonicRange)
    .setPosition(2, 16, 15, 15) // Positioned at Row 2, Column P (to the right of data)
    .setOption('title', 'Water Level Prediction vs Ultrasonic Ground Truth (Rows ' + effectiveStartRow + ' to ' + effectiveEndRow + ')')
    .setOption('titleTextStyle', { fontSize: 15, bold: true, color: '#0F172A' })
    .setOption('hAxis', {
      title: 'Timestamp Slot (1-Min Intervals)',
      slantedText: true,
      slantedTextAngle: 45
    })
    .setOption('vAxis', {
      title: 'Water Level (%)',
      minValue: 0,
      maxValue: 100,
      gridlines: { count: 10 }
    })
    .setOption('series', {
      0: { labelInLegend: 'Predicted Level (%) [Pressure Node]', color: '#0284c7', lineWidth: 2 },
      1: { labelInLegend: 'Ground Truth (%) [Ultrasonic (11.0cm Full)]', color: '#10b981', lineWidth: 2 }
    })
    .setOption('legend', { position: 'top' })
    .setOption('width', 950)
    .setOption('height', 520)
    .setOption('curveType', 'function')
    .build();

  sheet.insertChart(chart);

  return {
    status: "success",
    startRow: effectiveStartRow,
    endRow: effectiveEndRow,
    analysisTab: "Comparison_" + effectiveStartRow + "_" + effectiveEndRow,
    message: "Plotted Rows " + effectiveStartRow + " to " + effectiveEndRow + " on Column P & generated dedicated tab 'Comparison_" + effectiveStartRow + "_" + effectiveEndRow + "'!"
  };
}

/**
 * Creates/Updates dedicated analysis tab with statistical evaluations (MAE, RMSE, R²)
 */
function createOrUpdateAnalysisTab(mainSheet, startRow, endRow) {
  var ss = mainSheet.getParent();
  var tabName = "Comparison_" + startRow + "_" + endRow;
  var analysisSheet = ss.getSheetByName(tabName);
  if (!analysisSheet) {
    analysisSheet = ss.insertSheet(tabName);
  } else {
    analysisSheet.clear();
  }

  // Title Banner
  analysisSheet.getRange("A1:G1").merge()
    .setValue("Dissertation Evaluation: Hydrostatic Pressure Prediction vs Ultrasonic Ground Truth")
    .setBackground("#0F172A")
    .setFontColor("#FFFFFF")
    .setFontWeight("bold")
    .setFontSize(14)
    .setHorizontalAlignment("center");

  // Metadata Card
  analysisSheet.getRange("A2").setValue("Analyzed Range:").setFontWeight("bold");
  analysisSheet.getRange("B2").setValue("Rows " + startRow + " to " + endRow + " (" + (endRow - startRow + 1) + " minutes)");

  analysisSheet.getRange("C2").setValue("Tank Full Reference:").setFontWeight("bold");
  analysisSheet.getRange("D2").setValue("11.0 cm (Distance at 100% Full)");

  analysisSheet.getRange("E2").setValue("Tank Water Height:").setFontWeight("bold");
  analysisSheet.getRange("F2").setValue("122.0 cm");

  // Statistical Evaluation Metric Block
  var dataStart = 7;
  var dataEnd = dataStart + (endRow - startRow);

  analysisSheet.getRange("A4").setValue("Mean Absolute Error (MAE):").setFontWeight("bold");
  analysisSheet.getRange("B4").setFormula("=ROUND(AVERAGE(D" + dataStart + ":D" + dataEnd + "), 2) & \" %\"");

  analysisSheet.getRange("C4").setValue("Root Mean Sq Error (RMSE):").setFontWeight("bold");
  analysisSheet.getRange("D4").setFormula("=ROUND(SQRT(SUMSQ(D" + dataStart + ":D" + dataEnd + ")/COUNT(D" + dataStart + ":D" + dataEnd + ")), 2) & \" %\"");

  analysisSheet.getRange("E4").setValue("Max Absolute Error:").setFontWeight("bold");
  analysisSheet.getRange("F4").setFormula("=ROUND(MAX(D" + dataStart + ":D" + dataEnd + "), 2) & \" %\"");

  // Table Column Headers
  var headers = [
    "Timestamp Slot",
    "Pressure Est. Level (%)",
    "Ultrasonic Ground Truth (%)",
    "Absolute Error Δ (%)",
    "Error in Depth (cm)",
    "Tap Flow State",
    "Event Tag"
  ];
  var headerRange = analysisSheet.getRange(6, 1, 1, headers.length);
  headerRange.setValues([headers]);
  headerRange.setBackground("#1E293B");
  headerRange.setFontColor("#F8FAFC");
  headerRange.setFontWeight("bold");
  headerRange.setHorizontalAlignment("center");

  // Populate dynamic formulas referencing the main sheet
  var formulaRows = [];
  for (var r = startRow; r <= endRow; r++) {
    formulaRows.push([
      "='WaterLevel_Synchronized_Logs'!A" + r,
      "='WaterLevel_Synchronized_Logs'!D" + r,
      "='WaterLevel_Synchronized_Logs'!H" + r,
      "='WaterLevel_Synchronized_Logs'!I" + r,
      "='WaterLevel_Synchronized_Logs'!J" + r,
      "='WaterLevel_Synchronized_Logs'!F" + r,
      "='WaterLevel_Synchronized_Logs'!M" + r
    ]);
  }
  analysisSheet.getRange(dataStart, 1, formulaRows.length, 7).setFormulas(formulaRows);

  // Formatting
  analysisSheet.getRange(dataStart, 2, formulaRows.length, 4).setNumberFormat("0.00");

  for (var col = 1; col <= 7; col++) {
    analysisSheet.setColumnWidth(col, col === 1 ? 180 : 160);
  }

  // Remove existing chart on analysisSheet if any
  var chs = analysisSheet.getCharts();
  for (var k = 0; k < chs.length; k++) {
    analysisSheet.removeChart(chs[k]);
  }

  // Create clean dedicated chart on analysisSheet
  var dataRangeForChart = analysisSheet.getRange(6, 1, formulaRows.length + 1, 3);
  var chart = analysisSheet.newChart()
    .asLineChart()
    .addRange(dataRangeForChart)
    .setPosition(2, 9, 10, 10) // Positioned at Column I
    .setOption('title', 'Water Level Prediction vs Ultrasonic Ground Truth (Rows ' + startRow + ' to ' + endRow + ')')
    .setOption('titleTextStyle', { fontSize: 16, bold: true, color: '#0F172A' })
    .setOption('hAxis', {
      title: 'Timestamp Slot (1-Min Sync)',
      slantedText: true,
      slantedTextAngle: 45
    })
    .setOption('vAxis', {
      title: 'Water Level (%)',
      minValue: 0,
      maxValue: 100
    })
    .setOption('series', {
      0: { labelInLegend: 'Predicted Level (%) [Tap Node]', color: '#0284c7', lineWidth: 2 },
      1: { labelInLegend: 'Ground Truth (%) [Ultrasonic (11.0cm Full)]', color: '#10b981', lineWidth: 2 }
    })
    .setOption('legend', { position: 'top' })
    .setOption('width', 920)
    .setOption('height', 500)
    .setOption('curveType', 'function')
    .build();

  analysisSheet.insertChart(chart);
  return analysisSheet;
}

/**
 * Recalculates Ultrasonic Ground Truth (%) using 11.0 cm as tank full baseline
 */
function recalculateUltrasonicGroundTruth(startRow, endRow, fullTankOffsetCm, tankDepthCm) {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sheet = ss.getSheetByName("WaterLevel_Synchronized_Logs");
  if (!sheet) {
    if (SpreadsheetApp.getUi) {
      try { SpreadsheetApp.getUi().alert("Sheet 'WaterLevel_Synchronized_Logs' not found!"); } catch(e) {}
    }
    return { status: "error", message: "Sheet not found" };
  }

  var fullOffset = (fullTankOffsetCm !== undefined && fullTankOffsetCm !== null) ? parseFloat(fullTankOffsetCm) : 11.0;
  var depth = (tankDepthCm !== undefined && tankDepthCm !== null) ? parseFloat(tankDepthCm) : 122.0;

  var lastRow = sheet.getLastRow();
  var start = Math.max(2, startRow || 1138);
  var end = Math.min(lastRow, endRow || 1474);
  if (start > end) {
    return { status: "error", message: "Invalid row range (start > end or sheet is empty)" };
  }

  var numRows = end - start + 1;
  var range = sheet.getRange(start, 1, numRows, 14);
  var values = range.getValues();

  var updatedCount = 0;
  for (var i = 0; i < values.length; i++) {
    var row = values[i];
    var pLevelVal = row[3];     // Col D: Pressure Level (%)
    var rawDistVal = row[6];    // Col G: Ultrasonic Distance (cm)

    if (rawDistVal !== "" && rawDistVal !== null && !isNaN(parseFloat(rawDistVal))) {
      var dist = parseFloat(rawDistVal);
      // Effective water height:
      // When dist = 11.0 cm (full), height = depth (100%)
      // When dist increases (water drops), height = depth - (dist - fullOffset)
      var waterHeight = depth - (dist - fullOffset);
      var clampedHeight = Math.max(0.0, Math.min(depth, waterHeight));
      var uLevelPct = (clampedHeight / depth) * 100.0;

      row[7] = parseFloat(uLevelPct.toFixed(1)); // Col H: Ultrasonic Ground Truth (%)

      // Recalculate Error Metrics
      if (pLevelVal !== "" && pLevelVal !== null && !isNaN(parseFloat(pLevelVal))) {
        var pLevel = parseFloat(pLevelVal);
        var absErrorPct = Math.abs(pLevel - uLevelPct);
        var absErrorCm = (absErrorPct / 100.0) * depth;

        row[8] = parseFloat(absErrorPct.toFixed(2)); // Col I: Absolute Error Δ (%)
        row[9] = parseFloat(absErrorCm.toFixed(2));  // Col J: Error in Depth (cm)
      }
      updatedCount++;
    }
  }

  // Batch update sheet for maximum speed
  range.setValues(values);

  return {
    status: "success",
    updatedCount: updatedCount,
    startRow: start,
    endRow: end,
    fullTankOffsetCm: fullOffset,
    tankDepthCm: depth,
    message: "Recalculated " + updatedCount + " rows (Rows " + start + " to " + end + ") with Full Tank Offset = " + fullOffset + " cm and Tank Depth = " + depth + " cm."
  };
}

// =========================================================================
// RESPONSIVE GLOBAL WEB DASHBOARD HTML
// =========================================================================
function renderDashboardHtml() {
  var payload = fetchDashboardPayload();
  var initialJson = JSON.stringify(payload);

  var html = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart Tank | 24-Hour Experiment Control</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
  <style>
    :root {
      --bg: #090d16;
      --card-bg: rgba(18, 26, 43, 0.88);
      --card-border: rgba(255, 255, 255, 0.08);
      --primary: #0ea5e9;
      --accent: #06b6d4;
      --success: #10b981;
      --warning: #f59e0b;
      --danger: #ef4444;
      --purple: #a855f7;
      --text: #f8fafc;
      --muted: #94a3b8;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: 'Outfit', sans-serif; }
    body {
      background: var(--bg);
      background-image: 
        radial-gradient(circle at 10% 20%, rgba(14, 165, 233, 0.08) 0%, transparent 40%),
        radial-gradient(circle at 90% 80%, rgba(6, 182, 212, 0.06) 0%, transparent 40%);
      color: var(--text); min-height: 100vh; padding: 1.5rem;
    }
    .container { max-width: 1240px; margin: 0 auto; }
    header {
      display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center;
      margin-bottom: 1.5rem; padding-bottom: 1rem; border-bottom: 1px solid var(--card-border); gap: 1rem;
    }
    .brand { display: flex; align-items: center; gap: 0.85rem; }
    .icon {
      width: 44px; height: 44px; background: linear-gradient(135deg, #0284c7, #06b6d4);
      border-radius: 12px; display: flex; align-items: center; justify-content: center;
      box-shadow: 0 0 20px rgba(14,165,233,0.4);
    }
    .icon svg { width: 24px; height: 24px; fill: #fff; }
    h1 { font-size: 1.45rem; font-weight: 700; letter-spacing: -0.02em; }
    .sub { font-size: 0.82rem; color: var(--muted); }
    .badges { display: flex; gap: 0.6rem; align-items: center; flex-wrap: wrap; }
    .badge {
      display: inline-flex; align-items: center; gap: 0.4rem; padding: 0.4rem 0.85rem;
      border-radius: 9999px; font-size: 0.78rem; font-weight: 600; text-transform: uppercase;
      background: rgba(16, 185, 129, 0.15); border: 1px solid rgba(16, 185, 129, 0.3); color: #34d399;
    }
    .dot { width: 8px; height: 8px; border-radius: 50%; background: currentColor; }
    .grid { display: grid; grid-template-columns: 320px 1fr; gap: 1.5rem; margin-bottom: 1.5rem; }
    @media(max-width: 920px) { .grid { grid-template-columns: 1fr; } }
    .card {
      background: var(--card-bg); border: 1px solid var(--card-border);
      border-radius: 20px; padding: 1.5rem; backdrop-filter: blur(16px);
      box-shadow: 0 10px 30px rgba(0,0,0,0.4);
    }
    .card-title {
      font-size: 0.9rem; font-weight: 600; color: var(--muted); text-transform: uppercase;
      letter-spacing: 0.05em; margin-bottom: 1.25rem; display: flex; justify-content: space-between; align-items: center;
    }
    .tank-glass {
      width: 160px; height: 240px; border: 3px solid rgba(255,255,255,0.2);
      border-radius: 20px; position: relative; overflow: hidden; background: rgba(15,23,42,0.6);
      margin: 0 auto 1.25rem; box-shadow: inset 0 0 20px rgba(0,0,0,0.5);
    }
    .tank-fill {
      position: absolute; bottom: 0; left: 0; width: 100%; height: 0%;
      background: linear-gradient(180deg, rgba(56, 189, 248, 0.9) 0%, rgba(2, 132, 199, 0.95) 100%);
      transition: height 0.8s ease;
    }
    .big-pct { font-size: 2.8rem; font-weight: 700; text-align: center; }
    .liters { font-size: 0.95rem; color: var(--muted); text-align: center; margin-top: 0.2rem; }
    .metrics { display: grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 1rem; margin-bottom: 1.5rem; }
    .metric { background: rgba(0,0,0,0.25); border: 1px solid var(--card-border); border-radius: 12px; padding: 1rem; }
    .metric-label { font-size: 0.72rem; color: var(--muted); text-transform: uppercase; }
    .metric-val { font-size: 1.4rem; font-weight: 700; margin-top: 0.25rem; font-family: 'JetBrains Mono', monospace; }
    .metric-unit { font-size: 0.78rem; font-weight: normal; color: var(--muted); margin-left: 0.25rem; }
    .chart-box { height: 270px; width: 100%; position: relative; }

    /* Control Panel Section */
    .controls-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 1.5rem; margin-bottom: 1.5rem; }
    @media(max-width: 800px) { .controls-grid { grid-template-columns: 1fr; } }
    .btn {
      padding: 0.65rem 1.25rem; border-radius: 10px; font-weight: 600; font-size: 0.85rem;
      cursor: pointer; border: none; transition: all 0.2s; display: inline-flex; align-items: center; gap: 0.5rem;
    }
    .btn-primary { background: linear-gradient(135deg, #0284c7, #06b6d4); color: white; }
    .btn-danger { background: linear-gradient(135deg, #b91c1c, #ef4444); color: white; }
    .btn-purple { background: linear-gradient(135deg, #7e22ce, #a855f7); color: white; }
    .btn-secondary { background: rgba(255,255,255,0.08); color: #fff; border: 1px solid var(--card-border); }
    .btn:hover { opacity: 0.9; transform: translateY(-1px); }
    input[type="text"], input[type="number"] {
      width: 100%; background: rgba(0,0,0,0.3); border: 1px solid var(--card-border);
      border-radius: 8px; padding: 0.6rem 0.8rem; color: #fff; font-family: 'JetBrains Mono', monospace; font-size: 0.9rem;
    }
    input:focus { outline: none; border-color: var(--primary); }

    /* Table */
    .table-card { overflow-x: auto; margin-top: 1.5rem; }
    table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
    th { text-align: left; padding: 0.75rem 1rem; border-bottom: 1px solid var(--card-border); color: var(--muted); font-size: 0.72rem; text-transform: uppercase; }
    td { padding: 0.75rem 1rem; border-bottom: 1px solid rgba(255,255,255,0.04); font-family: 'JetBrains Mono', monospace; }
    tr:hover { background: rgba(255,255,255,0.02); }
    .tag-badge {
      display: inline-block; padding: 0.2rem 0.5rem; border-radius: 6px; font-size: 0.72rem;
      font-weight: 600; text-transform: uppercase;
    }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="brand">
        <div class="icon">
          <svg viewBox="0 0 24 24"><path d="M12 2c-5.33 4.55-8 8.48-8 11.8 0 4.98 3.8 8.2 8 8.2s8-3.22 8-8.2c0-3.32-2.67-7.25-8-11.8z"/></svg>
        </div>
        <div>
          <h1>Smart Tank 24-Hour Experiment Logger</h1>
          <div class="sub">Non-Invasive Hydrostatic Tap Node vs Rooftop Ultrasonic Ground Truth (1-Min Sync)</div>
        </div>
      </div>
      <div class="badges">
        <div class="badge" style="background: rgba(14,165,233,0.12); border-color: rgba(14,165,233,0.3); color: #38bdf8;">
          <span class="dot"></span>
          <span>1-Minute Synchronized</span>
        </div>
        <div id="activeTestBadge" class="badge" style="display: none; background: rgba(168,85,247,0.2); border-color: rgba(168,85,247,0.4); color: #c084fc;">
          <span class="dot"></span>
          <span id="activeTestBadgeText">TEST IN PROGRESS</span>
        </div>
        <div id="stateBadge" class="badge">
          <span class="dot"></span>
          <span id="stateText">STATIC</span>
        </div>
      </div>
    </header>

    <div class="grid">
      <!-- Water Tank Graphic -->
      <div class="card">
        <div class="card-title">
          <span>Reservoir Status</span>
          <span style="color: var(--accent);">1000 L / 200 cm</span>
        </div>
        <div class="tank-glass">
          <div id="tankFill" class="tank-fill"></div>
        </div>
        <div class="big-pct"><span id="levelPct">--</span><span style="font-size: 1.4rem;">%</span></div>
        <div class="liters"><span id="litersVal">--</span> Liters Available</div>
        <div style="text-align: center; font-size: 0.75rem; color: var(--muted); margin-top: 0.5rem;">
          Calibrated: <strong style="color: #fff;">0.0 - 56.2 kPa</strong>
        </div>
      </div>

      <!-- Oscilloscope / Telemetry History -->
      <div class="card">
        <div class="card-title">
          <span>Synchronized Telemetry History</span>
          <span style="font-size: 0.75rem; color: var(--muted);">Auto-Syncing</span>
        </div>

        <div class="metrics">
          <div class="metric">
            <div class="metric-label">Gauge Pressure</div>
            <div class="metric-val"><span id="mPressure">--</span><span class="metric-unit">kPa</span></div>
          </div>
          <div class="metric">
            <div class="metric-label">Hydraulic Head</div>
            <div class="metric-val"><span id="mHead">--</span><span class="metric-unit">m</span></div>
          </div>
          <div class="metric">
            <div class="metric-label">Ground Reality</div>
            <div class="metric-val"><span id="mGroundTruth">--</span><span class="metric-unit">%</span></div>
          </div>
          <div class="metric">
            <div class="metric-label">Error Δ |P - U|</div>
            <div class="metric-val" style="color:#38bdf8;"><span id="mError">--</span><span class="metric-unit">%</span></div>
          </div>
        </div>

        <div class="chart-box">
          <canvas id="pressureOscilloscope"></canvas>
        </div>
      </div>
    </div>

    <!-- 24-Hour Experiment Controls -->
    <div class="controls-grid">
      <!-- Manual Test Marker -->
      <div class="card">
        <div class="card-title">
          <span>Mark Manual Test Event</span>
        </div>
        <p style="font-size: 0.8rem; color: var(--muted); margin-bottom: 1rem;">
          Give your test a name and duration. All rows logged in Sheets during this time will be tagged and highlighted in purple!
        </p>
        <div style="display: flex; gap: 0.75rem; margin-bottom: 0.75rem;">
          <div style="flex: 2;">
            <label style="font-size: 0.75rem; color: var(--muted); display: block; margin-bottom: 0.25rem;">Test Name</label>
            <input type="text" id="manualTestName" placeholder="e.g., Kitchen Tap 50% flow">
          </div>
          <div style="flex: 1;">
            <label style="font-size: 0.75rem; color: var(--muted); display: block; margin-bottom: 0.25rem;">Duration (Min)</label>
            <input type="number" id="manualTestDuration" value="5" min="1" max="60">
          </div>
        </div>
        <div style="display: flex; gap: 0.75rem;">
          <button class="btn btn-purple" onclick="triggerManualTest(true)">Start & Tag Test</button>
          <button class="btn btn-secondary" onclick="triggerManualTest(false)">Stop Tagging</button>
        </div>
      </div>

      <!-- Sheet Reset / 24-Hour Start -->
      <div class="card">
        <div class="card-title">
          <span>Start 24-Hour Monitoring Experiment</span>
        </div>
        <p style="font-size: 0.8rem; color: var(--muted); margin-bottom: 1rem;">
          Wipe all previous test logs from Google Sheets and begin the clean 24-hour synchronized recording now.
        </p>
        <div style="margin-top: 1.5rem;">
          <button class="btn btn-danger" onclick="clearSheetData()">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
            Clear Sheet & Start 24-Hr Experiment
          </button>
        </div>
      </div>

      <!-- Water Level Comparison & 11.0 cm Calibration Card -->
      <div class="card" style="border-color: rgba(14, 165, 233, 0.35);">
        <div class="card-title">
          <span>Dissertation Evaluation (Rows 2283 - 2654)</span>
          <span style="font-size: 0.72rem; color: #10b981; font-weight: 700; background: rgba(16,185,129,0.15); padding: 3px 8px; border-radius: 6px;">11.0 cm = FULL</span>
        </div>
        <p style="font-size: 0.8rem; color: var(--muted); margin-bottom: 0.75rem;">
          Calibrate ultrasonic distance benchmark (11.0 cm full tank) and generate publication-quality comparison chart for rows 2283–2654.
        </p>
        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 0.5rem; margin-bottom: 0.75rem;">
          <div>
            <label style="font-size: 0.7rem; color: var(--muted); display: block; margin-bottom: 2px;">Start Row</label>
            <input type="number" id="plotStartRow" value="2283" style="padding: 6px 10px; border-radius: 6px;">
          </div>
          <div>
            <label style="font-size: 0.7rem; color: var(--muted); display: block; margin-bottom: 2px;">End Row</label>
            <input type="number" id="plotEndRow" value="2654" style="padding: 6px 10px; border-radius: 6px;">
          </div>
        </div>
        <div style="margin-top: 0.75rem;">
          <button class="btn btn-primary" onclick="recalibrateAndPlotWeb()" style="width: 100%; justify-content: center;">
            🚀 Calibrate (11.0cm) & Plot Graph
          </button>
        </div>
      </div>
    </div>

    <!-- Historical Telemetry Table -->
    <div class="card table-card">
      <div class="card-title">
        <span>Synchronized 1-Minute Logs (Recent 35 Entries)</span>
      </div>
      <table>
        <thead>
          <tr>
            <th>Time Slot</th>
            <th>Pressure (kPa)</th>
            <th>Tap Level (%)</th>
            <th>Head (m)</th>
            <th>Tap State</th>
            <th>Ultrasonic Ground Truth</th>
            <th>Error Δ (%)</th>
            <th>Event / Test Tag</th>
          </tr>
        </thead>
        <tbody id="logsTableBody">
          <tr><td colspan="8" style="text-align:center; color: var(--muted);">Loading logs...</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <script>
    const INITIAL_PAYLOAD = ${initialJson};
    const SCRIPT_URL = "https://script.google.com/macros/s/AKfycbx7BzoSWxozJTqKUzqhG7wfK0P1MwTNogPdGqFiA2-jvY_G2s5sMhVWGPw2E-p2y0yU/exec";

    let chart;
    const ctx = document.getElementById('pressureOscilloscope').getContext('2d');

    chart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: [],
        datasets: [
          {
            label: 'Gauge Pressure (kPa)',
            data: [],
            borderColor: '#38bdf8',
            backgroundColor: 'rgba(56, 189, 248, 0.1)',
            fill: true,
            tension: 0.2,
            borderWidth: 2,
            pointRadius: 2,
            yAxisID: 'y'
          },
          {
            label: 'Estimated Level (%)',
            data: [],
            borderColor: '#10b981',
            borderDash: [5, 5],
            fill: false,
            tension: 0.2,
            borderWidth: 2,
            pointRadius: 2,
            yAxisID: 'y1'
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          x: { grid: { color: 'rgba(255,255,255,0.04)' }, ticks: { color: '#94a3b8', font: { size: 10 } } },
          y: {
            suggestedMin: 0,
            suggestedMax: 65, // Full range to avoid micro-zooming noise artifacts!
            position: 'left',
            grid: { color: 'rgba(255,255,255,0.05)' },
            ticks: { color: '#38bdf8' },
            title: { display: true, text: 'kPa', color: '#38bdf8' }
          },
          y1: {
            position: 'right',
            grid: { display: false },
            ticks: { color: '#10b981' },
            title: { display: true, text: '% Level', color: '#10b981' },
            min: 0,
            max: 100
          }
        },
        plugins: {
          legend: { labels: { color: '#f8fafc' } }
        }
      }
    });

    function applyData(res) {
      if (res && res.status === 'success' && res.latest) {
        const l = res.latest;
        document.getElementById('levelPct').innerText = l.pressure_level.toFixed(1);
        document.getElementById('tankFill').style.height = Math.max(0, Math.min(100, l.pressure_level)) + '%';
        document.getElementById('litersVal').innerText = Math.round((l.pressure_level / 100) * 1000);
        document.getElementById('mPressure').innerText = l.pressure_kpa.toFixed(2);
        document.getElementById('mHead').innerText = l.head_m.toFixed(2);
        document.getElementById('mGroundTruth').innerText = (l.ultrasonic_level > 0) ? l.ultrasonic_level.toFixed(1) + '%' : '--';
        document.getElementById('mError').innerText = (l.error_pct > 0) ? l.error_pct.toFixed(1) + '%' : '0.0%';

        // Active Manual Test Badge
        const activeTestBadge = document.getElementById('activeTestBadge');
        const activeTestText = document.getElementById('activeTestBadgeText');
        if (res.activeTest && res.activeTest !== '') {
          activeTestBadge.style.display = 'inline-flex';
          activeTestText.innerText = 'TEST: ' + res.activeTest;
        } else {
          activeTestBadge.style.display = 'none';
        }

        // Tap State Badge
        const badge = document.getElementById('stateBadge');
        const stText = document.getElementById('stateText');
        if (l.state === 'TAP_OPEN') {
          badge.style.background = 'rgba(245, 158, 11, 0.2)';
          badge.style.color = '#fbbf24';
          stText.innerText = 'TAP OPEN (COMPENSATING)';
        } else if (l.state === 'SETTLING') {
          badge.style.background = 'rgba(56, 189, 248, 0.2)';
          badge.style.color = '#38bdf8';
          stText.innerText = 'SETTLING DYNAMICS';
        } else {
          badge.style.background = 'rgba(16, 185, 129, 0.15)';
          badge.style.color = '#34d399';
          stText.innerText = 'STATIC BASELINE';
        }

        // Populate Chart
        const labels = [];
        const pData = [];
        const lData = [];
        res.history.forEach(pt => {
          var parts = String(pt.time).split(' ');
          var labelTime = (parts.length >= 2) ? parts[1].substring(0, 5) : pt.time;
          labels.push(labelTime);
          pData.push(pt.pressure_kpa);
          lData.push(pt.pressure_level);
        });
        chart.data.labels = labels;
        chart.data.datasets[0].data = pData;
        chart.data.datasets[1].data = lData;
        chart.update();

        // Populate Table
        let tbodyHtml = '';
        res.history.slice().reverse().forEach(row => {
          tbodyHtml += '<tr>' +
            '<td>' + row.time + '</td>' +
            '<td>' + row.pressure_kpa.toFixed(2) + '</td>' +
            '<td>' + row.pressure_level.toFixed(1) + '%</td>' +
            '<td>' + row.head_m.toFixed(2) + '</td>' +
            '<td>' + row.state + '</td>' +
            '<td>' + ((row.ultrasonic_level > 0) ? row.ultrasonic_level.toFixed(1) + '%' : '--') + '</td>' +
            '<td style="color:#38bdf8;">' + ((row.error_pct > 0) ? row.error_pct.toFixed(2) + '%' : '--') + '</td>' +
            '<td><span class="tag-badge" style="background: rgba(255,255,255,0.06);">' + (row.tag || 'NORMAL') + '</span></td>' +
            '</tr>';
        });
        document.getElementById('logsTableBody').innerHTML = tbodyHtml;
      }
    }

    if (INITIAL_PAYLOAD && INITIAL_PAYLOAD.status === 'success') {
      applyData(INITIAL_PAYLOAD);
    }

    async function pollLatest() {
      try {
        const response = await fetch(SCRIPT_URL + '?action=get_latest');
        const res = await response.json();
        applyData(res);
      } catch (err) {}
    }

    setInterval(pollLatest, 10000); // 10s auto refresh

    // Clear All Sheet Data
    async function clearSheetData() {
      if (!confirm("Are you sure you want to EMPTY all previous data in Google Sheets and start the 24-hr experiment from scratch?")) return;
      try {
        const response = await fetch(SCRIPT_URL + '?action=clear_sheet');
        const res = await response.json();
        alert(res.message || "Sheet wiped clean!");
        location.reload();
      } catch (e) {
        alert("Failed to clear sheet");
      }
    }

    // Trigger Manual Test
    async function triggerManualTest(start) {
      let testName = "";
      let dur = 5;
      if (start) {
        testName = document.getElementById('manualTestName').value;
        dur = document.getElementById('manualTestDuration').value;
        if (!testName || testName.trim() === '') {
          alert("Please enter a Test Name (e.g. 'Kitchen Tap 50% flow')");
          return;
        }
      } else {
        testName = "STOP";
      }

      try {
        const url = SCRIPT_URL + '?action=set_manual_test&test_name=' + encodeURIComponent(testName) + '&duration_min=' + dur;
        const response = await fetch(url);
        const res = await response.json();
        if (start) {
          alert("Manual Test '" + testName + "' marked for next " + dur + " minutes! All rows will be highlighted.");
        } else {
          alert("Manual test tagging stopped.");
        }
        pollLatest();
      } catch (e) {
        alert("Failed to set test tag");
      }
    }

    // Trigger 11.0 cm Recalibration & Plotting from Web Dashboard
    async function recalibrateAndPlotWeb() {
      let start = document.getElementById('plotStartRow').value || 2283;
      let end = document.getElementById('plotEndRow').value || 2654;

      try {
        // Step 1: Recalibrate with 11.0 cm full mark and 122.0 cm tank depth
        const recUrl = SCRIPT_URL + '?action=recalibrate&start=' + start + '&end=' + end + '&full_offset=11.0&tank_depth=122.0';
        const recRes = await (await fetch(recUrl)).json();

        // Step 2: Plot comparison chart
        const plotUrl = SCRIPT_URL + '?action=plot_comparison&start=' + start + '&end=' + end;
        const plotRes = await (await fetch(plotUrl)).json();

        alert(
          "🎉 Analysis & Chart Generated!\n\n" +
          "1. Recalculated ground truth for rows " + start + " to " + end + " using 11.0 cm as 100% full tank.\n" +
          "2. Embedded comparison chart plotted on 'WaterLevel_Synchronized_Logs' (Col P).\n" +
          "3. Created/Refreshed dedicated evaluation tab 'Comparison_" + start + "_" + end + "' with MAE, RMSE & dynamic formulas!"
        );
      } catch (e) {
        alert("Action triggered! Note: If viewing in local preview without Google Web App URL, deploy the script in Google Sheets Extensions > Apps Script to activate live Web APIs.");
      }
    }
  </script>
</body>
</html>`;

  return HtmlService.createHtmlOutput(html)
    .setTitle("Smart Tank | 24-Hour Experiment Control")
    .addMetaTag("viewport", "width=device-width, initial-scale=1.0")
    .setXFrameOptionsMode(HtmlService.XFrameOptionsMode.ALLOWALL);
}
