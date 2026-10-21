/**
 * i18n.js — Web UI 多語系引擎（來源字串目錄式）
 *
 * 設計（與 docs/WEB_I18N.md 同步）：
 * - **以「中文原文」作為 key**：目錄檔 `locales/<code>.json` 的 `messages` 為
 *   `{"中文原文": "Translation", ...}`。因此 HTML/JS 不需標 key，共用本腳本即可翻譯全站。
 * - **DOM 掃描**：翻譯 text node 與 `title` / `placeholder` / `aria-label` / `alt` 屬性，
 *   搭配 `MutationObserver`，因此 JS 動態建立/更新與 `innerHTML` 插入的內容也會被翻譯。
 * - **wildcard pattern**：目錄 key 含 `*` 時視為樣板（用於 `'連接失敗 - ' + err`、
 *   `` `腿${i}: ...` `` 這類串接/模板字串），`*` 依序帶入英文值的對應欄位。
 * - **來源語系＝zh-Hant**：選中文時全部為 no-op（不碰 DOM）， untranslated 時退回原文，
 *   因此缺譯只是顯示中文，不會破功能。
 * - **可擴充**：新增語系只要放 `locales/<code>.json` 並在 `locales/index.json` 註冊一行。
 * - **跳過**：`<script>`、`<style>`、`<textarea>`、`<pre>`、以及標示 `data-i18n-skip` 的 subtree
 *   （`<pre>`＝狀態訊息頁的後端 log 整塊，不做逐字翻譯）。
 * - 同時提供 `window.I18N.t('中文')` 給新程式碼使用，並支援 `data-i18n="key"` 顯式翻譯。
 *
 * 載入方式：放在各頁面 `<head>`（在 auth.js / 其他 script 之前）。引擎會在 `<head>` 解析階段
 * 就掛上 MutationObserver，因此 body 逐步解析時即同步翻譯，避免閃動。
 */
(function () {
    'use strict';

    // 中日韓文字範圍（含日文假名，方便日後擴充 ja 語系）。
    // 一律用 \uXXXX 轉義寫範圍：直接放字面字元時，V8 對某些重疊範圍
    //（例：豈 U+8C48 – 﫿 U+FAFF）會把 surrogate 也判為符合，
    // 造成 emoji（如 🏠＝D83C DFE0）被誤認為「含中文」。
    var CJK_RE = /[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]/;
    var TEXT_ATTRS = ['title', 'placeholder', 'aria-label', 'alt'];
    var SKIP_TAGS = { SCRIPT: 1, STYLE: 1, TEXTAREA: 1, PRE: 1, CODE: 0 };
    var MAX_EXACT_LEN = 2000;   // 單個 text node 超過就不做 exact 比對（長 log 塊）
    var MAX_PATTERN_LEN = 400;  // pattern 比對的長度上限
    var LOCALE_KEY = 'wusoem_locale_v1';
    var urlLang = null;

    // ---------------- 狀態 ----------------
    var registry = null;        // { source, order: [code...], byCode: { code: meta } }
    var locale = null;          // 目前語系 code
    var catalog = null;         // Map: 正規化原文 -> 譯文（精確）
    var patterns = [];          // [{ key, re, value, wild }] 由含 * 的 key 編譯
    var patternsByFirst = null; // Map: 首個中文字元 -> [pattern]
    var patternsWildcardFirst = [];  // 以 * 開頭的樣板（無法用首字元索引）
    var trackedText = new Map();   // TextNode -> 原文（換語系時還原）
    var trackedAttrs = new Map();  // Element -> { attr: 原文 }
    var trackedTitle = null;       // document.title 原文
    var pendingWrite = new Map();  // TextNode -> 引擎自己寫入的值（避免自-trigger）
    var listeners = [];
    var debugMode = false;
    var missing = new Set();

    var baseDir = null;         // resources/web 的絕對 URL（含尾端 /）
    try {
        var selfScript = document.currentScript ||
            (function () {
                var list = document.getElementsByTagName('script');
                for (var i = list.length - 1; i >= 0; i--) {
                    if (/\/js\/i18n\.js/i.test(list[i].src)) return list[i];
                }
                return null;
            })();
        baseDir = new URL('../', selfScript.src).href;   // /js/i18n.js -> /
    } catch (e) {
        baseDir = new URL('/', window.location.href).href;
    }

    // ---------------- 小工具 ----------------
    function norm(s) { return String(s).replace(/\s+/g, ' ').trim(); }
    function needsTranslate(s) { return !!s && CJK_RE.test(s); }

    function getJson(url) {
        return fetch(url, { credentials: 'same-origin', cache: 'default' })
            .then(function (r) {
                if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + url);
                return r.json();
            });
    }

    // ---------------- 目錄編譯 ----------------
    function compileCatalog(messages) {
        catalog = new Map();
        patterns = [];
        patternsByFirst = new Map();
        patternsWildcardFirst = [];
        Object.keys(messages || {}).forEach(function (key) {
            var nk = norm(key);
            if (!nk) return;
            if (nk.indexOf('*') >= 0) {
                var p = compilePattern(nk, messages[key]);
                if (p) {
                    patterns.push(p);
                    if (p.anchor) {
                        if (!patternsByFirst.has(p.anchor)) patternsByFirst.set(p.anchor, []);
                        patternsByFirst.get(p.anchor).push(p);
                    } else {
                        patternsWildcardFirst.push(p);
                    }
                }
            } else {
                catalog.set(nk, String(messages[key]));
            }
        });
        // 特異性：文字部分愈長、萬用字元愈少者优先
        patterns.sort(function (a, b) {
            return (b.literalLen - a.literalLen) || (a.wild - b.wild);
        });
    }

    function compilePattern(key, value) {
        var parts = key.split('*');
        var re = parts.map(function (p) { return escapeLit(p); }).join('((?:.|\\s)*?)');
        var lit = 0;
        for (var i = 0; i < parts.length; i++) lit += parts[i].length;
        if (lit < 2) return null;
        // 正規化後的 key 已無多餘空白；首段第一字元即比對文字的第一字元（^ 錨定）
        var anchor = parts[0].length >= 1 ? parts[0].charAt(0) : null;
        return { key: key, re: new RegExp('^' + re + '$'), value: String(value), wild: parts.length - 1, literalLen: lit, anchor: anchor };
    }

    function escapeRe(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }
    // 樣板裡的空白視為「零個或多個空白」，讓 `A: *` 同時比對 "A: x" 與 "A:x"
    function escapeLit(s) { return escapeRe(s).replace(/ /g, '[ \\t]*'); }

    // ---------------- 翻譯查表 ----------------
    /** 翻譯一條字串；回傳 null 代表不須/無法翻譯。 */
    function translateString(raw) {
        if (!catalog || locale === (registry && registry.source)) return null;
        if (!raw) return null;
        // 目錄 key 以正規化空白儲存，因此查表前也要正規化
        //（text node 常含換行＋縮排，例如 "ESI\n      檔案讀取"）
        var trimmed = norm(raw);
        if (!trimmed || !CJK_RE.test(trimmed)) return null;      // 無中文＝已是譯文或純數值
        var hit = catalog.get(trimmed);
        if (hit != null) return hit;
        if (trimmed.length <= MAX_PATTERN_LEN && patterns.length) {
            var m = patternLookup(trimmed);
            if (m) return m;
        }
        if (trimmed.length <= MAX_EXACT_LEN) missing.add(trimmed);
        return null;
    }

    function patternLookup(text) {
        // 樣板以 ^ 起頭，因此比對文字的.first 必等於樣板首段的 .first
        var indexed = patternsByFirst.get(text.charAt(0));
        var cands = indexed ? indexed.concat(patternsWildcardFirst) : patternsWildcardFirst;
        for (var k = 0; k < cands.length; k++) {
            var p = cands[k];
            var mm = p.re.exec(text);
            if (!mm) continue;
            var gi = 1;
            return p.value.replace(/\*/g, function () {
                var g = mm[gi++];
                return fillSegment(g);
            });
        }
        return null;
    }

    /**
     * 樣板帶入的段落也要查目錄，否則會拼成「英文骨架＋中文內容」的混合句。
     * 例：`已選擇網卡: 模擬測試 / 模擬6軸…` 的網卡名（下拉選單選項）、
     *     `[後端] ${msg}` 的訊息本體（`[後端] 逆運動學 API 失敗: HTTP 404`）。
     * 順序：先 exact，再試一次 pattern；`filling` 旗標讓內層帶入只走 exact，避免無限遞迴。
     * 查不到就原樣帶入（裝置／從站名稱等資料文字本來就不該翻），
     * 且刻意不記進 missing：動態帶入的資料不是缺譯。
     */
    var filling = false;
    function fillSegment(g) {
        if (g == null) return '';
        if (!CJK_RE.test(g)) return g;
        var key = norm(g);
        var hit = catalog.get(key);
        if (hit != null) return hit;
        if (filling || !patterns.length || key.length > MAX_PATTERN_LEN) return g;
        filling = true;
        try {
            var p = patternLookup(key);
            if (p != null) return p;
        } finally {
            filling = false;
        }
        return g;
    }

    // ---------------- DOM 套用 ----------------
    function skipEl(el) {
        try { return !!(el && el.closest && el.closest('[data-i18n-skip]')); } catch (e) { return false; }
    }

    function applyTextNode(node) {
        if (!node || node.nodeType !== 3) return;
        var parent = node.parentElement;
        if (!parent || SKIP_TAGS[parent.tagName]) return;
        if (skipEl(parent)) return;
        var original = node.nodeValue;
        if (original == null) return;
        var tr = translateString(original);
        if (tr == null) return;
        // 保留原文前後空白，讓排版（按鈕間距等）不變
        var lead = /^\s*/.exec(original)[0];
        var trail = /\s*$/.exec(original)[0];
        var next = lead + tr + trail;
        if (next === original) return;
        if (!trackedText.has(node)) trackedText.set(node, original);
        pendingWrite.set(node, next);
        node.nodeValue = next;
    }

    function applyAttrs(el) {
        if (!el || el.nodeType !== 1) return;
        if (!catalog || locale === registry.source) return;
        if (SKIP_TAGS[el.tagName] || skipEl(el)) return;
        var store = trackedAttrs.get(el);
        for (var i = 0; i < TEXT_ATTRS.length; i++) {
            var a = TEXT_ATTRS[i];
            if (!el.hasAttribute(a)) continue;
            var cur = el.getAttribute(a);
            var tr = translateString(cur);
            if (tr == null) continue;
            if (tr === cur) continue;
            if (!store) { store = {}; trackedAttrs.set(el, store); }
            if (!(a in store)) store[a] = cur;
            el.setAttribute(a, tr);
        }
        // 顯式 key：data-i18n="key"（新程式碼可用；值亦可寫 "attr:標題" 之類）
        var key = el.getAttribute('data-i18n');
        if (key) {
            var tv = catalog.get(norm(key));
            if (tv != null && el.textContent !== tv) {
                if (!el.__i18nExplicitText) el.__i18nExplicitText = el.textContent;
                setTextOf(el, tv);
            }
        }
    }

    function setTextOf(el, v) {
        // 只換文字、保留子元素：簡單情境用 textContent；含 markup 的 key 由呼叫端自行處理
        try { pendingWrite.set(el.firstChild || el, String(v)); } catch (e) { }
        el.textContent = v;
    }

    function applyTitle() {
        if (!catalog || locale === registry.source) return;
        var cur = document.title;
        if (trackedTitle == null) trackedTitle = cur;
        var tr = translateString(cur);
        if (tr != null && tr !== cur) document.title = tr;
    }

    function walk(root) {
        if (!catalog) return;
        var doc = root && (root.ownerDocument || root);
        if (!doc) return;
        var tw = doc.createTreeWalker(root, 4 /* SHOW_TEXT */, {
            acceptNode: function (n) {
                var p = n.parentElement;
                if (!p || SKIP_TAGS[p.tagName] || skipEl(p)) return 2; /* SKIP_SUBTREE */
                var v = n.nodeValue;
                if (!v || v.length > MAX_EXACT_LEN || !needsTranslate(v)) return 2;
                return 1;
            }
        });
        var n;
        var batch = [];
        while ((n = tw.nextNode())) batch.push(n);
        for (var i = 0; i < batch.length; i++) applyTextNode(batch[i]);
        var els;
        try {
            els = (root.nodeType === 1 && (root.hasAttribute('title') || root.hasAttribute('data-i18n') ||
                root.hasAttribute('placeholder') || root.hasAttribute('aria-label') || root.hasAttribute('alt')))
                ? [root] : Array.prototype.slice.call(root.querySelectorAll('[' + TEXT_ATTRS.join('],[') + '],[data-i18n]'));
        } catch (e) { els = []; }
        for (var j = 0; j < els.length; j++) applyAttrs(els[j]);
        applyTitle();
    }

    function revertAll() {
        trackedText.forEach(function (orig, node) {
            try { if (node.isConnected !== false) node.nodeValue = orig; } catch (e) { }
        });
        trackedText.clear();
        trackedAttrs.forEach(function (store, el) {
            try {
                Object.keys(store).forEach(function (a) { el.setAttribute(a, store[a]); });
            } catch (e) { }
        });
        trackedAttrs.clear();
        if (trackedTitle != null) { try { document.title = trackedTitle; } catch (e) { } trackedTitle = null; }
        missing.clear();
    }

    // ---------------- 自動掃描（MutationObserver） ----------------
    var queueText = [];
    var queueEls = [];
    var queueSubs = [];
    var queued = false;

    function enqueueText(n) { if (queueText.indexOf(n) < 0) queueText.push(n); schedule(); }
    function enqueueEl(n) { if (queueEls.indexOf(n) < 0) queueEls.push(n); schedule(); }
    function queueSub(n) { if (queueSubs.indexOf(n) < 0) queueSubs.push(n); schedule(); }

    function schedule() {
        if (queued) return;
        queued = true;
        // 微行程批次處理，避免 SSE／表格高頻更新時逐個觸發
        Promise.resolve().then(flush);
    }

    function flush() {
        queued = false;
        var texts = queueText; queueText = [];
        var els = queueEls; queueEls = [];
        var subs = queueSubs; queueSubs = [];
        for (var i = 0; i < texts.length; i++) applyTextNode(texts[i]);
        for (var j = 0; j < els.length; j++) applyAttrs(els[j]);
        for (var s = 0; s < subs.length; s++) {
            var sub = subs[s];
            if (sub.isConnected === false) continue;
            applyAttrs(sub);
            walk(sub);
        }
        if (pendingWrite.size > 2000) pendingWrite.clear();
        if (trackedText.size > 3000 || trackedAttrs.size > 3000) pruneTracked();
        if (debugMode) markMissing();
    }

    // 頁面不斷重建 DOM 時，清掉已脫離文件的追蹤項，避免記憶體無限成長
    function pruneTracked() {
        trackedText.forEach(function (v, n) { if (n.isConnected === false) trackedText.delete(n); });
        trackedAttrs.forEach(function (v, e) { if (e.isConnected === false) trackedAttrs.delete(e); });
    }

    function onMutations(recs) {
        for (var i = 0; i < recs.length; i++) {
            var m = recs[i];
            if (m.type === 'characterData') {
                var t = m.target;
                if (pendingWrite.get(t) === t.nodeValue) { pendingWrite.delete(t); continue; }  // 自己寫的
                enqueueText(t);
            } else if (m.type === 'attributes') {
                enqueueEl(m.target);
            } else {
                var add = m.addedNodes;
                for (var k = 0; k < add.length; k++) {
                    var n = add[k];
                    if (n.nodeType === 3) enqueueText(n);
                    else if (n.nodeType === 1) {
                        // innerHTML/insertAdjacentHTML 只回報最上層新增節點，
                        // 因此整棵子樹交給 walk() 處理（文字 + 屬性 + data-i18n）
                        if (!catalog) continue;
                        queueSub(n);
                    }
                }
            }
        }
    }

    var observer = null;
    function startObserver() {
        if (observer || !document.documentElement) return;
        observer = new MutationObserver(onMutations);
        observer.observe(document.documentElement, {
            subtree: true, childList: true, characterData: true,
            attributes: true, attributeFilter: TEXT_ATTRS.concat(['data-i18n'])
        });
    }

    // ---------------- 除錯標記 ----------------
    function markMissing() {
        if (!document.documentElement.classList.contains('i18n-debug')) {
            document.documentElement.classList.add('i18n-debug');
            var st = document.createElement('style');
            st.textContent = '.i18n-debug [data-i18n-missing]{outline:1px dashed #e05f5f;outline-offset:1px}';
            document.head.appendChild(st);
        }
        trackedText.forEach(function (o, n) {
            if (n.parentElement && !translateString(o)) n.parentElement.removeAttribute('data-i18n-missing');
        });
        Array.prototype.forEach.call(document.body.querySelectorAll('p,span,td,th,label,button,h1,h2,h3'), function (el) {
            if (needsTranslate(el.textContent || '') && el.children.length === 0) el.setAttribute('data-i18n-missing', '1');
        });
    }

    // ---------------- 語系載入 ----------------
    function resolveLocale(req) {
        if (!registry) return null;
        if (!req) return null;
        var c = String(req).trim();
        if (registry.byCode[c]) return c;
        var lower = c.toLowerCase();
        var codes = Object.keys(registry.byCode);
        for (var i = 0; i < codes.length; i++) if (codes[i].toLowerCase() === lower) return codes[i];
        var base = lower.split('-')[0];
        for (var j = 0; j < codes.length; j++) {
            var cl = codes[j].toLowerCase();
            if (cl === base || cl.split('-')[0] === base) return codes[j];
        }
        return null;
    }

    // 依瀏覽器語系決定預設：取「第一個能對應到內建語系」的偏好。
    // 若該偏好就是來源語系（例如 zh-TW / zh-CN），維持原文、不往後找英文；
    // 否則中文瀏覽器會被塞進第二种偏好（en）的語系。
    function browserLocale() {
        var list = [];
        try { list = (navigator.languages && navigator.languages.length) ? navigator.languages : [navigator.language]; } catch (e) { }
        for (var i = 0; i < list.length; i++) {
            var code = resolveLocale(list[i]);
            if (code) return code === registry.source ? null : code;
        }
        return null;
    }

    function desiredLocale() {
        if (urlLang) return urlLang;
        var saved = null;
        try { saved = localStorage.getItem(LOCALE_KEY); } catch (e) { }
        if (saved) { var c = resolveLocale(saved); if (c) return c; }
        var bl = browserLocale();
        if (bl) return bl;
        return registry.source;
    }

    function loadMessages(meta) {
        if (!meta || !meta.file) return Promise.resolve({});
        return getJson(new URL(meta.file, baseDir + 'locales/').href)
            .then(function (data) { return data && data.messages ? data.messages : {}; })
            .catch(function (err) {
                console.warn('[i18n] 語系檔載入失敗', meta.file, err);
                return {};
            });
    }

    function loadCatalogFor(code) {
        var meta = registry.byCode[code];
        if (!meta) return Promise.resolve({});
        var chain = [];
        var seen = {};
        var cur = meta;
        while (cur && !seen[cur.code]) { seen[cur.code] = 1; chain.push(cur); cur = cur.fallback ? registry.byCode[cur.fallback] : null; }
        // 鏈尾＝來源語系（來源語系本身不需要目錄）
        return Promise.all(chain.map(loadMessages)).then(function (list) {
            var merged = {};
            for (var i = list.length - 1; i >= 0; i--) {   // fallback 先套，自己的覆寫在後
                var src = list[i];
                Object.keys(src).forEach(function (k) { merged[k] = src[k]; });
            }
            return merged;
        });
    }

    function setLocale(code, opts) {
        opts = opts || {};
        var target = resolveLocale(code) || registry.source;
        return loadCatalogFor(target).then(function (messages) {
            revertAll();
            compileCatalog(target === registry.source ? {} : messages);
            locale = target;
            try { if (opts.persist !== false) localStorage.setItem(LOCALE_KEY, target); } catch (e) { }
            var meta = registry.byCode[target] || {};
            try {
                document.documentElement.setAttribute('lang', meta.htmlLang || target);
                document.documentElement.setAttribute('data-locale', target);
            } catch (e) { }
            walk(document.documentElement);
            try { observers_notify(); } catch (e) { }
            console.info('[i18n] locale=' + target + ' 目錄=' + catalog.size + ' 樣板=' + patterns.length +
                (missing.size ? ' 缺譯=' + missing.size : ''));
            if (debugMode && missing.size) console.debug('[i18n] 缺譯样本：', Array.from(missing).slice(0, 30));
            return target;
        });
    }

    function observers_notify() {
        var info = { locale: locale, source: registry.source, missing: missing };
        listeners.forEach(function (fn) { try { fn(info); } catch (e) { } });
        try {
            window.dispatchEvent(new CustomEvent('wusoem-i18n-change', { detail: info }));
        } catch (e) { }
    }

    function init() {
        try { debugMode = /[?&]i18ndbg\b/.test(window.location.search); } catch (e) { }
        startObserver();          // 先掛觀察器，之後解析到的節點即同步翻譯
        return getJson(new URL('index.json', baseDir + 'locales/').href)
            .then(function (data) {
                registry = { source: data.source || 'zh-Hant', byCode: {}, order: [] };
                (data.supported || []).forEach(function (m) {
                    registry.byCode[m.code] = m;
                    registry.order.push(m.code);
                });
                if (!registry.byCode[registry.source]) {
                    registry.byCode[registry.source] = { code: registry.source, name: registry.source, nativeName: registry.source };
                    registry.order.unshift(registry.source);
                }
                try {
                    var q = new URLSearchParams(window.location.search);
                    var l = q.get('lang');
                    if (l) { urlLang = resolveLocale(l); if (urlLang) q.delete('lang'); }
                } catch (e) { }
                return setLocale(desiredLocale(), { persist: !!urlLang });
            })
            .catch(function (err) {
                console.warn('[i18n] 語系註冊表載入失敗，維持原文', err);
                registry = registry || { source: 'zh-Hant', byCode: { 'zh-Hant': { code: 'zh-Hant' } }, order: ['zh-Hant'] };
                locale = registry.source;
            });
    }

    // ---------------- 對外 API ----------------
    var I18N = {
        /** 翻譯（來源語系時直接回傳原文） */
        t: function (s) {
            if (!catalog || locale === registry.source) return s;
            var tr = translateString(s);
            return tr == null ? s : tr;
        },
        getLocale: function () { return locale || (registry && registry.source); },
        getSourceLocale: function () { return registry ? registry.source : 'zh-Hant'; },
        listLocales: function () { return registry ? registry.order.slice() : []; },
        getLocaleInfo: function (code) { return registry ? (registry.byCode[code || locale] || null) : null; },
        setLocale: setLocale,
        onChange: function (fn) { if (typeof fn === 'function') listeners.push(fn); },
        ready: function () { return readyPromise; },
        missingKeys: function () { return Array.from(missing); },
        /** 手動重掃（頁面用了不觸發 MutationObserver 的寫法時可呼叫） */
        refresh: function (root) { walk(root || document.documentElement); }
    };

    var readyPromise = init();
    window.I18N = I18N;

    // 原生彈窗（alert/confirm/prompt）與 canvas 文字也走同一目錄
    readyPromise.then(function () {
        ['alert', 'confirm', 'prompt'].forEach(function (name) {
            try {
                var orig = window[name];
                if (typeof orig !== 'function' || orig.__i18nWrapped) return;
                var wrapped = function (msg) {
                    return orig.call(window, typeof msg === 'string' ? I18N.t(msg) : msg);
                };
                wrapped.__i18nWrapped = true;
                window[name] = wrapped;
            } catch (e) { }
        });
        try {
            var proto = window.CanvasRenderingContext2D && CanvasRenderingContext2D.prototype;
            ['fillText', 'strokeText'].forEach(function (fn) {
                if (!proto || typeof proto[fn] !== 'function' || proto[fn].__i18nWrapped) return;
                var orig = proto[fn];
                var wrapped = function (text) {
                    var args = Array.prototype.slice.call(arguments);
                    args[0] = I18N.t(text);
                    return orig.apply(this, args);
                };
                wrapped.__i18nWrapped = true;
                proto[fn] = wrapped;
            });
        } catch (e) { }
    });
})();
