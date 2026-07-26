(function() {
  window._serverAckRms = 0.0;
  window._serverDownlinkRms = 0.0;
  window._activeServerSessionId = null;
  var _monitorWs = null;
  var _debugWs = null;

  window._sendServerConfig = function() {
    if (window._wasmProtocolWs && window._wasmProtocolWs.readyState === 1 && window.store) {
      window._wasmProtocolWs.send(JSON.stringify({
        type: 'config',
        tenet: window.store.tenetMode,
        max_duration: window.store.serverCap
      }));
    }
  };

  var Module = window.Module || {};
  window.Module = Module;

  Module.onWasmProtocolState = function(stateText) {
    if (window.store) {
      window.store.protoStatus = 'WasmProtocol Status: ' + stateText;
    }
    console.log('[WASM_PROTOCOL] State update -> ' + stateText);
  };

  Module.onWasmProtocolTxJson = function(jsonPayload) {
    if (window.store) {
      window.store.protoStatus = 'Last Outgoing TX: ' + jsonPayload;
    }
    console.log('[WASM_PROTOCOL:TX] Transmitted -> ' + jsonPayload);
  };

  function initMonitorWebSocket(sessionId) {
    if (!sessionId) return;
    try {
      var proto = (window.location && window.location.protocol === 'https:') ? 'wss://' : 'ws://';
      if (_monitorWs) try { _monitorWs.close(); } catch(e) {}
      var ws = new WebSocket(proto + window.location.host + '/ws_monitor?session_id=' + encodeURIComponent(sessionId));
      ws.onopen = function() { console.log('[WASM_MONITOR] ✓ Connected to /ws_monitor bound to session: ' + sessionId.slice(0, 8) + '...'); };
      ws.onmessage = function(evt) {
        try {
          var d = JSON.parse(evt.data);
          if (d.type === 'uplink_rms') {
            window._serverAckRms = d.rms;
            clearTimeout(window._uplinkRmsTimeout);
            window._uplinkRmsTimeout = setTimeout(function() { window._serverAckRms = 0.0; }, 200);
          } else if (d.type === 'downlink_rms') {
            window._serverDownlinkRms = d.rms;
            clearTimeout(window._downlinkRmsTimeout);
            window._downlinkRmsTimeout = setTimeout(function() { window._serverDownlinkRms = 0.0; }, 200);
          }
        } catch(err) {
          console.error('[WASM_MONITOR] JSON parse error:', err);
        }
      };
      ws.onclose = function(evt) {
        if (evt && (evt.code === 1000 || evt.code === 1008 || evt.code === 4001 || evt.code === 4004)) {
          console.warn('[WASM_MONITOR] Session cleanly closed or rejected (code ' + evt.code + '). Halting auto-reconnect loop.');
          if (window._activeServerSessionId === sessionId) window._activeServerSessionId = null;
          return;
        }
        if (window._activeServerSessionId === sessionId) {
          setTimeout(function() { initMonitorWebSocket(sessionId); }, 2000);
        }
      };
      _monitorWs = ws;
      window._monitorWs = ws;
    } catch(e) {
      console.error('[WASM_MONITOR] WebSocket initialization error:', e);
    }
  }

  function initDebugWebSocket(sessionId) {
    if (!sessionId) return;
    try {
      var proto = (window.location.protocol === 'https:') ? 'wss://' : 'ws://';
      if (_debugWs) try { _debugWs.close(); } catch(e) {}
      var debugWs = new WebSocket(proto + window.location.host + '/ws_debug?session_id=' + encodeURIComponent(sessionId));
      debugWs.onopen = function() { console.log('[WASM_DEBUG] ✓ Connected to /ws_debug bound to session: ' + sessionId.slice(0, 8) + '...'); };
      debugWs.onmessage = function(evt) {
        try {
          var d = JSON.parse(evt.data);
          if (d.type === 'state' && window.store) {
            window.store.dbgPhase = d.phase;
            window.store.dbgBuffered = d.buffered_seconds.toFixed(2);
            window.store.dbgChunks = String(d.chunk_count);
            window.store.dbgTenet = d.tenet ? 'ON' : 'OFF';
          } else if (d.type === 'progress' && window.store) {
            window.store.dbgProgress = d.sent + '/' + d.total + ' (' + d.remaining_seconds.toFixed(1) + 's left)';
          } else if (d.type === 'log') {
            var logDiv = document.getElementById('debug-log');
            if (logDiv) {
              var line = document.createElement('div');
              line.textContent = d.msg;
              logDiv.appendChild(line);
              while (logDiv.children.length > 200) logDiv.removeChild(logDiv.firstChild);
              logDiv.scrollTop = logDiv.scrollHeight;
            }
          }
        } catch(parseErr) {}
      };
      debugWs.onclose = function(evt) {
        if (evt && (evt.code === 1000 || evt.code === 1008 || evt.code === 4001 || evt.code === 4004)) {
          console.warn('[WASM_DEBUG] Session cleanly closed or rejected (code ' + evt.code + '). Halting auto-reconnect loop.');
          if (window._activeServerSessionId === sessionId) window._activeServerSessionId = null;
          return;
        }
        if (window._activeServerSessionId === sessionId) {
          setTimeout(function() { initDebugWebSocket(sessionId); }, 2000);
        }
      };
      _debugWs = debugWs;
      window._debugWs = debugWs;
    } catch(connErr) {
      console.error('[DEBUG_WS] Connection error:', connErr);
    }
  }

  window._onServerSessionAssigned = function(sessionId) {
    console.log('[WASM_APP] 🤝 Server session assigned: ' + sessionId.slice(0, 8) + '... (binding debug widgets exclusively to this session)');
    window._activeServerSessionId = sessionId;
    initMonitorWebSocket(sessionId);
    initDebugWebSocket(sessionId);
  };
})();
