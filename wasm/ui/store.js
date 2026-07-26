(function() {
  var initialMac = localStorage.getItem("app_config.mac_address") || "DA:E5:31:89:AB:CD";
  var initialUuid = localStorage.getItem("app_config.uuid") || ((typeof crypto !== 'undefined' && crypto.randomUUID) ? crypto.randomUUID() : "550e8400-e29b-41d4-a716-446655440000");
  var initialToken = localStorage.getItem("app_config.token") || "dev_token_8081";

  if (!localStorage.getItem("app_config.mac_address")) localStorage.setItem("app_config.mac_address", initialMac);
  if (!localStorage.getItem("app_config.uuid")) localStorage.setItem("app_config.uuid", initialUuid);
  if (!localStorage.getItem("app_config.token")) localStorage.setItem("app_config.token", initialToken);

  var store = PetiteVue.reactive({
    // Protocol / BLE Status text bindings
    protoStatus: "WasmProtocol Status: Disconnected (Tap screen canvas to start assistant)",
    bleStatus: "BLE GATT Server Ready | Service: e2e5e5e0-1234-5678-1234-56789abcdef0",
    wifiSsid: "StackChan_World_AP",
    wifiPass: "stackchan2026",
    
    // Auth Credentials bindings
    deviceMac: initialMac,
    clientUuid: initialUuid,
    authToken: initialToken,
    
    // Oscilloscope & Server controls
    vadThreshold: 0.004,
    chartYMax: 0.15,
    preampGain: 1.0,
    tenetMode: false,
    serverCap: 45,
    
    // Silero VAD Hyperparameters
    vadPos: 0.50,
    vadNeg: 0.35,
    vadMin: 3,
    vadRedemption: 8,
    
    // Server Debug Panel State
    dbgPhase: "—",
    dbgBuffered: "0.00",
    dbgChunks: "0",
    dbgTenet: "OFF",
    dbgProgress: "—",
    
    get serverCapText() {
      return this.serverCap === 0 ? "∞ (Off)" : this.serverCap + "s";
    },

    // Side-effect actions on slider or input changes
    onAuthTokenChange: function() {
      var val = (this.authToken || "").trim();
      localStorage.setItem("app_config.token", val);
      console.log('[WASM_AUTH] Updated active token in localStorage -> ' + val);
    },
    onPreampInput: function() {
      if (window._vadPreampNode) {
        window._vadPreampNode.gain.value = this.preampGain;
      }
    },
    onServerConfigChange: function() {
      console.log('[WASM_CFG] Server recording cap updated -> ' + this.serverCapText + ', tenet -> ' + this.tenetMode);
      if (window._sendServerConfig) {
        window._sendServerConfig();
      }
    },
    onVadHyperParamsInput: function() {
      if (window._sileroVad && window._sileroVad.options) {
        window._sileroVad.options.positiveSpeechThreshold = this.vadPos;
        window._sileroVad.options.negativeSpeechThreshold = this.vadNeg;
        window._sileroVad.options.minSpeechFrames = this.vadMin;
        window._sileroVad.options.redemptionFrames = this.vadRedemption;
      }
    }
  });

  window.store = store;

  window.addEventListener('DOMContentLoaded', function() {
    if (typeof PetiteVue !== 'undefined') {
      PetiteVue.createApp({ store: store }).mount('.container');
      console.log('🟢 [UI_STORE] ✓ Petite-Vue reactive store initialized and mounted on .container');
    } else {
      console.error('[UI_STORE:ERR] PetiteVue object not found in window scope!');
    }
  });
})();
