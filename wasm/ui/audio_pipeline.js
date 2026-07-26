(function() {
  var Module = window.Module || {};
  window.Module = Module;

  window._isScreenMicRecording = false;
  window._isAssistantLoopActive = false;
  window._assistantState = 'idle';
  window._mediaStream = null;
  window._recordedPcm = [];
  window._vadAudioCtx = null;
  window._vadPreampNode = null;
  window._playbackAnalyser = null;
  window._liveMicRms = 0.0;
  window._turnCount = 0;
  window._vadSpeechState = 0;     // 0: silent, 1: speech active (from Silero VAD)
  window._neuralSpeechActive = false; // Tracks active neural VAD classification
  window._recentRmsBuffer = [];       // Rolling 5-frame pre-roll RMS window (~300ms)

  window._onFirmwareStateChanged = function(stateStr) {
    window._assistantState = (stateStr === 'listening' || stateStr === 'speaking') ? stateStr : 'idle';
    console.log('[WASM_APP] UI State updated by authoritative firmware transition -> ' + stateStr + ' (mapped: ' + window._assistantState + ')');
  };

  Module.onAudioVoiceProcessing = function(enabled) {
    var isEnabled = (enabled === true || enabled === 1 || enabled === "1");
    console.log('[WASM_APP] onAudioVoiceProcessing gate set -> ' + (isEnabled ? 'ENABLED (1)' : 'DISABLED (0)'));
    if (isEnabled) {
      if (typeof window.ensureAudioContextActive === 'function') window.ensureAudioContextActive();
      if (!window._isAssistantLoopActive) {
        window._isAssistantLoopActive = true;
        window._turnCount = 0;
        window._isScreenMicRecording = true;
        window._recordedPcm = [];
        if (window._wasmProtocolWs && window._wasmProtocolWs.readyState === 1 && window.store) {
          window._wasmProtocolWs.send(JSON.stringify({
            type: 'config',
            tenet: window.store.tenetMode,
            max_duration: window.store.serverCap
          }));
        }
        console.log('[WASM_VAD] 🟢 Assistant audio loop ACTIVATED via authoritative C++ HAL state.');
      }
      window._isScreenMicRecording = true;
    } else {
      window._isScreenMicRecording = false;
    }
    if (window._sileroVad) {
      if (isEnabled) {
        window._vadSpeechState = 0;
        window._sileroVad.start();
        console.log('[WASM_VAD:LIFECYCLE] ▶️ Silero ONNX MicVAD active (Voice processing resumed).');
      } else {
        window._sileroVad.pause();
        window._vadSpeechState = 0;
        console.log('[WASM_VAD:LIFECYCLE] ⏸️ Silero ONNX MicVAD paused (Voice processing suspended).');
      }
    }
  };

  function initLiveMicrophone() {
    try {
      var actx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });
      if (actx.state === 'suspended') actx.resume();
      window._vadAudioCtx = actx;
    } catch(e) {
      console.log('[WASM_VAD] Web Audio AudioContext initialization error: ' + e);
    }

    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      console.log('[WASM_VAD] getUserMedia not available — waveform will remain flat at 0.0');
      return;
    }
    navigator.mediaDevices.getUserMedia({ audio: true }).then(function(stream) {
      window._mediaStream = stream;
      var actx = window._vadAudioCtx;
      if (!window._playbackAnalyser) {
        window._playbackAnalyser = actx.createAnalyser();
        window._playbackAnalyser.fftSize = 512;
        window._playbackAnalyser.smoothingTimeConstant = 0.4;
        window._playbackAnalyser.connect(actx.destination);
      }
      var micSrc = actx.createMediaStreamSource(stream);
      var preamp = actx.createGain();
      preamp.gain.value = (window.store && window.store.preampGain) ? window.store.preampGain : 1.0;
      window._vadPreampNode = preamp;

      var micProc = actx.createScriptProcessor(1024, 1, 1);
      var micMute = actx.createGain();
      micMute.gain.value = 0;
      micProc.onaudioprocess = function(evt) {
        var d = evt.inputBuffer.getChannelData(0);
        var sum = 0.0;
        for (var i = 0; i < d.length; i++) sum += d[i] * d[i];
        window._liveMicRms = Math.sqrt(sum / d.length);

        if (Module._audioProcessingEnabled && Module.ccall && Module._malloc && Module.HEAP16) {
          if (!window._pcmBridgePtr || window._pcmBridgeCap < d.length) {
            if (window._pcmBridgePtr) Module._free(window._pcmBridgePtr);
            window._pcmBridgeCap = d.length;
            window._pcmBridgePtr = Module._malloc(d.length * 2);
          }
          var offset = window._pcmBridgePtr >> 1;
          for (var k = 0; k < d.length; k++) {
            Module.HEAP16[offset + k] = Math.max(-32768, Math.min(32767, Math.floor(d[k] * 32767)));
          }
          Module.ccall('hal_feed_pcm_audio', null, ['number', 'number'], [window._pcmBridgePtr, d.length]);
        }
      };
      micSrc.connect(preamp);
      preamp.connect(micProc);
      micProc.connect(micMute);
      micMute.connect(actx.destination);

      if (window.vad && window.vad.MicVAD) {
        window.vad.MicVAD.new({
          stream: stream,
          positiveSpeechThreshold: (window.store ? window.store.vadPos : 0.50),
          negativeSpeechThreshold: (window.store ? window.store.vadNeg : 0.35),
          minSpeechFrames: (window.store ? window.store.vadMin : 3),
          redemptionFrames: (window.store ? window.store.vadRedemption : 8),
          onSpeechStart: function() {
            if (!window._isAssistantLoopActive || !Module._audioProcessingEnabled) return;
            window._neuralSpeechActive = true;
            var thresh = (window.store ? window.store.vadThreshold : 0.004);
            var maxRecentRms = (window._recentRmsBuffer && window._recentRmsBuffer.length) ? Math.max.apply(null, window._recentRmsBuffer) : window._liveMicRms;
            if (maxRecentRms < thresh) {
              console.log('[WASM_VAD:GATE] ⏳ Neural speech detected below acoustic floor (MaxRMS=' + maxRecentRms.toFixed(4) + ' < Thresh=' + thresh.toFixed(3) + '). Awaiting acoustic onset climb...');
              return;
            }
            if (window._vadSpeechState === 1) return;
            window._vadSpeechState = 1;
            console.log('[WASM_VAD:SILERO] 🗣️ Speech start detected (MaxRMS=' + maxRecentRms.toFixed(4) + ' >= Thresh=' + thresh.toFixed(3) + ')!');
            if (Module.ccall) Module.ccall('hal_on_vad_state_change', null, ['number'], [1]);
          },
          onSpeechEnd: function(audio) {
            window._neuralSpeechActive = false;
            if (!window._isAssistantLoopActive || !Module._audioProcessingEnabled) return;
            if (window._vadSpeechState === 1) {
              window._vadSpeechState = 0;
              console.log('[WASM_VAD:SILERO] 🤫 Speech end detected!');
              if (Module.ccall) Module.ccall('hal_on_vad_state_change', null, ['number'], [0]);
            } else {
              console.log('[WASM_VAD:GATE] 🚫 Neural speech segment ended without reaching volume floor. Suppressed.');
            }
          },
          onFrameProcessed: function(probabilities, frame) {
            window._lastOnFrameProcessedArgs = [typeof probabilities, typeof frame, probabilities ? Object.keys(probabilities) : null];
            var f = frame || (probabilities && probabilities.frame);
            if (f && f.length) {
              var sum = 0.0;
              for (var fIdx = 0; fIdx < f.length; fIdx++) sum += f[fIdx] * f[fIdx];
              window._liveMicRms = Math.sqrt(sum / f.length);
              window._debugLiveMicRms = window._liveMicRms;

              if (typeof window._recentRmsBuffer === 'undefined') window._recentRmsBuffer = [];
              window._recentRmsBuffer.push(window._liveMicRms);
              if (window._recentRmsBuffer.length > 5) window._recentRmsBuffer.shift();

              var thresh = (window.store ? window.store.vadThreshold : 0.004);
              if (window._neuralSpeechActive && window._vadSpeechState === 0 && window._isAssistantLoopActive && Module._audioProcessingEnabled) {
                var maxRecentRms = Math.max.apply(null, window._recentRmsBuffer);
                if (maxRecentRms >= thresh) {
                  window._vadSpeechState = 1;
                  console.log('[WASM_VAD:SILERO] 🗣️ Speech start confirmed via acoustic onset climb (MaxRMS=' + maxRecentRms.toFixed(4) + ' >= Thresh=' + thresh.toFixed(3) + ')!');
                  if (Module.ccall) Module.ccall('hal_on_vad_state_change', null, ['number'], [1]);
                }
              }
            }
          }
        }).then(function(myvad) {
          window._sileroVad = myvad;
          myvad.start();
          console.log('[WASM_VAD:SILERO] ✓ Silero ONNX MicVAD active (continuous PCM streaming -> WasmAudioProcessor)');
        }).catch(function(err) {
          console.log('[WASM_VAD:SILERO] MicVAD load error: ' + err + ' (Using script processing fallback)');
        });
      } else {
        console.log('[WASM_VAD:SILERO] @ricky0123/vad-web not loaded — test injection available');
      }

      console.log('[WASM_VAD] ✓ Live mic initialized (AudioContext=' + actx.state + ', PreAmp=' + preamp.gain.value.toFixed(1) + 'x+BPF)');
    }).catch(function(err) {
      console.log('[WASM_VAD] Mic permission denied (' + err + ') — waveform will remain flat at 0.0');
    });
  }

  var _nextPlaybackTime = 0;
  window._streamPlaybackChunk = function(arrayBuffer) {
    if (!window._vadAudioCtx) return;
    if (window._vadAudioCtx.state === 'suspended') window._vadAudioCtx.resume();

    var pcm16 = new Int16Array(arrayBuffer);
    var float32 = new Float32Array(pcm16.length);
    for (var i = 0; i < pcm16.length; i++) {
      float32[i] = pcm16[i] / 32768.0;
    }

    var audioBuf = window._vadAudioCtx.createBuffer(1, float32.length, 16000);
    audioBuf.getChannelData(0).set(float32);

    var srcNode = window._vadAudioCtx.createBufferSource();
    srcNode.buffer = audioBuf;
    if (window._playbackAnalyser) {
      srcNode.connect(window._playbackAnalyser);
    } else {
      srcNode.connect(window._vadAudioCtx.destination);
    }

    var now = window._vadAudioCtx.currentTime;
    var startAt = Math.max(now, _nextPlaybackTime);
    srcNode.start(startAt);
    _nextPlaybackTime = startAt + audioBuf.duration;
  };

  window.onProtocolRxJson = function(jsonStr) {
    try {
      var d = JSON.parse(jsonStr);
      if (d.type === 'tts' && d.state === 'start') {
        _nextPlaybackTime = 0;
        console.log('[WASM_APP] TTS start received — streaming playback pipeline ready.');
      } else if (d.type === 'tts' && (d.state === 'stop' || d.state === 'end')) {
        console.log('[WASM_APP] TTS stream ended cleanly.');
      }
    } catch(e) {}
  };

  window.ensureAudioContextActive = function() {
    if (!window._vadAudioCtx || !window._mediaStream) {
      initLiveMicrophone();
    } else if (window._vadAudioCtx.state === 'suspended') {
      window._vadAudioCtx.resume();
    }
  };

  window.returnToListening = function() {
    window._assistantState = 'listening';
    window._isScreenMicRecording = true;
    window._recordedPcm = [];
    if (Module.ccall) {
      Module.ccall('wasm_app_start_listening', null, [], []);
    }
    if (window.store) {
      window.store.protoStatus = 'WasmProtocol Status: 🟢 Listening (Turn #' + (window._turnCount + 1) + ')... speak to continue';
    }
    console.log('[WASM_VAD] 🟢 Auto-returned to LISTENING (AutoStop loop, next turn #' + (window._turnCount + 1) + ')');
  };

  window.stopAssistantLoop = function() {
    window._isAssistantLoopActive = false;
    window._isScreenMicRecording = false;
    window._recordedPcm = [];
    if (Module.ccall) {
      Module.ccall('wasm_app_toggle_chat', null, [], []);
    }
    if (window.store) {
      window.store.protoStatus = 'WasmProtocol Status: Assistant loop stopped. Tap canvas to restart.';
    }
    console.log('[WASM_VAD] ⏹ Assistant loop manually toggled off via canvas tap');
  };

  window.toggleScreenMicVad = function() {
    window.ensureAudioContextActive();
    if (!Module._isAssistantAppRunning) return;

    if (!window._isAssistantLoopActive) {
      window._isAssistantLoopActive = true;
      window._turnCount = 0;
      window._isScreenMicRecording = true;
      window._recordedPcm = [];
      if (Module.ccall) {
        Module.ccall('wasm_app_toggle_chat', null, [], []);
      }
      if (window._wasmProtocolWs && window._wasmProtocolWs.readyState === 1 && window.store) {
        window._wasmProtocolWs.send(JSON.stringify({type: 'config', tenet: window.store.tenetMode, max_duration: window.store.serverCap}));
      }
      console.log('[WASM_VAD] 🟢 Assistant loop ACTIVATED via wasm_app_toggle_chat');
      if (window.store) {
        window.store.protoStatus = 'WasmProtocol Status: 🟢 Listening (Turn #1)... speak to interact';
      }
    } else {
      window.stopAssistantLoop();
    }
  };
})();
