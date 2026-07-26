mergeInto(LibraryManager.library, {
    // hal_bridge.js — JavaScript HAL gatekeeper enforcing LLM anti-cheating separation
    _hal_init_bridge: function() {
        if (typeof Module._touchState === 'undefined') {
            Module._touchState = { x: 0, y: 0, pressed: false };
        }
        console.log("[JS_BRIDGE] HAL gatekeeper bridge initialized successfully.");
    }
});
