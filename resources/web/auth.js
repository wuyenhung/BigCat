/*
 * Shared authentication helpers for web UI pages
 * - Provides a simple modal-based login (POST /login with X-API-Key)
 * - Exposes window.Auth with: showPinModal(), hidePinModal(), isLoggedIn(), logout()
 * - Dispatches custom event 'wusoem-auth-change' when login state changes (detail: { loggedIn: bool })
 */
(function () {
    'use strict';

    const LOGIN_KEY_V1 = 'wusoem_logged_in_v1';
    const LEGACY_LOGIN_KEY = 'wusoem_logged_in';
    const STARTUP_AUTOLOGIN_PIN = '1234';
    let authInitPromise = null;

    function emitChange(loggedIn) {
        const ev = new CustomEvent('wusoem-auth-change', { detail: { loggedIn } });
        window.dispatchEvent(ev);
    }

    function isLoggedIn() {
        try {
            // Prefer explicit runtime flag set by the page (/server-verified) if present
            // Do NOT assume logged-in purely from localStorage on page open.
            if (typeof window._loggedIn !== 'undefined') return !!window._loggedIn;
            // Unknown/unverified state -> treat as not logged in until the page or server confirms.
            return false;
        } catch (e) { return false; }
    }

    async function logout() {
        try {
            await fetch('/logout', { method: 'POST', credentials: 'same-origin' });
        } catch (_) { /* ignore network errors */ }
        try { window._loggedIn = false; } catch (_) { }
        try { localStorage.removeItem(LOGIN_KEY_V1); localStorage.removeItem(LEGACY_LOGIN_KEY); } catch (_) { }
        emitChange(false);
        // Reload page after logout to ensure clean state and remove any stale data
        setTimeout(() => {
            window.location.reload();
        }, 100);
    }

    // Create a minimal modal for password entry. Simpler than index.html keypad but functionally equivalent.
    function ensureModal() {
        let modal = document.getElementById('wusoemAuthModal');
        if (modal) return modal;

        const html = `
            <div id="wusoemAuthModal" class="wusoem-auth-modal" style="display:none">
              <div class="wusoem-auth-box">
                <div class="wusoem-auth-title">請輸入 API 密鑰 / 密碼</div>
                <div class="wusoem-auth-body">
                  <!-- Disable browser auto-fill by default; keep placeholder but do not prefill masked text -->
                  <input id="wusoemAuthInput" type="password" autocomplete="off" placeholder="API Key / Pin" />
                  <div id="wusoemAuthMsg" class="wusoem-auth-msg"></div>
                </div>
                <div class="wusoem-auth-actions">
                  <button id="wusoemAuthOk" class="wusoem-auth-ok">確定</button>
                  <button id="wusoemAuthCancel" class="wusoem-auth-cancel">取消</button>
                </div>
              </div>
            </div>`;

        document.body.insertAdjacentHTML('beforeend', html);
        modal = document.getElementById('wusoemAuthModal');

        const ok = document.getElementById('wusoemAuthOk');
        const cancel = document.getElementById('wusoemAuthCancel');
        const input = document.getElementById('wusoemAuthInput');
        if (ok) ok.addEventListener('click', () => { pinOk(); });
        if (cancel) cancel.addEventListener('click', hidePinModal);
        if (input) input.addEventListener('keydown', (e) => { if (e.key === 'Enter') pinOk(); });

        return modal;
    }

    function showPinModal(prefill = '') {
        const modal = ensureModal();
        const input = document.getElementById('wusoemAuthInput');
        const msg = document.getElementById('wusoemAuthMsg');
        if (input) {
            // Avoid pre-filling masked text. Only accept an explicit non-empty prefill
            // and avoid values consisting solely of common mask characters like '*' or '•'.
            const isMask = /^[*•]+$/.test(String(prefill || ''));
            if (typeof prefill === 'string' && prefill.trim().length > 0 && !isMask) {
                input.value = prefill;
            } else {
                // Ensure the field is empty and avoid browser autofill on focus
                input.value = '';
            }
            // defensive: clear any 'value' attribute left in DOM
            try { input.removeAttribute('value'); } catch (_) { }
            setTimeout(() => input.focus(), 10);
        }
        if (msg) { msg.textContent = ''; msg.className = 'wusoem-auth-msg'; }
        modal.style.display = 'flex';
    }

    function hidePinModal() {
        const modal = document.getElementById('wusoemAuthModal');
        if (modal) modal.style.display = 'none';
        // clear the input when hiding to avoid a masked value persisting in DOM
        try { const input = document.getElementById('wusoemAuthInput'); if (input) input.value = ''; } catch (_) { }
    }

    function consumeStartupAutoLoginPin() {
        try {
            const hash = window.location.hash || '';
            if (hash.length <= 1) return '';

            const params = new URLSearchParams(hash.substring(1));
            if (params.get('autologin') !== STARTUP_AUTOLOGIN_PIN) return '';

            params.delete('autologin');
            const cleanHash = params.toString();
            const cleanUrl = window.location.pathname + window.location.search + (cleanHash ? '#' + cleanHash : '');
            try { window.history.replaceState(null, document.title, cleanUrl); } catch (_) { }
            return STARTUP_AUTOLOGIN_PIN;
        } catch (_) {
            return '';
        }
    }

    function postLogin(pin) {
        return fetch('/login', { method: 'POST', headers: { 'X-API-Key': pin }, credentials: 'same-origin' });
    }

    async function pinOk() {
        const input = document.getElementById('wusoemAuthInput');
        const msg = document.getElementById('wusoemAuthMsg');
        if (!input) return;
        const pin = (input.value || '').trim();
        if (!pin) { if (msg) { msg.textContent = '請輸入密碼/API Key'; msg.className = 'wusoem-auth-msg error'; } return; }

        try {
            // Follow index.html behaviour: POST /login with X-API-Key
            const r = await postLogin(pin);
            const txt = await r.text();
            if (!r.ok) {
                // Provide clearer messages based on HTTP status for easier debugging
                if (r.status === 401) {
                    if (msg) { msg.textContent = '未授權: 密碼錯誤或無權限'; msg.className = 'wusoem-auth-msg error'; }
                    console.warn('Login failed 401', txt);
                    return;
                }
                if (r.status === 429) {
                    if (msg) { msg.textContent = '嘗試次數過多，請稍後再試'; msg.className = 'wusoem-auth-msg error'; }
                    console.warn('Login rate-limited 429', txt);
                    return;
                }
                if (txt === 'not_logged_in') {
                    if (msg) { msg.textContent = '未授權: 密碼錯誤'; msg.className = 'wusoem-auth-msg error'; }
                    return;
                }
                if (msg) { msg.textContent = '登入失敗: ' + (txt || ('HTTP ' + r.status)); msg.className = 'wusoem-auth-msg error'; }
                console.warn('Login failed', r.status, txt);
                return;
            }

            // success
            try { window._loggedIn = true; } catch (_) { }
            try { localStorage.setItem(LOGIN_KEY_V1, '1'); localStorage.removeItem(LEGACY_LOGIN_KEY); } catch (_) { }
            hidePinModal();
            emitChange(true);
            // Reload page after login to ensure fresh data and proper state initialization
            setTimeout(() => {
                window.location.reload();
            }, 100);
        } catch (e) {
            if (msg) { msg.textContent = '網路錯誤: ' + e.toString(); msg.className = 'wusoem-auth-msg error'; }
        }
    }

    // Expose global API
    window.Auth = {
        showPinModal,
        hidePinModal,
        isLoggedIn,
        logout
    };

    // Initialize auth check: verify session with server and handle force logout
    // Call this function when page loads to ensure proper auth state
    async function initAuthCheck(forceRefresh) {
        if (!forceRefresh && authInitPromise) {
            return authInitPromise;
        }

        authInitPromise = (async function runAuthCheck() {
            const previousLoggedIn = !!window._loggedIn;
            let resolvedLoggedIn = previousLoggedIn;
            let sessionCheckSucceeded = false;
            const startupAutoLoginPin = consumeStartupAutoLoginPin();

            try {
                // Query server session status
                try {
                    const resp = await fetch('/session_status', { method: 'GET', credentials: 'same-origin', cache: 'no-store' });
                    if (resp && resp.ok) {
                        try {
                            const js = await resp.json();
                            resolvedLoggedIn = !!(js && js.loggedIn);
                            sessionCheckSucceeded = true;
                        } catch (_) {
                            resolvedLoggedIn = previousLoggedIn;
                        }
                    } else {
                        // Keep the previous known auth state on transient server errors.
                        resolvedLoggedIn = previousLoggedIn;
                    }
                } catch (e) {
                    // Keep the previous known auth state on transient network errors.
                    resolvedLoggedIn = previousLoggedIn;
                }

                window._loggedIn = resolvedLoggedIn;
                if (sessionCheckSucceeded) {
                    if (window._loggedIn) {
                        try { localStorage.setItem(LOGIN_KEY_V1, '1'); } catch (_) { }
                    } else {
                        try { localStorage.removeItem(LOGIN_KEY_V1); localStorage.removeItem(LEGACY_LOGIN_KEY); } catch (_) { }
                    }
                }

                // Next, check if main app requested a force logout
                try {
                    const response = await fetch('/check_force_logout', { cache: 'no-store' });
                    if (response.ok) {
                        const data = await response.json();
                        if (data.forceLogout) {
                            // force logout requested by PC -> clear both client/local and server session
                            try { localStorage.removeItem(LOGIN_KEY_V1); localStorage.removeItem(LEGACY_LOGIN_KEY); } catch (_) { }
                            window._loggedIn = false;
                            await fetch('/logout', { method: 'POST', credentials: 'same-origin' }).catch(() => { });
                            resolvedLoggedIn = false;
                        }
                    }
                } catch (e) {
                    // ignore force-logout check errors
                }

                if (startupAutoLoginPin && !resolvedLoggedIn) {
                    try {
                        const loginResponse = await postLogin(startupAutoLoginPin);
                        if (loginResponse && loginResponse.ok) {
                            resolvedLoggedIn = true;
                            window._loggedIn = true;
                            try { localStorage.setItem(LOGIN_KEY_V1, '1'); localStorage.removeItem(LEGACY_LOGIN_KEY); } catch (_) { }
                        } else {
                            resolvedLoggedIn = false;
                            window._loggedIn = false;
                        }
                    } catch (_) {
                        resolvedLoggedIn = false;
                        window._loggedIn = false;
                    }
                }
            } catch (e) {
                // Any other errors leave window._loggedIn as is
            }

            // notify UI about auth state change
            try { window.dispatchEvent(new CustomEvent('wusoem-auth-change', { detail: { loggedIn: !!window._loggedIn } })); } catch (_) { }
            return !!window._loggedIn;
        })();

        return authInitPromise;
    }

    function createKeepalive(options) {
        const cfg = options || {};
        const intervalMs = Number(cfg.intervalMs) > 0 ? Number(cfg.intervalMs) : 30000;
        const url = typeof cfg.url === 'string' && cfg.url.length > 0 ? cfg.url : '/session_status';
        let timer = null;
        let disposed = false;

        async function tick() {
            try {
                if (!window._loggedIn) {
                    stop();
                    return;
                }
                const response = await fetch(url, { credentials: 'same-origin', cache: 'no-store' });
                if (!response || !response.ok) {
                    return;
                }

                let payload = null;
                try {
                    payload = await response.json();
                } catch (_) {
                    return;
                }

                if (payload && payload.loggedIn === false) {
                    try { localStorage.removeItem(LOGIN_KEY_V1); localStorage.removeItem(LEGACY_LOGIN_KEY); } catch (_) { }
                    window._loggedIn = false;
                    emitChange(false);
                    stop();
                    return;
                }

                if (payload && payload.loggedIn === true) {
                    window._loggedIn = true;
                    try { localStorage.setItem(LOGIN_KEY_V1, '1'); } catch (_) { }
                }
            } catch (_) { }
        }

        function start() {
            if (disposed || timer) return;
            timer = setInterval(() => {
                tick();
            }, intervalMs);
        }

        function stop() {
            if (!timer) return;
            clearInterval(timer);
            timer = null;
        }

        function handleAuthChange(e) {
            const loggedIn = !!(e && e.detail && e.detail.loggedIn);
            if (loggedIn) start(); else stop();
        }

        window.addEventListener('wusoem-auth-change', handleAuthChange);
        if (window._loggedIn) {
            start();
        }

        return {
            start,
            stop,
            dispose: function () {
                if (disposed) return;
                disposed = true;
                stop();
                try { window.removeEventListener('wusoem-auth-change', handleAuthChange); } catch (_) { }
            }
        };
    }

    function installBfcacheReload(options) {
        const cfg = options || {};
        const message = typeof cfg.message === 'string' && cfg.message.length > 0
            ? cfg.message
            : 'Page restored from bfcache, reloading for fresh data';

        if (window.__wusoemBfcacheReloadInstalled) {
            return;
        }
        window.__wusoemBfcacheReloadInstalled = true;

        window.addEventListener('pageshow', (event) => {
            if (event && event.persisted) {
                try { console.log(message); } catch (_) { }
                try { window.location.reload(); } catch (_) { }
            }
        });
    }

    // Expose initAuthCheck globally
    window.Auth.initAuthCheck = initAuthCheck;
    window.Auth.ready = function () { return initAuthCheck(); };
    window.Auth.createKeepalive = createKeepalive;
    window.Auth.installBfcacheReload = installBfcacheReload;
    window.initAuthCheck = initAuthCheck; // also expose at top level for backwards compatibility
    window.waitForAuthReady = window.Auth.ready;

    function autoInitAuthCheck() {
        try {
            initAuthCheck();
        } catch (_) { }
    }

    if (!window.__wusoemAuthAutoInitScheduled) {
        window.__wusoemAuthAutoInitScheduled = true;
        if (document.readyState === 'loading') {
            document.addEventListener('DOMContentLoaded', autoInitAuthCheck, { once: true });
        } else {
            setTimeout(autoInitAuthCheck, 0);
        }
    }

    // Backwards compat: some pages call top-level showPinModal / logout
    try {
        if (!window.showPinModal) window.showPinModal = window.Auth.showPinModal;
        if (!window.hidePinModal) window.hidePinModal = window.Auth.hidePinModal;
        if (!window.logout) window.logout = window.Auth.logout;
    } catch (_) { }

    // Add minimal styles (fallback if other CSS not present)
    try {
        const css = `
        .wusoem-auth-modal { position: fixed; inset: 0; display: none; align-items: center; justify-content: center; z-index: 200000; background: rgba(0,0,0,0.4); }
        .wusoem-auth-box { width: 360px; max-width: 92vw; background: #222; color: #fff; border-radius: 8px; padding: 14px; border: 1px solid #444; box-shadow: 0 10px 30px rgba(0,0,0,0.6); }
        .wusoem-auth-title { font-weight: bold; margin-bottom: 10px; }
        .wusoem-auth-body { display:flex; flex-direction:column; gap:8px; }
        /* Prevent iOS Safari auto-zoom when focusing: ensure font-size >= 16px */
        .wusoem-auth-body input { padding:8px 10px; border-radius:6px; border:1px solid #555; background: #111; color:#fff; font-size:16px; -webkit-text-size-adjust:100%; }
        .wusoem-auth-actions { display:flex; gap:8px; justify-content: flex-end; margin-top:10px; }
        .wusoem-auth-actions button { padding: var(--btn-padding); border-radius:6px; border:none; cursor:pointer; }
        .wusoem-auth-ok { background: linear-gradient(180deg,#3b8bff,#276bd6); color:#fff; }
        .wusoem-auth-cancel { background: #444; color:#fff; }
        .wusoem-auth-msg { color:#ddd; font-size:0.9rem; min-height:18px; }
        .wusoem-auth-msg.error { color:#f77; }
        `;
        const st = document.createElement('style'); st.textContent = css; document.head.appendChild(st);
    } catch (_) { }

    // --- Shared Connection/Status Indicator UI ---
    try {
        // Add a small CSS snippet for the connection status indicator
        const statusCss = `
        #conn-status, .status-indicator { position: fixed; top: 10px; right: 10px; width: 12px; height: 12px; border-radius: 50%; background-color: #555; z-index: 100; }
        #conn-status.connected, .status-indicator.connected { background-color: #00ff00; box-shadow: 0 0 8px #00ff00; }
        #conn-status.disconnected, .status-indicator.disconnected { background-color: #ff0000; box-shadow: none; }
        @keyframes wusoem-heartbeat {
            0%, 49.999% { opacity: 1; box-shadow: 0 0 8px #00ff00; }
            50%, 100% { opacity: 0; box-shadow: none; }
        }
        #conn-status.heartbeat, .status-indicator.heartbeat { animation: wusoem-heartbeat 1s linear infinite; background-color: #00ff00; box-shadow: 0 0 8px #00ff00; }
        #conn-status.disconnected.heartbeat, .status-indicator.disconnected.heartbeat { animation: none !important; opacity: 1 !important; background-color: #ff0000 !important; box-shadow: none !important; }
        `;
        const st2 = document.createElement('style'); st2.textContent = statusCss; document.head.appendChild(st2);
    } catch (_) { }

    function findOrCreateStatusIndicator() {
        try {
            // Prefer page-provided status element id/class first, then fallback to conn-status
            let el = document.getElementById('statusIndicator') || document.querySelector('.status-indicator') || document.getElementById('conn-status');
            if (el) return el;
            // If not present, create a new indicator (id=conn-status)
            el = document.createElement('div');
            el.id = 'conn-status';
            el.title = 'Connection Status';
            document.body.appendChild(el);
            return el;
        } catch (_) { return null; }
    }

    const StatusUI = {
        init: function () {
            this.el = findOrCreateStatusIndicator();
            return this.el;
        },
        setConnected: function () {
            try { const el = this.el || findOrCreateStatusIndicator(); if (el) { el.classList.add('connected'); el.classList.remove('disconnected'); } } catch (_) { }
        },
        setDisconnected: function () {
            try { const el = this.el || findOrCreateStatusIndicator(); if (el) { el.classList.add('disconnected'); el.classList.remove('connected'); el.classList.remove('heartbeat'); } } catch (_) { }
        },
        setHeartbeat: function (enable) {
            try { const el = this.el || findOrCreateStatusIndicator(); if (!el) return; if (enable) el.classList.add('heartbeat'); else el.classList.remove('heartbeat'); } catch (_) { }
        }
    };

    // Expose StatusUI globally
    try { window.StatusUI = StatusUI; } catch (_) { }

    // Initialize indicator now (will create if missing)
    try { StatusUI.init(); } catch (_) { }

    // If the page adds an element with id='statusIndicator' / class='status-indicator' later
    // (common when pages build the DOM after scripts load), observe mutations and prefer
    // the page-provided element. This prevents StatusUI from controlling a duplicated
    // 'conn-status' element and ensures RT_scope's indicator blinks correctly.
    try {
        const mo = new MutationObserver((records) => {
            try {
                const found = document.getElementById('statusIndicator') || document.querySelector('.status-indicator');
                if (!found) return;
                // If we find a page-provided element and it's different from current el, switch to it
                if (window.StatusUI && window.StatusUI.el !== found) {
                    try {
                        const old = window.StatusUI.el;
                        // copy relevant classes from old indicator to new one
                        if (old && old.classList) {
                            const classes = ['connected', 'disconnected', 'heartbeat'];
                            classes.forEach(c => { if (old.classList.contains(c)) found.classList.add(c); else found.classList.remove(c); });
                        }
                        // Prefer the page-provided element
                        window.StatusUI.el = found;
                        // If we had created a temporary conn-status that is not the same as found, remove it
                        try { const tmp = document.getElementById('conn-status'); if (tmp && tmp !== found) tmp.parentNode && tmp.parentNode.removeChild(tmp); } catch (_) { }
                    } catch (_) { }
                }
            } catch (_) { }
        });
        mo.observe(document.documentElement || document.body, { childList: true, subtree: true });
        // After a short delay, disconnect observer to avoid continuous overhead
        setTimeout(() => { try { mo.disconnect(); } catch (_) { } }, 5000);
    } catch (_) { }

    // Toggle heartbeat on login/logout
    try {
        window.addEventListener('wusoem-auth-change', (e) => {
            try {
                const loggedIn = !!(e && e.detail && e.detail.loggedIn);
                if (loggedIn) { try { window.StatusUI.setHeartbeat(true); } catch (_) { } }
                else { try { window.StatusUI.setHeartbeat(false); } catch (_) { } }
            } catch (_) { }
        });
    } catch (_) { }

    // Ensure heartbeat reflects current login state (if we missed event earlier)
    try { if (window._loggedIn) { try { StatusUI.setHeartbeat(true); } catch (_) { } } else { try { StatusUI.setHeartbeat(false); } catch (_) { } } } catch (_) { }

    // Global handler: Reload page when an authenticated session transitions to logged-out.
    // We only reload on a true -> false transition to avoid reloading during the
    // initial auth check (which dispatches wusoem-auth-change with loggedIn=false).
    try {
        if (typeof window._prevLoggedIn === 'undefined') window._prevLoggedIn = !!window._loggedIn;
        window.addEventListener('wusoem-auth-change', (e) => {
            try {
                const newVal = !!(e && e.detail && e.detail.loggedIn);
                const prevVal = !!window._prevLoggedIn;
                window._prevLoggedIn = newVal;
                // Only reload when we transition from logged-in -> logged-out
                if (prevVal && !newVal) {
                    setTimeout(() => {
                        try { window.location.reload(); } catch (_) { }
                    }, 100);
                }
            } catch (_) { }
        });
    } catch (_) { }

    // Wrap EventSource so any SSE error triggers an auth re-check.
    // This ensures pages detect server-side session expiry (SSE socket closed)
    // and will dispatch auth-change appropriately via initAuthCheck().
    try {
        if (window.EventSource) {
            const _OrigES = window.EventSource;
            const WrappedEventSource = function (url, config) {
                const es = new _OrigES(url, config);
                try {
                    es.addEventListener('error', (ev) => {
                        // Give the browser a moment to attempt reconnects before checking auth
                        setTimeout(() => {
                            try { if (typeof window.initAuthCheck === 'function') window.initAuthCheck(); } catch (_) { }
                        }, 250);
                    });
                } catch (_) { }
                return es;
            };
            // Preserve prototype and static constants
            WrappedEventSource.prototype = _OrigES.prototype;
            try { WrappedEventSource.CONNECTING = _OrigES.CONNECTING; } catch (_) { }
            try { WrappedEventSource.OPEN = _OrigES.OPEN; } catch (_) { }
            try { WrappedEventSource.CLOSED = _OrigES.CLOSED; } catch (_) { }
            window.EventSource = WrappedEventSource;
        }
    } catch (_) { }

})();
