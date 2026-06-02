// dashboard.js - 数据采集管理平台前端逻辑
// 功能：
// 1. 数据统计概览（今日/本月/本季/全年统计卡片）
// 2. 图表可视化（年度占比仪表盘、数据类型分布、采集趋势柱状图、使用流程漏斗图）
// 3. 数据浏览（采集数据/转换数据列表，支持年/月/日筛选和搜索）
// 4. 数据详情弹窗（视频预览、IMU 数据表、文件列表）
// 5. 实时轮询更新数据

// ===== 数据看板应用入口 =====
(function() {
    'use strict';

    // ---- 侧边栏收缩/展开（localStorage 跨页面联动） ----
    const SIDEBAR_KEY = 'sidebar_collapsed';
    const sidebar = document.querySelector('.sidebar');
    const sidebarBtn = document.getElementById('sidebarToggleBtn');
    function syncDashboardSidebar(collapsed) {
        if (sidebar) {
            if (collapsed) sidebar.classList.add('collapsed');
            else sidebar.classList.remove('collapsed');
        }
        if (collapsed) document.body.classList.add('sidebar-collapsed');
        else document.body.classList.remove('sidebar-collapsed');
        if (sidebarBtn) sidebarBtn.textContent = collapsed ? '›' : '‹';
    }
    if (sidebarBtn) {
        syncDashboardSidebar(localStorage.getItem(SIDEBAR_KEY) === '1');
        sidebarBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            const nowCollapsed = !document.body.classList.contains('sidebar-collapsed');
            localStorage.setItem(SIDEBAR_KEY, nowCollapsed ? '1' : '0');
            syncDashboardSidebar(nowCollapsed);
        });
        window.addEventListener('storage', function(e) {
            if (e.key === SIDEBAR_KEY) syncDashboardSidebar(e.newValue === '1');
        });
    }

    // API 基础路径（同源部署，留空）
    const API = '';

    // ===== 目录浏览器（看板独立实现） =====
    let _dirBrowserCb = null;
    let _dirBrowserPath = '/';
    function openDashboardDirBrowser(dirType) {
        if (!document.getElementById('db-dir-overlay')) {
            const overlay = document.createElement('div');
            overlay.id = 'db-dir-overlay';
            overlay.style.cssText = 'position:fixed;inset:0;z-index:300;background:rgba(0,0,0,0.4);display:flex;align-items:center;justify-content:center;';
            overlay.innerHTML = `<div style="background:#fff;border-radius:8px;width:480px;max-height:70vh;display:flex;flex-direction:column;box-shadow:0 8px 30px rgba(0,0,0,0.2);">
                <div style="padding:12px 16px;display:flex;justify-content:space-between;align-items:center;font-weight:600;border-bottom:1px solid #e2e8f0;">
                    <span>选择目录</span><button id="db-dir-close" style="background:none;border:none;font-size:20px;cursor:pointer;color:#8891a5;">&times;</button>
                </div>
                <div id="db-dir-path" style="padding:8px 16px;font-size:11px;color:#3b82f6;font-family:monospace;background:#f0f2f5;border-bottom:1px solid #e2e8f0;word-break:break-all;">/</div>
                <div id="db-dir-list" style="flex:1;overflow-y:auto;padding:4px 0;max-height:40vh;"></div>
                <div style="padding:10px 16px;display:flex;justify-content:flex-end;gap:8px;border-top:1px solid #e2e8f0;">
                    <button id="db-dir-cancel" style="padding:6px 16px;border:1px solid #e2e8f0;border-radius:6px;background:#fff;cursor:pointer;font-size:13px;">取消</button>
                    <button id="db-dir-ok" style="padding:6px 16px;border:none;border-radius:6px;background:#3b82f6;color:#fff;cursor:pointer;font-size:13px;">确认选择</button>
                </div>
            </div>`;
            document.body.appendChild(overlay);
            document.getElementById('db-dir-close').onclick = closeDashboardDirBrowser;
            document.getElementById('db-dir-cancel').onclick = closeDashboardDirBrowser;
            document.getElementById('db-dir-ok').onclick = () => {
                if (_dirBrowserCb) _dirBrowserCb(_dirBrowserPath, overlay.getAttribute('data-dir-type'));
                closeDashboardDirBrowser();
            };
            overlay.onclick = (e) => { if (e.target === overlay) closeDashboardDirBrowser(); };
        }
        const overlay = document.getElementById('db-dir-overlay');
        overlay.style.display = 'flex';
        overlay.setAttribute('data-dir-type', dirType);
        _dirBrowserCb = (path, type) => {
            const key = type === 'converted' ? 'converted' : 'collect';
            fetch(`${API}/api/paths`, {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ [key]: path })
            }).then(r => r.json()).then(d => {
                const pathEl = document.getElementById(`dashboard-path-${type}`);
                if (pathEl) pathEl.textContent = key === 'converted' ? (d.converted || '--') : (d.collect || '--');
                // 更新统计页上的当前数据目录显示。
                const statEl = document.getElementById(`dashboard-stat-path-${type}`);
                if (statEl) statEl.textContent = key === 'converted' ? (d.converted || '--') : (d.collect || '--');
                loadAllData();
            });
        };
        // 默认从当前已配置的数据目录开始浏览。
        fetch(`${API}/api/paths`).then(r => r.json()).then(d => {
            _dirBrowserPath = dirType === 'converted' ? (d.converted || '/') : (d.collect || '/');
            dbLoadDirList(_dirBrowserPath);
        });
    }
    function closeDashboardDirBrowser() {
        const el = document.getElementById('db-dir-overlay');
        if (el) el.style.display = 'none';
        _dirBrowserCb = null;
    }
    function dbLoadDirList(path) {
        _dirBrowserPath = path;
        document.getElementById('db-dir-path').textContent = path;
        const listEl = document.getElementById('db-dir-list');
        listEl.innerHTML = '<div style="padding:16px;color:#bcc3d0">加载中...</div>';
        fetch(`${API}/api/browse-dir?path=${encodeURIComponent(path)}`).then(r => r.json()).then(d => {
            let html = '';
            if (d.parent && d.parent !== d.path) {
                html += `<div style="padding:8px 16px;cursor:pointer;font-size:13px;color:#8891a5;font-style:italic;" data-path="${d.parent}">&#128281; ..</div>`;
            }
            let base = d.path || '';
            if (base.length > 0 && base.charAt(base.length - 1) !== '\\') base += '\\';
            (d.dirs || []).forEach(name => {
                html += `<div style="padding:8px 16px;cursor:pointer;font-size:13px;" data-path="${base}${name}">&#128193; ${name}</div>`;
            });
            if (!html) html = '<div style="padding:16px;color:#bcc3d0">空目录</div>';
            listEl.innerHTML = html;
            listEl.querySelectorAll('[data-path]').forEach(el => {
                el.onmouseover = () => el.style.background = '#eff6ff';
                el.onmouseout = () => el.style.background = '';
                el.onclick = () => dbLoadDirList(el.getAttribute('data-path'));
            });
        }).catch(() => {
            listEl.innerHTML = '<div style="padding:16px;color:#ef4444">无法读取目录</div>';
        });
    }

    // 当前视图状态
    let currentView = 'statistics';
    let currentSubView = '';
    // 采集数据和转换数据缓存
    let collectData = [];
    let convertData = [];
    // 实时轮询定时器
    let pollTimer = null;
    // 图表动画帧 ID 列表（用于取消动画）
    let chartAnimFrames = [];
    // 柱状图参数：周期、年、月、日、偏移
    let barPeriod = 'day';
    let barFilter = 'all'; // 'all', 'collect', 'converted'
    let barYear = new Date().getFullYear();
    let barMonth = new Date().getMonth();
    let barDay = new Date().getDate();
    let barOffset = 0;
    // 仪表盘年份选择
    let gaugeYear = new Date().getFullYear();
    // 统计卡片年/月/季/日选择
    let statYear = new Date().getFullYear();
    let statMonth = new Date().getMonth();
    let statQuarter = Math.floor(new Date().getMonth() / 3) + 1;
    let statDay = new Date().getDate();

    // ===== 页面初始化 =====
    // ===== 初始化：绑定导航、加载数据、启动轮询 =====
    document.addEventListener('DOMContentLoaded', () => {
        bindNavigation();
        loadAllData();
        startPolling();
        startDevicePolling();
    });

    // ===== 设备信息轮询 =====
    function startDevicePolling() {
        function update() {
            // 多设备 API
            fetch(`${API}/api/devices`).then(r => r.json()).then(d => {
                var slots = d.slots || {};
                ['left', 'right'].forEach(function(pos) {
                    var s = slots[pos] || {};
                    var el = document.getElementById('sb-' + pos + '-status');
                    if (el) {
                        if (s.connected) {
                            var typeLabel = s.type === 'orbbec' ? 'Orbbec' : (s.type === 'hikvision' ? '海康' : s.type);
                            el.textContent = typeLabel + ' 已连接';
                            el.style.color = 'var(--accent-green)';
                        } else {
                            el.textContent = '未连接'; el.style.color = 'var(--text-dim)';
                        }
                    }
                });
                // 夹爪状态
                var gripperSlots = d.gripperSlots || {};
                ['left', 'right'].forEach(function(pos) {
                    var gs = gripperSlots[pos] || {};
                    var el = document.getElementById('sb-gripper-' + pos + '-status');
                    if (el) {
                        if (gs.connected) {
                            var gtype = gs.type === 'manual' ? '手动' : (gs.type === 'electric' ? '电动' : gs.type);
                            el.textContent = gtype + ' 已连接';
                            el.style.color = 'var(--accent-green)';
                        } else {
                            el.textContent = '未连接'; el.style.color = 'var(--text-dim)';
                        }
                    }
                });
            }).catch(() => {});
        }
        update();
        setInterval(update, 3000);
    }

    // ===== 实时轮询：每 3 秒检查采集/转换数据变化 =====
    function startPolling() {
        pollTimer = setInterval(async () => {
            try {
                const [cRes, dRes] = await Promise.all([
                    fetch(`${API}/api/data/browse?dir=collect`),
                    fetch(`${API}/api/data/browse?dir=converted`)
                ]);
                const newCollect = (await cRes.json()).sessions || [];
                const newConvert = (await dRes.json()).sessions || [];

                const changed = newCollect.length !== collectData.length || newConvert.length !== convertData.length;
                collectData = newCollect;
                convertData = newConvert;

                if (changed && (!document.getElementById('modal-overlay') || !document.getElementById('modal-overlay').classList.contains('active'))) {
                    if (currentView === 'statistics') {
                        renderStatistics(document.getElementById('main-content'));
                    } else if (currentView === 'data-view') {
                        // 不重建列表，保留筛选条件，只更新数量统计。
                        const countEl = document.querySelector('.page-subtitle strong');
                        if (countEl) {
                            const dir = currentSubView === 'converted' ? 'converted' : 'collect';
                            const data = dir === 'converted' ? convertData : collectData;
                            countEl.textContent = data.length;
                        }
                    }
                }
            } catch (e) {
                // 静默处理，避免轮询失败时频繁打断用户操作。
            }
        }, 3000);
    }

    // ===== 导航绑定：侧边栏菜单点击事件 =====
    function bindNavigation() {
        const toggle = document.getElementById('data-view-toggle');
        const sub = document.getElementById('data-view-sub');
        if (toggle && sub) {
            toggle.addEventListener('click', (e) => {
                e.stopPropagation();
                toggle.classList.toggle('expanded');
                sub.classList.toggle('open');
            });
        }

        document.querySelectorAll('.nav-item[data-view]').forEach(el => {
            el.addEventListener('click', () => {
                const view = el.dataset.view;
                const subVal = el.dataset.sub || '';
                if (el.id === 'data-view-toggle') return;
                switchView(view, subVal);
            });
        });
    }

    // 切换视图：更新导航高亮和主内容区域
    function switchView(view, sub) {
        currentView = view;
        currentSubView = sub;

        document.querySelectorAll('.nav-item').forEach(el => el.classList.remove('active'));

        if (sub) {
            const toggle = document.getElementById('data-view-toggle');
            const subMenu = document.getElementById('data-view-sub');
            if (toggle && subMenu) {
                toggle.classList.add('expanded');
                subMenu.classList.add('open');
            }
            const active = document.querySelector(`.nav-item[data-view="${view}"][data-sub="${sub}"]`);
            if (active) active.classList.add('active');
        } else if (view === 'data-view') {
            const toggle = document.getElementById('data-view-toggle');
            const subMenu = document.getElementById('data-view-sub');
            if (toggle && subMenu) {
                toggle.classList.add('expanded');
                subMenu.classList.add('open');
            }
        } else {
            const active = document.querySelector(`.nav-item[data-view="${view}"]:not([data-sub])`);
            if (active) active.classList.add('active');
        }

        cancelChartAnimations();
        const main = document.getElementById('main-content');
        switch (view) {
            case 'statistics':
                renderStatistics(main);
                break;
            case 'data-view':
                renderDataView(main, sub || 'collect');
                break;
        }
    }

    // 取消所有正在进行的图表动画
    function cancelChartAnimations() {
        chartAnimFrames.forEach(id => cancelAnimationFrame(id));
        countUpFrames.forEach(id => cancelAnimationFrame(id));
        chartAnimFrames = [];
        countUpFrames = [];
    }

    // ===== 数据加载：从 API 获取采集和转换数据 =====
    async function loadAllData() {
        try {
            const [cRes, dRes] = await Promise.all([
                fetch(`${API}/api/data/browse?dir=collect`),
                fetch(`${API}/api/data/browse?dir=converted`)
            ]);
            collectData = (await cRes.json()).sessions || [];
            convertData = (await dRes.json()).sessions || [];
        } catch (e) {
            console.error('Failed to load data:', e);
        }
        if (currentView === 'statistics') {
            renderStatistics(document.getElementById('main-content'));
        } else if (currentView === 'data-view') {
            renderDataView(document.getElementById('main-content'), currentSubView);
        }
    }

    // ===== 入场动画：卡片和列表项渐入效果 =====
    function animateEntrance() {
        requestAnimationFrame(() => {
            document.querySelectorAll('.stat-card:not(.visible)').forEach((el, i) => {
                setTimeout(() => {
                    el.classList.add('visible');
                    const valEl = el.querySelector('.stat-card-value');
                    if (valEl) {
                        const target = parseInt(valEl.dataset.target) || 0;
                        animateCountUp(valEl, target, 800);
                    }
                }, i * 60);
            });
            document.querySelectorAll('.chart-card:not(.visible)').forEach((el, i) => {
                setTimeout(() => el.classList.add('visible'), 150 + i * 100);
            });
            document.querySelectorAll('.data-item:not(.visible)').forEach((el, i) => {
                setTimeout(() => el.classList.add('visible'), i * 40);
            });
        });
    }

    // ===== 统计视图：统计卡片 + 四个图表 =====
    function renderStatistics(container) {
        const now = new Date();

        function getStatYears(sessions) {
            const years = new Set();
            sessions.forEach(s => {
                const d = parseSessionDate(s.id);
                if (d) years.add(d.getFullYear());
            });
            years.add(now.getFullYear());
            return [...years].sort((a, b) => b - a);
        }

        const allSessions = [...collectData, ...convertData];
        const statYears = getStatYears(allSessions);

        function classify(sessions, y, m, q, d) {
            const stats = { today: 0, month: 0, quarter: 0, year: 0 };
            sessions.forEach(s => {
                const dt = parseSessionDate(s.id);
                if (!dt) return;
                if (dt.getFullYear() === y) {
                    stats.year++;
                    if (Math.floor(dt.getMonth() / 3) + 1 === q) stats.quarter++;
                    if (dt.getMonth() === m) stats.month++;
                    if (dt.getDate() === d) stats.today++;
                }
            });
            return stats;
        }

        function refreshStats() {
            const cStats = classify(collectData, statYear, statMonth, statQuarter, statDay);
            const dStats = classify(convertData, statYear, statMonth, statQuarter, statDay);

            const yearOptsHtml = statYears.map(y => `<option value="${y}" ${y === statYear ? 'selected' : ''}>${y}</option>`).join('');

            const cards = [
                { label: '今日新增', value: cStats.today + dStats.today, sub: `采集 ${cStats.today} / 转换 ${dStats.today}`, picker: 'day' },
                { label: '本月累计', value: cStats.month + dStats.month, sub: `采集 ${cStats.month} / 转换 ${dStats.month}`, picker: 'month' },
                { label: '本季合计', value: cStats.quarter + dStats.quarter, sub: `Q${statQuarter} · ${statYear}`, picker: 'quarter' },
                { label: '全年总计', value: cStats.year + dStats.year, sub: `采集 ${cStats.year} / 转换 ${dStats.year}`, picker: 'year' }
            ];

            container.querySelector('.stat-cards').innerHTML = cards.map(c => statCard(c.label, c.value, c.sub, pickerHtml(c.picker, yearOptsHtml))).join('');
            container.querySelectorAll('.stat-card').forEach(el => el.classList.add('visible'));
            container.querySelectorAll('.stat-card-value').forEach(el => animateCountUp(el, +el.dataset.target, 600));
            bindStatPickers();
        }

        function pickerHtml(type, yearOptsHtml) {
            if (type === 'year') {
                return `<select class="stat-year-card-select" id="stat-year-card-select">${yearOptsHtml}</select>`;
            }
            if (!type) return '';
            const triggerLabel = type === 'day' ? `${statMonth + 1}月${statDay}日` : type === 'month' ? `${statMonth + 1}月` : `Q${statQuarter}`;
            let grid = '';
            if (type === 'day') {
                const maxD = new Date(statYear, statMonth + 1, 0).getDate();
                for (let d = 1; d <= maxD; d++) grid += `<span class="sp-item${d === statDay ? ' active' : ''}" data-v="${d}">${d}</span>`;
            } else if (type === 'month') {
                for (let m = 0; m < 12; m++) grid += `<span class="sp-item${m === statMonth ? ' active' : ''}" data-v="${m}">${m + 1}月</span>`;
            } else {
                for (let q = 1; q <= 4; q++) grid += `<span class="sp-item${q === statQuarter ? ' active' : ''}" data-v="${q}">Q${q}</span>`;
            }
            return `<div style="position:relative;">
                <button class="stat-picker-trigger">${triggerLabel}</button>
                <div class="stat-picker-panel" data-type="${type}" style="display:none;">
                    <div class="sp-grid ${type}-grid">${grid}</div>
                </div>
            </div>`;
        }

        function bindStatPickers() {
            const yearSelect = container.querySelector('#stat-year-card-select');
            if (yearSelect) {
                yearSelect.addEventListener('change', () => {
                    statYear = +yearSelect.value;
                    const gaugeSelect = container.querySelector('#gauge-year-select');
                    const typeSelect = container.querySelector('#type-year-select');
                    if (gaugeSelect) { gaugeSelect.value = statYear; gaugeYear = statYear; cancelChartAnimations(); drawYearChart(all); drawTypeChart(collectData, convertData); }
                    if (typeSelect) typeSelect.value = statYear;
                    refreshStats();
                });
            }
            container.querySelectorAll('.stat-picker-trigger').forEach(btn => {
                btn.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const panel = btn.nextElementSibling;
                    const open = panel.style.display === 'grid';
                    container.querySelectorAll('.stat-picker-panel').forEach(p => p.style.display = 'none');
                    container.querySelectorAll('.stat-card').forEach(c => c.style.zIndex = '');
                    if (!open) {
                        panel.style.display = 'grid';
                        btn.closest('.stat-card').style.zIndex = 20;
                    }
                });
            });
            container.querySelectorAll('.sp-item').forEach(item => {
                item.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const type = item.closest('.stat-picker-panel').dataset.type;
                    const v = +item.dataset.v;
                    if (type === 'day') statDay = v;
                    else if (type === 'month') statMonth = v;
                    else if (type === 'quarter') statQuarter = v;
                    refreshStats();
                });
            });
        }

        container.innerHTML = `
            <div class="page-header">
                <div>
                    <div class="page-title"><span class="realtime-dot"></span>数据统计概览</div>
                    <div class="page-subtitle">实时数据采集与转换统计</div>
                </div>
            </div>
            <div class="stat-cards"></div>
            <div class="charts-grid">
                <div class="chart-card">
                    <div class="chart-title">年度采集占比</div>
                    <div class="gauge-nav">
                        <select class="gauge-year-select" id="gauge-year-select"></select>
                    </div>
                    <div class="chart-container">
                        <canvas id="chart-year" width="280" height="320"></canvas>
                    </div>
                </div>
                <div class="chart-card">
                    <div class="chart-title">数据类型分布</div>
                    <div class="gauge-nav">
                        <select class="gauge-year-select" id="type-year-select"></select>
                    </div>
                    <div class="chart-container">
                        <canvas id="chart-type" width="280" height="320"></canvas>
                    </div>
                </div>
                <div class="chart-card">
                    <div class="chart-header-row">
                        <div class="chart-title">采集趋势</div>
                        <div class="chart-period-btns">
                        <button class="chart-period-btn" id="bar-filter-btn" title="切换数据来源" style="background:var(--primary,#3b82f6);color:#fff;font-weight:600;border-color:var(--primary,#3b82f6);">全部</button>
                        <button class="chart-period-btn active" data-period="day">按天</button>
                        <button class="chart-period-btn" data-period="week">按周</button>
                        <button class="chart-period-btn" data-period="month">按月</button>
                        <button class="chart-period-btn" data-period="quarter">按季</button>
                        <button class="chart-period-btn" data-period="year">按年</button>
                        <button class="chart-nav-btn" id="bar-prev" title="上一页">&#9664;</button>
                        <span class="chart-nav-label" id="bar-nav-label"></span>
                        <button class="chart-nav-btn" id="bar-next" title="下一页">&#9654;</button>
                    </div>
                    </div>
                    <div class="chart-container">
                        <canvas id="chart-month" width="500" height="300"></canvas>
                    </div>
                </div>
                <a href="info.html" target="_blank" style="text-decoration:none;color:inherit;">
                <div class="chart-card" style="cursor:pointer;" title="点击查看使用说明">
                    <div class="chart-title">使用流程</div>
                    <div class="chart-container">
                        <canvas id="chart-funnel" width="520" height="420"></canvas>
                    </div>
                </div>
                </a>
            </div>
            <div class="data-path-section" style="margin-top:20px;padding:16px 20px;background:var(--card-bg);border-radius:10px;border:1px solid var(--border);">
                <div style="font-size:13px;font-weight:600;color:var(--text-primary);margin-bottom:10px;">数据路径配置</div>
                <div style="display:flex;gap:24px;flex-wrap:wrap;">
                    <div style="display:flex;align-items:center;gap:6px;">
                        <span style="font-size:12px;color:var(--text-dim);white-space:nowrap;">采集数据:</span>
                        <span id="dashboard-stat-path-collect" style="font-size:11px;color:var(--primary);font-family:monospace;max-width:350px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">加载中...</span>
                        <button class="info-btn" id="dashboard-stat-btn-collect" title="选择目录" style="font-size:11px;padding:1px 5px;width:22px;height:22px;cursor:pointer;border:1px solid var(--border);border-radius:4px;background:var(--card-bg);">&#128193;</button>
                    </div>
                    <div style="display:flex;align-items:center;gap:6px;">
                        <span style="font-size:12px;color:var(--text-dim);white-space:nowrap;">转换数据:</span>
                        <span id="dashboard-stat-path-converted" style="font-size:11px;color:var(--primary);font-family:monospace;max-width:350px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">加载中...</span>
                        <button class="info-btn" id="dashboard-stat-btn-converted" title="选择目录" style="font-size:11px;padding:1px 5px;width:22px;height:22px;cursor:pointer;border:1px solid var(--border);border-radius:4px;background:var(--card-bg);">&#128193;</button>
                    </div>
                </div>
            </div>
        `;

        // 加载当前采集目录和转换目录。
        fetch(`${API}/api/paths`).then(r => r.json()).then(d => {
            const cEl = document.getElementById('dashboard-stat-path-collect');
            const dEl = document.getElementById('dashboard-stat-path-converted');
            if (cEl) cEl.textContent = d.collect || '--';
            if (dEl) dEl.textContent = d.converted || '--';
        }).catch(() => {});

        // 绑定路径选择按钮，复用数据看板的目录选择弹窗。
        const statCollectBtn = document.getElementById('dashboard-stat-btn-collect');
        const statConvertedBtn = document.getElementById('dashboard-stat-btn-converted');
        if (statCollectBtn) statCollectBtn.addEventListener('click', () => openDashboardDirBrowser('collect'));
        if (statConvertedBtn) statConvertedBtn.addEventListener('click', () => openDashboardDirBrowser('converted'));

        cancelChartAnimations();
        refreshStats();
        const all = [...collectData, ...convertData];

        // 点击下拉框外部时关闭筛选器。
        document.addEventListener('click', () => {
            container.querySelectorAll('.stat-picker-panel').forEach(p => p.style.display = 'none');
            container.querySelectorAll('.stat-card').forEach(c => c.style.zIndex = '');
        });

        // 绑定类型筛选按钮：全部 → 采集 → 转换 → 全部。
        const filterBtn = container.querySelector('#bar-filter-btn');
        if (filterBtn) {
            filterBtn.addEventListener('click', () => {
                const filterMap = ['all', 'collect', 'converted'];
                const labelMap = { all: '全部', collect: '采集', converted: '转换' };
                const idx = filterMap.indexOf(barFilter);
                barFilter = filterMap[(idx + 1) % 3];
                filterBtn.textContent = labelMap[barFilter];
                barOffset = 0;
                cancelChartAnimations();
                drawBarChart(getFilteredBarData());
                updateNavBarLabel();
            });
        }

        function getFilteredBarData() {
            if (barFilter === 'collect') return collectData;
            if (barFilter === 'converted') return convertData;
            return all;
        }

        // 绑定时间范围切换按钮。
        container.querySelectorAll('.chart-period-btn').forEach(btn => {
            btn.addEventListener('click', () => {
                container.querySelectorAll('.chart-period-btn').forEach(b => b.classList.remove('active'));
                btn.classList.add('active');
                barPeriod = btn.dataset.period;
                barOffset = 0;
                if (btn.dataset.period !== 'day') barDay = new Date().getDate();
                cancelChartAnimations();
                drawBarChart(getFilteredBarData());
                updateNavBarLabel();
            });
        });

        // 绑定柱状图前后翻页箭头，并更新当前时间范围标签。
        const navLabel = container.querySelector('#bar-nav-label');
        const allData = all;

        function updateNavBarLabel() {
            const m = barMonth + 1;
            if (barPeriod === 'day') {
                const daysInMonth = new Date(barYear, barMonth + 1, 0).getDate();
                const anchor = Math.max(1, Math.min(barDay, daysInMonth));
                const s = Math.max(1, anchor - 6);
                const e = Math.min(daysInMonth, s + 6);
                navLabel.textContent = barYear + '年' + m + '月' + s + '-' + e + '日';
            } else if (barPeriod === 'week') {
                navLabel.textContent = barYear + '年' + m + '月';
            } else if (barPeriod === 'month') {
                navLabel.textContent = barYear + '年';
            } else if (barPeriod === 'quarter') {
                const endY = new Date().getFullYear() - barOffset * 3;
                const startY = endY - 2;
                navLabel.textContent = startY + '-' + endY + '年';
            } else {
                const endY = new Date().getFullYear() - barOffset * 5;
                const startY = endY - 4;
                navLabel.textContent = startY + '-' + endY + '年';
            }
        }
        updateNavBarLabel();

        function navigateBar(direction) {
            if (barPeriod === 'day') {
                barDay += direction * 7;
                const daysInMonth = new Date(barYear, barMonth + 1, 0).getDate();
                if (barDay > daysInMonth) { barDay -= daysInMonth; barMonth++; }
                if (barMonth > 11) { barMonth = 0; barYear++; }
                if (barDay < 1) { barMonth--; if (barMonth < 0) { barMonth = 11; barYear--; } barDay += new Date(barYear, barMonth + 1, 0).getDate(); }
            } else if (barPeriod === 'week') {
                barMonth += direction;
                if (barMonth > 11) { barMonth = 0; barYear++; }
                if (barMonth < 0) { barMonth = 11; barYear--; }
            } else if (barPeriod === 'month') {
                barYear += direction;
            } else if (barPeriod === 'quarter') {
                barOffset += direction;
            } else {
                barOffset += direction;
            }
            cancelChartAnimations();
            drawBarChart(getFilteredBarData());
            updateNavBarLabel();
        }

        container.querySelector('#bar-prev').addEventListener('click', () => navigateBar(-1));
        container.querySelector('#bar-next').addEventListener('click', () => navigateBar(1));

        // 时间范围变化时同步更新上下文标签。
        const origPeriodBtns = container.querySelectorAll('.chart-period-btn[data-period]');
        origPeriodBtns.forEach(btn => {
            btn.addEventListener('click', () => {
                setTimeout(updateNavBarLabel, 10);
            });
        });

        setTimeout(() => {
            animateEntrance();
            drawYearChart(all);
            drawTypeChart(collectData, convertData);
            drawBarChart(all);
            drawFunnelChart(all);
        }, 50);

        // 绑定环形统计图年份下拉框。
        const gaugeSelect = container.querySelector('#gauge-year-select');
        const typeSelect = container.querySelector('#type-year-select');

        function populateGaugeYears() {
            const years = new Set();
            years.add(new Date().getFullYear());
            all.forEach(s => { const d = parseSessionDate(s.id); if (d) years.add(d.getFullYear()); });
            const sorted = [...years].sort((a, b) => b - a);
            const html = sorted.map(y => `<option value="${y}" ${y === gaugeYear ? 'selected' : ''}>${y}</option>`).join('');
            gaugeSelect.innerHTML = html;
            typeSelect.innerHTML = html;
        }
        populateGaugeYears();

        gaugeSelect.addEventListener('change', () => {
            gaugeYear = +gaugeSelect.value;
            typeSelect.value = gaugeYear;
            cancelChartAnimations();
            drawYearChart(all);
            drawTypeChart(collectData, convertData);
        });
        typeSelect.addEventListener('change', () => {
            gaugeYear = +typeSelect.value;
            gaugeSelect.value = gaugeYear;
            cancelChartAnimations();
            drawYearChart(all);
            drawTypeChart(collectData, convertData);
        });
    }

    // 统计卡片 HTML 生成
    function statCard(label, value, sub, selectorsHtml) {
        return `<div class="stat-card">
            <div class="stat-card-header">
                <div class="stat-card-label">${label}</div>
                <div>${selectorsHtml}</div>
            </div>
            <div class="stat-card-value" data-target="${value}">0</div>
            <div class="stat-card-sub">${sub}</div>
        </div>`;
    }

    // 数字递增动画
    let countUpFrames = [];

    function animateCountUp(el, target, duration) {
        const start = performance.now();
        function tick(now) {
            const t = Math.min((now - start) / duration, 1);
            const ease = 1 - Math.pow(1 - t, 3);
            el.textContent = Math.round(target * ease);
            if (t < 1) {
                countUpFrames.push(requestAnimationFrame(tick));
            }
        }
        countUpFrames.push(requestAnimationFrame(tick));
    }

    // 从 Session ID 解析日期（格式：YYYYMMDD_HHMMSS）
    function parseSessionDate(id) {
        // 兼容带格式后缀的会话 ID，例如 20260424_174813_lerobot。
        const base = id.replace(/_(lerobot|hdf5|rlds)$/, '');
        const m = base.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})$/);
        if (!m) return null;
        return new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
    }

    // ===== 科技风环形统计图（270 度弧形）=====
    // ===== 仪表盘图表绘制（通用） =====
    function drawGaugeChart(ctx, W, H, segments, total, centerLabel, legendItems, accentColor) {
        const cx = W / 2, cy = H / 2 - 10;
        const R = Math.min(W, H) / 2 - 50;
        const lw = 20;
        const arcSpan = Math.PI * 1.5;
        const arcStart = Math.PI * 0.75;
        const duration = 1200;
        const t0 = performance.now();

        function draw(now) {
            const t = Math.min((now - t0) / duration, 1);
            const ease = 1 - Math.pow(1 - t, 3);
            ctx.clearRect(0, 0, W, H);

            // 外圈刻度装饰。
            for (let i = 0; i <= 27; i++) {
                const a = arcStart + arcSpan * i / 27;
                const major = i % 9 === 0;
                const r1 = R + (major ? 6 : 10);
                const r2 = R + 15;
                ctx.beginPath();
                ctx.moveTo(cx + r1 * Math.cos(a), cy + r1 * Math.sin(a));
                ctx.lineTo(cx + r2 * Math.cos(a), cy + r2 * Math.sin(a));
                ctx.strokeStyle = major ? '#94a3b8' : '#e2e8f0';
                ctx.lineWidth = major ? 2 : 1;
                ctx.stroke();
            }

            // 背景轨道。
            ctx.beginPath();
            ctx.arc(cx, cy, R, arcStart, arcStart + arcSpan);
            ctx.strokeStyle = '#f1f5f9';
            ctx.lineWidth = lw;
            ctx.lineCap = 'round';
            ctx.stroke();

            // 彩色数据分段。
            let offset = 0;
            for (let i = 0; i < segments.length; i++) {
                if (segments[i] === 0) continue;
                const sweep = (segments[i] / total) * arcSpan;
                const drawn = sweep * ease;
                if (drawn < 0.001) { offset += sweep; continue; }
                const sa = arcStart + offset;
                const col = legendItems[i].color;

                // 光晕效果。
                ctx.beginPath();
                ctx.arc(cx, cy, R, sa, sa + drawn);
                ctx.strokeStyle = hexToRgba(col, 0.12);
                ctx.lineWidth = lw + 14;
                ctx.lineCap = 'round';
                ctx.stroke();

                // 主弧线。
                ctx.beginPath();
                ctx.arc(cx, cy, R, sa, sa + drawn);
                ctx.strokeStyle = col;
                ctx.lineWidth = lw;
                ctx.lineCap = 'round';
                ctx.stroke();

                offset += sweep;
            }

            // 内圈装饰环。
            ctx.beginPath();
            ctx.arc(cx, cy, R - lw / 2 - 3, arcStart, arcStart + arcSpan);
            ctx.strokeStyle = '#e2e8f0';
            ctx.lineWidth = 1;
            ctx.stroke();

            // 中心数值。
            ctx.fillStyle = '#0f172a';
            ctx.font = 'bold 38px sans-serif';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(String(Math.round(ease * total)), cx, cy - 10);

            ctx.fillStyle = '#94a3b8';
            ctx.font = '12px sans-serif';
            ctx.fillText(centerLabel, cx, cy + 16);

            ctx.fillStyle = accentColor || '#3b82f6';
            ctx.font = 'bold 15px sans-serif';
            ctx.fillText(Math.round(ease * 100) + '%', cx, cy + 36);

            // 图例。
            const ly = H - 16;
            ctx.font = '12px sans-serif';
            let tw = 0;
            const ws = legendItems.map((item, i) => {
                const w = ctx.measureText(item.label + ': ' + segments[i]).width + 26;
                tw += w;
                return w;
            });
            let lx = (W - tw) / 2;
            legendItems.forEach((item, i) => {
                ctx.beginPath();
                ctx.arc(lx + 6, ly, 5, 0, Math.PI * 2);
                ctx.fillStyle = item.color;
                ctx.fill();
                ctx.fillStyle = '#475569';
                ctx.textAlign = 'left';
                ctx.textBaseline = 'middle';
                ctx.fillText(item.label + ': ' + segments[i], lx + 16, ly);
                lx += ws[i];
            });
            ctx.textBaseline = 'alphabetic';

            if (t < 1) {
                const id = requestAnimationFrame(draw);
                chartAnimFrames.push(id);
            }
        }
        chartAnimFrames.push(requestAnimationFrame(draw));
    }

    // ===== 图表：年度采集占比（按季度仪表盘） =====
    function drawYearChart(sessions) {
        const canvas = document.getElementById('chart-year');
        if (!canvas) return;
        const ctx = canvas.getContext('2d');
        const W = canvas.width, H = canvas.height;

        const counts = [0, 0, 0, 0];
        sessions.forEach(s => {
            const d = parseSessionDate(s.id);
            if (d && d.getFullYear() === gaugeYear) counts[Math.floor(d.getMonth() / 3)]++;
        });
        const total = counts.reduce((a, b) => a + b, 0);

        drawGaugeChart(ctx, W, H, counts, total || 1, gaugeYear + '年度总计',
            [{ label: 'Q1', color: '#3b82f6' }, { label: 'Q2', color: '#06b6d4' },
             { label: 'Q3', color: '#8b5cf6' }, { label: 'Q4', color: '#10b981' }],
            '#3b82f6');
    }

    // ===== 图表：数据类型分布（采集/转换仪表盘） =====
    function drawTypeChart(collect, convert) {
        const canvas = document.getElementById('chart-type');
        if (!canvas) return;
        const ctx = canvas.getContext('2d');
        const W = canvas.width, H = canvas.height;

        const values = [0, 0];
        collect.forEach(s => { const d = parseSessionDate(s.id); if (d && d.getFullYear() === gaugeYear) values[0]++; });
        convert.forEach(s => { const d = parseSessionDate(s.id); if (d && d.getFullYear() === gaugeYear) values[1]++; });
        const total = Math.max(1, values[0] + values[1]);

        drawGaugeChart(ctx, W, H, values, total, gaugeYear + '年全部数据',
            [{ label: '采集', color: '#f59e0b' }, { label: '转换', color: '#06b6d4' }],
            '#f59e0b');
    }

    // ===== 柱状图：自动缩放 Y 轴刻度 =====
    // 刻度模式：0, 10, 30, 60, 100, 150, 210, 280, 360, 450, 550, 660, 780, 910, 1050, ...
    function autoScale(maxVal) {
        if (maxVal <= 0) maxVal = 10;
        const base = [0, 10, 30, 60, 100, 150, 210, 280, 360, 450, 550, 660, 780, 910, 1050, 1200, 1360, 1530, 1710, 1900, 2100];
        for (let i = 1; i < base.length; i++) {
            if (base[i] >= maxVal) return base.slice(0, i + 1);
        }
        const step = Math.ceil((maxVal - 2100) / 6 / 10) * 10;
        const ticks = [...base];
        let v = 2100;
        while (v < maxVal + step) {
            v += Math.max(step, 100);
            ticks.push(v);
        }
        return ticks;
    }

    // 按周期（天/周/月/季/年）聚合柱状图数据
    function buildBarData(sessions, period) {
        const year = barYear;
        const month = barMonth;
        const buckets = [];

        if (period === 'day') {
            const daysInMonth = new Date(year, month + 1, 0).getDate();
            const anchorDay = Math.max(1, Math.min(barDay, daysInMonth));
            const startDay = Math.max(1, anchorDay - 6);
            const endDay = Math.min(daysInMonth, startDay + 6);
            for (let d = startDay; d <= endDay; d++) {
                const label = (month + 1) + '/' + d;
                buckets.push({ label, key: label, count: 0 });
            }
            sessions.forEach(s => {
                const d = parseSessionDate(s.id);
                if (!d || d.getFullYear() !== year || d.getMonth() !== month) return;
                const key = (d.getMonth() + 1) + '/' + d.getDate();
                const b = buckets.find(x => x.key === key);
                if (b) b.count++;
            });
        } else if (period === 'week') {
            const daysInMonth = new Date(year, month + 1, 0).getDate();
            const firstDow = (new Date(year, month, 1).getDay() + 6) % 7;
            const weeks = [];
            let day = 1;
            while (day <= daysInMonth) {
                const start = day;
                const end = Math.min(day + (6 - ((day - 1 + firstDow) % 7)), daysInMonth);
                const wIdx = Math.floor((day - 1 + firstDow) / 7);
                weeks.push({ start, end, wIdx });
                day = end + 1;
            }
            weeks.forEach(w => {
                const label = w.start + '-' + w.end + '日';
                buckets.push({ label, key: String(w.wIdx), count: 0 });
            });
            sessions.forEach(s => {
                const d = parseSessionDate(s.id);
                if (!d || d.getFullYear() !== year || d.getMonth() !== month) return;
                const wIdx = Math.floor((d.getDate() - 1 + firstDow) / 7);
                const b = buckets.find(x => x.key === String(wIdx));
                if (b) b.count++;
            });
        } else if (period === 'quarter') {
            const curYear = new Date().getFullYear() - barOffset * 3;
            for (let yOff = 2; yOff >= 0; yOff--) {
                const yr = curYear - yOff;
                for (let q = 0; q < 4; q++) {
                    buckets.push({ label: yr + ' Q' + (q + 1), key: yr + '-' + q, count: 0 });
                }
            }
            sessions.forEach(s => {
                const d = parseSessionDate(s.id);
                if (!d) return;
                const yr = d.getFullYear();
                const q = Math.floor(d.getMonth() / 3);
                const b = buckets.find(x => x.key === yr + '-' + q);
                if (b) b.count++;
            });
        } else if (period === 'year') {
            const curYear = new Date().getFullYear() - barOffset * 5;
            for (let i = 4; i >= 0; i--) {
                const yr = curYear - i;
                buckets.push({ label: yr + '年', key: String(yr), count: 0 });
            }
            sessions.forEach(s => {
                const d = parseSessionDate(s.id);
                if (!d) return;
                const b = buckets.find(x => x.key === String(d.getFullYear()));
                if (b) b.count++;
            });
        } else {
            for (let m = 0; m < 12; m++) buckets.push({ label: (m + 1) + '月', key: String(m), count: 0 });
            sessions.forEach(s => {
                const d = parseSessionDate(s.id);
                if (!d || d.getFullYear() !== year) return;
                buckets[d.getMonth()].count++;
            });
        }
        return buckets;
    }

    // 绘制采集趋势柱状图（带动画）
    function drawBarChart(sessions) {
        const canvas = document.getElementById('chart-month');
        if (!canvas) return;
        const ctx = canvas.getContext('2d');
        const W = canvas.width, H = canvas.height;

        const data = buildBarData(sessions, barPeriod);
        const maxCount = Math.max(1, ...data.map(d => d.count));
        const yTicks = autoScale(maxCount);
        const yMax = yTicks[yTicks.length - 1];

        const leftPad = 44, rightPad = 10, topPad = 12, bottomPad = 34;
        const chartW = W - leftPad - rightPad;
        const chartH = H - topPad - bottomPad;
        const baseY = H - bottomPad;
        const barCount = data.length;
        const gap = chartW / barCount;
        const barW = Math.max(14, Math.min(40, gap * 0.55));
        const duration = 800;
        const t0 = performance.now();

        function draw(now) {
            const t = Math.min((now - t0) / duration, 1);
            const ease = 1 - Math.pow(1 - t, 3);
            ctx.clearRect(0, 0, W, H);

            // Y 轴刻度。
            ctx.textAlign = 'right';
            ctx.textBaseline = 'middle';
            yTicks.forEach(v => {
                const y = baseY - (v / yMax) * chartH;
                ctx.beginPath();
                ctx.moveTo(leftPad, y);
                ctx.lineTo(W - rightPad, y);
                ctx.strokeStyle = v === 0 ? '#cbd5e1' : '#f1f5f9';
                ctx.lineWidth = 1;
                ctx.stroke();
                ctx.fillStyle = '#94a3b8';
                ctx.font = '11px sans-serif';
                ctx.fillText(String(v), leftPad - 6, y);
            });

            // 柱状条。
            data.forEach((d, i) => {
                const x = leftPad + gap * i + (gap - barW) / 2;
                const barH = (d.count / yMax) * chartH * ease;
                const y = baseY - barH;

                if (barH > 0.5) {
                    const grad = ctx.createLinearGradient(0, y, 0, baseY);
                    grad.addColorStop(0, '#3b82f6');
                    grad.addColorStop(1, '#93c5fd');
                    ctx.fillStyle = grad;

                    const r = Math.min(4, barH / 2);
                    ctx.beginPath();
                    ctx.moveTo(x, baseY);
                    ctx.lineTo(x, y + r);
                    ctx.quadraticCurveTo(x, y, x + r, y);
                    ctx.lineTo(x + barW - r, y);
                    ctx.quadraticCurveTo(x + barW, y, x + barW, y + r);
                    ctx.lineTo(x + barW, baseY);
                    ctx.closePath();
                    ctx.fill();

                    ctx.shadowColor = hexToRgba('#3b82f6', 0.2);
                    ctx.shadowBlur = 6;
                    ctx.fill();
                    ctx.shadowColor = 'transparent';
                    ctx.shadowBlur = 0;

                    if (d.count > 0 && ease > 0.5) {
                        ctx.fillStyle = '#1e293b';
                        ctx.font = 'bold 11px sans-serif';
                        ctx.textAlign = 'center';
                        ctx.textBaseline = 'bottom';
                        ctx.globalAlpha = (ease - 0.5) * 2;
                        ctx.fillText(String(d.count), x + barW / 2, y - 4);
                        ctx.globalAlpha = 1;
                    }
                }

                ctx.fillStyle = '#94a3b8';
                ctx.textAlign = 'center';
                ctx.textBaseline = 'top';
                const isLong = d.label.includes(' Q');
                if (isLong) {
                    ctx.save();
                    ctx.translate(x + barW / 2, baseY + 6);
                    ctx.rotate(-Math.PI / 6);
                    ctx.font = '10px sans-serif';
                    ctx.textAlign = 'right';
                    ctx.fillText(d.label, 0, 0);
                    ctx.restore();
                } else {
                    ctx.font = '11px sans-serif';
                    ctx.fillText(d.label, x + barW / 2, baseY + 8);
                }
            });
            ctx.textBaseline = 'alphabetic';

            if (t < 1) {
                const id = requestAnimationFrame(draw);
                chartAnimFrames.push(id);
            }
        }
        chartAnimFrames.push(requestAnimationFrame(draw));
    }

    // ===== 图表：使用流程漏斗图 =====
    function drawFunnelChart(sessions) {
        const canvas = document.getElementById('chart-funnel');
        if (!canvas) return;
        const ctx = canvas.getContext('2d');
        const W = canvas.width, H = canvas.height;

        const layers = [
            { label: '硬件连接', color: '#10b981' },
            { label: '安装依赖', color: '#f59e0b' },
            { label: '环境配置', color: '#8b5cf6' },
            { label: '参数校准', color: '#06b6d4' },
            { label: '数据采集', color: '#3b82f6' }
        ];

        const widths = [
            [1.00, 0.82],
            [0.82, 0.64],
            [0.64, 0.48],
            [0.48, 0.32],
            [0.32, 0.0 ]
        ];

        const padding = 20;
        const topY = 10;
        const bottomY = H - 30;
        const layerH = (bottomY - topY) / layers.length;
        const maxW = W - padding * 2;
        const duration = 1200;
        const startTime = performance.now();

        function frame(now) {
            const t = Math.min((now - startTime) / duration, 1);
            ctx.clearRect(0, 0, W, H);

            for (let idx = 0; idx < layers.length; idx++) {
                const delay = idx * 0.12;
                const lt = Math.max(0, Math.min(1, (t - delay) / (1 - delay)));
                const lEase = 1 - Math.pow(1 - lt, 3);
                if (lEase <= 0) continue;

                const yTop = bottomY - (idx + 1) * layerH;
                const yBot = bottomY - idx * layerH;
                const g = 2;
                const drawY = yTop + g;
                const drawH = (yBot - yTop) - g * 2;

                const botW = widths[idx][0] * maxW * lEase;
                const topW = widths[idx][1] * maxW * lEase;
                const botLeft = (W - botW) / 2;
                const topLeft = (W - topW) / 2;

                const grad = ctx.createLinearGradient(0, drawY, 0, drawY + drawH);
                grad.addColorStop(0, hexToRgba(layers[idx].color, 0.93));
                grad.addColorStop(1, hexToRgba(layers[idx].color, 0.6));

                ctx.beginPath();
                if (topW < maxW * 0.02) {
                    // 三角形：尖端指向当前层顶部中心。
                    ctx.moveTo(W / 2, drawY);
                    ctx.lineTo(botLeft + botW, drawY + drawH);
                    ctx.lineTo(botLeft, drawY + drawH);
                } else {
                    ctx.moveTo(topLeft, drawY);
                    ctx.lineTo(topLeft + topW, drawY);
                    ctx.lineTo(botLeft + botW, drawY + drawH);
                    ctx.lineTo(botLeft, drawY + drawH);
                }
                ctx.closePath();
                ctx.fillStyle = grad;
                ctx.fill();

                ctx.strokeStyle = hexToRgba(layers[idx].color, 0.38);
                ctx.lineWidth = 1;
                ctx.stroke();

                ctx.beginPath();
                if (topW < maxW * 0.02) {
                    ctx.moveTo(W / 2, drawY);
                } else {
                    ctx.moveTo(topLeft, drawY);
                    ctx.lineTo(topLeft + topW, drawY);
                }
                ctx.strokeStyle = layers[idx].color;
                ctx.lineWidth = 2;
                ctx.stroke();

                const isTop = idx === layers.length - 1;
                const labelY = isTop ? drawY + drawH * 0.60 : drawY + drawH / 2 + 1;
                ctx.globalAlpha = lEase;
                ctx.fillStyle = '#fff';
                ctx.font = 'bold 14px sans-serif';
                ctx.textAlign = 'center';
                ctx.textBaseline = 'middle';
                ctx.fillText(layers[idx].label, W / 2, labelY);
                ctx.globalAlpha = 1;
            }

            ctx.globalAlpha = Math.min(1, t * 2);
            for (let idx = 0; idx < layers.length - 1; idx++) {
                const arrowY = bottomY - (idx + 1) * layerH - 1;
                ctx.fillStyle = '#94a3b8';
                ctx.font = '20px sans-serif';
                ctx.textAlign = 'center';
                ctx.fillText('▲', W / 2, arrowY);
            }
            ctx.globalAlpha = 1;

            if (t < 1) {
                const id = requestAnimationFrame(frame);
                chartAnimFrames.push(id);
            }
        }
        chartAnimFrames.push(requestAnimationFrame(frame));
    }

    // ===== 数据浏览视图：采集数据/转换数据列表 =====
    function renderDataView(container, sub) {
        const dir = sub === 'converted' ? 'converted' : 'collect';
        const data = dir === 'converted' ? convertData : collectData;
        const title = sub === 'converted' ? '转换数据' : '采集数据';

        const years = new Set();
        years.add(new Date().getFullYear());
        data.forEach(s => { const d = parseSessionDate(s.id); if (d) years.add(d.getFullYear()); });
        const sortedYears = [...years].sort((a, b) => b - a);
        const now = new Date();
        let filterYear = now.getFullYear();
        let filterMonth = now.getMonth() + 1;
        let filterDay = 0;

        const monthNames = ['1月','2月','3月','4月','5月','6月','7月','8月','9月','10月','11月','12月'];

        function getDaysInMonth(y, m) { return new Date(y, m, 0).getDate(); }

        function buildFilterHtml() {
            const daysInMonth = getDaysInMonth(filterYear, filterMonth);
            const dayItems = Array.from({length: daysInMonth}, (_, i) => {
                const d = i + 1;
                return `<div class="sp-item${d === filterDay ? ' active' : ''}" data-day="${d}">${d}</div>`;
            }).join('');
            return `
                <div style="position:relative;display:inline-flex;align-items:center;gap:6px;">
                    <select class="stat-picker-trigger data-filter-year" style="appearance:auto;padding-right:4px;">
                        <option value="0"${filterYear === 0 ? ' selected' : ''}>全部年份</option>
                        ${sortedYears.map(y => `<option value="${y}"${y === filterYear ? ' selected' : ''}>${y}年</option>`).join('')}
                    </select>
                    <select class="stat-picker-trigger data-filter-month" style="appearance:auto;padding-right:4px;">
                        <option value="0"${filterMonth === 0 ? ' selected' : ''}>全部月份</option>
                        ${monthNames.map((n, i) => `<option value="${i+1}"${i+1 === filterMonth ? ' selected' : ''}>${n}</option>`).join('')}
                    </select>
                    <div style="position:relative;">
                        <button class="stat-picker-trigger data-filter-day-trigger">${filterDay > 0 ? filterDay + '日' : '全部'}</button>
                        <div class="stat-picker-panel sp-grid day-grid" style="display:none;width:240px;">
                            <div class="sp-item${filterDay === 0 ? ' active' : ''}" data-day="0">全部</div>
                            ${dayItems}
                        </div>
                    </div>
                </div>
            `;
        }

        function getFilteredData(searchQuery) {
            return data.filter(s => {
                const d = parseSessionDate(s.id);
                if (!d) return false;
                if (filterYear > 0 && d.getFullYear() !== filterYear) return false;
                if (filterMonth > 0 && d.getMonth() + 1 !== filterMonth) return false;
                if (filterDay > 0 && d.getDate() !== filterDay) return false;
                if (searchQuery) return s.id.toLowerCase().includes(searchQuery);
                return true;
            });
        }

        function refreshList(searchQuery) {
            const filtered = getFilteredData(searchQuery);
            const el = document.querySelector('.page-subtitle strong');
            if (el) el.textContent = filtered.length;
            renderDataList(filtered, dir);
            setTimeout(animateEntrance, 10);
        }

        function rebuildToolbar() {
            const toolbar = container.querySelector('.data-toolbar-left');
            toolbar.innerHTML = buildFilterHtml() + '<input type="text" class="search-box" id="search-input" placeholder="搜索 Session ID...">';
            bindFilterEvents();
            const searchEl = document.getElementById('search-input');
            if (searchEl) {
                searchEl.value = searchQuery;
                searchEl.addEventListener('input', (ev) => { searchQuery = ev.target.value.toLowerCase(); refreshList(searchQuery); });
            }
        }

        function bindFilterEvents() {
            container.querySelector('.data-filter-year').addEventListener('change', (e) => {
                filterYear = +e.target.value;
                rebuildToolbar();
                refreshList(searchQuery);
            });
            container.querySelector('.data-filter-month').addEventListener('change', (e) => {
                filterMonth = +e.target.value;
                rebuildToolbar();
                refreshList(searchQuery);
            });
            const dayTrigger = container.querySelector('.data-filter-day-trigger');
            const dayPanel = dayTrigger ? dayTrigger.nextElementSibling : null;
            if (dayTrigger && dayPanel) {
                dayTrigger.addEventListener('click', (e) => {
                    e.stopPropagation();
                    const open = dayPanel.style.display === 'grid';
                    dayPanel.style.display = open ? 'none' : 'grid';
                });
                dayPanel.querySelectorAll('.sp-item').forEach(item => {
                    item.addEventListener('click', (e) => {
                        e.stopPropagation();
                        filterDay = +item.dataset.day;
                        dayTrigger.textContent = filterDay > 0 ? filterDay + '日' : '全部';
                        dayPanel.style.display = 'none';
                        dayPanel.querySelectorAll('.sp-item').forEach(el => el.classList.remove('active'));
                        item.classList.add('active');
                        refreshList(searchQuery);
                    });
                });
            }
        }

        container.innerHTML = `
            <div class="page-header">
                <div>
                    <div class="page-title"><span class="realtime-dot"></span>${title}</div>
                    <div class="page-subtitle" style="display:flex;align-items:center;gap:6px;">
                        共 <strong>0</strong> 条数据记录
                        <span style="color:var(--text-dim);font-size:11px;margin-left:8px;">路径:</span>
                        <span id="dashboard-path-${dir}" style="font-size:11px;color:var(--primary);font-family:monospace;max-width:300px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">加载中...</span>
                        <button class="info-btn" id="dashboard-path-btn-${dir}" title="选择目录" style="font-size:11px;padding:1px 5px;width:22px;height:22px;">&#128193;</button>
                    </div>
                </div>
                <div class="data-toolbar-left">
                    ${buildFilterHtml()}
                    <input type="text" class="search-box" id="search-input" placeholder="搜索 Session ID...">
                </div>
            </div>
            <div class="data-list" id="data-list"></div>
        `;

        // 加载并显示当前目录。
        fetch(`${API}/api/paths`).then(r => r.json()).then(d => {
            const pathEl = document.getElementById(`dashboard-path-${dir}`);
            if (pathEl) pathEl.textContent = dir === 'converted' ? (d.converted || '--') : (d.collect || '--');
        }).catch(() => {});

        // 目录选择按钮。
        const pathBtn = document.getElementById(`dashboard-path-btn-${dir}`);
        if (pathBtn) {
            pathBtn.addEventListener('click', () => {
                openDashboardDirBrowser(dir);
            });
        }

        let searchQuery = '';
        refreshList();
        bindFilterEvents();

        document.getElementById('search-input').addEventListener('input', (e) => {
            searchQuery = e.target.value.toLowerCase();
            refreshList(searchQuery);
        });

        document.addEventListener('click', () => {
            const dp = container.querySelector('.stat-picker-panel');
            if (dp) dp.style.display = 'none';
        });
    }

    // 汇总旧版顶层 metadata 和新版 slots metadata。
    // 多槽位并行录制时，时长按每槽位最大帧数估计，而不是简单求和。
    function aggregateMeta(meta) {
        const result = { maxFrames: 0, totalColorFrames: 0, totalDepthFrames: 0, gripperCount: 0, resolutions: [] };

        // 旧版扁平格式。
        const fc = meta.frameCount || {};
        const oldMax = Math.max(fc.color || 0, fc.depth || 0, (fc['ir-left'] || 0) + (fc['ir-right'] || 0));
        if (oldMax > 0) {
            result.maxFrames = oldMax;
            result.totalColorFrames = fc.color || 0;
            result.totalDepthFrames = fc.depth || 0;
        }
        result.gripperCount = Math.max(meta.gripperCount || 0, (meta.gripper && meta.gripper.frames) || 0);
        if (meta.videos && meta.videos.color && meta.videos.color.width) {
            result.resolutions.push(`${meta.videos.color.width}x${meta.videos.color.height}`);
        }

        // 新版槽位格式：各槽位并行采集，跨槽位取最大帧数估计时长。
        const slots = meta.slots;
        if (slots && typeof slots === 'object') {
            for (const slotName in slots) {
                const slot = slots[slotName] || {};
                const slotFc = slot.frameCount || {};
                const slotMax = Math.max(slotFc.color || 0, slotFc.depth || 0, (slotFc['ir-left'] || 0) + (slotFc['ir-right'] || 0));
                if (slotMax > result.maxFrames) result.maxFrames = slotMax;
                result.totalColorFrames += slotFc.color || 0;
                result.totalDepthFrames += slotFc.depth || 0;
                const slotGrip = Math.max(slot.gripperCount || 0, (slot.gripper && slot.gripper.frames) || 0);
                result.gripperCount = Math.max(result.gripperCount, slotGrip);
                const slotVideos = slot.videos || {};
                if (slotVideos.color && slotVideos.color.width) result.resolutions.push(`${slotVideos.color.width}x${slotVideos.color.height}`);
                if (slotVideos.depth && slotVideos.depth.width) result.resolutions.push(`${slotVideos.depth.width}x${slotVideos.depth.height}`);
            }
        }
        return result;
    }

    // 渲染数据列表项
    function renderDataList(sessions, dir) {
        const list = document.getElementById('data-list');
        if (!list) return;

        if (sessions.length === 0) {
            list.innerHTML = `
                <div class="empty-state">
                    <div class="empty-state-icon">&#128194;</div>
                    <div class="empty-state-text">暂无数据</div>
                    <div class="empty-state-sub">开始采集数据后将在此处显示</div>
                </div>`;
            return;
        }

        const posLabels = { head: '头部', left: '左手', right: '右手' };

        list.innerHTML = sessions.map((s, i) => {
            const date = parseSessionDate(s.id);
            const timeStr = date ? formatDate(date) : '--';
            const meta = s.metadata || {};
            const fps = +(meta.fps || 0) || 30;
            const agg = aggregateMeta(meta);
            const sizeStr = formatSize(+(s.size) || 0);
            const uniqueResolutions = [...new Set(agg.resolutions)];

            // 根据会话 ID 后缀或 metadata 判断数据格式。
            const formatMap = { lerobot: 'LeRobot v3.0', hdf5: 'HDF5 v1.0', rlds: 'RLDS v0.1' };
            let formatTag = '';
            if (meta.format && formatMap[meta.format]) {
                formatTag = `<span class="data-meta-tag" style="color:var(--primary);font-weight:600">${formatMap[meta.format]}</span>`;
            } else if (s.id.match(/_lerobot$/)) {
                formatTag = `<span class="data-meta-tag" style="color:var(--primary);font-weight:600">LeRobot v3.0</span>`;
            } else if (s.id.match(/_hdf5$/)) {
                formatTag = `<span class="data-meta-tag" style="color:var(--primary);font-weight:600">HDF5 v1.0</span>`;
            } else if (s.id.match(/_rlds$/)) {
                formatTag = `<span class="data-meta-tag" style="color:var(--primary);font-weight:600">RLDS v0.1</span>`;
            }

            // 新格式下构建每个槽位的详细信息。
            let slotDetail = '';
            const slots = meta.slots;
            if (slots && typeof slots === 'object' && Object.keys(slots).length > 0) {
                const parts = [];
                for (const slotName in slots) {
                    const slot = slots[slotName] || {};
                    const slotFc = slot.frameCount || {};
                    const pos = posLabels[slot.position] || slot.position || slotName;
                    const tags = [];
                    if (slotFc.color > 0) tags.push(`彩色${slotFc.color}帧`);
                    if (slotFc.depth > 0) tags.push(`深度${slotFc.depth}帧`);
                    if ((slotFc['ir-left'] || 0) > 0 || (slotFc['ir-right'] || 0) > 0) tags.push(`红外`);
                    const slotGrip = Math.max(slot.gripperCount || 0, (slot.gripper && slot.gripper.frames) || 0);
                    if (slotGrip > 0) tags.push(`夹爪${slotGrip}`);
                    if (tags.length > 0) parts.push(pos + ': ' + tags.join(' · '));
                }
                if (parts.length > 0) slotDetail = `<div class="data-item-slot-detail">${parts.join(' &nbsp;|&nbsp; ')}</div>`;
            }

            let infoTags = `
                ${formatTag}
                <span class="data-meta-tag"><span class="tag-icon">&#128337;</span> ${timeStr}</span>
                <span class="data-meta-tag"><span class="tag-icon">&#9889;</span> ${fps} FPS</span>
                ${uniqueResolutions.length > 0 ? `<span class="data-meta-tag"><span class="tag-icon">&#128250;</span> ${uniqueResolutions.join(', ')}</span>` : ''}
                <span class="data-meta-tag"><span class="tag-icon">&#128202;</span> ${sizeStr}</span>
            `;

            return `<div class="data-item">
                <div class="data-item-index">${i + 1}</div>
                <div class="data-item-info">
                    <div class="data-item-title">${formatSessionId(s.id)}</div>
                    <div class="data-item-meta">${infoTags}</div>
                    ${slotDetail}
                </div>
                <div class="data-item-actions">
                    <button class="btn btn-view" onclick="window.__dashboard.viewDetail('${escapeAttr(s.id)}','${dir}')">
                        <svg class="icon-eye icon-eye-default" viewBox="0 0 1024 1024" width="16" height="16"><path d="M928.842245 512.091074c0-5.006014-0.846274-9.193383-1.086751-9.691733-0.182149-2.480494-1.028423-7.001461-1.815345-9.374508-0.210801-0.590448-0.484024-1.209548-0.724501-1.799996-0.424672-1.360997-0.876973-2.691295-1.390673-3.749394-76.871785-168.137395-242.376213-281.144168-411.782507-281.144168-169.375595 0-334.865697 112.902396-411.388535 280.130072-0.921999 1.815345-1.572822 3.553942-1.981121 5.066389-0.181125 0.49835-0.39295 0.967024-0.558725 1.406023-1.512447 4.430916-1.542122 7.514137-1.421372 6.712889-0.710175 3.251044-1.360997 9.722432-1.360997 9.722432-0.181125 1.949398-0.181125 3.50687 0.030699 5.442966 0 0 0.649799 5.65479 0.968048 6.80294 0.090051 1.602498 0.483001 3.931542 0.951675 6.048763l-0.030699 0c0.408299 1.814322 0.968048 3.568269 1.738597 5.291516 0.393973 1.330298 0.862647 2.570545 1.270946 3.507894 76.976162 168.166047 242.436588 281.20352 411.781484 281.20352 169.436994 0 334.941422-112.945375 410.936233-279.328823 1.177825-2.177596 1.935072-4.233418 2.448772-6.018064 0.2415-0.543376 0.454348-1.027399 0.604774-1.511423 1.331321-3.872191 1.602498-7.227612 1.481747-7.227612l-0.028653 0.029676C928.027693 520.921183 928.842245 516.89959 928.842245 512.091074zM872.717993 514.146896c-0.029676 0.121773-0.091074 0.272199-0.151449 0.393973-0.090051 0.36225-0.240477 0.785899-0.332575 1.209548-68.403926 147.420561-212.830293 246.337431-360.191502 246.337431-146.997935 0-291.168476-98.642624-360.252901-246.578931-0.166799-0.5137-0.287549-0.998747-0.468674-1.481747-0.030699-0.484024-0.12075-0.876973-0.150426-1.150196-0.060375-0.300852-0.12075-0.724501-0.166799-1.088798l0-0.3776c0.166799-0.620124 0.286526-1.239224 0.347924-1.919722 0.12075-0.36225 0.211824-0.710175 0.347924-1.103124C220.132094 360.89042 364.680235 261.928524 512.041444 261.928524c147.420561 0 291.940049 99.051947 360.161826 246.322082 0.060375 0.287549 0.121773 0.530073 0.212848 0.726547 0.060375 0.2415 0.119727 0.484024 0.240477 0.740874 0.151449 1.104147 0.272199 2.192945 0.423649 2.736321C872.899118 513.028423 872.809067 513.572822 872.717993 514.146896z" fill="#8a8a8a"/><path d="M512.041444 373.060601c-76.598562 0-138.954749 62.325487-138.954749 138.939399 0 76.598562 62.356187 138.954749 138.954749 138.954749 76.598562 0 138.954749-62.356187 138.954749-138.954749C650.996193 435.386088 588.640006 373.060601 512.041444 373.060601zM512.041444 595.372849c-45.935192 0-83.371826-37.406958-83.371826-83.371826 0-45.950542 37.436634-83.356476 83.371826-83.356476 45.964868 0 83.373873 37.406958 83.373873 83.356476C595.414293 557.965891 558.006312 595.372849 512.041444 595.372849z" fill="#8a8a8a"/></svg>
                        <svg class="icon-eye icon-eye-hover" viewBox="0 0 1024 1024" width="16" height="16"><path d="M850.880127 442.989253l-48.063669-48.074925 106.260116-106.257046 48.061622 48.074925L850.880127 442.989253zM521.806343 901.576763c-95.499028 0-267.341814-68.695568-397.427568-282.381345C243.570411 418.524049 410.983305 342.043167 516.564969 342.043167c91.125417 0 214.228129 41.094954 386.967332 277.151227C740.13793 861.653495 617.308441 901.576763 521.806343 901.576763zM516.564969 399.574465c-87.37807 0-215.043704 50.818409-334.669222 224.850048 128.264269 176.190815 262.859733 219.634256 334.669222 219.634256 71.806419 0 211.782428-44.243667 329.452407-224.864374C735.765343 461.969537 607.008864 399.574465 516.564969 399.574465zM511.348154 760.39223c-77.976956 0-141.197836-63.206554-141.197836-141.197836 0-77.973886 63.22088-141.18351 141.197836-141.18351 77.973886 0 141.197836 63.209624 141.197836 141.18351C652.54599 697.185676 589.323063 760.39223 511.348154 760.39223zM511.348154 535.527856c-46.216601 0-83.680864 37.46631-83.680864 83.666538 0 46.216601 37.464263 83.669608 83.680864 83.669608 46.213531 0 83.652212-37.453007 83.652212-83.669608C595.001389 572.995189 557.561685 535.527856 511.348154 535.527856zM479.966423 122.423237l67.978231 0 0 172.563194-67.978231 0L479.966423 122.423237zM66.861804 338.635557l47.245024-48.822962 104.465237 107.943454-47.246047 48.820915L66.861804 338.635557z" fill="#8a8a8a"/></svg>
                        查看
                    </button>
                    <button class="btn btn-delete" onclick="window.__dashboard.confirmDelete('${escapeAttr(s.id)}','${escapeAttr(s.path)}','${dir}')">
                        <svg class="icon-trash" viewBox="0 0 1024 1024" width="16" height="16"><path d="M358.925672 596.814688v30.450522c0 17.248849 13.985526 31.233352 31.233352 31.233352 17.248849 0 31.233352-13.985526 31.233352-31.233352v-30.450522c0-17.248849-13.985526-31.233352-31.233352-31.233352-17.248849 0-31.233352 13.985526-31.233352 31.233352zM602.506317 596.814688v30.450522c0 17.248849 13.985526 31.233352 31.233352 31.233352s31.233352-13.985526 31.233351-31.233352v-30.450522c0-17.248849-13.984503-31.233352-31.233351-31.233352s-31.233352 13.985526-31.233352 31.233352zM437.047937 699.686636c-14.650675 9.104355-19.155269 28.360931-10.04989 43.01263 11.015891 17.73185 41.238216 47.740304 84.651982 47.740304 43.195801 0 73.79368-29.780257 85.059258-47.379077 9.216919-14.391778 5.03262-33.338293-9.237385-42.742477-14.270005-9.393951-33.576723-5.409197-43.159985 8.739035-0.12689 0.188288-13.049201 18.915815-32.661888 18.915815-19.028379 0-30.93864-17.274432-31.772634-18.530028-9.175987-14.412244-28.259624-18.788925-42.829458-9.756202zM907.576407 160.082952H699.352015v-26.882254c0-40.145325-32.692586-72.807213-72.878844-72.807213h-229.046626c-40.186258 0-72.878844 32.661887-72.878844 72.807213v26.882254H116.323309c-17.248849 0-31.233352 13.984503-31.233352 31.233352s13.984503 31.233352 31.233352 31.233351h791.253098c17.248849 0 31.233352-13.984503 31.233352-31.233351s-13.985526-31.233352-31.233352-31.233352z m-270.692119 0H387.014404v-26.882254c0-5.607718 4.768607-10.340509 10.411117-10.340509h229.046627c5.64251 0 10.411117 4.732791 10.411117 10.340509v26.882254z" fill="#8a8a8a"/><path d="M824.286446 259.279185c-17.248849 0-31.233352 13.984503-31.233352 31.233352v530.07261c0 40.089044-32.692586 72.705905-72.878844 72.705906H303.725466c-40.186258 0-72.878844-32.616862-72.878844-72.705906v-530.07261c0-17.248849-13.984503-31.233352-31.233352-31.233352s-31.233352 13.984503-31.233352 31.233352v530.07261c0 74.535577 60.71378 135.172609 135.345548 135.172609h416.448784c74.632791 0 135.345548-60.637032 135.345548-135.172609v-530.07261c0-17.248849-13.984503-31.233352-31.233352-31.233352z" fill="#8a8a8a"/><path d="M355.781052 259.279185c-17.248849 0-31.233352 13.984503-31.233351 31.233352v167.494758c0 17.248849 13.985526 31.233352 31.233351 31.233352 17.248849 0 31.233352-13.985526 31.233352-31.233352v-167.494758c0-17.248849-13.984503-31.233352-31.233352-31.233352zM699.352015 458.007295v-167.494758c0-17.248849-13.984503-31.233352-31.233351-31.233352s-31.233352 13.984503-31.233352 31.233352v167.494758c0 17.248849 13.985526 31.233352 31.233352 31.233352s31.233352-13.984503 31.233351-31.233352zM511.949858 489.240647c17.248849 0 31.233352-13.985526 31.233352-31.233352v-167.494758c0-17.248849-13.985526-31.233352-31.233352-31.233352s-31.233352 13.984503-31.233352 31.233352v167.494758c-0.001023 17.248849 13.984503 31.233352 31.233352 31.233352z" fill="#8a8a8a"/></svg>
                        删除
                    </button>
                </div>
            </div>`;
        }).join('');
    }

    // ===== 数据详情弹窗 =====
    async function viewDetail(id, dir) {
        const modal = document.getElementById('modal-overlay');
        const modalBody = document.getElementById('modal-body');
        const modalTitle = document.getElementById('modal-title');

        modalTitle.textContent = `数据详情 - ${id}`;
        modalBody.innerHTML = '<div class="empty-state"><div class="empty-state-icon">&#8987;</div><div class="empty-state-text">加载中...</div></div>';
        modal.classList.add('active');

        try {
            const res = await fetch(`${API}/api/data/detail?id=${encodeURIComponent(id)}&dir=${dir}`);
            const data = await res.json();

            if (data.error) {
                modalBody.innerHTML = `<div class="empty-state"><div class="empty-state-icon">&#9888;</div><div class="empty-state-text">加载失败</div><div class="empty-state-sub">${data.error}</div></div>`;
                return;
            }

            let html = '';
            const meta = data.metadata || {};
            const info = data.info || {};
            html += '<div class="modal-section"><div class="modal-section-title">基本信息</div><div class="metadata-grid">';

            const metaItems = [
                ['Session ID', formatSessionId(data.id)],
            ];

            // 格式版本显示。
            const formatMap = { lerobot: 'LeRobot v3.0', hdf5: 'HDF5 v1.0', rlds: 'RLDS v0.1' };
            if (info.codebase_version) {
                metaItems.push(['格式版本', info.codebase_version]);
            } else if (meta.format && formatMap[meta.format]) {
                metaItems.push(['格式版本', formatMap[meta.format]]);
            } else if (data.id.match(/_lerobot$/)) {
                metaItems.push(['格式版本', 'LeRobot v3.0']);
            } else if (data.id.match(/_hdf5$/)) {
                metaItems.push(['格式版本', 'HDF5 v1.0']);
            } else if (data.id.match(/_rlds$/)) {
                metaItems.push(['格式版本', 'RLDS v0.1']);
            }
            metaItems.push(['机器人类型', 'UMI夹爪']);

            metaItems.forEach(([key, val]) => {
                html += `<div class="metadata-item"><div class="metadata-key">${key}</div><div class="metadata-value">${val}</div></div>`;
            });
            html += '</div></div>';

            // 视频与传感器信息：同时兼容旧扁平格式和新槽位格式。
            const detailSlots = meta.slots;
            if (detailSlots && typeof detailSlots === 'object' && Object.keys(detailSlots).length > 0) {
                // 新版按槽位分目录格式。
                const posLabels = { head: '头部', left: '左手', right: '右手' };
                const videoTypeLabels = { color: '彩色', depth: '深度', 'ir': '红外' };
                html += '<div class="modal-section"><div class="modal-section-title">槽位数据详情</div>';
                for (const slotName in detailSlots) {
                    const slot = detailSlots[slotName] || {};
                    const pos = posLabels[slot.position] || slot.position || slotName;
                    const slotFc = slot.frameCount || {};
                    const slotVideos = slot.videos || {};
                    const slotGrip = Math.max(slot.gripperCount || 0, (slot.gripper && slot.gripper.frames) || 0);

                    const entries = [];
                    for (const vtype in slotFc) {
                        if (slotFc[vtype] > 0) {
                            const vi = slotVideos[vtype] || {};
                            const res = vi.width ? ` · ${vi.width}x${vi.height}` : '';
                            const label = videoTypeLabels[vtype] || vtype;
                            entries.push(`${label} ${slotFc[vtype]}帧${res}`);
                        }
                    }
                    if (slot.imuCount > 0) entries.push(`IMU ${slot.imuCount}`);
                    if (slotGrip > 0) entries.push(`夹爪 ${slotGrip}行`);
                    if ((slot.pose && slot.pose.frames) > 0) entries.push(`位姿 ${slot.pose.frames}`);
                    if (slot.pointCloudCount > 0) entries.push(`点云 ${slot.pointCloudCount}`);

                    if (entries.length > 0) {
                        html += `<div class="metadata-item" style="margin-bottom:6px">
                            <div class="metadata-key" style="font-weight:600">${pos} (${slotName})</div>
                            <div class="metadata-value">${entries.join(' &nbsp;·&nbsp; ')}</div>
                        </div>`;
                    }
                }
                html += '</div>';
            } else if (meta.videos) {
                // 旧版扁平目录格式。
                html += '<div class="modal-section"><div class="modal-section-title">视频信息</div>';
                ['color', 'depth'].forEach(type => {
                    const v = meta.videos[type];
                    if (!v) return;
                    const label = type === 'color' ? '彩色视频' : '深度视频';
                    html += `<div class="metadata-item" style="margin-bottom:6px">
                        <div class="metadata-key">${label}</div>
                        <div class="metadata-value">${v.format || ''} · ${v.width}x${v.height} · ${v.frames} 帧</div>
                    </div>`;
                });
                const imuCount = meta.imuCount || 0;
                const gripCount = Math.max(meta.gripperCount || 0, (meta.gripper && meta.gripper.frames) || 0);
                if (imuCount > 0 || gripCount > 0) {
                    const sensorParts = [];
                    if (imuCount > 0) sensorParts.push(`IMU ${imuCount}`);
                    if (gripCount > 0) sensorParts.push(`夹爪 ${gripCount}行`);
                    html += `<div class="metadata-item" style="margin-bottom:6px">
                        <div class="metadata-key">传感器</div>
                        <div class="metadata-value">${sensorParts.join(' · ')}</div>
                    </div>`;
                }
                html += '</div>';
            }

            const videoFiles = (data.files || []).filter(f => f.type === 'video');
            if (videoFiles.length > 0) {
                const posLabels = { head: '头部', left: '左手', right: '右手', 'Head-umi': '头部', 'Left-umi': '左手', 'Right-umi': '右手' };
                function getVideoLabel(name) {
                    // 尝试从路径中提取槽位前缀和视频类型。
                    const parts = name.split('/');
                    let slotLabel = '';
                    let vidLabel = '';
                    for (const part of parts) {
                        if (posLabels[part]) slotLabel = posLabels[part];
                        if (part.startsWith('color')) vidLabel = '彩色';
                        else if (part.startsWith('depth')) vidLabel = '深度';
                        else if (part.startsWith('ir')) vidLabel = '红外';
                    }
                    // 同时检查 color_video、depth_video 等路径片段。
                    if (!vidLabel) {
                        if (name.includes('color')) vidLabel = '彩色';
                        else if (name.includes('depth')) vidLabel = '深度';
                        else if (name.includes('ir')) vidLabel = '红外';
                    }
                    if (slotLabel && vidLabel) return slotLabel + ' · ' + vidLabel;
                    if (vidLabel) return vidLabel + '视频';
                    return parts.pop() || name;
                }
                html += '<div class="modal-section"><div class="modal-section-title">视频预览</div>';
                if (videoFiles.length > 1) {
                    html += '<div class="video-tabs">';
                    videoFiles.forEach((vf, vi) => {
                        html += `<button class="video-tab${vi === 0 ? ' active' : ''}" data-video-idx="${vi}">${getVideoLabel(vf.name)}</button>`;
                    });
                    html += '</div>';
                }
                videoFiles.forEach((vf, vi) => {
                    const videoPath = encodeURIComponent(data.path + '/' + vf.name);
                    html += `<div class="video-preview" style="${vi > 0 ? 'display:none;' : ''}"><video controls preload="metadata" style="width:100%;max-height:360px;background:#000;border-radius:6px;"><source src="/api/data/file?path=${videoPath}" type="video/mp4">浏览器不支持视频播放</video></div>`;
                });
                html += '</div>';
            }

            if (data.files && data.files.length > 0) {
                html += '<div class="modal-section"><div class="modal-section-title">文件列表</div>';
                data.files.forEach(f => {
                    const icon = f.type === 'video' ? '&#127909;' :
                                 f.type === 'csv' ? '&#128202;' :
                                 f.type === 'json' ? '&#128196;' :
                                 f.type === 'ply' ? '&#9729;' :
                                 f.type === 'parquet' ? '&#128451;' : '&#128196;';
                    html += `<div class="file-item">
                        <span class="file-icon">${icon}</span>
                        <span class="file-name">${f.name}</span>
                        <span class="file-size">${formatSize(f.size)}</span>
                        <span class="file-type-badge ${f.type}">${f.type}</span>
                    </div>`;
                });
                html += '</div>';
            }

            modalBody.innerHTML = html;

            // 视频预览标签切换。
            modalBody.querySelectorAll('.video-tab').forEach(tab => {
                tab.addEventListener('click', () => {
                    modalBody.querySelectorAll('.video-tab').forEach(t => t.classList.remove('active'));
                    tab.classList.add('active');
                    const idx = +tab.dataset.videoIdx;
                    modalBody.querySelectorAll('.video-preview').forEach((p, pi) => {
                        p.style.display = pi === idx ? '' : 'none';
                        const v = p.querySelector('video');
                        if (v) { v.pause(); v.currentTime = 0; }
                    });
                });
            });

        } catch (e) {
            modalBody.innerHTML = `<div class="empty-state"><div class="empty-state-icon">&#9888;</div><div class="empty-state-text">请求失败</div><div class="empty-state-sub">${e.message}</div></div>`;
        }
    }

    // ===== 删除数据 =====
    // 确认删除数据弹窗
    function confirmDelete(id, path, dir) {
        const modal = document.getElementById('modal-overlay');
        const modalBody = document.getElementById('modal-body');
        const modalTitle = document.getElementById('modal-title');

        modalTitle.textContent = '确认删除';
        modalBody.innerHTML = `
            <div class="confirm-dialog">
                <div class="confirm-dialog-icon">&#9888;&#65039;</div>
                <div class="confirm-dialog-text">确定要删除这条数据吗？</div>
                <div class="confirm-dialog-sub">${id}<br>此操作不可恢复，将删除本地文件夹及所有内容</div>
                <div class="confirm-dialog-actions">
                    <button class="btn-confirm-cancel" onclick="window.__dashboard.closeModal()">取消</button>
                    <button class="btn-confirm-delete" onclick="window.__dashboard.doDelete('${escapeAttr(id)}','${escapeAttr(path)}','${dir}')">确认删除</button>
                </div>
            </div>`;
        modal.classList.add('active');
    }

    async function doDelete(id, path, dir) {
        try {
            const res = await fetch(`${API}/api/data/delete`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ path: path })
            });
            const data = await res.json();
            if (data.deleted) {
                showToast('删除成功', 'success');
                closeModal();
                await loadAllData();
            } else {
                showToast('删除失败: ' + (data.error || ''), 'error');
            }
        } catch (e) {
            showToast('删除失败: ' + e.message, 'error');
        }
    }

    // 关闭模态框
    function closeModal() {
        document.getElementById('modal-overlay').classList.remove('active');
    }

    // ===== Toast 提示 =====
    // 显示 Toast 提示消息
    function showToast(msg, type) {
        const container = document.getElementById('toast-container');
        const toast = document.createElement('div');
        toast.className = `toast ${type || ''}`;
        toast.textContent = msg;
        container.appendChild(toast);
        setTimeout(() => {
            toast.style.opacity = '0';
            toast.style.transform = 'translateX(40px)';
            toast.style.transition = '0.3s ease';
            setTimeout(() => toast.remove(), 300);
        }, 3000);
    }

    // ===== 通用工具函数 =====
    // 会话ID转中文日期格式: "20260523_125201" → "2026年05月23日 12:52"
    function formatSessionId(id) {
        const m = id.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})(?:_(.+))?$/);
        if (m) {
            let result = `${m[1]}年${m[2]}月${m[3]}日 ${m[4]}:${m[5]}:${m[6]}`;
            if (m[7]) result += ` · ${m[7]}`;
            return result;
        }
        const m2 = id.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(?:_(.+))?$/);
        if (m2) {
            let result = `${m2[1]}年${m2[2]}月${m2[3]}日 ${m2[4]}:${m2[5]}`;
            if (m2[6]) result += ` · ${m2[6]}`;
            return result;
        }
        return id;
    }

    // 文件大小格式化（B/KB/MB/GB）
    function formatSize(bytes) {
        if (bytes === 0) return '0 B';
        const units = ['B', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(1024));
        return (bytes / Math.pow(1024, i)).toFixed(i > 0 ? 1 : 0) + ' ' + units[i];
    }

    function formatTimestamp(ms) {
        const d = new Date(ms);
        return d.toLocaleString('zh-CN', { year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit' });
    }

    // 日期格式化
    function formatDate(d) {
        return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')} ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
    }

    // HTML 属性转义（防止 XSS）
    function escapeAttr(str) {
        return String(str).replace(/'/g, "\\'").replace(/"/g, '&quot;');
    }

    // 十六进制颜色转 RGBA
    function hexToRgba(hex, alpha) {
        const r = parseInt(hex.slice(1, 3), 16);
        const g = parseInt(hex.slice(3, 5), 16);
        const b = parseInt(hex.slice(5, 7), 16);
        return `rgba(${r},${g},${b},${alpha})`;
    }

    // 暴露全局接口（供 HTML onclick 调用）
    window.__dashboard = { viewDetail, confirmDelete, doDelete, closeModal };
})();
