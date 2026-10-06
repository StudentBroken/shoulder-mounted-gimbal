#pragma once
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>GIMBAL POD // TACTICAL HUD & RC COCKPIT</title>
<style>
  :root {
    --bg: #07090e;
    --card: rgba(16, 20, 32, 0.85);
    --border: rgba(0, 240, 255, 0.28);
    --border-dim: rgba(255, 255, 255, 0.08);
    --cyan: #00f0ff;
    --cyan-glow: rgba(0, 240, 255, 0.4);
    --red: #ff2a55;
    --red-glow: rgba(255, 42, 85, 0.5);
    --emerald: #00ff9d;
    --emerald-glow: rgba(0, 255, 157, 0.4);
    --amber: #ffb700;
    --amber-glow: rgba(255, 183, 0, 0.4);
    --purple: #b026ff;
    --text: #f0f4fc;
    --text-dim: #7f8ca3;
    --radius: 12px;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Outfit', sans-serif; -webkit-tap-highlight-color: transparent; }
  body {
    background-color: var(--bg);
    background-image: 
      radial-gradient(circle at 50% 0%, rgba(0, 240, 255, 0.09), transparent 60%),
      radial-gradient(circle at 100% 100%, rgba(255, 42, 85, 0.06), transparent 50%),
      linear-gradient(rgba(255, 255, 255, 0.015) 1px, transparent 1px),
      linear-gradient(90deg, rgba(255, 255, 255, 0.015) 1px, transparent 1px);
    background-size: 100% 100%, 100% 100%, 30px 30px, 30px 30px;
    color: var(--text);
    padding: 14px;
    min-height: 100vh;
  }
  .container { max-width: 960px; margin: 0 auto; display: flex; flex-direction: column; gap: 16px; }
  header {
    display: flex; justify-content: space-between; align-items: center;
    background: var(--card); backdrop-filter: blur(16px);
    border: 1px solid var(--border); border-radius: var(--radius);
    padding: 12px 18px; box-shadow: 0 8px 32px rgba(0,0,0,0.55);
  }
  .title-group { display: flex; align-items: center; gap: 12px; }
  .logo-icon { width: 34px; height: 34px; border-radius: 8px; background: linear-gradient(135deg, var(--cyan), var(--red)); display: flex; align-items: center; justify-content: center; font-weight: 900; font-size: 17px; color: #000; box-shadow: 0 0 16px var(--cyan-glow); }
  h1 { font-size: 16px; font-weight: 800; letter-spacing: 1.5px; text-transform: uppercase; }
  .subtitle { font-size: 11px; color: var(--text-dim); letter-spacing: 1px; }
  .header-actions { display: flex; align-items: center; gap: 10px; }
  .network-badge { display: flex; align-items: center; gap: 8px; font-size: 11px; font-weight: 700; background: rgba(0,240,255,0.1); border: 1px solid var(--border); border-radius: 20px; padding: 6px 12px; }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--emerald); box-shadow: 0 0 10px var(--emerald); animation: pulse 1.8s infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; transform: scale(1); } 50% { opacity: 0.4; transform: scale(0.85); } }

  .btn-rc-launch {
    background: linear-gradient(135deg, #00f0ff, #0077b6);
    border: 1px solid var(--cyan);
    border-radius: 20px;
    padding: 8px 16px;
    font-size: 12px;
    font-weight: 800;
    color: #000;
    cursor: pointer;
    box-shadow: 0 0 18px var(--cyan-glow);
    display: flex;
    align-items: center;
    gap: 6px;
    transition: transform 0.15s, box-shadow 0.2s;
  }
  .btn-rc-launch:active { transform: scale(0.96); }

  .grid-2 { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
  .card {
    background: var(--card); backdrop-filter: blur(14px);
    border: 1px solid var(--border-dim); border-radius: var(--radius);
    padding: 16px; box-shadow: 0 6px 24px rgba(0,0,0,0.4);
    transition: border-color 0.25s, box-shadow 0.25s;
  }
  .card:hover { border-color: var(--border); }
  .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; padding-bottom: 10px; border-bottom: 1px solid var(--border-dim); }
  .card-title { font-size: 13px; font-weight: 800; letter-spacing: 1.2px; text-transform: uppercase; color: var(--cyan); display: flex; align-items: center; gap: 8px; }
  .status-pill { font-size: 10px; font-weight: 800; text-transform: uppercase; letter-spacing: 1px; padding: 4px 10px; border-radius: 12px; }
  .pill-green { background: rgba(0,255,157,0.12); color: var(--emerald); border: 1px solid rgba(0,255,157,0.3); }
  .pill-red { background: rgba(255,42,85,0.12); color: var(--red); border: 1px solid rgba(255,42,85,0.3); }
  .pill-blue { background: rgba(0,240,255,0.12); color: var(--cyan); border: 1px solid rgba(0,240,255,0.3); }
  .pill-amber { background: rgba(255,183,0,0.12); color: var(--amber); border: 1px solid rgba(255,183,0,0.3); }
  .pill-purple { background: rgba(176,38,255,0.18); color: #d880ff; border: 1px solid rgba(176,38,255,0.5); }

  .battery-container { margin: 10px 0 14px 0; }
  .battery-header { display: flex; justify-content: space-between; font-size: 12px; margin-bottom: 5px; }
  .battery-val { font-weight: 800; font-size: 13px; }
  .battery-bar-bg { width: 100%; height: 10px; background: rgba(255,255,255,0.08); border-radius: 6px; overflow: hidden; border: 1px solid rgba(255,255,255,0.1); }
  .battery-bar-fill { height: 100%; width: 0%; border-radius: 5px; transition: width 0.4s cubic-bezier(0.2, 0.9, 0.4, 1), background-color 0.4s; }

  .telemetry-row { display: flex; justify-content: space-between; align-items: center; margin: 8px 0; font-size: 12px; }
  .telem-label { color: var(--text-dim); }
  .telem-val { font-weight: 700; color: #fff; font-feature-settings: 'tnum'; }

  .angle-display { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; }
  .angle-box { background: rgba(0,0,0,0.3); border: 1px solid var(--border-dim); border-radius: 8px; padding: 8px; text-align: center; }
  .angle-num { font-size: 19px; font-weight: 900; color: var(--cyan); text-shadow: 0 0 12px var(--cyan-glow); }
  .angle-sub { font-size: 9px; color: var(--text-dim); margin-top: 2px; }

  .controls-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; margin-top: 8px; }
  .btn {
    border: none; border-radius: 8px; padding: 13px 8px; font-size: 12px; font-weight: 800; letter-spacing: 0.8px;
    text-transform: uppercase; cursor: pointer; display: flex; flex-direction: column; align-items: center; gap: 3px;
    transition: transform 0.12s, box-shadow 0.2s; color: #fff; text-shadow: 0 1px 3px rgba(0,0,0,0.6);
  }
  .btn:active { transform: scale(0.96); }
  .btn-deploy { background: linear-gradient(135deg, #00b4d8, #0077b6); border: 1px solid var(--cyan); box-shadow: 0 4px 16px var(--cyan-glow); }
  .btn-fold { background: linear-gradient(135deg, #2b2d42, #1f2232); border: 1px solid rgba(255,255,255,0.2); }
  .btn-zero { background: linear-gradient(135deg, #00a86b, #007f5f); border: 1px solid var(--emerald); box-shadow: 0 4px 16px var(--emerald-glow); }
  .btn-fire { background: linear-gradient(135deg, #d90429, #9a031e); border: 1px solid var(--red); box-shadow: 0 4px 18px var(--red-glow); grid-column: span 2; }
  .btn-calib { background: rgba(255,183,0,0.12); color: var(--amber); border: 1px solid rgba(255,183,0,0.3); grid-column: span 2; padding: 10px; font-size: 11px; }

  .color-presets { display: flex; gap: 8px; margin: 10px 0; }
  .swatch { width: 30px; height: 30px; border-radius: 8px; border: 2px solid rgba(255,255,255,0.2); cursor: pointer; transition: transform 0.15s, border-color 0.15s; }
  .swatch:hover, .swatch.active { transform: scale(1.15); border-color: #fff; }

  .slider-group { margin: 12px 0; }
  .slider-header { display: flex; justify-content: space-between; font-size: 12px; margin-bottom: 5px; }
  input[type="range"] {
    width: 100%; -webkit-appearance: none; height: 6px; border-radius: 3px; background: rgba(255,255,255,0.1); outline: none;
  }
  input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none; width: 18px; height: 18px; border-radius: 50%; background: var(--cyan);
    box-shadow: 0 0 10px var(--cyan-glow); cursor: pointer; transition: transform 0.1s;
  }
  input[type="range"]::-webkit-slider-thumb:active { transform: scale(1.3); }

  .apply-btn { width: 100%; margin-top: 12px; background: linear-gradient(90deg, var(--cyan), #0077b6); border: none; padding: 11px; border-radius: 8px; color: #fff; font-weight: 800; font-size: 12px; text-transform: uppercase; cursor: pointer; box-shadow: 0 4px 14px var(--cyan-glow); }
  .apply-btn:active { transform: scale(0.98); }

  #toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%) translateY(100px); background: rgba(0,240,255,0.95); color: #000; font-weight: 800; padding: 10px 20px; border-radius: 20px; font-size: 12px; box-shadow: 0 4px 20px rgba(0,240,255,0.5); transition: transform 0.3s cubic-bezier(0.2, 0.9, 0.4, 1); z-index: 10000; pointer-events: none; }
  #toast.show { transform: translateX(-50%) translateY(0); }

  /* ========================================================================= */
  /* FULLSCREEN RC COCKPIT OVERLAY                                             */
  /* ========================================================================= */
  #rcCockpit {
    position: fixed; inset: 0; width: 100vw; height: 100vh; z-index: 9999;
    background-color: #06080d;
    background-image:
      radial-gradient(circle at 50% 15%, rgba(0, 240, 255, 0.13), transparent 55%),
      radial-gradient(circle at 85% 85%, rgba(255, 42, 85, 0.09), transparent 45%),
      linear-gradient(rgba(0, 240, 255, 0.035) 1px, transparent 1px),
      linear-gradient(90deg, rgba(0, 240, 255, 0.035) 1px, transparent 1px);
    background-size: 100% 100%, 100% 100%, 25px 25px, 25px 25px;
    display: none; flex-direction: column; overflow-y: auto; overflow-x: hidden;
    padding: 10px; gap: 10px;
  }
  #rcCockpit.active { display: flex; animation: cockpitIn 0.22s cubic-bezier(0.1, 0.9, 0.2, 1); }
  @keyframes cockpitIn { from { opacity: 0; transform: scale(0.98); } to { opacity: 1; transform: scale(1); } }

  .cockpit-header {
    display: flex; justify-content: space-between; align-items: center;
    background: rgba(14, 18, 28, 0.88); backdrop-filter: blur(16px);
    border: 1px solid var(--border); border-radius: var(--radius);
    padding: 10px 14px; box-shadow: 0 4px 20px rgba(0,0,0,0.6);
  }
  .btn-close-rc {
    background: rgba(255,42,85,0.15); border: 1px solid var(--red); color: var(--red);
    padding: 6px 14px; border-radius: 8px; font-weight: 800; font-size: 11px; cursor: pointer;
    text-transform: uppercase; letter-spacing: 1px; transition: transform 0.15s, background 0.15s;
  }
  .btn-close-rc:active { transform: scale(0.95); background: rgba(255,42,85,0.3); }

  .cockpit-body {
    display: grid; grid-template-columns: 1fr; gap: 10px; max-width: 900px; width: 100%; margin: 0 auto;
  }
  @media (min-width: 768px) {
    .cockpit-body { grid-template-columns: 1fr 1fr; align-items: start; gap: 14px; }
  }

  /* Mode Switch Card */
  .mode-switch-card {
    background: var(--card); border: 1px solid var(--border); border-radius: var(--radius);
    padding: 12px; box-shadow: 0 4px 20px rgba(0,0,0,0.4);
  }
  .mode-toggle-group {
    display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-top: 8px;
    background: rgba(0,0,0,0.45); padding: 4px; border-radius: 10px; border: 1px solid var(--border-dim);
  }
  .mode-tab {
    padding: 12px 6px; text-align: center; border-radius: 8px; font-size: 11px; font-weight: 800;
    letter-spacing: 0.8px; cursor: pointer; color: var(--text-dim); transition: all 0.2s;
  }
  .mode-tab.active.tracking {
    background: linear-gradient(135deg, rgba(0,255,157,0.25), rgba(0,200,120,0.45));
    color: var(--emerald); border: 1px solid var(--emerald); box-shadow: 0 0 14px var(--emerald-glow);
  }
  .mode-tab.active.rc {
    background: linear-gradient(135deg, rgba(0,240,255,0.25), rgba(0,119,182,0.45));
    color: var(--cyan); border: 1px solid var(--cyan); box-shadow: 0 0 16px var(--cyan-glow);
  }

  /* Reticle & Live Aiming Radar */
  .radar-card {
    background: var(--card); border: 1px solid var(--border-dim); border-radius: var(--radius);
    padding: 12px; display: flex; flex-direction: column; align-items: center;
  }
  .reticle-box {
    position: relative; width: 100%; max-width: 300px; height: 140px;
    background: radial-gradient(circle, rgba(0,240,255,0.06), transparent 70%), rgba(0,0,0,0.6);
    border: 1px solid var(--border); border-radius: 10px; overflow: hidden; margin: 8px 0;
    cursor: crosshair; touch-action: none;
  }
  .reticle-grid {
    position: absolute; inset: 0;
    background-image: 
      linear-gradient(rgba(0,240,255,0.12) 1px, transparent 1px),
      linear-gradient(90deg, rgba(0,240,255,0.12) 1px, transparent 1px);
    background-size: 20px 20px; pointer-events: none;
  }
  .reticle-axis-x { position: absolute; left: 0; right: 0; top: 50%; height: 1px; background: rgba(0,240,255,0.3); pointer-events: none; }
  .reticle-axis-y { position: absolute; top: 0; bottom: 0; left: 50%; width: 1px; background: rgba(0,240,255,0.3); pointer-events: none; }
  .reticle-crosshair {
    position: absolute; width: 22px; height: 22px; border: 2px solid var(--red); border-radius: 50%;
    transform: translate(-50%, -50%); pointer-events: none; transition: left 0.08s ease-out, top 0.08s ease-out;
    box-shadow: 0 0 12px var(--red-glow); display: flex; align-items: center; justify-content: center;
  }
  .reticle-crosshair::after { content: ''; width: 4px; height: 4px; border-radius: 50%; background: var(--red); }

  /* Virtual Joystick Section */
  .joystick-card {
    background: var(--card); border: 1px solid var(--border); border-radius: var(--radius);
    padding: 14px; display: flex; flex-direction: column; align-items: center;
  }
  .joystick-zone {
    position: relative; width: 220px; height: 220px; margin: 8px auto;
    display: flex; align-items: center; justify-content: center;
    touch-action: none; -webkit-user-select: none; user-select: none;
    cursor: grab;
  }
  .joystick-zone:active { cursor: grabbing; }
  .joystick-base {
    position: absolute; inset: 0; border-radius: 50%;
    background: radial-gradient(circle, rgba(20,28,45,0.95), rgba(7,9,14,0.98));
    border: 2px solid var(--cyan); box-shadow: 0 0 24px var(--cyan-glow), inset 0 0 24px rgba(0,240,255,0.12);
    pointer-events: none;
  }
  .joystick-base::before {
    content: ''; position: absolute; inset: 24px; border-radius: 50%;
    border: 1px dashed rgba(0,240,255,0.25); pointer-events: none;
  }
  .joystick-base::after {
    content: ''; position: absolute; left: 50%; top: 12px; bottom: 12px; width: 1px;
    background: rgba(0,240,255,0.2); transform: translateX(-50%); pointer-events: none;
  }
  .joystick-base-horiz {
    position: absolute; top: 50%; left: 12px; right: 12px; height: 1px;
    background: rgba(0,240,255,0.2); transform: translateY(-50%); pointer-events: none;
  }
  .joystick-thumb {
    position: absolute; width: 68px; height: 68px; border-radius: 50%;
    background: radial-gradient(circle at 35% 35%, #00f0ff, #005f73);
    border: 2.5px solid #fff; box-shadow: 0 6px 20px rgba(0,0,0,0.8), 0 0 20px var(--cyan-glow);
    left: 50%; top: 50%; transform: translate(-50%, -50%);
    pointer-events: none; display: flex; align-items: center; justify-content: center;
    font-size: 20px; color: #000; font-weight: 900;
  }

  /* Nudge D-Pad Controls */
  .nudge-grid {
    display: grid; grid-template-columns: repeat(3, 46px); grid-template-rows: repeat(2, 38px);
    gap: 6px; justify-content: center; margin-top: 8px;
  }
  .btn-nudge {
    background: rgba(255,255,255,0.06); border: 1px solid var(--border-dim); border-radius: 6px;
    color: var(--cyan); font-weight: 800; font-size: 15px; cursor: pointer; display: flex;
    align-items: center; justify-content: center; transition: background 0.12s, transform 0.12s;
  }
  .btn-nudge:active { background: rgba(0,240,255,0.25); transform: scale(0.92); }

  /* Quick Action Buttons in Cockpit */
  .cockpit-actions {
    display: grid; grid-template-columns: repeat(2, 1fr); gap: 8px; width: 100%;
  }
  .btn-cockpit {
    border: none; border-radius: 8px; padding: 12px 6px; font-size: 11px; font-weight: 800;
    text-transform: uppercase; cursor: pointer; display: flex; flex-direction: column; align-items: center;
    gap: 3px; transition: transform 0.12s; color: #fff;
  }
  .btn-cockpit:active { transform: scale(0.96); }
  .btn-cockpit-deploy { background: linear-gradient(135deg, #00b4d8, #0077b6); border: 1px solid var(--cyan); box-shadow: 0 4px 14px var(--cyan-glow); }
  .btn-cockpit-fold { background: linear-gradient(135deg, #2b2d42, #1f2232); border: 1px solid rgba(255,255,255,0.2); }
  .btn-cockpit-zero { background: linear-gradient(135deg, #00a86b, #007f5f); border: 1px solid var(--emerald); box-shadow: 0 4px 14px var(--emerald-glow); }
  .btn-cockpit-center { background: linear-gradient(135deg, #5a189a, #3c096c); border: 1px solid var(--purple); box-shadow: 0 4px 14px rgba(176,38,255,0.3); }
  .btn-cockpit-fire {
    grid-column: span 2; background: linear-gradient(135deg, #d90429, #9a031e);
    border: 1px solid var(--red); box-shadow: 0 4px 20px var(--red-glow); padding: 15px; font-size: 13px;
  }
</style>
</head>
<body>

<!-- MAIN HUD CONTAINER -->
<div class="container" id="mainDashboard">
  <header>
    <div class="title-group">
      <div class="logo-icon">&#9670;</div>
      <div>
        <h1>Gimbal Pod HUD</h1>
        <div class="subtitle">TACTICAL EQUIPMENT INTERFACE</div>
      </div>
    </div>
    <div class="header-actions">
      <button class="btn-rc-launch" onclick="openRcCockpit()">
        <span>&#127918; RC COCKPIT</span>
      </button>
      <div class="network-badge">
        <div class="dot"></div>
        <span id="headerStatus">AP ACTIVE</span>
      </div>
    </div>
  </header>

  <!-- Dual Status Cards -->
  <div class="grid-2">
    <!-- Gimbal Equipment Card -->
    <div class="card">
      <div class="card-header">
        <div class="card-title">&#128065; Shoulder Gimbal</div>
        <div class="status-pill pill-blue" id="gimbalStatePill">FOLDED</div>
      </div>
      <div class="battery-container">
        <div class="battery-header">
          <span class="telem-label">Battery Voltage (GPIO 4)</span>
          <span class="battery-val" id="gimbalVbatText">-- V (--%)</span>
        </div>
        <div class="battery-bar-bg">
          <div class="battery-bar-fill" id="gimbalVbatBar"></div>
        </div>
      </div>
      <div class="angle-display">
        <div class="angle-box">
          <div class="angle-num" id="gimbalYawVal">0.0&deg;</div>
          <div class="angle-sub">YAW SERVO (0-180&deg;)</div>
        </div>
        <div class="angle-box">
          <div class="angle-num" id="gimbalPitchVal">0.0&deg;</div>
          <div class="angle-sub">PITCH SERVO (0-175&deg;)</div>
        </div>
      </div>
      <div class="telemetry-row" style="margin-top:12px;">
        <span class="telem-label">Control Mode:</span>
        <span class="telem-val" id="controlModeVal">HEAD TRACKING</span>
      </div>
      <div class="telemetry-row">
        <span class="telem-label">Neck IMU (0x69):</span>
        <span class="telem-val" id="neckImuVal">CHECKING</span>
      </div>
      <div class="telemetry-row">
        <span class="telem-label">Shoulder IMU (0x68):</span>
        <span class="telem-val" id="shoulderImuVal">CHECKING</span>
      </div>
      <div class="telemetry-row">
        <span class="telem-label">Zero Reference Lock:</span>
        <span class="telem-val" id="zeroedVal">UNLOCKED</span>
      </div>
    </div>

    <!-- Webshooter Equipment Card -->
    <div class="card">
      <div class="card-header">
        <div class="card-title">&#128376; Webshooter Master</div>
        <div class="status-pill pill-amber" id="webshooterStatePill">SEARCHING</div>
      </div>
      <div class="battery-container">
        <div class="battery-header">
          <span class="telem-label">Remote Battery (GPIO 4)</span>
          <span class="battery-val" id="webshooterVbatText">-- V (--%)</span>
        </div>
        <div class="battery-bar-bg">
          <div class="battery-bar-fill" id="webshooterVbatBar"></div>
        </div>
      </div>
      <div class="telemetry-row">
        <span class="telem-label">ESP-NOW Link:</span>
        <span class="telem-val" id="wsLinkVal">OFFLINE</span>
      </div>
      <div class="telemetry-row">
        <span class="telem-label">Extension Stance:</span>
        <span class="telem-val" id="wsStanceVal">UNKNOWN</span>
      </div>
      <div class="telemetry-row">
        <span class="telem-label">Gimbal Armed State:</span>
        <span class="telem-val" id="wsArmedVal">DISARMED</span>
      </div>
      <div class="telemetry-row">
        <span class="telem-label">Firing Status:</span>
        <span class="telem-val" id="wsFiringVal">IDLE</span>
      </div>
      <div class="telemetry-row">
        <span class="telem-label">Telemetry Age:</span>
        <span class="telem-val" id="wsLastSeenVal">-- ms</span>
      </div>
    </div>
  </div>

  <!-- Command Deck -->
  <div class="card">
    <div class="card-header">
      <div class="card-title">&#9889; Tactical Command Deck</div>
      <div class="subtitle">REAL-TIME OVERRIDE</div>
    </div>
    <div class="controls-grid">
      <button class="btn btn-deploy" onclick="sendCmd('deploy')">
        <span>ARM & DEPLOY</span>
        <small style="font-size:10px;opacity:0.85;">Unfold & track live angles</small>
      </button>
      <button class="btn btn-fold" onclick="sendCmd('fold')">
        <span>DISARM & STOW</span>
        <small style="font-size:10px;opacity:0.85;">Retract simultaneously to 0&deg;, 0&deg;</small>
      </button>
      <button class="btn btn-zero" onclick="sendCmd('zero')">
        <span>ZERO YAW & PITCH</span>
        <small style="font-size:10px;opacity:0.85;">Align forward horizon baseline</small>
      </button>
      <button class="btn btn-deploy" style="background: linear-gradient(135deg, #00f0ff, #0077b6);" onclick="openRcCockpit()">
        <span>ENTER RC COCKPIT</span>
        <small style="font-size:10px;opacity:0.85;">Fullscreen Virtual Joystick Mode</small>
      </button>
      <button class="btn btn-calib" onclick="sendCmd('calibrate_ground')">
        RUN GROUND GYRO CALIBRATION (DESK ONLY)
      </button>
      <button class="btn btn-fire" onmousedown="fireStart()" onmouseup="fireStop()" ontouchstart="fireStart()" ontouchend="fireStop()">
        <span>HOLD TO FIRE ELECTROMAGNET</span>
        <small style="font-size:10px;opacity:0.85;">Full 25W pulse (Max 8.0s safety limit)</small>
      </button>
    </div>
  </div>

  <!-- LED & Dynamic Settings Deck -->
  <div class="grid-2">
    <!-- LED Controls -->
    <div class="card">
      <div class="card-header">
        <div class="card-title">&#128161; Barrel RGB LEDs</div>
        <div class="subtitle">DUAL PIXELS (GPIO 21)</div>
      </div>
      <div class="slider-group">
        <div class="slider-header">
          <span>Standby Color</span>
          <span id="standbyColorHex">#FFFFFF</span>
        </div>
        <div class="color-presets">
          <div class="swatch active" style="background:#FFFFFF;" onclick="setStandbyColor(255,255,255,'#FFFFFF')"></div>
          <div class="swatch" style="background:#00F0FF;" onclick="setStandbyColor(0,240,255,'#00F0FF')"></div>
          <div class="swatch" style="background:#00FF9D;" onclick="setStandbyColor(0,255,157,'#00FF9D')"></div>
          <div class="swatch" style="background:#FFB700;" onclick="setStandbyColor(255,183,0,'#FFB700')"></div>
          <div class="swatch" style="background:#B026FF;" onclick="setStandbyColor(176,38,255,'#B026FF')"></div>
        </div>
      </div>
      <div class="slider-group">
        <div class="slider-header">
          <span>Deployed Color</span>
          <span id="deployedColorHex">#FF2A55</span>
        </div>
        <div class="color-presets">
          <div class="swatch active" style="background:#FF2A55;" onclick="setDeployedColor(255,42,85,'#FF2A55')"></div>
          <div class="swatch" style="background:#FF6D00;" onclick="setDeployedColor(255,109,0,'#FF6D00')"></div>
          <div class="swatch" style="background:#76FF03;" onclick="setDeployedColor(118,255,3,'#76FF03')"></div>
          <div class="swatch" style="background:#00F0FF;" onclick="setDeployedColor(0,240,255,'#00F0FF')"></div>
        </div>
      </div>
      <div class="slider-group">
        <div class="slider-header">
          <span>Min Floor Brightness (Never 0%)</span>
          <span id="minBrightVal">12</span>
        </div>
        <input type="range" id="minBrightSlider" min="2" max="60" value="12" oninput="document.getElementById('minBrightVal').innerText = this.value">
      </div>
      <div class="slider-group">
        <div class="slider-header">
          <span>Max Peak Brightness</span>
          <span id="maxBrightVal">220</span>
        </div>
        <input type="range" id="maxBrightSlider" min="80" max="255" value="220" oninput="document.getElementById('maxBrightVal').innerText = this.value">
      </div>
      <div class="slider-group">
        <div class="slider-header">
          <span>Breathing Cycle Period</span>
          <span id="breathPeriodVal">3600 ms</span>
        </div>
        <input type="range" id="breathPeriodSlider" min="1500" max="6000" step="100" value="3600" oninput="document.getElementById('breathPeriodVal').innerText = this.value + ' ms'">
      </div>
      <button class="apply-btn" onclick="saveLedSettings()">Apply LED Settings</button>
    </div>

    <!-- Gimbal Motion Tuning -->
    <div class="card">
      <div class="card-header">
        <div class="card-title">&#9881; Motion Tuning</div>
        <div class="subtitle">KINEMATICS & TIMING</div>
      </div>
      <div class="slider-group">
        <div class="slider-header">
          <span>Tracking Smoothing Alpha (&alpha;)</span>
          <span id="alphaVal">0.22</span>
        </div>
        <input type="range" id="alphaSlider" min="0.05" max="0.45" step="0.01" value="0.22" oninput="document.getElementById('alphaVal').innerText = this.value">
      </div>
      <div class="slider-group">
        <div class="slider-header">
          <span>Max Slew Speed Limit</span>
          <span id="speedVal">280 deg/s</span>
        </div>
        <input type="range" id="speedSlider" min="100" max="500" step="10" value="280" oninput="document.getElementById('speedVal').innerText = this.value + ' deg/s'">
      </div>
      <div class="slider-group">
        <div class="slider-header">
          <span>Deploy Duration (Simultaneous)</span>
          <span id="deployTimeVal">850 ms</span>
        </div>
        <input type="range" id="deployTimeSlider" min="400" max="2000" step="50" value="850" oninput="document.getElementById('deployTimeVal').innerText = this.value + ' ms'">
      </div>
      <div class="slider-group">
        <div class="slider-header">
          <span>Fold Duration (Simultaneous)</span>
          <span id="foldTimeVal">650 ms</span>
        </div>
        <input type="range" id="foldTimeSlider" min="400" max="2000" step="50" value="650" oninput="document.getElementById('foldTimeVal').innerText = this.value + ' ms'">
      </div>
      <button class="apply-btn" onclick="saveMotionSettings()">Apply Kinematics Settings</button>
    </div>
  </div>
</div>

<!-- ========================================================================= -->
<!-- FULLSCREEN RC COCKPIT PAGE                                                -->
<!-- ========================================================================= -->
<div id="rcCockpit">
  <div class="cockpit-header">
    <div style="display:flex;align-items:center;gap:10px;">
      <button class="btn-close-rc" onclick="closeRcCockpit()">&#10005; EXIT</button>
      <div>
        <div style="font-weight:800;font-size:13px;letter-spacing:1px;color:var(--cyan);text-transform:uppercase;">TURRET RC COCKPIT</div>
        <div style="font-size:10px;color:var(--text-dim);" id="rcVbatHeader">GIMBAL: --V</div>
      </div>
    </div>
    <div style="display:flex;align-items:center;gap:8px;">
      <span class="status-pill pill-blue" id="rcGimbalPill">FOLDED</span>
      <span class="status-pill pill-purple" id="rcModeBadge">RC ACTIVE</span>
    </div>
  </div>

  <div class="cockpit-body">
    <!-- Left Column: Mode Switch, Radar, & Quick Buttons -->
    <div style="display:flex;flex-direction:column;gap:10px;">
      <!-- Mode Toggle Switch -->
      <div class="mode-switch-card">
        <div style="display:flex;justify-content:space-between;align-items:center;">
          <span style="font-weight:800;font-size:12px;color:var(--cyan);letter-spacing:1px;text-transform:uppercase;">CONTROL SOURCE</span>
          <span style="font-size:10px;color:var(--text-dim);" id="rcActiveLabel">RC DIRECT</span>
        </div>
        <div class="mode-toggle-group">
          <div class="mode-tab tracking" id="tabTracking" onclick="setRcMode(false)">
            &#128065; HEAD TRACKING
          </div>
          <div class="mode-tab active rc" id="tabRc" onclick="setRcMode(true)">
            &#127918; RC JOYSTICK
          </div>
        </div>
      </div>

      <!-- Radar Reticle Box with Tap-to-Aim -->
      <div class="radar-card">
        <div style="width:100%;display:flex;justify-content:space-between;align-items:center;">
          <span style="font-weight:800;font-size:11px;color:var(--text-dim);letter-spacing:1px;">AIMING RETICLE (TAP TO AIM)</span>
          <span style="font-size:11px;font-weight:800;color:var(--cyan);" id="rcAnglesReadout">Y: 90.0&deg; | P: 110.0&deg;</span>
        </div>
        <div class="reticle-box" id="reticleBox">
          <div class="reticle-grid"></div>
          <div class="reticle-axis-x"></div>
          <div class="reticle-axis-y"></div>
          <div class="reticle-crosshair" id="reticleCrosshair"></div>
        </div>
        <div style="width:100%;display:flex;justify-content:space-between;font-size:10px;color:var(--text-dim);">
          <span>0&deg; (LEFT)</span>
          <span>CENTER (90&deg;, 110&deg;)</span>
          <span>180&deg; (RIGHT)</span>
        </div>
      </div>

      <!-- Tactical Deck Quick Buttons -->
      <div class="cockpit-actions">
        <button class="btn-cockpit btn-cockpit-deploy" onclick="sendCmd('deploy')">
          <span>ARM & DEPLOY</span>
        </button>
        <button class="btn-cockpit btn-cockpit-fold" onclick="sendCmd('fold')">
          <span>DISARM & STOW</span>
        </button>
        <button class="btn-cockpit btn-cockpit-zero" onclick="sendCmd('zero')">
          <span>ZERO HORIZON</span>
        </button>
        <button class="btn-cockpit btn-cockpit-center" onclick="rcCenter()">
          <span>RE-CENTER (90&deg;,110&deg;)</span>
        </button>
        <button class="btn-cockpit btn-cockpit-fire" onmousedown="fireStart()" onmouseup="fireStop()" ontouchstart="fireStart()" ontouchend="fireStop()">
          <span>HOLD TO FIRE ELECTROMAGNET</span>
        </button>
      </div>
    </div>

    <!-- Right Column: Precision Touch Virtual Joystick & Micro-Nudge -->
    <div class="joystick-card">
      <div style="width:100%;display:flex;justify-content:space-between;align-items:center;margin-bottom:4px;">
        <span style="font-weight:800;font-size:12px;color:var(--cyan);letter-spacing:1px;">2-AXIS JOYSTICK</span>
        <span style="font-size:10px;color:var(--text-dim);">TOUCH & DRAG TO STEER</span>
      </div>

      <!-- Touch container with pointer-events: none on children so drag never drops -->
      <div class="joystick-zone" id="joyZone">
        <div class="joystick-base">
          <div class="joystick-base-horiz"></div>
        </div>
        <div class="joystick-thumb" id="joyThumb">&#9678;</div>
      </div>

      <div style="font-size:11px;font-weight:700;color:var(--text-dim);margin-top:4px;" id="joyFeedback">
        JOYSTICK: NEUTRAL
      </div>

      <!-- Micro-Nudge D-Pad -->
      <div style="margin-top:10px;width:100%;text-align:center;">
        <div style="font-size:10px;color:var(--text-dim);letter-spacing:1px;margin-bottom:4px;">5&deg; MICRO-NUDGE</div>
        <div class="nudge-grid">
          <div></div>
          <button class="btn-nudge" onclick="nudge(0, 5)">&#9650;</button>
          <div></div>
          <button class="btn-nudge" onclick="nudge(-5, 0)">&#9664;</button>
          <button class="btn-nudge" onclick="nudge(0, -5)">&#9660;</button>
          <button class="btn-nudge" onclick="nudge(5, 0)">&#9654;</button>
        </div>
      </div>

      <button style="margin-top:10px;background:rgba(255,183,0,0.12);color:var(--amber);border:1px solid rgba(255,183,0,0.3);border-radius:6px;padding:8px 12px;font-size:11px;font-weight:800;cursor:pointer;width:100%;text-transform:uppercase;" onclick="sendCmd('calibrate_ground')">
        GROUND GYRO CALIBRATION (DESK ONLY)
      </button>
    </div>
  </div>
</div>

<div id="toast">Command Sent</div>

<script>
  let standbyR = 255, standbyG = 255, standbyB = 255;
  let deployedR = 255, deployedG = 42, deployedB = 85;
  let currentRcYaw = 90.0, currentRcPitch = 110.0;
  let currentRcMode = false;
  let isDraggingJoy = false;

  function showToast(msg) {
    const t = document.getElementById('toast');
    t.innerText = msg;
    t.classList.add('show');
    setTimeout(() => t.classList.remove('show'), 1800);
  }

  function openRcCockpit() {
    document.getElementById('rcCockpit').classList.add('active');
    if (!currentRcMode) {
      setRcMode(true);
    }
  }

  function closeRcCockpit() {
    document.getElementById('rcCockpit').classList.remove('active');
    // When exiting the RC cockpit, automatically default back to Head Tracking
    if (currentRcMode) {
      setRcMode(false);
    }
  }

  function setRcMode(enabled) {
    fetch('/api/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'action=rc_mode&enabled=' + (enabled ? '1' : '0')
    })
    .then(r => r.json())
    .then(d => {
      currentRcMode = enabled;
      updateRcModeUI(enabled);
      showToast(enabled ? 'RC Direct Joystick Activated' : 'Head Tracking Activated');
    })
    .catch(e => showToast('Mode switch failed'));
  }

  function updateRcModeUI(isRc) {
    const tabTrk = document.getElementById('tabTracking');
    const tabRc = document.getElementById('tabRc');
    const badge = document.getElementById('rcModeBadge');
    const activeLbl = document.getElementById('rcActiveLabel');
    const modeVal = document.getElementById('controlModeVal');

    if (isRc) {
      if (tabTrk) tabTrk.className = 'mode-tab tracking';
      if (tabRc) tabRc.className = 'mode-tab active rc';
      if (badge) {
        badge.innerText = 'RC DIRECT';
        badge.className = 'status-pill pill-blue';
      }
      if (activeLbl) {
        activeLbl.innerText = 'MANUAL RC JOYSTICK';
        activeLbl.style.color = 'var(--cyan)';
      }
      if (modeVal) {
        modeVal.innerText = 'RC MANUAL JOYSTICK';
        modeVal.style.color = 'var(--cyan)';
      }
    } else {
      if (tabTrk) tabTrk.className = 'mode-tab active tracking';
      if (tabRc) tabRc.className = 'mode-tab rc';
      if (badge) {
        badge.innerText = 'HEAD TRACKING';
        badge.className = 'status-pill pill-green';
      }
      if (activeLbl) {
        activeLbl.innerText = 'DUAL-IMU TRACKING';
        activeLbl.style.color = 'var(--emerald)';
      }
      if (modeVal) {
        modeVal.innerText = 'HEAD TRACKING (0x68/0x69)';
        modeVal.style.color = 'var(--emerald)';
      }
    }
  }

  function nudge(dYaw, dPitch) {
    currentRcYaw += dYaw;
    currentRcPitch += dPitch;
    currentRcYaw = Math.max(0, Math.min(180, currentRcYaw));
    currentRcPitch = Math.max(0, Math.min(175, currentRcPitch));
    updateReticle(currentRcYaw, currentRcPitch);

    fetch('/api/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `action=rc_target&yaw=${currentRcYaw.toFixed(1)}&pitch=${currentRcPitch.toFixed(1)}`
    })
    .then(r => r.json())
    .then(() => showToast(`Nudged (${dYaw >= 0 ? '+' : ''}${dYaw}°, ${dPitch >= 0 ? '+' : ''}${dPitch}°)`))
    .catch(() => {});
  }

  function rcCenter() {
    currentRcYaw = 90.0;
    currentRcPitch = 110.0;
    updateReticle(currentRcYaw, currentRcPitch);
    sendCmd('rc_center');
  }

  function setStandbyColor(r, g, b, hex) {
    standbyR = r; standbyG = g; standbyB = b;
    document.getElementById('standbyColorHex').innerText = hex;
    showToast('Standby color selected: ' + hex);
  }

  function setDeployedColor(r, g, b, hex) {
    deployedR = r; deployedG = g; deployedB = b;
    document.getElementById('deployedColorHex').innerText = hex;
    showToast('Deployed color selected: ' + hex);
  }

  function sendCmd(action) {
    fetch('/api/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'action=' + encodeURIComponent(action)
    })
    .then(r => r.json())
    .then(data => { showToast(data.message || 'Action Executed'); })
    .catch(e => { showToast('Error sending command'); });
  }

  let fireActive = false;
  function fireStart() {
    if (!fireActive) {
      fireActive = true;
      sendCmd('fire_start');
    }
  }
  function fireStop() {
    if (fireActive) {
      fireActive = false;
      sendCmd('fire_stop');
    }
  }

  function saveLedSettings() {
    const minB = document.getElementById('minBrightSlider').value;
    const maxB = document.getElementById('maxBrightSlider').value;
    const period = document.getElementById('breathPeriodSlider').value;
    const params = new URLSearchParams({
      action: 'led',
      sr: standbyR, sg: standbyG, sb: standbyB,
      dr: deployedR, dg: deployedG, db: deployedB,
      min_b: minB, max_b: maxB, period: period
    });
    fetch('/api/settings', { method: 'POST', body: params })
      .then(r => r.json())
      .then(d => showToast('LED Settings Saved!'))
      .catch(e => showToast('Save Failed'));
  }

  function saveMotionSettings() {
    const alpha = document.getElementById('alphaSlider').value;
    const speed = document.getElementById('speedSlider').value;
    const deployT = document.getElementById('deployTimeSlider').value;
    const foldT = document.getElementById('foldTimeSlider').value;
    const params = new URLSearchParams({
      action: 'motion',
      alpha: alpha, speed: speed, deploy_ms: deployT, fold_ms: foldT
    });
    fetch('/api/settings', { method: 'POST', body: params })
      .then(r => r.json())
      .then(d => showToast('Kinematics Saved!'))
      .catch(e => showToast('Save Failed'));
  }

  function updateBatteryUI(vbatMv, textElId, barElId) {
    const vText = document.getElementById(textElId);
    const vBar = barElId ? document.getElementById(barElId) : null;
    if (!vbatMv || vbatMv < 1000) {
      if (vText) vText.innerText = "NO BATTERY (USB)";
      if (vBar) vBar.style.width = "0%";
      return;
    }
    const volts = (vbatMv / 1000.0).toFixed(2);
    let pct = 0;
    if (vbatMv > 5000) {
      pct = Math.round(((vbatMv - 6600) / (8400 - 6600)) * 100);
    } else {
      pct = Math.round(((vbatMv - 3300) / (4200 - 3300)) * 100);
    }
    pct = Math.max(0, Math.min(100, pct));
    if (vText) vText.innerText = volts + "V (" + pct + "%)";
    if (vBar) {
      vBar.style.width = pct + "%";
      if (pct > 50) {
        vBar.style.backgroundColor = "var(--emerald)";
        vBar.style.boxShadow = "0 0 10px var(--emerald-glow)";
      } else if (pct > 20) {
        vBar.style.backgroundColor = "var(--amber)";
        vBar.style.boxShadow = "0 0 10px rgba(255,183,0,0.4)";
      } else {
        vBar.style.backgroundColor = "var(--red)";
        vBar.style.boxShadow = "0 0 10px var(--red-glow)";
      }
    }
  }

  function updateReticle(yaw, pitch) {
    const crosshair = document.getElementById('reticleCrosshair');
    const readout = document.getElementById('rcAnglesReadout');
    if (!crosshair) return;

    const posX = Math.max(4, Math.min(96, (yaw / 180.0) * 100));
    const posY = Math.max(4, Math.min(96, 100 - (pitch / 175.0) * 100));

    crosshair.style.left = posX.toFixed(1) + '%';
    crosshair.style.top = posY.toFixed(1) + '%';
    if (readout) readout.innerHTML = 'Y: ' + yaw.toFixed(1) + '&deg; | P: ' + pitch.toFixed(1) + '&deg;';
  }

  // Tap-to-aim on the Reticle Radar
  (function initRadarTap() {
    const box = document.getElementById('reticleBox');
    if (!box) return;
    box.addEventListener('pointerdown', (e) => {
      const rect = box.getBoundingClientRect();
      const x = Math.max(0, Math.min(rect.width, e.clientX - rect.left));
      const y = Math.max(0, Math.min(rect.height, e.clientY - rect.top));

      currentRcYaw = (x / rect.width) * 180.0;
      currentRcPitch = (1.0 - (y / rect.height)) * 175.0;
      currentRcYaw = Math.max(0, Math.min(180, currentRcYaw));
      currentRcPitch = Math.max(0, Math.min(175, currentRcPitch));

      updateReticle(currentRcYaw, currentRcPitch);

      fetch('/api/cmd', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `action=rc_target&yaw=${currentRcYaw.toFixed(1)}&pitch=${currentRcPitch.toFixed(1)}`
      }).catch(() => {});
    });
  })();

  function pollStatus() {
    fetch('/api/status')
      .then(r => r.json())
      .then(data => {
        const gState = document.getElementById('gimbalStatePill');
        const rcGimbalPill = document.getElementById('rcGimbalPill');
        if (gState) gState.innerText = data.gimbal.state;
        if (rcGimbalPill) rcGimbalPill.innerText = data.gimbal.state;

        const pillCls = (data.gimbal.state === 'TRACKING' ? 'pill-red' : (data.gimbal.state === 'DEPLOYING' ? 'pill-amber' : 'pill-blue'));
        if (gState) gState.className = 'status-pill ' + pillCls;
        if (rcGimbalPill) rcGimbalPill.className = 'status-pill ' + pillCls;

        updateBatteryUI(data.gimbal.vbat_mv, 'gimbalVbatText', 'gimbalVbatBar');
        const vHeader = document.getElementById('rcVbatHeader');
        if (vHeader) {
          const v = data.gimbal.vbat_mv ? (data.gimbal.vbat_mv / 1000.0).toFixed(2) + 'V' : 'USB';
          vHeader.innerText = 'GIMBAL: ' + v;
        }

        const yEl = document.getElementById('gimbalYawVal');
        const pEl = document.getElementById('gimbalPitchVal');
        if (yEl) yEl.innerHTML = data.gimbal.yaw.toFixed(1) + '&deg;';
        if (pEl) pEl.innerHTML = data.gimbal.pitch.toFixed(1) + '&deg;';

        // Keep reticle and RC angles synchronized when not actively dragging the joystick
        if (!isDraggingJoy) {
          currentRcYaw = data.gimbal.yaw;
          currentRcPitch = data.gimbal.pitch;
          updateReticle(data.gimbal.yaw, data.gimbal.pitch);
        }

        currentRcMode = !!data.gimbal.rc_mode;
        updateRcModeUI(currentRcMode);

        const neckEl = document.getElementById('neckImuVal');
        if (neckEl) {
          neckEl.innerText = data.gimbal.neck_ok ? "ONLINE (0x69)" : "ERROR";
          neckEl.style.color = data.gimbal.neck_ok ? "var(--emerald)" : "var(--red)";
        }

        const shldrEl = document.getElementById('shoulderImuVal');
        if (shldrEl) {
          shldrEl.innerText = data.gimbal.shoulder_ok ? "ONLINE (0x68)" : "ERROR";
          shldrEl.style.color = data.gimbal.shoulder_ok ? "var(--emerald)" : "var(--red)";
        }

        const zeroEl = document.getElementById('zeroedVal');
        if (zeroEl) {
          zeroEl.innerText = data.gimbal.zeroed ? "LOCKED (Ready)" : "UNLOCKED / PENDING";
          zeroEl.style.color = data.gimbal.zeroed ? "var(--cyan)" : "var(--amber)";
        }

        // Webshooter State
        const wsPill = document.getElementById('webshooterStatePill');
        const wsLink = document.getElementById('wsLinkVal');
        if (data.webshooter.connected) {
          if (wsPill) {
            wsPill.innerText = data.webshooter.armed ? "ARMED" : "STANDBY";
            wsPill.className = 'status-pill ' + (data.webshooter.armed ? 'pill-red' : 'pill-green');
          }
          if (wsLink) {
            wsLink.innerText = "ACTIVE (Ch 1)";
            wsLink.style.color = "var(--emerald)";
          }
          const posEl = document.getElementById('wsStanceVal');
          if (posEl) posEl.innerText = data.webshooter.pos === 0 ? "EXTENDED (0°)" : "RETRACTED (90°)";
          const armEl = document.getElementById('wsArmedVal');
          if (armEl) armEl.innerText = data.webshooter.armed ? "ARMED (Active)" : "DISARMED";
          const firEl = document.getElementById('wsFiringVal');
          if (firEl) firEl.innerText = data.webshooter.firing ? "FIRING ACTIVE" : "IDLE";
          const seenEl = document.getElementById('wsLastSeenVal');
          if (seenEl) seenEl.innerText = data.webshooter.last_seen_ms + " ms ago";
          updateBatteryUI(data.webshooter.vbat_mv, 'webshooterVbatText', 'webshooterVbatBar');
        } else {
          if (wsPill) {
            wsPill.innerText = "OFFLINE";
            wsPill.className = 'status-pill pill-amber';
          }
          if (wsLink) {
            wsLink.innerText = "NO PACKETS";
            wsLink.style.color = "var(--amber)";
          }
          const seenEl = document.getElementById('wsLastSeenVal');
          if (seenEl) seenEl.innerText = "DISCONNECTED";
          updateBatteryUI(0, 'webshooterVbatText', 'webshooterVbatBar');
        }
      })
      .catch(e => {})
      .finally(() => {
        setTimeout(pollStatus, 200);
      });
  }

  // =========================================================================
  // VIRTUAL JOYSTICK CONTROLLER ENGINE (POINTER-CAPTURE & TOUCH-OPTIMIZED)
  // =========================================================================
  (function initVirtualJoystick() {
    const zone = document.getElementById('joyZone');
    const thumb = document.getElementById('joyThumb');
    const feedback = document.getElementById('joyFeedback');
    if (!zone || !thumb) return;

    let zoneRect = null;
    let centerX = 0, centerY = 0;
    const maxRadius = 65; // px max displacement

    let currentNormX = 0.0; // -1.0 (left) to +1.0 (right)
    let currentNormY = 0.0; // -1.0 (down) to +1.0 (up)
    let joyInterval = null;
    let joySending = false;

    function resetThumb() {
      thumb.style.left = '50%';
      thumb.style.top = '50%';
      currentNormX = 0.0;
      currentNormY = 0.0;
      if (feedback) {
        feedback.innerText = 'JOYSTICK: NEUTRAL';
        feedback.style.color = 'var(--text-dim)';
      }
    }

    resetThumb();

    function onPointerDown(e) {
      isDraggingJoy = true;
      try { zone.setPointerCapture(e.pointerId); } catch(err) {}
      zoneRect = zone.getBoundingClientRect();
      centerX = zoneRect.left + zoneRect.width / 2;
      centerY = zoneRect.top + zoneRect.height / 2;
      updateFromPointer(e);

      // Start rapid transmission loop (20Hz = 50ms) while dragging
      if (!joyInterval) {
        joyInterval = setInterval(transmitJoyVelocity, 50);
      }
    }

    function onPointerMove(e) {
      if (!isDraggingJoy) return;
      updateFromPointer(e);
    }

    function onPointerUp(e) {
      if (!isDraggingJoy) return;
      isDraggingJoy = false;
      try { zone.releasePointerCapture(e.pointerId); } catch(err) {}
      if (joyInterval) {
        clearInterval(joyInterval);
        joyInterval = null;
      }
      resetThumb();

      // Final position confirmation
      fetch('/api/cmd', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `action=rc_target&yaw=${currentRcYaw.toFixed(1)}&pitch=${currentRcPitch.toFixed(1)}`
      }).catch(() => {});
    }

    function updateFromPointer(e) {
      const dx = e.clientX - centerX;
      const dy = e.clientY - centerY;
      const dist = Math.hypot(dx, dy);
      const angle = Math.atan2(dy, dx);

      const clampedDist = Math.min(dist, maxRadius);
      const thumbX = Math.cos(angle) * clampedDist;
      const thumbY = Math.sin(angle) * clampedDist;

      thumb.style.left = `calc(50% + ${thumbX.toFixed(1)}px)`;
      thumb.style.top = `calc(50% + ${thumbY.toFixed(1)}px)`;

      // Deadzone threshold (10%)
      const deadzone = 0.10;
      const rawNormX = thumbX / maxRadius;
      const rawNormY = -thumbY / maxRadius; // inverted: dragging up is positive pitch

      currentNormX = Math.abs(rawNormX) > deadzone ? rawNormX : 0.0;
      currentNormY = Math.abs(rawNormY) > deadzone ? rawNormY : 0.0;

      if (feedback) {
        feedback.innerText = `STEER X: ${(currentNormX * 100).toFixed(0)}% | Y: ${(currentNormY * 100).toFixed(0)}%`;
        feedback.style.color = (currentNormX !== 0 || currentNormY !== 0) ? 'var(--cyan)' : 'var(--text-dim)';
      }
    }

    function transmitJoyVelocity() {
      if (joySending) return;
      if (currentNormX === 0.0 && currentNormY === 0.0) return;

      // Rate: up to 60 deg/sec at full deflection (50ms interval)
      const dt = 0.05;
      const speedDps = 60.0;

      // Right (+X) steers turret right (decreases yaw angle on servo)
      // Up (+Y) steers turret up (increases pitch angle on servo)
      currentRcYaw += (-currentNormX * speedDps * dt);
      currentRcPitch += (currentNormY * speedDps * dt);

      currentRcYaw = Math.max(0, Math.min(180, currentRcYaw));
      currentRcPitch = Math.max(0, Math.min(175, currentRcPitch));

      updateReticle(currentRcYaw, currentRcPitch);

      joySending = true;
      fetch('/api/cmd', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `action=rc_target&yaw=${currentRcYaw.toFixed(1)}&pitch=${currentRcPitch.toFixed(1)}`
      })
      .finally(() => { joySending = false; });
    }

    zone.addEventListener('pointerdown', onPointerDown);
    zone.addEventListener('pointermove', onPointerMove);
    zone.addEventListener('pointerup', onPointerUp);
    zone.addEventListener('pointercancel', onPointerUp);
  })();

  // =========================================================================
  // FAILSAFE: AUTOMATICALLY DEFAULT TO HEAD TRACKING ON UNLOAD / HIDE
  // =========================================================================
  window.addEventListener('beforeunload', () => {
    if (currentRcMode) {
      navigator.sendBeacon('/api/cmd', new URLSearchParams({ action: 'rc_mode', enabled: '0' }));
    }
  });

  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden' && currentRcMode) {
      // Auto-default back to head tracking if user switches away from browser / locks phone
      navigator.sendBeacon('/api/cmd', new URLSearchParams({ action: 'rc_mode', enabled: '0' }));
    }
  });

  // Start continuous polling loop
  pollStatus();
</script>
</body>
</html>
)rawliteral";
