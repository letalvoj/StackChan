(function() {
  var _rawHistory = new Array(320).fill(0.001);
  var _backendHistory = new Array(320).fill(0.0);
  var _agentHistory = new Array(320).fill(0.0);
  var _agentNetworkHistory = new Array(320).fill(0.0);
  var _vadMarkerHistory = new Array(320).fill(0);
  var _stateHistory = new Array(320).fill('idle');
  var _lastVadDiagTime = 0;

  function drawVadChart() {
    var chartCvs = document.getElementById('vad-chart');
    if (!chartCvs) {
      requestAnimationFrame(drawVadChart);
      return;
    }
    var ctx = chartCvs.getContext('2d');
    var w = chartCvs.width, h = chartCvs.height;

    var liveRms = window._liveMicRms || 0.0;
    window._liveMicRms = liveRms * 0.95; // Smooth inter-frame decay

    var now = Date.now();
    if (_lastVadDiagTime === 0 || now - _lastVadDiagTime > 60000) {
      _lastVadDiagTime = now;
      var actxState = (window._vadAudioCtx ? window._vadAudioCtx.state : 'null');
      console.log('[WASM_VAD:DIAG] AudioContext state=' + actxState + ' | Live Mic RMS=' + liveRms.toFixed(4) + ' (Throttled to once per minute)');
    }

    var localAgentRms = 0.0;
    var analyser = window._playbackAnalyser;
    var actx = window._vadAudioCtx;
    if (analyser && actx && actx.state === 'running') {
      var fftData = new Float32Array(analyser.fftSize);
      analyser.getFloatTimeDomainData(fftData);
      var sumSq = 0.0;
      for (var fIdx = 0; fIdx < fftData.length; fIdx++) sumSq += fftData[fIdx] * fftData[fIdx];
      localAgentRms = Math.sqrt(sumSq / fftData.length);
    }
    var agentRms = localAgentRms;
    window._agentPlaybackRms = agentRms;

    var serverAckRms = window._serverAckRms || 0.0;
    var serverDownlinkRms = window._serverDownlinkRms || 0.0;
    var vadSpeechState = window._vadSpeechState || 0;
    var assistantState = window._assistantState || 'idle';
    var turnCount = window._turnCount || 0;

    var vadThresh = (window.store ? window.store.vadThreshold : 0.004);
    var chartYMax = (window.store ? window.store.chartYMax : 0.15);

    _rawHistory.shift();
    _rawHistory.push(liveRms);
    _backendHistory.shift();
    _backendHistory.push(serverAckRms);
    _agentHistory.shift();
    _agentHistory.push(agentRms);
    _agentNetworkHistory.shift();
    _agentNetworkHistory.push(serverDownlinkRms);
    _vadMarkerHistory.shift();
    _vadMarkerHistory.push(vadSpeechState);
    _stateHistory.shift();
    _stateHistory.push(assistantState);

    ctx.fillStyle = '#020617';
    ctx.fillRect(0, 0, w, h);

    var plotH = h - 32;
    var plotBase = h - 6;

    // Render Operational Stage Background Bands
    for (var s = 0; s < _stateHistory.length; s++) {
      var startX = (s / (_stateHistory.length - 1)) * w;
      var barW = Math.max(3, w / _stateHistory.length);
      if (_stateHistory[s] === 'listening') {
        ctx.fillStyle = 'rgba(22, 101, 52, 0.25)';
        ctx.fillRect(startX, 18, barW, plotH);
      } else if (_stateHistory[s] === 'speaking') {
        ctx.fillStyle = 'rgba(6, 182, 212, 0.25)';
        ctx.fillRect(startX, 18, barW, plotH);
      }
    }

    // Render VAD Speech Active Window Highlights
    for (var m = 0; m < _vadMarkerHistory.length; m++) {
      if (_vadMarkerHistory[m] === 1) {
        var startX = (m / (_vadMarkerHistory.length - 1)) * w;
        var barW = Math.max(3, w / _vadMarkerHistory.length);
        ctx.fillStyle = 'rgba(34, 197, 94, 0.3)';
        ctx.fillRect(startX, 18, barW, plotH);
      }
    }

    // Draw dotted RMS threshold line
    var threshNorm = Math.min(1.0, vadThresh / chartYMax);
    var threshY = plotBase - threshNorm * plotH;
    ctx.strokeStyle = '#f43f5e';
    ctx.lineWidth = 1.5;
    ctx.setLineDash([5, 4]);
    ctx.beginPath();
    ctx.moveTo(0, threshY);
    ctx.lineTo(w, threshY);
    ctx.stroke();
    ctx.setLineDash([]);

    // Trace 1: Local Raw Pre-VAD Microphone (Green Dotted Line)
    ctx.strokeStyle = 'rgba(34, 197, 94, 0.95)';
    ctx.lineWidth = 2.0;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    for (var i = 0; i < _rawHistory.length; i++) {
      var norm = Math.min(1.0, _rawHistory[i] / chartYMax);
      var py = plotBase - norm * plotH;
      var px = (i / (_rawHistory.length - 1)) * w;
      if (i === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.stroke();
    ctx.setLineDash([]);

    // Trace 1b: Server Downlink Transmission (dim dotted cyan line, network arrival timestamps)
    ctx.strokeStyle = '#22d3ee';
    ctx.lineWidth = 1.5;
    ctx.globalAlpha = 0.4;
    ctx.setLineDash([2, 4]);
    ctx.beginPath();
    for (var i = 0; i < _agentNetworkHistory.length; i++) {
      var normN = Math.min(1.0, _agentNetworkHistory[i] / chartYMax);
      var pyN = plotBase - normN * plotH;
      var pxN = (i / (_agentNetworkHistory.length - 1)) * w;
      if (i === 0) ctx.moveTo(pxN, pyN);
      else ctx.lineTo(pxN, pyN);
    }
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.globalAlpha = 1.0;

    // Trace 2: Local Speaker Playback (solid bright cyan line, actual time-domain acoustic output)
    ctx.strokeStyle = '#38bdf8';
    ctx.lineWidth = 2.5;
    ctx.setLineDash([]);
    ctx.beginPath();
    for (var i = 0; i < _agentHistory.length; i++) {
      var normC = Math.min(1.0, _agentHistory[i] / chartYMax);
      var pyC = plotBase - normC * plotH;
      var pxC = (i / (_agentHistory.length - 1)) * w;
      if (i === 0) ctx.moveTo(pxC, pyC);
      else ctx.lineTo(pxC, pyC);
    }
    ctx.stroke();

    // Trace 3: Backend Delayed Uplink Stream (Purple Long-Dashed Line drawn on top)
    ctx.strokeStyle = '#a855f7';
    ctx.lineWidth = 3.5;
    ctx.setLineDash([8, 3]);
    ctx.beginPath();
    for (var i = 0; i < _backendHistory.length; i++) {
      var normB = Math.min(1.0, _backendHistory[i] / chartYMax);
      var pyB = plotBase - normB * plotH;
      var pxB = (i / (_backendHistory.length - 1)) * w;
      if (i === 0) ctx.moveTo(pxB, pyB);
      else ctx.lineTo(pxB, pyB);
    }
    ctx.stroke();
    ctx.setLineDash([]);

    // Threshold annotation
    ctx.fillStyle = '#f43f5e';
    ctx.font = '11px monospace';
    ctx.fillText('--- THRESHOLD (' + vadThresh.toFixed(3) + ') ---', w - 210, threshY - 4);

    // Top Status Badge & 3-Color Legend
    var stateColors = { idle: '#64748b', listening: '#22c55e', speaking: '#38bdf8' };
    var stateEmoji = { idle: '⏸', listening: '🟢', speaking: '🔊' };
    ctx.fillStyle = stateColors[assistantState] || '#64748b';
    ctx.font = 'bold 12px monospace';
    var badgeLabel = (assistantState === 'listening' && vadSpeechState === 1) ? '🟢🗣️ RECORDING' : ((stateEmoji[assistantState] || '') + ' ' + assistantState.toUpperCase());
    ctx.fillText(badgeLabel + (turnCount > 0 ? ' #' + turnCount : ''), 12, 16);

    ctx.font = '10px monospace';
    ctx.fillStyle = '#22c55e';
    ctx.fillText('🟢 MIC:' + liveRms.toFixed(3), 110, 14);

    ctx.fillStyle = serverAckRms > 0 ? '#c084fc' : '#64748b';
    ctx.fillText('🟣 UPLINK:' + serverAckRms.toFixed(3), 190, 14);

    ctx.fillStyle = agentRms > 0 ? '#38bdf8' : '#475569';
    ctx.fillText('🔵 AGENT:' + agentRms.toFixed(3), 275, 14);

    requestAnimationFrame(drawVadChart);
  }

  window.addEventListener('DOMContentLoaded', function() {
    requestAnimationFrame(drawVadChart);
  });
})();
