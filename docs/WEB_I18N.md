# Web UI 多語系（i18n）

`resources/web/` 的 7 個網頁（`index.html`、`RT_scope.html`、`viewer_6dof.html`、
`stewart_platform.html`、`status.html`、`auto.html`、`help.html`）共用一套**來源字串目錄式**
多語系機制：中文（`zh-Hant`）是來源語系，英文（`en`）是第一個翻譯語系，
新增語系只需要「一個 JSON ＋ 註冊表一行」，不用改 HTML/JS。

- 實作：`resources/web/js/i18n.js`（引擎）、`resources/web/locales/index.json`（語系註冊表）、
  `resources/web/locales/<code>.json`（目錄）、`resources/web/sidebar-menu.js`（側選單語言切換列）。
- 後端（`src/server/HttpServer.Static.*`）**沒有改動**：`locales/*.json` 只是 `resources/web/`
  下的靜態檔，`.json` 的 MIME 本來就支援；靜態檔在 auth 之前送出，因此登入畫面也能翻譯。

## 1. 運作方式

1. 每個頁面的 `<head>`（`<meta name="viewport">` 之後、其他 script 之前）載入
   `<script src="js/i18n.js">`。
2. 引擎立刻掛 `MutationObserver`，接著非同步載入 `locales/index.json` → 決定語系 → 載入
   `locales/<code>.json` → 編譯目錄 → 全頁掃描一次，之後由 observer 持續處理動態節點。
3. 翻譯對象：
   - text node（包含 JS `innerHTML`／`insertAdjacentHTML` 插入的節點）
   - `title` / `placeholder` / `aria-label` / `alt` 屬性
   - `document.title`、`<html lang>`、`<html data-locale>`
   - `alert()` / `confirm()` / `prompt()` 的訊息（包裝全域函式）
   - canvas 的 `fillText` / `strokeText`（包裝原型，覆蓋示波器／機構圖的圖内文字）
4. **key ＝ 中文原文**：目錄是 `{"中文原文": "Translation"}`。查表前先正規化空白
   （text node 常含換行＋縮排），所以 HTML 排版換行不影響比對。
5. **wildcard pattern**：key 含 `*` 視為樣板，用於 JS 串接／樣板字串，
   `*` 依序帶入英文值中的對應 `*`：

   | key（中文） | value（英文） | 執行期實際文字 | 結果 |
   |---|---|---|---|
   | `連接失敗 - *` | `Connect failed - *` | `連接失敗 - ECONNREFUSED` | `Connect failed - ECONNREFUSED` |
   | `腿*: 目標距離 * mm 超出可達範圍` | `Leg *: target distance * mm is outside the reachable range` | `腿3: 目標距離 250.00 mm 超出可達範圍` | `Leg 3: target distance 250.00 mm is outside the reachable range` |

   樣板中的空白在比對時視為「零個或多個空白」，比對範圍一律用 `\uXXXX` 轉義寫
   （見 §5 的 V8 陷阱）。**英文值不要用 `*`**（會被當成帶入欄位）。
6. 找不到 key → 顯示原文（不會破功能、不會空白）；缺譯可從
   `window.I18N.missingKeys()` 或覆蓋率報表取得。
7. 選擇來源語系（`zh-Hant`）時引擎是 **no-op**（目錄為空、完全不寫 DOM），
   所以中文模式的執行成本只剩每個節點一次正則測試。

## 2. 語系選擇順序

1. URL 參數 `?lang=<code>`（會寫回 localStorage，方便用選單切換／測試）。
2. `localStorage.wusoem_locale_v1`（使用者上次選的）。
3. 瀏覽器語系 `navigator.languages`：取**第一個能對應到內建語系**的偏好；
   若該偏好就是來源語系（`zh-TW`、`zh-CN` → `zh-Hant`）則維持原文，不會退到第二偏好。
4. 都不符合 → 來源語系 `zh-Hant`。

切換語系是**即時**的（不需要重新整理）：引擎會先把改過的節點還原成原文，
套新目錄後重掃；並發出 `wusoem-i18n-change` 事件（`detail = { locale, source, missing }`）
讓頁面自行重繪。切換後會持久化到 localStorage。

## 3. 語言切換 UI

`sidebar-menu.js` 在側選單登入鈕下方建立語言列（`.lang-switcher`）：

- 按鈕來自 `I18N.listLocales()`（＝`locales/index.json` 的顺序），**新增語系不需要改 JS**。
- 按鈕文字用各語系的 `nativeName`（繁體中文／English），整列標 `data-i18n-skip` 不翻譯
  （語言名稱要用自己的語言顯示）。
- 按鈕與整列都標 `data-login-exempt`，未登入（全站控制項鎖定）時仍能切語系。
- 樣式在 `sidebar-menu.css` 的「語言切換（i18n）」區段。

## 4. 新增語系（例：日本語）

1. 複製目錄檔：`resources/web/locales/ja.json`，內容：

   ```json
   {
     "code": "ja",
     "name": "Japanese",
     "nativeName": "日本語",
     "fallback": null,
     "messages": { "主頁": "ホーム", "示波器": "オシロスコープ" }
   }
   ```

2. 在 `locales/index.json` 的 `supported` 追加一行（顺序＝選單顺序）：

   ```json
   { "code": "ja", "name": "Japanese", "nativeName": "日本語", "file": "ja.json", "htmlLang": "ja" }
   ```

3. 完成。`fallback` 可指向其他語系 code（缺譯先退回該語系，再退回原文）。
   不需要動 HTML、JS 或 C++。

`zh-Hant` 的 `messages` 通常留空（原文即正確）；只有「不改程式碼但要改文案」時才寫條目。

## 5. 維護要點

- **目錄是人工維護的資產**：`locales/en.json` 一鍵一條、依「pattern 群 → exact 群」排序。
  新增 UI 文字後跑 `scripts/i18n_report.ps1` 看缺譯，補進目錄即可。
- 檢查覆蓋率（可接 CI，`-FailOnMissing` 缺譯即 exit 1）：

  ```powershell
  .\scripts\i18n_report.ps1 -Detailed -OutFile build\i18n_report.txt
  .\scripts\i18n_report.ps1 -FailOnMissing
  ```

  報表會列出缺譯原文與來源。報表比對時會忽略空白差異、解碼 HTML entity、
  把 `${...}` 樣板轉成 `*`，但跨行樣板／整塊 markup 仍可能列為缺譯（其文字節點其實已被
  個別 key 覆蓋）——以瀏覽器實測為準。
- **快取**：靜態檔帶 `Cache-Control: public, max-age=300`，改完目錄/引擎要 `Ctrl+F5`
  （或 `?lang=en&v=<timestamp>` 绕過快取）才拿到新版。
- 除錯模式：URL 加 `?i18ndbg=1` → 缺譯元素加虛線外框（`data-i18n-missing`）並在 console
  列前 30 條缺譯。每個語系載入完成會印 `[i18n] locale=en 目錄=… 樣板=… 缺譯=…`。
- 跳過範圍：`<script>`、`<style>`、`<textarea>`、`<pre>`，以及任何標了
  `data-i18n-skip` 的節點（含其子樹）。`<pre>` 是刻意跳過——`status.html` 的後端 log
  是一整塊 text node，不做逐字翻譯（見 §7）。
- 新程式碼可用 `window.I18N.t('中文原文')`（來源語系時直接回傳原文），也可用
  `data-i18n="中文原文"` 讓元素文字由目錄決定；API：`getLocale()`、`setLocale(code)`、
  `listLocales()`、`getLocaleInfo()`、`onChange(fn)`、`ready()`、`missingKeys()`、`refresh(root)`。
- 若之後實作「依瀏覽器語言挑選 Native GUI 語系」之類的原生改動，記得同步更新本檔與
  `.github/project-memory/repo-facts.md`。

### 版面要容得下較長譯文（CSS 慣例）

繁中標籤通常 2–6 個字，英文常是 8–18 個字（甚至更長），**任何以中文寬度為準的固定版面都會在
英文模式下爆開**。已踩過的實例：`auto.html` 三大按鈕在英文模式下被推到容器外、
`container` 出現橫向溢出（1280px 時 626 > 600；480px 時整頁會橫向滾動）。

根因不是「英文字太長」本身，而是 CSS 預設行為：`grid-template-columns: 1fr` 的 `1fr`
下限是 **min-content**，中文可按字元斷行 → min-content 只有一個字；英文以「單字」為單位
（`Immediate`、`messages`）→ min-content 比欄位寬，欄位被撐大、整列溢出容器。

寫樣式時遵守：

- **欄位一律 `minmax(0, 1fr)`**，不要寫裸 `1fr`（含 `repeat()` 內的 `1fr`）。
  `minmax(0, 1fr)` 才允許欄位收縮到內容以下，由文字換行吸收長度差異。
- 文字容器加 `min-width: 0; overflow-wrap: anywhere;`（flex/grid 子項預設 `min-width:auto`
  同樣會被內容撐大）。需要保留整字不斷行時改用 `break-word` 或調整字級。
- **別用 `white-space: nowrap` accommodate 固定寬度**：需要截字才用 `text-overflow: ellipsis`，
  而且只用在數值欄（如 `auto.html` 的 `td`）；表頭 `th` 改讓它換行，英文才不會被截掉。
- flex 列（如 `stewart_platform.html` 的 `.viz-header`）加 `flex-wrap: wrap` 並讓子項可縮。
- 字級用 `clamp()` 時要算「最小容器寬 × 最長單字」：手機 ≤600px 的按鈕若用 32px，
  `Immediate` 這類單字會斷在中間、按鈕高低不一，故 `auto.html` 手機樣式壓到
  `clamp(17px, 5.2vw, 26px)`（中文 4 字標籤仍單行，不影響原本的樣子）。
- 改完一定要**雙語並排量測**，不要只看中文：對同一頁跑 zh／en，比較每個含文字節點的
  `scrollWidth>clientWidth`（截字）與「右緣超出父容器」（擠出去）；只有「英文才有、中文沒有」
  的差異才是翻譯造成的版面問題（`stewart_platform.html` 兩個 checkbox label 的 6px 裁切在
  中文模式同樣存在，屬既有版面，不算 i18n 回歸）。


### 已知陷阱（寫引擎/腳本時必讀）

- **V8 字元類別重疊範圍**：`/[豈-﫿]/`（U+8C48–U+FAFF）這類起點落在 CJK 主區塊內的範圍，
  會把 surrogate 也判為符合，導致 emoji（`🏠` = D83C DFE0）被當成「含中文」。
  字元範圍一律寫 `\uXXXX` 轉義，且區段起點用區塊真正起點（`u4E00`、`uF900`）。
- **後端送過來的中文（下拉選單選項、狀態列）不會出現在靜態覆蓋率報告裡**：`i18n_report.ps1` 只掃
  `resources/web/` 的檔案，C++ 送來的字串（例：網卡選單的 `模擬測試 / 模擬6軸 EtherCAT 虛擬電機驅動`、
  `[後端] ${msg}`）只能靠執行期抓：開 `?i18ndbg=1`、讀 `I18N.missingKeys()`，或用 route stub 假造 API 回應
  去跑頁面（本輪就是這樣找到 3 條缺譯）。發現後把**原文精確字串**補進目錄即可，不需要改 C++。
- **目錄帶入的段落會自動再查一次目錄**（`fillSegment`：先 exact、再試一次 pattern，`filling` 旗標限制
  只遞迴一層）：所以只要補齊後端標籤，`已選擇網卡: 模擬測試 / …` 這種組合句就會整句變成英文，
  不會出現「英文骨架＋中文內容」的混合句。查不到就原樣帶入（裝置名、從站名本來就不該翻），
  也刻意不記進 `missingKeys()`。
- **括號／空白變體是不同的 key**：`(未選擇)` 不會被 `未選擇` 覆蓋、`6DoF姿態輸出 (正運動學)`（JS innerHTML
  的 text node）不會被 `6DoF姿態輸出 (正運動學) ✓`（靜態 HTML）覆蓋；補目錄時要照**執行期實際文字節點**補。
- **目錄裡不要留含 HTML tag 的 key**（執行期文字節點永不含 tag＝幽靈 key，只會把覆蓋率頂成達標）。
  `i18n_report.ps1` 會列出「幽靈 key」並自動用去 tag 的形状比對 JS 裡的 innerHTML 片段。
- **寫進 `<pre>` 或需立即成稿的動態訊息要自己呼叫 `I18N.t()`**：引擎跳過 `<pre>` 子樹，所以 JS 直接
  `log.textContent = ...` 賦值的內容不會被掃描器翻譯。`status.html` 的 `無法載入狀態: 404`、`錯誤: …`
  兩條 JS 訊息就是改成先過 `T()`（內部呼叫 `window.I18N.t`，i18n.js 未載入時退回原文）；pattern key 會
  吃掉整串組好的文字，所以呼叫時要傳**完整句子**（含動態值），不要只傳前綴。
- **自寫 mutation 迴圈**：引擎寫 `nodeValue`／`setAttribute` 會再觸發 observer，
  用 `pendingWrite`（預期值比對）跳過自己寫的記錄，否則會無限回圈。
- **純拉丁字元的顯示文字永遠不會被翻譯**：引擎只處理「含中日韓字元」的字串，所以寫死的 `oEtherCatOk`、`Busy`、`Ready` 這類英文標籤在中文模式也一樣是英文（本輪修正：`auto.html` 6 個燈號改為「EtherCAT 正常／忙碌／錯誤／就緒／原點完成／上線」，英文目錄給 `EtherCAT OK／Busy／Error／Ready／Home done／Online`，並去掉原本當欄位名用的 `o` 前綴；`help.html` 燈號欄同步）。**新 UI 文字一律先寫中文**，英文交給目錄。
- **不要用 DOM 文字作為邏輯判斷**：翻譯後 `textContent` 是譯文。既有碼已檢查過
  （`stewart_platform.html` 的 `riskLevel` 比較是 JS 內部值，不讀 DOM）；
  新增功能請用 `dataset`/`id`/值域，不要比對顯示文字。
  唯一已修的例外：`sidebar-menu.js` 去重側選單項目時同時比對原文與譯文。

## 6. 驗證方式（不碰硬體）

後端沒跑也能驗：以任意靜態伺服器指向 `resources/web`（本輪用 `build/static_server.js`
的臨時 node server），再用瀏覽器開 `index.html?lang=en`：

- console 出現 `[i18n] locale=en 目錄=… 樣板=…`（缺譯應為 0）
- 切換語言後中文可完整還原（revert）、`<html lang>` 跟著變
- 側選單語言列 native 名稱不被翻譯、未登入時仍可點

真實環境要跑 BigCat（`EnableLocalHttpServer=1`）才算完整（含登入、SSE、log 頁）。

## 7. 尚未涵蓋（刻意保留／後續可加）

- **後端送出的中文狀態／log 訊息**（`/status` 整塊、SSE 推的 log 行）：本輪依使用者決定
  不做；`<pre>` 已排除，要做的話需要為常見訊息加 pattern 目錄（含動態數值），
  或改由 C++ 送 key＋參數。（頁面 JS 自己產生的訊息——載入失敗、清除結果 toast——已改走 `I18N.t()`，只有後端推過來的 log 本體維持原文。）
- Win32 原生 GUI 仍是中文（本檔只管 Web UI）。
- 目前只有 `zh-Hant` + `en` 兩個目錄；`zh-Hans`、`ja` 等依 §4 擴充。
