/**n * 侧滑菜单共享功能
 * 可在所有网页中使用
 *
 * 新增功能 (2025-12):
 * - 在側選單加入「登入/登出」按鈕
 * - 未登入 (localStorage 未設定) 時，會自動將頁面上所有互動元件 (button/input/select/textarea/a) 設為 disable / inert
 *   但標示了 data-login-exempt="true" 的元素（例如側選單上的登入按鈕）不會被禁用
 * - 登入狀態使用 localStorage 的 'wusoem_logged_in_v1' 鍵 (值 '1' 表示已登入)
 *
 * 注意：目前登入流程為示範 (prompt 輸入) — 若要接入真實認證，請替換 setLoggedIn 與相關 prompt/模態至後端驗證流程。
 */

(function () {
    'use strict';

    // We will navigate within the same window/tab when switching pages

    // 菜单项配置（模块级，供多处使用）
    const MENU_ITEMS = [
        { text: '主頁', url: '/index.html', window: 'wusoem_index' },
        { text: '示波器', url: '/RT_scope.html', window: 'wusoem_oscilloscope' },
        { text: '姿態顯示', url: '/viewer_6dof.html', window: 'wusoem_attitude' },
        { text: '機構設定', url: '/stewart_platform.html', window: 'wusoem_mechanism' },
        { text: '狀態訊息', url: '/status.html', window: 'wusoem_status' },
        { text: '自動畫面', url: '/auto.html', window: 'wusoem_auto' },
        { text: '說明', url: '/help.html', window: 'wusoem_help', exempt: true }
    ];

    /**
     * 检测是否为移动设备
     */
    function isMobileDevice() {
        return /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent)
            || (navigator.maxTouchPoints && navigator.maxTouchPoints > 2);
    }

    /**
     * 打开或聚焦窗口
     * 移动设备：在当前标签页中导航（保持session）
     * 桌面设备：打开新窗口或聚焦现有窗口
     */
    function openOrFocusWindow(url, windowName) {
        // Navigate within the same tab/window instead of opening new windows/tabs.
        // This keeps the user in a single browsing context and prevents popup
        // behavior caused by window.open.
        try {
            if (window.location.href !== url) {
                // Use assign so history behaves predictably
                window.location.assign(url);
            }
        } catch (e) {
            console.error('[openOrFocusWindow] Failed to navigate to', url, e);
            // As a last resort try direct assignment
            try { window.location.href = url; } catch (e2) { /* ignore */ }
        }
    }

    /**
     * 打开侧边栏
     */
    function openSidebar() {
        const sidebar = document.getElementById('sidebar');
        const overlay = document.getElementById('sidebarOverlay');
        if (sidebar) sidebar.classList.add('active');
        if (overlay) overlay.classList.add('active');
        document.body.style.overflow = 'hidden';
        // keep menu button state for accessibility
        const menuToggle = document.getElementById('menuToggle');
        if (menuToggle) menuToggle.setAttribute('aria-pressed', 'true');
    }

    /**
     * 关闭侧边栏
     */
    function closeSidebar() {
        const sidebar = document.getElementById('sidebar');
        const overlay = document.getElementById('sidebarOverlay');
        if (sidebar) sidebar.classList.remove('active');
        if (overlay) overlay.classList.remove('active');
        document.body.style.overflow = '';
        // reset menu button state for accessibility
        const menuToggle = document.getElementById('menuToggle');
        if (menuToggle) menuToggle.setAttribute('aria-pressed', 'false');
    }

    /**
     * Toggle sidebar open/close when hamburger is pressed
     */
    function toggleSidebar() {
        console.log('[SidebarMenu] toggleSidebar called');
        const sidebar = document.getElementById('sidebar');
        if (!sidebar) {
            console.error('[SidebarMenu] sidebar element not found!');
            return;
        }
        if (sidebar.classList.contains('active')) {
            console.log('[SidebarMenu] closing sidebar');
            closeSidebar();
        } else {
            console.log('[SidebarMenu] opening sidebar');
            openSidebar();
        }
    }

    /**
     * 初始化侧边栏控制
     */
    function initSidebarControls() {
        const menuToggle = document.getElementById('menuToggle');
        // close button removed by design; we only use overlay and ESC to close
        const overlay = document.getElementById('sidebarOverlay');
        const sidebar = document.getElementById('sidebar');

        console.log('[SidebarMenu] initSidebarControls: menuToggle=', menuToggle, 'overlay=', overlay, 'sidebar=', sidebar);

        if (menuToggle) {
            // Remove any existing listeners first to prevent duplicates
            menuToggle.onclick = null;
            menuToggle.addEventListener('click', function (e) {
                console.log('[SidebarMenu] menuToggle clicked!');
                e.preventDefault();
                e.stopPropagation();
                toggleSidebar();
            });
            // initialize aria-pressed to false for accessibility
            menuToggle.setAttribute('aria-pressed', 'false');
        } else {
            console.warn('[SidebarMenu] menuToggle not found!');
        }

        if (overlay) {
            overlay.addEventListener('click', closeSidebar);
        }

        const topSpacer = document.getElementById('sidebarTopSpacer');
        if (topSpacer) {
            topSpacer.addEventListener('click', closeSidebar);
        }

        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') {
                closeSidebar();
            }
        });

        // Robust fallback: if another script or markup prevents a direct listener on the
        // menu toggle from being attached, add a delegated click handler on the document
        // to ensure the hamburger always toggles the sidebar across pages.
        // We guard so we don't double-bind when init runs multiple times.
        if (!window.__wusoem_menu_delegation_installed) {
            window.__wusoem_menu_delegation_installed = true;
            document.body.addEventListener('click', (ev) => {
                const t = ev.target && ev.target.closest ? ev.target.closest('.menu-toggle') : null;
                if (t) {
                    console.log('[SidebarMenu] delegated click on .menu-toggle');
                    // Prevent other handlers from interfering
                    ev.preventDefault();
                    ev.stopPropagation();
                    try { toggleSidebar(); } catch (e) { /* best-effort */ }
                }
            }, { capture: true }); // Use capture phase to catch event early
        }
    }

    /**
     * 語言切換列（i18n）
     * - 語系清單來自 window.I18N（locales/index.json 註冊表），新增語系不需要改這裡
     * - 按鈕顯示各語系的 nativeName，因此整列標 data-i18n-skip 讓翻譯引擎跳過
     * - data-login-exempt 確保未登入時語言仍可切換
     */
    function appendLangSwitcher(container) {
        if (!window.I18N || typeof window.I18N.listLocales !== 'function') return;
        if (container.querySelector('.lang-switcher')) return;

        const build = () => {
            let codes = [];
            try { codes = window.I18N.listLocales() || []; } catch (e) { codes = []; }
            if (codes.length < 2) return;
            if (container.isConnected === false || container.querySelector('.lang-switcher')) return;

            const row = document.createElement('div');
            row.className = 'lang-switcher';
            row.setAttribute('data-i18n-skip', 'true');
            row.setAttribute('data-login-exempt', 'true');

            const label = document.createElement('span');
            label.className = 'lang-switcher-label';
            const renderLabel = () => {
                let text = '語系';
                try { text = window.I18N.t('語系'); } catch (e) { }
                label.textContent = text;
            };
            renderLabel();
            row.appendChild(label);

            const buttons = [];
            codes.forEach((code) => {
                let info = null;
                try { info = window.I18N.getLocaleInfo(code); } catch (e) { }
                const btn = document.createElement('button');
                btn.type = 'button';
                btn.className = 'lang-btn';
                btn.dataset.locale = code;
                btn.textContent = (info && info.nativeName) ? info.nativeName : code;
                btn.title = (info && info.name) ? info.name : code;
                btn.setAttribute('data-login-exempt', 'true');
                btn.addEventListener('click', (e) => {
                    e.preventDefault();
                    e.stopPropagation();
                    try {
                        const p = window.I18N.setLocale(code);
                        if (p && typeof p.then === 'function') p.then(refreshActive).catch(() => { });
                    } catch (err) { console.warn('[SidebarMenu] setLocale failed', code, err); }
                });
                buttons.push(btn);
                row.appendChild(btn);
            });

            const refreshActive = () => {
                let cur = null;
                try { cur = window.I18N.getLocale(); } catch (e) { }
                buttons.forEach((b) => b.setAttribute('aria-pressed', b.dataset.locale === cur ? 'true' : 'false'));
                renderLabel();
            };
            refreshActive();
            window.addEventListener('wusoem-i18n-change', refreshActive);

            container.appendChild(row);
        };

        // 語系註冊表是非同步載入的：等 I18N.ready() 後再建立按鈕，避免清單還是空列表
        try {
            const ready = window.I18N.ready && window.I18N.ready();
            if (ready && typeof ready.then === 'function') {
                ready.then(build).catch(() => { });
            } else {
                build();
            }
        } catch (e) { build(); }
    }

    /**
     * 创建侧边栏菜单项
     */
    function createSidebarMenu() {
        const sidebarButtons = document.getElementById('sidebarButtons');
        if (!sidebarButtons) return;

        // 清空现有内容
        sidebarButtons.innerHTML = '';

        // 菜单项配置
        const menuItems = MENU_ITEMS;

        // 创建菜单按钮
        menuItems.forEach(item => {
            const btn = document.createElement('a');
            btn.className = 'wusoem-sidebar-btn';
            btn.href = '#';
            btn.textContent = item.text;
            if (item.exempt) {
                btn.setAttribute('data-login-exempt', 'true');
            }
            btn.addEventListener('click', (e) => {
                e.preventDefault();
                openOrFocusWindow(item.url, item.window);
                closeSidebar();
            });
            sidebarButtons.appendChild(btn);
        });

        // Add login/logout button at the bottom of the menu (always present)
        const loginBtn = document.createElement('button');
        loginBtn.id = 'sidebarLoginButton';
        // mark as exempt so global disable doesn't affect it
        loginBtn.setAttribute('data-login-exempt', 'true');
        loginBtn.className = 'login-btn wusoem-sidebar-btn';
        // Default to '登入' until the server (via page) emits a 'wusoem-auth-change' event
        // which will update the label to '登出' when authenticated.
        loginBtn.textContent = '登入';
        loginBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            // prefer a shared auth UI (Auth.showPinModal) when available
            if (window.Auth && typeof window.Auth.showPinModal === 'function') {
                // if already logged in, call logout flow; else show modal
                if (isLoggedIn()) {
                    if (confirm('確定要登出嗎？')) {
                        // call shared logout when present
                        if (window.Auth && typeof window.Auth.logout === 'function') window.Auth.logout();
                        setLoggedIn(false);
                    }
                } else {
                    window.Auth.showPinModal();
                }
                return;
            }

            // fallback: old behaviour (prompt)
            if (isLoggedIn()) {
                if (confirm('確定要登出嗎？')) setLoggedIn(false);
            } else {
                const pw = prompt('請輸入密碼以登入（示範）');
                if (pw && pw.length > 0) setLoggedIn(true);
            }
        });

        // Separator before login
        const sepLogin = document.createElement('div');
        sepLogin.className = 'sidebar-sep';
        sidebarButtons.appendChild(sepLogin);

        sidebarButtons.appendChild(loginBtn);

        // 語言切換（i18n）：放在登入之后，使用 I18N 語系註冊表產生按鈕
        appendLangSwitcher(sidebarButtons);

        // If the page provides #authButtons (header action buttons), clone them into the
        // sidebar so the mobile menu and header match. Avoid duplicating navigation
        // items that are already present in MENU_ITEMS and skip header-only controls.
        try {
            const auth = document.getElementById('authButtons');
            if (auth && auth.children && auth.children.length > 0) {
                const skipIds = new Set(['btnConnectHeader', 'btnDisconnectHeader', 'btnStartAutoHeader', 'btnStopAutoHeader', 'btnLogin', 'btnLogout']);
                // build a set of existing nav text to avoid duplicates
                // （語言切換後 textContent 會是譯文，因此同時放入原文與譯文比對）
                const navTexts = new Set();
                menuItems.forEach(it => {
                    navTexts.add(it.text);
                    try { if (window.I18N) navTexts.add(window.I18N.t(it.text)); } catch (e) { /* i18n 未載入 */ }
                });

                // Append a separator for clarity
                const sep = document.createElement('div'); sep.className = 'sidebar-sep'; sidebarButtons.appendChild(sep);

                // Clone each button from auth (only those that are appropriate)
                const nodes = Array.from(auth.querySelectorAll('button, a'));
                nodes.forEach(btn => {
                    try {
                        if (btn.id && skipIds.has(btn.id)) return;
                        const txt = (btn.textContent || '').trim();
                        if (!txt) return;
                        // Avoid adding duplicate nav items
                        if (navTexts.has(txt)) return;

                        const clone = btn.cloneNode(true);
                        clone.classList.add('wusoem-sidebar-btn');
                        // Ensure login button remains enabled when cloned
                        if (btn.id === 'btnLogin') clone.disabled = false;

                        // When clicked, invoke the original element's action (to preserve handlers)
                        if (btn.id) {
                            clone.addEventListener('click', (e) => { e.preventDefault(); try { const orig = document.getElementById(btn.id); if (orig) { orig.click(); } } catch (_) { } });
                        }

                        // add to sidebar
                        sidebarButtons.appendChild(clone);
                    } catch (_) { /* best-effort cloning, ignore errors */ }
                });
            }
        } catch (_) { /* ignore */ }
    }

    // ------------------------- Login / UI lock helpers -------------------------

    const LOGIN_KEY = 'wusoem_logged_in_v1';
    const LEGACY_LOGIN_KEY = 'wusoem_logged_in';

    function isLoggedIn() {
        try {
            // Only trust the explicit runtime flag set by the page/server (window._loggedIn)
            // Do NOT default to localStorage on initial page load — the server is authoritative.
            if (typeof window._loggedIn !== 'undefined') return !!window._loggedIn;
            return false;
        } catch (e) { return false; }
    }

    function setLoggedIn(state) {
        try {
            localStorage.setItem(LOGIN_KEY, state ? '1' : '0');
            // maintain legacy key for compatibility with pages that still use it
            if (state) localStorage.setItem(LEGACY_LOGIN_KEY, '1'); else localStorage.removeItem(LEGACY_LOGIN_KEY);
        } catch (e) { /* ignore */ }

        // update login button label
        const loginBtn = document.getElementById('sidebarLoginButton');
        if (loginBtn) loginBtn.textContent = state ? '登出' : '登入';

        // apply UI enable/disable across the page
        applyLoginState();
    }

    /**
     * Apply UI disabled/enabled state across the page.
     * When not logged in, all interactive controls are disabled except elements
     * marked with data-login-exempt="true" (e.g. the login button itself).
     */
    function applyLoginState() {
        const loggedIn = isLoggedIn();

        // selectors to consider interactive elements
        const selectors = 'button, input, select, textarea, a[href], [role="button"]';
        const elems = Array.from(document.querySelectorAll(selectors));

        elems.forEach(el => {
            // Skip login-exempt elements
            if (el.getAttribute && el.getAttribute('data-login-exempt') === 'true') return;

            // Skip elements inside <dialog> or the login modal itself if present
            if (el.closest && el.closest('.login-modal')) return;

            // For inputs/selects/textarea/button use disabled attribute
            const tag = (el.tagName || '').toLowerCase();
            if (!loggedIn) {
                // disable
                if (tag === 'input' || tag === 'select' || tag === 'textarea' || tag === 'button') {
                    // store prior state
                    if (typeof el.disabled !== 'undefined') {
                        el.dataset.wusoemPrevDisabled = el.disabled ? '1' : '0';
                        el.disabled = true;
                    }
                } else if (tag === 'a' || el.getAttribute('role') === 'button') {
                    // anchor: save original href/tabindex then make inert
                    if (el.hasAttribute('href')) {
                        el.dataset.wusoemOrigHref = el.getAttribute('href');
                        el.setAttribute('href', 'javascript:void(0)');
                    }
                    if (el.hasAttribute('tabindex')) {
                        el.dataset.wusoemOrigTabindex = el.getAttribute('tabindex');
                    }
                    el.setAttribute('tabindex', '-1');
                }

                el.classList.add('wusoem-ui-disabled');
                el.setAttribute('aria-disabled', 'true');
            } else {
                // enable (restore previous state)
                if (tag === 'input' || tag === 'select' || tag === 'textarea' || tag === 'button') {
                    if (typeof el.disabled !== 'undefined') {
                        if (el.dataset.wusoemPrevDisabled) {
                            el.disabled = el.dataset.wusoemPrevDisabled === '1';
                        } else {
                            el.disabled = false;
                        }
                        delete el.dataset.wusoemPrevDisabled;
                    }
                } else if (tag === 'a' || el.getAttribute('role') === 'button') {
                    if (el.dataset.wusoemOrigHref) {
                        el.setAttribute('href', el.dataset.wusoemOrigHref);
                        delete el.dataset.wusoemOrigHref;
                    }
                    if (el.dataset.wusoemOrigTabindex) {
                        el.setAttribute('tabindex', el.dataset.wusoemOrigTabindex);
                        delete el.dataset.wusoemOrigTabindex;
                    } else {
                        el.removeAttribute('tabindex');
                    }
                }

                el.classList.remove('wusoem-ui-disabled');
                el.removeAttribute('aria-disabled');
            }
        });

        // update top-level menu toggle: if not logged in the menu toggle still works
        const menuToggle = document.getElementById('menuToggle');
        if (menuToggle) {
            // keep it enabled so user can access the login button
            menuToggle.disabled = false;
            menuToggle.classList.remove('wusoem-ui-disabled');
            menuToggle.removeAttribute('aria-disabled');
        }
    }

    // Listen for shared auth changes (e.g. Auth.logout()/Auth.showPinModal success)
    try {
        window.addEventListener('wusoem-auth-change', (e) => {
            try { applyLoginState(); } catch (_) { }
            // update sidebar login button label if present
            const lb = document.getElementById('sidebarLoginButton');
            if (lb) lb.textContent = (e && e.detail && e.detail.loggedIn) ? '登出' : '登入';
        });
    } catch (_) { }

    /**
     * 初始化侧边栏HTML结构（如果不存在）
     */
    function ensureSidebarHTML() {
        // 检查是否已存在侧边栏
        if (document.getElementById('sidebar')) return;

        // 创建侧边栏HTML
        const sidebarHTML = `
            <!-- Mobile/Desktop Sidebar Menu -->
            <div id="sidebarOverlay" class="sidebar-overlay"></div>
            <div id="sidebar" class="sidebar">
                <div class="sidebar-content">
                    <div id="sidebarTopSpacer" class="sidebar-top-spacer" title="點擊關閉選單"></div>
                    <div id="sidebarButtons" class="sidebar-buttons">
                        <!-- Buttons will be populated by JavaScript -->
                    </div>
                </div>
            </div>
        `;

        // 插入側欄與遮罩到 body 開頭
        document.body.insertAdjacentHTML('afterbegin', sidebarHTML);

        // menu toggle: always add to body top for consistent z-index stacking
        // This ensures the hamburger is not affected by header's stacking context
        if (!document.getElementById('menuToggle')) {
            const btnHtml = '<button id="menuToggle" class="menu-toggle" aria-label="開啟側邊欄"><span></span><span></span><span></span></button>';
            document.body.insertAdjacentHTML('afterbegin', btnHtml);
        }
    }

    /**
     * 页面加载完成后初始化
     */
    function init() {
        ensureSidebarHTML();
        createSidebarMenu();
        initSidebarControls();

        // 自动设置当前窗口的名称，以便其他窗口可以重用它
        const currentPath = window.location.pathname;

        // 查找当前页面对应的配置
        const currentItem = MENU_ITEMS.find(item => currentPath.endsWith(item.url) || (item.url === '/index.html' && (currentPath === '/' || currentPath === '')));

        if (currentItem) {
            // 如果当前窗口还没有名字，或者名字不匹配，则设置它
            if (window.name !== currentItem.window) {
                window.name = currentItem.window;
                console.debug('[SidebarMenu] Set window name to:', currentItem.window);
            }
        }

        // enforce saved login state across the page
        try { applyLoginState(); } catch (e) { /* safe: if helper not present nothing to do */ }
        console.log('[SidebarMenu] Initialized');
    }

    // 等待DOM加载完成
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

    // 导出到全局（以便其他脚本使用）
    window.SidebarMenu = {
        open: openSidebar,
        close: closeSidebar,
        openOrFocusWindow: openOrFocusWindow
    };

})();
