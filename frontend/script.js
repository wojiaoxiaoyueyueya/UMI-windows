// script.js - 采集控制台前端逻辑
// 功能：
// 1. 视频流开关控制（彩色/IMU/夹爪）
// 2. MJPEG 视频流实时显示
// 3. IMU 波形图绘制（加速度计/陀螺仪）
// 4. 数据录制（开始/停止/状态轮询）
// 5. 数据转换（选择会话、批量转换、进度轮询）
// 6. 设备信息查询
// 7. UMI V2 夹爪控制（位置/按键/LED）

(function() {
    // ---- 侧边栏收缩/展开（localStorage 跨页面联动） ----
    var SIDEBAR_KEY = 'sidebar_collapsed';
    function syncSidebars(collapsed) {
        document.querySelectorAll('.sidebar, .collect-sidebar, .eg-sidebar').forEach(function(sb) {
            if (collapsed) { sb.classList.add('collapsed'); } else { sb.classList.remove('collapsed'); }
        });
        if (collapsed) { document.body.classList.add('sidebar-collapsed'); } else { document.body.classList.remove('sidebar-collapsed'); }
        var btn = document.getElementById('sidebarToggleBtn');
        if (btn) btn.textContent = collapsed ? '›' : '‹';
    }
    function initSidebarToggle() {
        var collapsed = localStorage.getItem(SIDEBAR_KEY) === '1';
        syncSidebars(collapsed);
        var btn = document.getElementById('sidebarToggleBtn');
        if (btn) {
            btn.addEventListener('click', function(e) {
                e.stopPropagation();
                var nowCollapsed = !document.body.classList.contains('sidebar-collapsed');
                localStorage.setItem(SIDEBAR_KEY, nowCollapsed ? '1' : '0');
                syncSidebars(nowCollapsed);
            });
        }
        // 监听其他页面的 storage 变化，实现联动
        window.addEventListener('storage', function(e) {
            if (e.key === SIDEBAR_KEY) {
                syncSidebars(e.newValue === '1');
            }
        });
    }
    initSidebarToggle();

    // 每个流独立的状态 { key: true/false }
    var state = {};
    // 设备信息缓存
    var deviceInfo = { left: {}, right: {}, head: {} };
    var allDevices = []; // 所有检测到的设备列表
    var serverOnline = false;
    var currentPage = 'monitor';
    var lastToggleHash = '';
    var streamBackendKeys = {};
    var EG_MAX_POSITION_DEG = 4500;
    var EG_RPS_SCISSORS_A_DEG = 1800;
    var EG_RPS_SCISSORS_B_DEG = 3600;
    // 浏览器 HTTP/1.1 对同一主机最多 6 个并发连接，MJPEG 流会长期占用连接
    // 使用备用主机名访问 API，避免点云轮询被 MJPEG 连接池阻塞
    var pageHost = window.location.hostname;
    var pagePort = window.location.port || '8080';
    var altHost = (pageHost === '127.0.0.1') ? 'localhost' : '127.0.0.1';
    var API_ALT = 'http://' + altHost + ':' + pagePort;

    var videoGrid = document.getElementById('videoGrid');
    var gridCols = 2; // 默认两列

    // 在 videoGrid 外面包一层容器，按钮放在容器上（不会被 innerHTML 清除）
    var gridParent = videoGrid.parentNode;
    var gridWrapper = document.createElement('div');
    gridWrapper.style.position = 'relative';
    gridParent.insertBefore(gridWrapper, videoGrid);
    gridWrapper.appendChild(videoGrid);

    var layoutBtns = document.createElement('div');
    layoutBtns.className = 'video-grid-layout-btns';
    layoutBtns.innerHTML = '<button data-cols="2" class="active">X2</button><button data-cols="3">X3</button>';
    gridWrapper.appendChild(layoutBtns);
    layoutBtns.querySelectorAll('button').forEach(function(btn) {
        btn.addEventListener('click', function(e) {
            e.stopPropagation();
            gridCols = parseInt(btn.getAttribute('data-cols'));
            layoutBtns.querySelectorAll('button').forEach(function(b) { b.classList.remove('active'); });
            btn.classList.add('active');
            applyGridLayout();
        });
    });

    function applyGridLayout() {
        videoGrid.classList.remove('col-1','col-2','col-3');
        var cardCount = videoGrid.querySelectorAll('.video-card').length;
        if (cardCount <= 1) {
            videoGrid.style.gridTemplateColumns = '1fr';
            videoGrid.classList.add('col-1');
        } else {
            videoGrid.style.gridTemplateColumns = gridCols === 3 ? '1fr 1fr 1fr' : '1fr 1fr';
            videoGrid.classList.add(gridCols === 3 ? 'col-3' : 'col-2');
        }
    }

    var streamFps = {};
    var fpsOverlays = {};
    var statusDot = document.getElementById('statusDot');
    var statusText = document.getElementById('statusText');
    var clockEl = document.getElementById('clock');
    var gripperStatus = document.getElementById('gripperStatus');
    var rescanDevicesBtn = document.getElementById('rescanDevicesBtn');
    var scanningDevices = false;

    // 槽位映射：displayPos → backendSlot
    // 显示位置(左手/右手/头部) → 后端实际槽位(left/right/head)
    var slotMap = { left: 'left', right: 'right', head: 'head' };
    var headCameraSerial = ''; // 用户选择的头部摄像头序列号，空=不分配
    var handsSwapped = false; // 左右手是否已交换（持久状态，轮询不会覆盖）
    var gripperSlotMap = { left: 'left', right: 'right' }; // 固定映射，不可交换

    // 根据 headCameraSerial 和 deviceInfo 重建 slotMap
    // 核心约束：每个显示位置映射到不同的后端槽位
    function rebuildSlotMap() {
        var serialToSlot = {};
        ['left', 'right', 'head'].forEach(function(pos) {
            var info = deviceInfo[pos] || {};
            if (info.connected && info.serial) {
                serialToSlot[info.serial] = pos;
            }
        });

        var camSlots = [];
        Object.keys(serialToSlot).forEach(function(serial) {
            camSlots.push(serialToSlot[serial]);
        });

        // 用 used 集合确保每个后端槽位只被一个显示位置使用
        var used = {};
        function pickUnused() {
            for (var i = 0; i < 3; i++) {
                var s = ['left','right','head'][i];
                if (!used[s]) { used[s] = true; return s; }
            }
            return 'left';
        }

        if (headCameraSerial && serialToSlot[headCameraSerial]) {
            // 头部显示 → 选中的摄像头
            slotMap.head = serialToSlot[headCameraSerial];
            used[slotMap.head] = true;
            // 剩余摄像头 → 左手/右手
            var remaining = camSlots.filter(function(s) { return s !== slotMap.head; });
            slotMap.left = remaining.length > 0 ? (used[remaining[0]] = true, remaining[0]) : pickUnused();
            slotMap.right = remaining.length > 1 ? (used[remaining[1]] = true, remaining[1]) : pickUnused();
        } else {
            // 无头部：摄像头按顺序分配给左手/右手，空的显示位置映射到空后端槽位
            slotMap.left  = camSlots.length > 0 ? (used[camSlots[0]] = true, camSlots[0]) : pickUnused();
            slotMap.right = camSlots.length > 1 ? (used[camSlots[1]] = true, camSlots[1]) : pickUnused();
            slotMap.head  = camSlots.length > 2 ? (used[camSlots[2]] = true, camSlots[2]) : pickUnused();
        }

        // 应用交换状态
        if (handsSwapped) {
            var tmp = slotMap.left;
            slotMap.left = slotMap.right;
            slotMap.right = tmp;
        }
    }

    // 显示键名(如 left-color) → 后端流键名(如 right-color)
    function toBackendKey(displayKey) {
        var dash = displayKey.indexOf('-');
        if (dash < 0) return displayKey; // imu
        var displayPos = displayKey.substring(0, dash);
        var streamType = displayKey.substring(dash + 1);
        if (streamType === 'gripper') {
            // 夹爪使用 gripperSlotMap 映射
            return (gripperSlotMap[displayPos] || displayPos) + '-gripper';
        }
        return (slotMap[displayPos] || displayPos) + '-' + streamType;
    }

    // ---- 页面导航 ----
    var streamPrefs = null;
    function switchPage(pageName) {
        if (pageName === currentPage) return;
        document.querySelectorAll('.page').forEach(function(p) { p.classList.remove('active'); p.style.display = 'none'; });
        document.querySelectorAll('.nav-tab').forEach(function(t) { t.classList.remove('active'); });
        var page = document.getElementById('page-' + pageName);
        if (page) { page.style.display = ''; page.classList.add('active'); page.style.animation = 'none'; page.offsetHeight; page.style.animation = ''; }
        var tab = document.querySelector('.nav-tab[data-page="' + pageName + '"]');
        if (tab) tab.classList.add('active');

        if (currentPage === 'monitor') {
            streamPrefs = JSON.parse(JSON.stringify(state));
            stopAllStreamImgs();
            videoGrid.innerHTML = '';
            // 关闭所有活跃流
            var offCmds = [];
            Object.keys(state).forEach(function(k) { if (state[k]) offCmds.push(toggleStream(k, false)); });
            Promise.all(offCmds);
        }
        if (pageName === 'monitor') {
            var prefs = streamPrefs || {};
            buildCameraToggles();
            // 恢复之前活跃的流
            Object.keys(prefs).forEach(function(k) { if (prefs[k]) toggleStream(k, true); });
        }
        if (currentPage === 'collect' && recState.pollTimer) {
            clearInterval(recState.pollTimer);
            recState.pollTimer = null;
        }
        if (pageName === 'collect') initCollectPage();
        if (pageName === 'control' && window._drawLedStrip) setTimeout(window._drawLedStrip, 50);
        if (currentPage === 'convert' && cvState.pollTimer) {
            clearInterval(cvState.pollTimer);
            cvState.pollTimer = null;
        }
        currentPage = pageName;
    }
    document.querySelectorAll('.nav-tab').forEach(function(tab) {
        tab.addEventListener('click', function() { switchPage(tab.getAttribute('data-page')); });
    });

    // ---- 设备信息弹窗 ----
    var infoPopup = document.getElementById('infoPopup');
    var infoBtn = document.getElementById('infoBtn');
    var infoPopupClose = document.getElementById('infoPopupClose');
    var infoVisible = false;
    infoBtn.addEventListener('click', function(e) {
        e.stopPropagation();
        infoVisible = !infoVisible;
        infoPopup.style.display = infoVisible ? '' : 'none';
        if (gearMenu) gearMenu.classList.remove('open');
    });
    infoPopupClose.addEventListener('click', function(e) {
        e.stopPropagation();
        infoVisible = false;
        infoPopup.style.display = 'none';
    });
    document.addEventListener('click', function(e) {
        if (infoVisible && !infoPopup.contains(e.target) && e.target !== infoBtn) {
            infoVisible = false;
            infoPopup.style.display = 'none';
        }
    });

    var gearBtn = document.getElementById('gearBtn');
    var gearMenu = document.getElementById('gearMenu');
    if (gearBtn && gearMenu) {
        gearBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            gearMenu.classList.toggle('open');
            if (infoVisible) { infoVisible = false; infoPopup.style.display = 'none'; }
        });
        document.addEventListener('click', function(e) {
            if (!gearMenu.contains(e.target) && e.target !== gearBtn) gearMenu.classList.remove('open');
        });
    }

    var lastDataTime = null;
    function updateLastDataTime() { lastDataTime = new Date(); }

    // ---- 流控制：每个流独立开关 ----
    function toggleStream(key, on) {
        var action = on ? 'on' : 'off';
        var backendKey = on ? toBackendKey(key) : (streamBackendKeys[key] || toBackendKey(key));
        if (on) streamBackendKeys[key] = backendKey;
        else delete streamBackendKeys[key];
        state[key] = on;
        updateSwitchUI(key);
        updateActiveCount();
        updateVideoCards();
        return fetch('/api/control?stream=' + backendKey + '&action=' + action)
            .then(function() { serverOnline = true; statusDot.classList.add('online'); statusText.textContent = '已连接'; })
            .catch(function() {});
    }

    // 更新单个开关的 CSS 状态
    function updateSwitchUI(key) {
        var el = document.querySelector('.switch[data-stream="' + key + '"]');
        if (el) el.classList.toggle('active', !!state[key]);
    }

    // 收集当前真正可用的显示开关，摄像头和夹爪统一使用这一份结果。
    function getAvailableStreamKeys() {
        var availKeys = [];
        ['left', 'right', 'head'].forEach(function(displayPos) {
            var backendSlot = slotMap[displayPos] || displayPos;
            var info = deviceInfo[backendSlot] || {};
            if (info.connected) {
                getStreamTypes(info.type).forEach(function(st) {
                    availKeys.push(displayPos + '-' + st.key);
                });
            }
        });
        ['left', 'right'].forEach(function(displayPos) {
            var backendSlot = gripperSlotMap[displayPos] || displayPos;
            var gs = (deviceInfo.gripperSlots || {})[backendSlot] || {};
            if (gs.connected) availKeys.push(displayPos + '-gripper');
        });
        return availKeys;
    }

    function getAvailableStreamKeyMap() {
        var map = {};
        getAvailableStreamKeys().forEach(function(k) { map[k] = true; });
        return map;
    }

    // 更新活跃流计数
    function updateActiveCount() {
        var allBtn = document.getElementById('toggleAllBtn');
        if (!allBtn) return;
        var availKeys = getAvailableStreamKeys();
        var onCount = 0;
        availKeys.forEach(function(k) { if (state[k]) onCount++; });
        var el = document.getElementById('statActive');
        if (el) el.textContent = onCount;
        var allOn = availKeys.length > 0 && onCount >= availKeys.length;
        allBtn.textContent = allOn ? '一键全关' : '一键全开';
    }

    // ---- 设备信息轮询 ----
    function cameraSlotLabel(pos) {
        if (pos === 'left') return '左手摄像头';
        if (pos === 'right') return '右手摄像头';
        if (pos === 'head') return '头部摄像头';
        return pos || '未知槽位';
    }

    function gripperSlotLabel(pos) {
        if (pos === 'left') return '左夹爪';
        if (pos === 'right') return '右夹爪';
        return pos || '未知夹爪';
    }

    function deviceTypeLabel(type) {
        if (type === 'orbbec') return 'Orbbec';
        if (type === 'hikvision') return '海康';
        if (type === 'manual') return '手动夹爪';
        if (type === 'electric') return '电动夹爪';
        if (type === 'none' || !type) return '未连接';
        return type;
    }

    function renderDeviceCard(title, connected, detail, meta) {
        return '<div class="info-device-card' + (connected ? '' : ' offline') + '">'
            + '<div class="info-device-main">'
            + '<span class="info-device-title">' + escapeHtml(title) + '</span>'
            + '<span class="info-status ' + (connected ? 'online' : 'offline') + '">' + (connected ? '已连接' : '已断开') + '</span>'
            + '</div>'
            + '<div class="info-device-meta">' + escapeHtml(detail || (connected ? '已连接' : '当前未检测到设备')) + '</div>'
            + (meta ? '<div class="info-device-meta">' + escapeHtml(meta) + '</div>' : '')
            + '</div>';
    }

    function renderDetectedCard(title, detail, meta) {
        return '<div class="info-device-card">'
            + '<div class="info-device-main"><span class="info-device-title">' + escapeHtml(title) + '</span>'
            + '<span class="info-status online">已检测</span></div>'
            + '<div class="info-device-meta">' + escapeHtml(detail || '--') + '</div>'
            + (meta ? '<div class="info-device-meta">' + escapeHtml(meta) + '</div>' : '')
            + '</div>';
    }

    function renderInfoPopup(data, serviceDisconnected) {
        var el = document.getElementById('infoDeviceSummary');
        if (!el) return;

        var devices = serviceDisconnected ? [] : (data.devices || []);
        var grippers = serviceDisconnected ? [] : (data.grippers || []);
        var connectedCameraCount = devices.filter(function(dev) { return !!dev.serial; }).length;
        var connectedGripperCount = grippers.filter(function(g) { return g.connected !== false; }).length;

        var html = '';
        if (serviceDisconnected) {
            html += '<div class="info-empty">服务未连接，设备状态暂时不可用</div>';
        }
        html += '<div class="info-summary-grid">'
            + '<div class="info-summary-card"><span>摄像头数量</span><strong>' + connectedCameraCount + '</strong><span>当前检测到</span></div>'
            + '<div class="info-summary-card"><span>夹爪数量</span><strong>' + connectedGripperCount + '</strong><span>当前检测到</span></div>'
            + '</div>';

        html += '<div class="info-section-label">摄像头设备</div><div class="info-device-list">';
        if (devices.length === 0) {
            html += '<div class="info-empty">未检测到摄像头</div>';
        } else {
            devices.forEach(function(dev, idx) {
                html += renderDetectedCard(
                    '摄像头 ' + (idx + 1) + ' · ' + deviceTypeLabel(dev.type),
                    dev.name || '未返回设备名称',
                    'SN: ' + (dev.serial || '--')
                );
            });
        }
        html += '</div>';

        html += '<div class="info-section-label">夹爪设备</div><div class="info-device-list">';
        if (grippers.length === 0) {
            html += '<div class="info-empty">未检测到夹爪</div>';
        } else {
            grippers.forEach(function(g, idx) {
                html += renderDetectedCard(
                    '夹爪 ' + (idx + 1) + ' · ' + deviceTypeLabel(g.type),
                    '端口: ' + (g.port || '--'),
                    g.connected ? '状态: 已连接' : '状态: 已断开'
                );
            });
        }
        html += '</div>';
        el.innerHTML = html;
    }

    function fetchDeviceInfo() {
        return fetch('/api/devices').then(function(r){return r.json();}).then(function(d) {
            var slots = d.slots || {};
            allDevices = d.devices || [];
            ['left', 'right', 'head'].forEach(function(pos) {
                var s = slots[pos] || {};
                deviceInfo[pos] = s;
            });
            var gripperSlots = d.gripperSlots || {};
            deviceInfo.gripperSlots = gripperSlots;
            renderInfoPopup(d, false);
            var hasAnyGripper = false;
            for (var gpos in gripperSlots) { if (gripperSlots[gpos].connected) hasAnyGripper = true; }
            if (gripperStatus) {
                gripperStatus.textContent = hasAnyGripper ? '已连接' : '未连接';
                gripperStatus.style.color = hasAnyGripper ? 'var(--success)' : 'var(--text-dim)';
            }
            serverOnline = true;
            statusDot.classList.add('online');
            statusText.textContent = '已连接';
            if (currentPage === 'monitor') { rebuildSlotMap(); buildCameraToggles(); buildCameraAssignPanel(); }
        }).catch(function(){
            serverOnline = false;
            statusDot.classList.remove('online');
            statusText.textContent = '未连接';
            renderInfoPopup({}, true);
        });
    }
    window.fetchDeviceInfo = fetchDeviceInfo;

    function closeActiveStreamsBeforeScan() {
        var cmds = [];
        Object.keys(state).forEach(function(k) {
            if (!state[k]) return;
            var backendKey = streamBackendKeys[k] || toBackendKey(k);
            delete streamBackendKeys[k];
            state[k] = false;
            cmds.push(fetch('/api/control?stream=' + backendKey + '&action=off').catch(function(){}));
        });
        stopAllStreamImgs();
        updateActiveCount();
        return Promise.all(cmds);
    }

    function rescanDevices() {
        if (scanningDevices) return;
        scanningDevices = true;
        if (rescanDevicesBtn) {
            rescanDevicesBtn.disabled = true;
            rescanDevicesBtn.textContent = '正在扫描...';
        }
        closeActiveStreamsBeforeScan()
            .then(function() {
                return fetch('/api/scan', { method: 'POST' });
            })
            .then(function(r) { return r.json(); })
            .then(function(d) {
                allDevices = d.devices || [];
                lastToggleHash = '';
                assignPanelHash = '';
                renderInfoPopup(d, false);
                return fetchDeviceInfo();
            })
            .then(function() {
                if (currentPage === 'monitor') {
                    rebuildSlotMap();
                    buildCameraToggles();
                    buildCameraAssignPanel();
                }
                showToast('设备扫描完成', 'success');
            })
            .catch(function() {
                showToast('扫描失败，请检查服务或设备连接', 'error');
            })
            .finally(function() {
                scanningDevices = false;
                if (rescanDevicesBtn) {
                    rescanDevicesBtn.disabled = false;
                    rescanDevicesBtn.textContent = '重新扫描设备';
                }
            });
    }
    window.rescanDevices = rescanDevices;
    if (rescanDevicesBtn) {
        rescanDevicesBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            rescanDevices();
        });
    }
    setInterval(fetchDeviceInfo, 2000);

    // ---- 动态开关生成 ----
    // 根据设备类型决定可用流：奥比=彩色+深度+红外, 海康=彩色
    function getStreamTypes(deviceType) {
        if (deviceType === 'orbbec') return [
            { key: 'color', label: '彩色', sub: 'RGB' },
            { key: 'depth', label: '深度', sub: 'Depth' },
            { key: 'ir-left',  label: '左红外', sub: 'IR-L' },
            { key: 'ir-right', label: '右红外', sub: 'IR-R' },
            { key: 'pointcloud', label: '点云', sub: '3D' }
        ];
        if (deviceType === 'hikvision') return [
            { key: 'color', label: '彩色', sub: 'RGB' }
        ];
        return [{ key: 'color', label: '彩色', sub: 'RGB' }];
    }

    function buildCameraToggles() {
        var container = document.getElementById('cameraToggles');
        if (!container) return;
        var hash = JSON.stringify(deviceInfo) + '|' + JSON.stringify(slotMap) + '|' + headCameraSerial;
        if (hash === lastToggleHash) return;
        lastToggleHash = hash;
        var availableMap = getAvailableStreamKeyMap();
        var changedActiveState = false;
        // 设备变化时只关闭已经不可用的流，仍然在线的设备继续保持当前开关状态。
        Object.keys(state).forEach(function(k) {
            if (state[k] && !availableMap[k]) {
                state[k] = false;
                changedActiveState = true;
                var bk = streamBackendKeys[k] || toBackendKey(k);
                delete streamBackendKeys[k];
                fetch('/api/control?stream=' + bk + '&action=off').catch(function(){});
            }
        });

        var html = '';
        var positions = [
            { key: 'left', label: '左手摄像头', short: '左手' },
            { key: 'right', label: '右手摄像头', short: '右手' },
            { key: 'head', label: '头部摄像头', short: '头部' }
        ];
        positions.forEach(function(pos) {
            var displayPos = pos.key;
            var backendSlot = slotMap[displayPos] || displayPos;
            var info = deviceInfo[backendSlot] || {};
            var posLabel = pos.label;
            var shortLabel = pos.short;
            var typeLabel = info.type === 'orbbec' ? 'Orbbec' : (info.type === 'hikvision' ? '海康' : '未检测');

            html += '<div class="sidebar-section">';
            html += '<div class="sidebar-title">' + posLabel + ' (' + typeLabel + ')</div>';

            if (info.connected) {
                var streamTypes = getStreamTypes(info.type);
                streamTypes.forEach(function(st) {
                    var stateKey = displayPos + '-' + st.key;
                    // 确保 state 中有这个键
                    if (!(stateKey in state)) state[stateKey] = false;
                    html += '<div class="toggle-row">';
                    html += '<div class="toggle-info">';
                    html += '<span class="toggle-label">' + shortLabel + '-' + st.label + '</span>';
                    html += '<span class="toggle-sub">' + st.sub + '</span>';
                    html += '</div>';
                    html += '<div class="switch' + (state[stateKey] ? ' active' : '') + '" data-stream="' + stateKey + '"><div class="switch-slider"></div></div>';
                    html += '</div>';
                });
            } else {
                html += '<div class="toggle-sub" style="padding:4px 0;color:var(--text-dim)">未连接</div>';
            }
            html += '</div>';
        });

        // 传感器
        html += '<div class="sidebar-section">';
        html += '<div class="sidebar-title">传感器</div>';
        // 夹爪：只要有任何夹爪连接，就显示左/右两个开关
        var gripperSlots = (deviceInfo.gripperSlots || {});
        var hasAnyGripper = false;
        for (var _gp in gripperSlots) { if (gripperSlots[_gp].connected) hasAnyGripper = true; }
        if (hasAnyGripper) {
            ['left', 'right'].forEach(function(displayPos) {
                var backendSlot = gripperSlotMap[displayPos] || displayPos;
                var gs = gripperSlots[backendSlot] || {};
                if (!gs.connected) return; // 未连接的不显示开关
                var key = displayPos + '-gripper';
                if (!(key in state)) state[key] = false;
                var label = displayPos === 'left' ? '左夹爪' : '右夹爪';
                var sub = gs.type === 'manual' ? '手动' : (gs.type === 'electric' ? '电动' : '夹爪');
                html += '<div class="toggle-row"><div class="toggle-info"><span class="toggle-label">' + label + '</span><span class="toggle-sub">' + sub + '</span></div>';
                html += '<div class="switch' + (state[key] ? ' active' : '') + '" data-stream="' + key + '"><div class="switch-slider"></div></div></div>';
            });
        }
        html += '</div>';

        container.innerHTML = html;
        updateActiveCount();
        if (changedActiveState) updateVideoCards();

        // 绑定每个开关的点击事件
        container.querySelectorAll('.switch[data-stream]').forEach(function(sw) {
            sw.addEventListener('click', function(e) {
                e.stopPropagation();
                var key = sw.getAttribute('data-stream');
                var isOn = !!state[key];
                toggleStream(key, !isOn);
            });
        });
    }

    // ---- 摄像头分配面板 ----
    var assignPanelHash = '';
    function buildCameraAssignPanel() {
        var panel = document.getElementById('cameraAssignPanel');
        if (!panel) return;
        if (panel.querySelector('select:focus')) return;

        // 从 deviceInfo 收集已连接的摄像头（serial 与 rebuildSlotMap 一致）
        var connectedCams = [];
        var seenSerials = {};
        ['left', 'right', 'head'].forEach(function(pos) {
            var info = deviceInfo[pos] || {};
            if (info.connected && info.serial && !seenSerials[info.serial]) {
                seenSerials[info.serial] = true;
                var typeLabel = info.type === 'orbbec' ? 'Orbbec' : (info.type === 'hikvision' ? '海康' : info.type || '未知');
                connectedCams.push({
                    serial: info.serial,
                    label: typeLabel + ' ' + (info.name || info.serial)
                });
            }
        });

        var hash = JSON.stringify(connectedCams) + '|headSerial:' + headCameraSerial;
        if (hash === assignPanelHash) return;
        assignPanelHash = hash;

        if (connectedCams.length === 0) {
            panel.innerHTML = '<div style="color:var(--text-dim);padding:4px 0">未检测到摄像头</div>';
            return;
        }

        var html = '<div style="margin:4px 0">';
        html += '<select id="headCameraSelect" style="width:100%;font-size:11px;padding:3px 4px;border-radius:4px;border:1px solid var(--border-color);background:var(--bg-primary);color:var(--text);box-sizing:border-box">';
        html += '<option value=""' + (!headCameraSerial ? ' selected' : '') + '>不分配头部摄像头</option>';
        connectedCams.forEach(function(cam) {
            var selected = (cam.serial === headCameraSerial) ? ' selected' : '';
            html += '<option value="' + cam.serial + '"' + selected + '>' + cam.label + '</option>';
        });
        html += '</select></div>';
        panel.innerHTML = html;

        // 绑定事件 - 纯前端映射，不调后端
        var sel = document.getElementById('headCameraSelect');
        if (sel) {
            sel.addEventListener('change', function() {
                var activeKeys = Object.keys(state).filter(function(k) { return state[k]; });
                var oldBackendKeys = activeKeys.map(function(k) { return streamBackendKeys[k] || toBackendKey(k); });
                activeKeys.forEach(function(k) { delete streamBackendKeys[k]; });
                headCameraSerial = sel.value;
                // 先关闭旧映射对应的后端流，再按新映射恢复之前已经开启的显示开关
                oldBackendKeys.forEach(function(bk) {
                    fetch('/api/control?stream=' + bk + '&action=off').catch(function(){});
                });
                stopAllStreamImgs();
                lastToggleHash = '';
                assignPanelHash = '';
                rebuildSlotMap();
                buildCameraToggles();
                buildCameraAssignPanel();
                if (activeKeys.length > 0) {
                    activeKeys.forEach(function(k) {
                        state[k] = true;
                        updateSwitchUI(k);
                    });
                    updateVideoCards();
                    activeKeys.forEach(function(k) {
                        toggleStream(k, true);
                    });
                } else {
                    videoGrid.className = 'video-grid guide-mode';
                    videoGrid.innerHTML = emptyGuideHtml();
                    var _gs2 = document.getElementById('gripperSection');
                    if (_gs2) _gs2.style.display = 'none';
                }
                if (headCameraSerial) {
                    showToast('已选择头部摄像头', 'success');
                } else {
                    showToast('已取消头部摄像头', 'success');
                }
            });
        }
    }

    // ---- 初始化 ----
    // 1. 获取设备信息
    // 2. 生成开关（全部默认 OFF）
    // 3. 关闭后端所有流
    fetchDeviceInfo().then(function() {
        rebuildSlotMap();
        // 确保所有 per-slot state 键存在
        ['left', 'right', 'head'].forEach(function(pos) {
            var info = deviceInfo[pos] || {};
            if (info.connected) {
                var streamTypes = getStreamTypes(info.type);
                streamTypes.forEach(function(st) {
                    var key = pos + '-' + st.key;
                    if (!(key in state)) state[key] = false;
                });
            }
        });
        buildCameraToggles();
        buildCameraAssignPanel();
        // 默认显示操作指南
        if (Object.keys(state).every(function(k) { return !state[k]; })) {
            videoGrid.className = 'video-grid guide-mode';
            videoGrid.innerHTML = emptyGuideHtml();
        }
        // 关闭后端所有流（用后端键名）
        var cmds = [];
        ['left', 'right'].forEach(function(gp) { cmds.push(gp + '-gripper'); });
        ['left', 'right', 'head'].forEach(function(pos) {
            var info = deviceInfo[pos] || {};
            if (info.connected) {
                getStreamTypes(info.type).forEach(function(st) {
                    cmds.push(pos + '-' + st.key); // 这里 pos 是后端槽位名
                });
            }
        });
        Promise.all(cmds.map(function(k) {
            return fetch('/api/control?stream=' + k + '&action=off').catch(function(){});
        }));
    });

    // ---- 视频流管理 ----
    var activeImgs = {};
    function stopStreamImg(key) {
        if (activeImgs[key]) { activeImgs[key].src = ''; delete activeImgs[key]; }
    }
    function stopAllStreamImgs() { Object.keys(activeImgs).forEach(stopStreamImg); Object.keys(pcViewers).forEach(stopPointCloudViewer); streamFps = {}; fpsOverlays = {}; }

    function emptyGuideHtml() {
        return '<div class="empty-state"><div class="guide-panel">'
            + '<div class="guide-title">操作指南</div>'
            + '<div class="guide-item"><span class="guide-dot" style="background:#38bdf8"></span><span class="guide-text"><strong>左手 / 右手摄像头</strong> — 开启彩色、深度、红外视频流</span></div>'
            + '<div class="guide-item"><span class="guide-dot" style="background:#c084fc"></span><span class="guide-text"><strong>头部摄像头</strong> — 在分配面板选择设备后开启视频流</span></div>'
            + '<div class="guide-item"><span class="guide-dot" style="background:#818cf8"></span><span class="guide-text"><strong>摄像头分配</strong> — 选择一个已连接设备作为头部摄像头</span></div>'
            + '<div class="guide-item"><span class="guide-dot" style="background:#34d399"></span><span class="guide-text"><strong>交换左右手</strong> — 切换左右手摄像头的画面</span></div>'
            + '<div class="guide-item"><span class="guide-dot" style="background:#fbbf24"></span><span class="guide-text"><strong>一键全开 / 全关</strong> — 快速开启或关闭所有已连接设备的视频流和传感器</span></div>'
            + '<div class="guide-item"><span class="guide-dot" style="background:#f87171"></span><span class="guide-text"><strong>传感器</strong> — 开启夹爪数据采集</span></div>'
            + '</div></div>';
    }

    // ---- 视频卡片管理 ----
    function updateVideoCards() {
        var allStreams = [
            {key:'left-color',  label:'左手-彩色',  tag:'RGB'},
            {key:'left-depth',  label:'左手-深度',  tag:'Depth'},
            {key:'left-ir-left',  label:'左手-左红外',  tag:'IR-L'},
            {key:'left-ir-right', label:'左手-右红外',  tag:'IR-R'},
            {key:'right-color', label:'右手-彩色',  tag:'RGB'},
            {key:'right-depth', label:'右手-深度',  tag:'Depth'},
            {key:'right-ir-left',  label:'右手-左红外',  tag:'IR-L'},
            {key:'right-ir-right', label:'右手-右红外',  tag:'IR-R'},
            {key:'head-color',  label:'头部-彩色',  tag:'RGB'},
            {key:'head-depth',  label:'头部-深度',  tag:'Depth'},
            {key:'head-ir-left',  label:'头部-左红外',  tag:'IR-L'},
            {key:'head-ir-right', label:'头部-右红外',  tag:'IR-R'},
            {key:'left-pointcloud',  label:'左手-点云',  tag:'3D'},
            {key:'right-pointcloud', label:'右手-点云',  tag:'3D'},
            {key:'head-pointcloud',  label:'头部-点云',  tag:'3D'}
        ];
        // 根据设备能力过滤
        allStreams.forEach(function(s) {
            var bk = toBackendKey(s.key);
            var firstDash = bk.indexOf('-');
            var slot = bk.substring(0, firstDash);
            var type = bk.substring(firstDash + 1);
            var info = deviceInfo[slot] || {};
            if (type === 'depth' && !info.hasDepth) s.skip = true;
            if ((type === 'ir-left' || type === 'ir-right') && !info.hasIR) s.skip = true;
            if (type === 'pointcloud' && !info.hasDepth) s.skip = true;
        });
        allStreams = allStreams.filter(function(s) { return !s.skip; });
        var want = allStreams.filter(function(s) { return state[s.key]; });
        var wantK = want.map(function(s) { return s.key; }).sort().join(',');
        var allActiveKeys = Object.keys(activeImgs).concat(Object.keys(pcViewers));
        var prevK = allActiveKeys.filter(function(v, i, a) { return a.indexOf(v) === i; }).sort().join(',');

        if (wantK !== prevK) {
            stopAllStreamImgs();
            videoGrid.innerHTML = '';
            if (want.length === 0 && !state['left-gripper'] && !state['right-gripper']) {
                videoGrid.className = 'video-grid guide-mode';
                videoGrid.innerHTML = emptyGuideHtml();
                var _gs = document.getElementById('gripperSection');
                if (_gs) _gs.style.display = 'none';
                return;
            }
            videoGrid.className = 'video-grid';
            var ts = Date.now();
            want.forEach(function(s) {
                var card = document.createElement('div');
                card.className = 'video-card';
                var isPointCloud = s.key.indexOf('pointcloud') >= 0;
                if (isPointCloud) card.style.gridColumn = '1 / -1';
                var id = isPointCloud ? 'pc_' + s.key : 'img_' + s.key;
                card.innerHTML = '<div class="video-card-header"><span>'+s.label+'</span><span class="fps-badge" id="fps_'+s.key+'">-- fps</span><span class="tag">'+s.tag+'</span></div>'
                    +'<div class="video-frame' + (isPointCloud ? ' pc-frame' : '') + '">' + (isPointCloud
                        ? '<div class="pc-overlay-btns"><button class="pc-mode-btn active" data-mode="depth">深度</button><button class="pc-mode-btn" data-mode="rgb">彩色</button><button class="pc-mode-btn" data-mode="ir">红外</button><button class="pc-mode-btn" data-mode="height">高度</button></div>'
                          +'<canvas id="'+id+'" class="pointcloud-canvas" style="width:100%;height:100%;background:#1a1a2e;"></canvas><div class="placeholder">等待点云数据...</div>'
                        : '<img id="'+id+'" alt="'+s.label+'">') + '<div class="placeholder" style="display:none">等待中...</div></div>';
                videoGrid.appendChild(card);
                (function(c) { requestAnimationFrame(function() { requestAnimationFrame(function() { c.classList.add('visible'); }); }); })(card);
                streamFps[s.key] = { count: 0, fps: 0, lastTime: performance.now() };
                fpsOverlays[s.key] = document.getElementById('fps_' + s.key);

                if (isPointCloud) {
                    var canvas = document.getElementById(id);
                    var bk = toBackendKey(s.key);
                    var slot = bk.replace('-pointcloud', '');
                    console.log('[PC] createCard key=' + s.key + ' slot=' + slot + ' canvas=' + !!canvas + ' url=/api/pointcloud/' + slot);
                    startPointCloudViewer(s.key, canvas, slot);
                    // 模式按钮事件绑定
                    var pcCard = canvas.closest('.video-frame');
                    pcCard.querySelectorAll('.pc-mode-btn').forEach(function(btn) {
                        btn.addEventListener('click', function(e) {
                            e.stopPropagation();
                            pcCard.querySelectorAll('.pc-mode-btn').forEach(function(b) { b.classList.remove('active'); });
                            btn.classList.add('active');
                            var viewer = pcViewers[s.key];
                            if (viewer) { viewer.mode = btn.getAttribute('data-mode'); viewer._fitted = false; }
                        });
                    });
                } else {
                    // MJPEG 视频流
                    var img = document.getElementById(id);
                    img.onload = function() {
                        var ph = this.parentElement.querySelector('.placeholder');
                        if (ph) ph.style.display = 'none';
                        if (streamFps[s.key]) streamFps[s.key].count++;
                    };
                    activeImgs[s.key] = img;
                    var bk = toBackendKey(s.key);
                    var dash = bk.indexOf('-');
                    img.src = '/stream/' + bk.substring(0, dash) + '/' + bk.substring(dash + 1) + '?t=' + ts;
                }
            });
        }
        // 布局
        applyGridLayout();

        // 夹爪面板：根据类型切换手动/电动内容
        var gripperSection = document.getElementById('gripperSection');
        var anyGripperOn = state['left-gripper'] || state['right-gripper'];
        if (gripperSection) gripperSection.style.display = anyGripperOn ? '' : 'none';
        // 开启夹爪时隐藏操作指南
        if (anyGripperOn && videoGrid.classList.contains('guide-mode')) {
            videoGrid.className = 'video-grid';
            videoGrid.innerHTML = '';
        }
        // 关闭全部且无视频流时恢复操作指南
        if (!anyGripperOn && want.length === 0 && !videoGrid.classList.contains('guide-mode')) {
            videoGrid.className = 'video-grid guide-mode';
            videoGrid.innerHTML = emptyGuideHtml();
        }
        ['left', 'right'].forEach(function(displayPos) {
            var panel = document.getElementById(displayPos === 'left' ? 'gripperLeftPanel' : 'gripperRightPanel');
            var key = displayPos + '-gripper';
            if (panel) panel.style.display = state[key] ? '' : 'none';
            var backendSlot = gripperSlotMap[displayPos] || displayPos;
            var gs = (deviceInfo.gripperSlots || {})[backendSlot] || {};
            var isElectric = gs.type === 'electric';
            var manualEl = document.getElementById(displayPos === 'left' ? 'gripperLeftManual' : 'gripperRightManual');
            var electricEl = document.getElementById(displayPos === 'left' ? 'gripperLeftElectric' : 'gripperRightElectric');
            if (manualEl) manualEl.style.display = isElectric ? 'none' : '';
            if (electricEl) electricEl.style.display = isElectric ? 'flex' : 'none';
        });
    }
    function updateClock() { clockEl.textContent = new Date().toLocaleTimeString('zh-CN', {hour12:false}); }
    setInterval(updateClock, 1000); updateClock();

    // FPS 刷新定时器：优先显示真实采集帧率；预览编码帧率只作为兜底参考。
    setInterval(function() {
        fetch('/api/fps').then(function(r) { return r.json(); }).then(function(d) {
            // 建立 backend slot → display pos 的反向映射
            var revMap = {};
            for (var dp in slotMap) { revMap[slotMap[dp]] = dp; }
            var capture = d.capture || {};
            var preview = d.preview || {};
            var fpsKeys = {};
            Object.keys(preview).forEach(function(key) { fpsKeys[key] = true; });
            Object.keys(capture).forEach(function(key) { fpsKeys[key] = true; });
            for (var bkey in fpsKeys) {
                var captureValue = capture[bkey];
                var previewValue = preview[bkey];
                var value = (typeof captureValue === 'number' && captureValue > 0) ? captureValue : previewValue;
                if (typeof value !== 'number' || value <= 0) continue;
                // 先直接匹配，再尝试反向映射
                var el = fpsOverlays[bkey];
                if (!el) {
                    var dash = bkey.indexOf('-');
                    if (dash > 0) {
                        var bslot = bkey.substring(0, dash);
                        var stream = bkey.substring(dash + 1);
                        var dpos = revMap[bslot] || bslot;
                        el = fpsOverlays[dpos + '-' + stream];
                    }
                }
                if (el) {
                    el.textContent = value.toFixed(1) + ' fps';
                    el.title = (value === captureValue)
                        ? '真实采集帧率：设备数据进入后端的回调频率'
                        : '预览编码帧率：仅表示网页 MJPEG 预览刷新速度';
                }
            }
        }).catch(function() {});
    }, 1000);


    // ---- 夹爪轮询（按槽位）----
    var manualGripperButtonState = {
        left: { button1: false, button2: false },
        right: { button1: false, button2: false }
    };

    function isManualButtonPressed(value) {
        return value === 1 || value === 2 || value === true;
    }

    function handleManualGripperButtons(displayPos, data) {
        if (!data || data.connected === false) {
            manualGripperButtonState[displayPos] = { button1: false, button2: false };
            return;
        }
        var prev = manualGripperButtonState[displayPos] || { button1: false, button2: false };
        var button1Pressed = isManualButtonPressed(data.button1);
        var button2Pressed = isManualButtonPressed(data.button2);

        if (button1Pressed && !prev.button1) {
            triggerManualGripperRecordAction(displayPos, 'start');
        }
        if (button2Pressed && !prev.button2) {
            triggerManualGripperRecordAction(displayPos, 'stop');
        }

        manualGripperButtonState[displayPos] = {
            button1: button1Pressed,
            button2: button2Pressed
        };
    }

    function triggerManualGripperRecordAction(displayPos, action) {
        var label = displayPos === 'left' ? '左夹爪' : '右夹爪';
        if (action === 'start') {
            if (recState.recording) {
                showToast('已在录制中，忽略' + label + '上键', 'warning');
                return;
            }
            if (recState.finalizing) {
                // 后台仍在保存上一段视频，允许开始新录制（后端会等待上一次 finalize 完成）
            }
            showToast(label + '上键：开始录制', 'success');
            startRecording({ fallbackGripperPos: displayPos });
        } else if (action === 'stop') {
            if (!recState.recording) {
                if (!recState.finalizing) showToast(label + '下键：当前没有录制任务', 'warning');
                return;
            }
            showToast(label + '下键：停止录制', 'success');
            stopRecording();
        }
    }

    function updateGripperDisplay(displayPos, data) {
        handleManualGripperButtons(displayPos, data);
        var prefix = displayPos === 'left' ? 'gripperLeft' : 'gripperRight';
        // 闭合度：position 0=闭合 1=张开，显示为闭合百分比 (1-position)*100
        var pos = data.position !== undefined ? data.position : 0;
        var closePct = Math.round(pos * 100);
        var posEl = document.getElementById(prefix + 'Position');
        if (posEl) posEl.textContent = closePct + '%';
        // 进度条
        var barEl = document.getElementById(prefix + 'Bar');
        if (barEl) {
            barEl.style.width = closePct + '%';
            barEl.style.background = closePct > 80 ? 'linear-gradient(90deg,#ef4444,#f97316)' :
                                     closePct > 50 ? 'linear-gradient(90deg,#f59e0b,#eab308)' :
                                     'linear-gradient(90deg,#3b82f6,#8b5cf6)';
        }
        // 按键指示灯：默认绿色，按下变红色，松开回绿
        var btn1El = document.getElementById(prefix + 'Btn1');
        var btn2El = document.getElementById(prefix + 'Btn2');
        if (btn1El) btn1El.style.background = (data.button1 === 1 || data.button1 === 2) ? '#ef4444' : '#22c55e';
        if (btn2El) btn2El.style.background = (data.button2 === 1 || data.button2 === 2) ? '#ef4444' : '#22c55e';
        // 连接状态
        var connEl = document.getElementById(prefix + 'Conn');
        if (connEl) connEl.style.background = data.connected ? '#22c55e' : '#ef4444';
        // 顶部副标题状态。
        var valuesEl = document.getElementById(prefix + 'Values');
        if (valuesEl) valuesEl.textContent = closePct + '% | P:' + pos.toFixed(3) + ' B1:' + (data.button1 || 0) + ' B2:' + (data.button2 || 0);
        updateLastDataTime();
    }
    var gripperPollTimer = setInterval(function() {
        if (!serverOnline || currentPage !== 'monitor') return;
        ['left', 'right'].forEach(function(displayPos) {
            var key = displayPos + '-gripper';
            if (!state[key]) return;
            var backendSlot = gripperSlotMap[displayPos] || displayPos;
            var gs = (deviceInfo.gripperSlots || {})[backendSlot] || {};
            if (gs.type === 'electric') return; // 电动夹爪由内联轮询处理
            fetch(API_ALT + '/api/gripper/' + backendSlot).then(function(r){return r.json();}).then(function(data) {
                if (!data) return;
                if (data.has) {
                    updateGripperDisplay(displayPos, data);
                } else {
                    // 无数据时也更新连接指示灯
                    var prefix = displayPos === 'left' ? 'gripperLeft' : 'gripperRight';
                    var connEl = document.getElementById(prefix + 'Conn');
                    if (connEl) connEl.style.background = '#ef4444';
                }
            }).catch(function(){});
        });
    }, 150);

    // 录制控制需要在任意页面都能响应手动夹爪按键。
    setInterval(function() {
        if (!serverOnline) return;
        ['left', 'right'].forEach(function(displayPos) {
            if (currentPage === 'monitor' && state[displayPos + '-gripper']) return;
            var backendSlot = gripperSlotMap[displayPos] || displayPos;
            var gs = (deviceInfo.gripperSlots || {})[backendSlot] || {};
            if (!gs.connected || gs.type === 'electric') {
                manualGripperButtonState[displayPos] = { button1: false, button2: false };
                return;
            }
            fetch(API_ALT + '/api/gripper/' + backendSlot).then(function(r){return r.json();}).then(function(data) {
                handleManualGripperButtons(displayPos, data);
            }).catch(function(){});
        });
    }, 150);

    // ---- 电动夹爪采集速率统计 ----
    var egRateCounters = { left: 0, right: 0 };
    var egRateLast = performance.now();
    setInterval(function() {
        var now = performance.now();
        var dt = (now - egRateLast) / 1000;
        if (dt < 0.5) return;
        ['left', 'right'].forEach(function(dp) {
            var gs = (deviceInfo.gripperSlots || {})[gripperSlotMap[dp] || dp] || {};
            if (gs.type !== 'electric') return;
            var el = document.getElementById(getInlineEgPrefix(dp) + 'Rate');
            if (el) {
                var rate = egRateCounters[dp] / dt;
                el.textContent = rate.toFixed(0);
            }
            egRateCounters[dp] = 0;
        });
        egRateLast = now;
    }, 1000);

    // ---- 电动夹爪内联轮询（monitor页面夹爪面板中） ----
    function updateInlineElectricGripper(displayPos, data) {
        var p = displayPos === 'left' ? 'egLeft' : 'egRight';
        egRateCounters[displayPos] = (egRateCounters[displayPos] || 0) + 1;
        var posEl = document.getElementById(p + 'Pos');
        if (posEl) posEl.textContent = data.positionDeg.toFixed(1) + '°';
        var velEl = document.getElementById(p + 'Vel');
        if (velEl) velEl.textContent = data.velocity.toFixed(1);
        var curEl = document.getElementById(p + 'Cur');
        if (curEl) curEl.textContent = data.current.toFixed(2) + 'A';
        var mtEl = document.getElementById(p + 'Mt');
        if (mtEl) { mtEl.textContent = data.motorTemp.toFixed(1) + '°'; mtEl.style.color = data.motorTemp > 80 ? 'var(--danger)' : data.motorTemp > 60 ? 'var(--warning)' : 'var(--text)'; }
        var mosEl = document.getElementById(p + 'Mos');
        if (mosEl) { mosEl.textContent = data.mosTemp.toFixed(1) + '°'; mosEl.style.color = data.mosTemp > 80 ? 'var(--danger)' : data.mosTemp > 60 ? 'var(--warning)' : 'var(--text)'; }
        var errEl = document.getElementById(p + 'Err');
        if (errEl) { errEl.textContent = data.errorCode === 0 ? 'OK' : '0x' + data.errorCode.toString(16); errEl.style.color = data.errorCode === 0 ? 'var(--success)' : 'var(--danger)'; }
        // 顶部副标题状态。
        var valuesEl = document.getElementById(displayPos === 'left' ? 'gripperLeftValues' : 'gripperRightValues');
        if (valuesEl) valuesEl.textContent = data.positionDeg.toFixed(1) + '° | V:' + data.velocity.toFixed(0) + ' I:' + data.current.toFixed(1);
        // 同步控制页的状态显示
        var cpPosEl = document.getElementById('egPositionVal');
        if (cpPosEl) cpPosEl.textContent = data.positionDeg.toFixed(2) + '°';
        var cpBarEl = document.getElementById('egPositionBar');
        if (cpBarEl) {
            var barPct = Math.max(0, Math.min(100, data.positionDeg / EG_MAX_POSITION_DEG * 100));
            cpBarEl.style.width = barPct + '%';
        }
        var cpActualEl = document.getElementById('egActualPosLabel');
        if (cpActualEl) cpActualEl.textContent = '实际: ' + data.positionDeg.toFixed(1) + '°';
        var cpVelEl = document.getElementById('egVelocityVal');
        if (cpVelEl) cpVelEl.textContent = data.velocity.toFixed(1);
        var cpCurVEl = document.getElementById('egCurrentVal');
        if (cpCurVEl) cpCurVEl.textContent = data.current.toFixed(2);
        var cpMtEl = document.getElementById('egMotorTempVal');
        if (cpMtEl) { cpMtEl.textContent = data.motorTemp.toFixed(1); cpMtEl.style.color = data.motorTemp > 80 ? '#ef4444' : data.motorTemp > 60 ? '#f59e0b' : '#fff'; }
        var cpMosEl = document.getElementById('egMosTempVal');
        if (cpMosEl) { cpMosEl.textContent = data.mosTemp.toFixed(1); cpMosEl.style.color = data.mosTemp > 80 ? '#ef4444' : data.mosTemp > 60 ? '#f59e0b' : '#fff'; }
        var cpErrEl = document.getElementById('egErrorCodeVal');
        if (cpErrEl) { cpErrEl.textContent = data.errorCode === 0 ? '正常' : '0x' + data.errorCode.toString(16); cpErrEl.style.color = data.errorCode === 0 ? '#4ade80' : '#f87171'; }
    }
    var inlineEgPollTimer = setInterval(function() {
        if (!serverOnline || currentPage !== 'monitor') return;
        ['left', 'right'].forEach(function(displayPos) {
            var backendSlot = gripperSlotMap[displayPos] || displayPos;
            var gs = (deviceInfo.gripperSlots || {})[backendSlot] || {};
            if (gs.type !== 'electric' || !state[displayPos + '-gripper']) return;
            fetch(API_ALT + '/api/electric-gripper/' + backendSlot).then(function(r){return r.json();}).then(function(data) {
                if (data && data.has) updateInlineElectricGripper(displayPos, data);
            }).catch(function(){});
        });
    }, 150);

    // ---- 电动夹爪内联滑块控制 ----
    function getInlineEgPrefix(displayPos) { return displayPos === 'left' ? 'egLeft' : 'egRight'; }
    function setupInlineEgSlider(displayPos) {
        var p = getInlineEgPrefix(displayPos);
        var slider = document.getElementById(p + 'Slider');
        var label = document.getElementById(p + 'TargetPos');
        if (!slider) return;
        var throttlePending = false;
        var lastSent = 0;
        slider.addEventListener('input', function() {
            var val = parseFloat(this.value);
            if (label) label.textContent = val.toFixed(0);
            syncEgToControlPage(val);
            var now = Date.now();
            if (now - lastSent >= 50) {
                lastSent = now;
                doInlineEgSend(displayPos, val, true);
                throttlePending = false;
            } else if (!throttlePending) {
                throttlePending = true;
                var captured = val;
                setTimeout(function() { doInlineEgSend(displayPos, captured, true); throttlePending = false; }, 50);
            }
        });
    }
    function syncEgToControlPage(val) {
        var cpSlider = document.getElementById('egPositionSlider');
        if (cpSlider) cpSlider.value = val;
        var cpLabel = document.getElementById('egTargetPosLabel');
        if (cpLabel) cpLabel.textContent = val.toFixed(0) + '°';
    }
    function syncControlPageToInline(val) {
        ['left', 'right'].forEach(function(dp) {
            var gs = (deviceInfo.gripperSlots || {})[gripperSlotMap[dp] || dp] || {};
            if (gs.type !== 'electric') return;
            var p = getInlineEgPrefix(dp);
            var sl = document.getElementById(p + 'Slider');
            if (sl) sl.value = val;
            var lb = document.getElementById(p + 'TargetPos');
            if (lb) lb.textContent = val.toFixed(0);
            // 同步速度/电流
            var cpSpeed = document.getElementById('egSpeedInput');
            var cpCur = document.getElementById('egCurrentInput');
            var ilSpeed = document.getElementById(p + 'Speed');
            var ilCur = document.getElementById(p + 'Current');
            if (cpSpeed && ilSpeed) ilSpeed.value = cpSpeed.value;
            if (cpCur && ilCur) ilCur.value = cpCur.value;
        });
    }
    function doInlineEgSend(displayPos, position, silent) {
        var backendSlot = gripperSlotMap[displayPos] || displayPos;
        var p = getInlineEgPrefix(displayPos);
        var spd = Math.max(1, Math.min(3276, parseFloat(document.getElementById(p + 'Speed').value) || 10));
        var cur = Math.max(0.1, Math.min(4.0, parseFloat(document.getElementById(p + 'Current').value) || 1.0));
        // 同步控制页速度/电流
        var cpSpeed = document.getElementById('egSpeedInput');
        if (cpSpeed) cpSpeed.value = spd;
        var cpCur = document.getElementById('egCurrentInput');
        if (cpCur) cpCur.value = cur;
        fetch('/api/electric-gripper/' + backendSlot + '/control', {
            method: 'POST', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'set_position', position: position, speed: spd, current_limit: cur })
        }).then(function(r){return r.json();}).then(function(d) {
            if (!silent) showToast(d.success ? '位置控制已发送' : '失败: ' + (d.error || ''), d.success ? 'success' : 'error');
        }).catch(function(){});
    }
    // 内联电机控制按钮（使能/失能/清除错误/急停）
    function sendInlineEgAction(displayPos, action) {
        egCurrentSlot = gripperSlotMap[displayPos] || displayPos;
        sendEgCommand(action);
    }
    ['left', 'right'].forEach(function(dp) {
        var p = getInlineEgPrefix(dp);
        var enableBtn = document.getElementById(p + 'EnableBtn');
        if (enableBtn) enableBtn.addEventListener('click', function() { sendInlineEgAction(dp, 'enable'); });
        var disableBtn = document.getElementById(p + 'DisableBtn');
        if (disableBtn) disableBtn.addEventListener('click', function() { sendInlineEgAction(dp, 'disable'); });
        var clearErrBtn = document.getElementById(p + 'ClearErrBtn');
        if (clearErrBtn) clearErrBtn.addEventListener('click', function() { sendInlineEgAction(dp, 'clear_error'); });
        var haltBtn = document.getElementById(p + 'HaltBtn');
        if (haltBtn) haltBtn.addEventListener('click', function() { sendInlineEgAction(dp, 'halt'); });
    });
    setupInlineEgSlider('left');
    setupInlineEgSlider('right');

    // 速度/电流双向同步：内联 ↔ 控制页
    ['left', 'right'].forEach(function(dp) {
        var p = getInlineEgPrefix(dp);
        var ilSpeed = document.getElementById(p + 'Speed');
        var ilCur = document.getElementById(p + 'Current');
        if (ilSpeed) ilSpeed.addEventListener('input', function() {
            var cp = document.getElementById('egSpeedInput');
            if (cp) cp.value = this.value;
        });
        if (ilCur) ilCur.addEventListener('input', function() {
            var cp = document.getElementById('egCurrentInput');
            if (cp) cp.value = this.value;
        });
    });
    var cpSpeedEl = document.getElementById('egSpeedInput');
    var cpCurEl = document.getElementById('egCurrentInput');
    if (cpSpeedEl) cpSpeedEl.addEventListener('input', function() {
        ['left', 'right'].forEach(function(dp) {
            var gs = (deviceInfo.gripperSlots || {})[gripperSlotMap[dp] || dp] || {};
            if (gs.type !== 'electric') return;
            var el = document.getElementById(getInlineEgPrefix(dp) + 'Speed');
            if (el) el.value = cpSpeedEl.value;
        });
    });
    if (cpCurEl) cpCurEl.addEventListener('input', function() {
        ['left', 'right'].forEach(function(dp) {
            var gs = (deviceInfo.gripperSlots || {})[gripperSlotMap[dp] || dp] || {};
            if (gs.type !== 'electric') return;
            var el = document.getElementById(getInlineEgPrefix(dp) + 'Current');
            if (el) el.value = cpCurEl.value;
        });
    });

    // ---- 主渲染循环 ----
    function render() {
        requestAnimationFrame(render);
    }
    requestAnimationFrame(render);

    // ---- Session ID 格式化 ----
    function formatSessionId(id) {
        var m = id.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})(?:_(.+))?$/);
        if (!m) return id;
        var s = m[1] + '年' + m[2] + '月' + m[3] + '日 ' + m[4] + ':' + m[5] + ':' + m[6];
        if (m[7]) s += ' · ' + m[7];
        return s;
    }

    function escapeHtml(value) {
        return String(value == null ? '' : value)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#39;');
    }

    // ---- Toast 提示 ----
    var toastTimer = null;
    function showToast(msg, type) {
        var el = document.getElementById('toast');
        el.className = 'toast toast-show' + (type ? ' ' + type : '');
        el.textContent = msg;
        if(toastTimer) clearTimeout(toastTimer);
        toastTimer = setTimeout(function() {
            el.style.opacity = '0';
            el.style.transform = 'translateX(40px)';
            el.style.transition = '0.3s ease';
            setTimeout(function() {
                el.className = 'toast';
                el.style.opacity = '';
                el.style.transform = '';
                el.style.transition = '';
            }, 300);
        }, 3500);
    }

    // ---- 多任务保存状态面板（队列机制） ----
    // 每次停止录制动态添加一条状态卡片，支持排队/保存中/完成/取消
    // 每张卡片独立轮询后端 /api/record/save_status 跟踪自己的保存进度
    var savePanelContainer = null;
    var savePanelItems = {};  // sessionId -> { el, timer, pollTimer, cancelRequested }

    function getSavePanel() {
        if (!savePanelContainer) {
            savePanelContainer = document.createElement('div');
            savePanelContainer.style.cssText = 'position:fixed;top:70px;right:16px;z-index:10000;display:flex;flex-direction:column;gap:8px;pointer-events:none;max-width:420px;';
            document.body.appendChild(savePanelContainer);
        }
        return savePanelContainer;
    }

    function addSaveItem(sessionId) {
        removeSaveItem(sessionId); // 去重
        var panel = getSavePanel();
        var item = document.createElement('div');
        item.style.cssText = 'display:flex;align-items:center;gap:8px;min-height:40px;padding:10px 16px;border-radius:8px;font-size:13px;line-height:18px;color:#fff;background:linear-gradient(135deg,#3b82f6,#2563eb);transition:opacity 0.3s,transform 0.3s,background 0.2s;transform:translateX(0);box-shadow:0 2px 8px rgba(0,0,0,0.15);pointer-events:auto;';

        // ✕ 按钮表示取消保存并清理本地文件，不只是关闭提示。
        var closeBtn = document.createElement('button');
        closeBtn.textContent = '✕';
        closeBtn.type = 'button';
        closeBtn.title = '取消保存并清理本地文件';
        closeBtn.style.cssText = 'cursor:pointer;font-size:12px;line-height:1;opacity:0.7;flex-shrink:0;display:inline-flex;align-items:center;justify-content:center;width:18px;height:18px;padding:0;border:0;border-radius:4px;color:#fff;background:transparent;transition:background 0.2s,opacity 0.2s;user-select:none;';
        closeBtn.onmouseover = function() { closeBtn.style.opacity = '1'; closeBtn.style.background = 'rgba(255,255,255,0.2)'; };
        closeBtn.onmouseout = function() { closeBtn.style.opacity = '0.6'; closeBtn.style.background = 'transparent'; };
        closeBtn.onclick = function(e) { e.stopPropagation(); cancelSaveItem(sessionId); };

        // 旋转动画：尺寸和按钮保持一致，由父级 flex 统一垂直居中。
        var spinner = document.createElement('span');
        spinner.className = 'save-spinner';
        spinner.style.cssText = 'display:inline-flex;width:16px;height:16px;box-sizing:border-box;border:2px solid rgba(255,255,255,0.3);border-top-color:#fff;border-radius:50%;animation:spin 0.8s linear infinite;flex-shrink:0;margin:0;';

        // 文字标签
        var label = document.createElement('span');
        label.style.cssText = 'flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;line-height:18px;';
        label.textContent = formatSessionDateLabel(sessionId) + ' 排队中...';
        label.dataset.sessionId = sessionId;

        item.appendChild(closeBtn);
        item.appendChild(spinner);
        item.appendChild(label);
        item.dataset.sessionId = sessionId;
        panel.appendChild(item);

        var entry = { el: item, timer: null, pollTimer: null, cancelRequested: false };
        savePanelItems[sessionId] = entry;

        // 立即开始按 sessionId 轮询该任务的保存状态
        pollSessionSaveStatus(sessionId);
    }

    // 按 sessionId 独立轮询保存进度，不再依赖全局 recState
    function pollSessionSaveStatus(sessionId) {
        var entry = savePanelItems[sessionId];
        if (!entry) return;

        fetch('/api/record/save_status').then(function(r) {
            if (!r.ok) throw new Error('save_status not available');
            return r.json();
        }).then(function(d) {
            var entry2 = savePanelItems[sessionId];
            if (!entry2) return; // 已被移除

            var found = false;
            if (d.tasks) {
                for (var i = 0; i < d.tasks.length; i++) {
                    if (d.tasks[i].sessionId === sessionId) {
                        found = true;
                        var status = d.tasks[i].status;
                        var lbl = entry2.el.querySelector('span[data-session-id]');
                        if (status === 'pending') {
                            if (lbl) lbl.textContent = formatSessionDateLabel(sessionId) + ' 排队中...';
                            entry2.pollTimer = setTimeout(function() { pollSessionSaveStatus(sessionId); }, 1000);
                        } else if (status === 'running') {
                            if (lbl) lbl.textContent = formatSessionDateLabel(sessionId) + ' 正在保存...';
                            entry2.pollTimer = setTimeout(function() { pollSessionSaveStatus(sessionId); }, 1000);
                        } else if (status === 'completed') {
                            completeSaveItem(sessionId);
                        } else if (status === 'cancelled') {
                            removeSaveItem(sessionId);
                        }
                        break;
                    }
                }
            }
            // 后端队列里找不到该 sessionId：普通任务视为完成；取消任务视为清理完成。
            if (!found) {
                if (entry2.cancelRequested) removeSaveItem(sessionId);
                else completeSaveItem(sessionId);
            }
        }).catch(function() {
            // 兼容旧后端：如果 /api/record/save_status 不可用（404），
            // 回退到轮询全局 /api/record 的 finalizing 状态
            var entry3 = savePanelItems[sessionId];
            if (!entry3) return;
            fetch('/api/record').then(function(r) { return r.json(); }).then(function(d) {
                var entry4 = savePanelItems[sessionId];
                if (!entry4) return;
                if (!d.finalizing) {
                    // 后端已不在保存中，视为当前任务完成
                    completeSaveItem(sessionId);
                } else {
                    var lbl = entry4.el.querySelector('span[data-session-id]');
                    if (lbl) lbl.textContent = formatSessionDateLabel(sessionId) + ' 正在保存...';
                    entry4.pollTimer = setTimeout(function() { pollSessionSaveStatus(sessionId); }, 1500);
                }
            }).catch(function() {
                var entry5 = savePanelItems[sessionId];
                if (entry5) {
                    entry5.pollTimer = setTimeout(function() { pollSessionSaveStatus(sessionId); }, 2000);
                }
            });
        });
    }

    // 点击 ✕ 取消排队/保存
    function cancelSaveItem(sessionId) {
        var entry = savePanelItems[sessionId];
        if (!entry) return;
        entry.cancelRequested = true;
        setSaveItemCancelling(sessionId);
        fetch('/api/record/cancel_save', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ sessionId: sessionId })
        }).then(function(r) {
            if (!r.ok) {
                // 旧后端不支持取消接口，直接移除卡片
                removeSaveItem(sessionId);
                showToast(formatSessionDateLabel(sessionId) + ' 已关闭提示，保存任务仍可能继续', 'warning');
                return null;
            }
            return r.json();
        }).then(function(d) {
            if (!d) return;
            if (d.success) {
                showToast(formatSessionDateLabel(sessionId) + ' 正在取消并清理文件', 'warning');
                pollSessionSaveStatus(sessionId);
            } else {
                // 任务可能已完成，直接标记完成
                completeSaveItem(sessionId);
            }
        }).catch(function() {
            var entry2 = savePanelItems[sessionId];
            if (entry2) entry2.cancelRequested = false;
            showToast('取消保存请求发送失败', 'error');
        });
    }

    function setSaveItemCancelling(sessionId) {
        var entry = savePanelItems[sessionId];
        if (!entry || !entry.el) return;
        var btn = entry.el.querySelector('button');
        var lbl = entry.el.querySelector('span[data-session-id]');
        entry.el.style.background = 'linear-gradient(135deg,#64748b,#475569)';
        if (btn) btn.style.display = 'none';
        if (lbl) lbl.textContent = formatSessionDateLabel(sessionId) + ' 正在取消并清理...';
    }

    function completeSaveItem(sessionId) {
        var entry = savePanelItems[sessionId];
        if (!entry || !entry.el) return;
        // 停止轮询
        if (entry.pollTimer) { clearTimeout(entry.pollTimer); entry.pollTimer = null; }
        // 更新 UI 为完成状态
        entry.el.style.background = 'linear-gradient(135deg,#10b981,#059669)';
        entry.el.innerHTML = '<span style="display:inline-flex;align-items:center;justify-content:center;width:18px;height:18px;font-size:16px;line-height:1;flex-shrink:0">✓</span>'
            + '<span style="flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;line-height:18px">' + formatSessionDateLabel(sessionId) + ' 保存完成</span>';
        // 3秒后自动消失
        entry.timer = setTimeout(function() { removeSaveItem(sessionId); }, 3000);
    }

    function removeSaveItem(sessionId) {
        var entry = savePanelItems[sessionId];
        if (!entry) return;
        if (entry.timer) clearTimeout(entry.timer);
        if (entry.pollTimer) clearTimeout(entry.pollTimer);
        if (entry.el && entry.el.parentNode) {
            entry.el.style.opacity = '0';
            entry.el.style.transform = 'translateX(40px)';
            var el = entry.el;
            setTimeout(function() { if (el.parentNode) el.remove(); }, 300);
        }
        delete savePanelItems[sessionId];
        // 如果面板空了就移除
        if (savePanelContainer && Object.keys(savePanelItems).length === 0) {
            savePanelContainer.remove();
            savePanelContainer = null;
        }
    }

    // ---- 数据采集 ----
    var recState = {
        recording: false,
        finalizing: false,
        startingNew: false,  // 正在发起新录制时为 true，防止轮询覆盖状态
        sessionId: '',
        selectedStreams: {},  // { 'left-color': true, 'left-depth': false, ... }
        slotStats: {},
        streamFps: {},
        captureFps: {},
        startTime: null,
        elapsedMs: 0,
        elapsedBaseMs: 0,
        durationText: '00:00:00',
        saveMode: localStorage.getItem('recordSaveMode') || 'strict',
        pollTimer: null,
        savePollTimer: null,
        lastRateTime: null,
        prevSlotStats: {},
        liveRateKeysSignature: '',
        availableStreams: []
    };

    // 根据设备信息逐项生成录制开关：每个位置 × 每个数据流 = 一个独立开关
    function buildRecordToggles() {
        var container = document.getElementById('recordTogglesContainer');
        if (!container) return;
        var posLabels = { left: '左手', right: '右手', head: '头部' };
        var posColors = { left: '#ef4444', right: '#eab308', head: '#3b82f6' };
        var html = '';
        var available = [];
        ['left', 'right', 'head'].forEach(function(pos) {
            var backendSlot = slotMap[pos] || pos;
            var info = deviceInfo[backendSlot] || {};
            var gripperInfo = (deviceInfo.gripperSlots || {})[pos] || {};
            var camConnected = info.connected;
            var gripConnected = gripperInfo.connected;
            if (!camConnected && !gripConnected) return;

            var streams = [];
            if (camConnected) {
                streams.push({ key: pos + '-color', label: '彩色视频', sub: 'H.264 · 逐帧时间戳' });
                if (info.type === 'orbbec') {
                    streams.push({ key: pos + '-depth', label: '深度视频', sub: 'JET 伪彩 · 逐帧时间戳' });
                    streams.push({ key: pos + '-ir-left', label: '左红外', sub: 'IR-L · 逐帧时间戳' });
                    streams.push({ key: pos + '-ir-right', label: '右红外', sub: 'IR-R · 逐帧时间戳' });
                    streams.push({ key: pos + '-pointcloud', label: '点云数据', sub: 'PLY · 每N帧保存' });
                }
            }
            if (gripConnected) {
                streams.push({ key: pos + '-gripper', label: '夹爪数据', sub: '位置+按键 · CSV' });
            }

            streams.forEach(function(s) {
                available.push(s.key);
                if (!(s.key in recState.selectedStreams)) recState.selectedStreams[s.key] = false;
                var active = recState.selectedStreams[s.key] ? ' active' : '';
                html += '<label class="toggle-row">'
                    + '<div class="toggle-info">'
                    + '<span class="toggle-label"><span style="background:' + posColors[pos] + ';color:#fff;padding:1px 6px;border-radius:3px;font-size:11px;">' + posLabels[pos] + '</span> ' + s.label + '</span>'
                    + '<span class="toggle-sub">' + s.sub + '</span>'
                    + '</div>'
                    + '<div class="switch' + active + '" data-record-stream="' + s.key + '"><div class="switch-slider"></div></div>'
                    + '</label>';
            });
        });
        container.innerHTML = html;
        container.querySelectorAll('.switch[data-record-stream]').forEach(function(sw) {
            sw.addEventListener('click', function() {
                var key = sw.getAttribute('data-record-stream');
                recState.selectedStreams[key] = !recState.selectedStreams[key];
                sw.classList.toggle('active', recState.selectedStreams[key]);
            });
        });
        recState.availableStreams = available;
        updateSelectAllButton();
    }

    function updateSelectAllButton() {
        var btn = document.getElementById('recordSelectAllBtn');
        if (!btn) return;
        var total = recState.availableStreams.length;
        var selected = recState.availableStreams.filter(function(key) { return recState.selectedStreams[key]; }).length;
        var allSelected = total > 0 && selected === total;
        btn.textContent = total > 0
            ? ((allSelected ? '一键全关' : '一键全开') + '（' + selected + '/' + total + '）')
            : '一键全开';
        btn.disabled = recState.recording || total === 0;
    }

    function toggleAllRecordStreams() {
        if (recState.recording) {
            showToast('录制中不能修改录制项', 'warning');
            return;
        }
        if (!recState.availableStreams.length) {
            showToast('当前没有可开启的录制项', 'warning');
            return;
        }
        var selected = recState.availableStreams.filter(function(key) { return recState.selectedStreams[key]; }).length;
        var shouldSelect = selected !== recState.availableStreams.length;
        recState.availableStreams.forEach(function(key) { recState.selectedStreams[key] = shouldSelect; });
        buildRecordToggles();
        showToast(shouldSelect ? '已开启全部可录制项' : '已关闭全部录制项', 'success');
    }

    function initCollectPage() {
        buildRecordToggles();
        initRecordSaveModeToggle();
        var selectAllBtn = document.getElementById('recordSelectAllBtn');
        if (selectAllBtn && !selectAllBtn.dataset.bound) {
            selectAllBtn.dataset.bound = '1';
            selectAllBtn.addEventListener('click', toggleAllRecordStreams);
        }
        updateRecordUI();
        fetchRecordStatus();
        if (!recState.pollTimer) {
            recState.pollTimer = setInterval(fetchRecordStatus, 1000);
        }
        fetchHistory();
    }

    // 页面加载时自动清理残留的录制状态
    (function cleanupStaleState() {
        fetch('/api/record').then(function(r) { return r.json(); }).then(function(d) {
            if (d.recording === true) {
                fetch('/api/record', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ action: 'stop' })
                }).then(function() {}).catch(function() {});
            }
        }).catch(function() {});
    })();

    function formatDuration(seconds) {
        seconds = Math.max(0, Math.floor(seconds || 0));
        var h = String(Math.floor(seconds / 3600)).padStart(2, '0');
        var m = String(Math.floor((seconds % 3600) / 60)).padStart(2, '0');
        var s = String(seconds % 60).padStart(2, '0');
        return h + ':' + m + ':' + s;
    }

    function normalizeStartTimeMs(value) {
        var ts = Number(value || 0);
        if (!Number.isFinite(ts) || ts <= 0) return 0;
        // 兼容旧后端：部分版本曾把微秒值误放在 startTimeMs 字段里。
        if (ts > 100000000000000) ts = Math.floor(ts / 1000);
        return ts;
    }

    function cacheCurrentDuration() {
        var elapsed = getCurrentRecordingElapsedMs();
        if (elapsed <= 0) return;
        recState.durationText = formatDuration(elapsed / 1000);
    }

    function initRecordSaveModeToggle() {
        var toggle = document.getElementById('recordSaveModeToggle');
        var desc = document.getElementById('recordSaveModeDesc');
        if (!toggle) return;
        function applyMode(mode) {
            recState.saveMode = mode === 'fast' ? 'fast' : 'strict';
            localStorage.setItem('recordSaveMode', recState.saveMode);
            toggle.querySelectorAll('.record-mode-btn').forEach(function(btn) {
                btn.classList.toggle('active', btn.getAttribute('data-save-mode') === recState.saveMode);
            });
            if (desc) {
                desc.textContent = recState.saveMode === 'fast'
                    ? '录制中实时写入 MP4，停止后更快；播放器时长可能有轻微偏差，CSV 时间戳仍准确。'
                    : '视频时长与页面录制时长优先对齐，停止后后台统一保存。';
            }
        }
        toggle.querySelectorAll('.record-mode-btn').forEach(function(btn) {
            btn.addEventListener('click', function() {
                if (recState.recording) {
                    showToast('录制中不能切换保存模式', 'warning');
                    return;
                }
                applyMode(btn.getAttribute('data-save-mode'));
            });
        });
        applyMode(recState.saveMode);
    }

    function applyServerElapsedMs(value) {
        var ms = Number(value || 0);
        if (!Number.isFinite(ms) || ms < 0) return;
        recState.elapsedMs = ms;
        recState.elapsedBaseMs = Date.now();
    }

    function getCurrentRecordingElapsedMs() {
        if (recState.recording && recState.elapsedBaseMs) {
            return recState.elapsedMs + Math.max(0, Date.now() - recState.elapsedBaseMs);
        }
        if (recState.elapsedMs > 0) return recState.elapsedMs;
        if (recState.startTime) return Math.max(0, Date.now() - recState.startTime);
        return 0;
    }

    function parseRecordStreamKey(key) {
        var dash = key.indexOf('-');
        if (dash < 0) return { pos: key, stream: '' };
        return { pos: key.substring(0, dash), stream: key.substring(dash + 1) };
    }

    // 为所有已连接相机和夹爪构建实时频率行，不只显示已勾选录制的流。
    var recPosColors = { left: '#ef4444', right: '#eab308', head: '#3b82f6' };

    function ensureLiveRateRows(streamLabels) {
        var grid = document.getElementById('recLiveRatesGrid');
        if (!grid) return [];
        var posOrder = ['left', 'right', 'head'];
        var keys = [];
        posOrder.forEach(function(displayPos) {
            var backendSlot = slotMap[displayPos] || displayPos;
            var info = deviceInfo[backendSlot] || {};
            var gripperInfo = (deviceInfo.gripperSlots || {})[displayPos] || {};
            if (info.connected) {
                keys.push(displayPos + '-color');
                if (info.type === 'orbbec') {
                    keys.push(displayPos + '-depth');
                    keys.push(displayPos + '-ir-left');
                    keys.push(displayPos + '-ir-right');
                    keys.push(displayPos + '-pointcloud');
                }
            }
            if (gripperInfo.connected) {
                keys.push(displayPos + '-gripper');
            }
        });
        var signature = keys.join('|');
        if (signature !== recState.liveRateKeysSignature) {
            recState.liveRateKeysSignature = signature;
            grid.innerHTML = keys.map(function(key) {
                var parts = parseRecordStreamKey(key);
                var id = 'recRate_' + parts.pos + '_' + parts.stream.replace(/-/g, '_');
                var posTag = '<span style="background:' + (recPosColors[parts.pos] || '#888') + ';color:#fff;padding:1px 6px;border-radius:3px;font-size:10px;">' + (recPosLabels[parts.pos] || parts.pos) + '</span>';
                return '<div class="status-item" data-rate-key="' + key + '">'
                    + '<span class="status-label">' + posTag + ' ' + (streamLabels[parts.stream] || parts.stream) + '</span>'
                    + '<span class="status-value live-rate-value" id="' + id + '">--</span>'
                    + '</div>';
            }).join('');
        }
        return keys;
    }

    function updateLiveRates(now, streamLabels) {
        var panel = document.getElementById('recLiveRates');
        if (!panel) return;
        panel.style.display = '';

        var keys = ensureLiveRateRows(streamLabels);
        if (keys.length === 0) { panel.style.display = 'none'; return; }

        var dt = recState.lastRateTime ? ((now - recState.lastRateTime) / 1000) : 0;
        var nextPrevStats = null;
        if (recState.recording && dt >= 0.5) {
            nextPrevStats = JSON.parse(JSON.stringify(recState.slotStats || {}));
        }

        keys.forEach(function(key) {
            var parts = parseRecordStreamKey(key);
            var backendKey = toBackendKey(key);
            var stats = (recState.slotStats || {})[parts.pos] || {};
            var prev = (recState.prevSlotStats || {})[parts.pos] || {};
            var value = '--';
            var unit = parts.stream === 'gripper' ? '/s' : 'fps';

            // 页面上的“实时采集频率”统一按后端采集回调计算，避免和 MJPEG 预览帧率混在一起。
            if (recState.captureFps[backendKey] !== undefined && recState.captureFps[backendKey] > 0) {
                value = recState.captureFps[backendKey].toFixed(1) + ' ' + unit;
            } else if (recState.recording && dt >= 0.5 && prev[parts.stream] !== undefined) {
                var current = stats[parts.stream] || 0;
                value = ((current - prev[parts.stream]) / dt).toFixed(1) + ' ' + unit;
            } else if (recState.streamFps[backendKey] !== undefined) {
                value = recState.streamFps[backendKey].toFixed(1) + ' fps';
            }

            var id = 'recRate_' + parts.pos + '_' + parts.stream.replace(/-/g, '_');
            var el = document.getElementById(id);
            if (el && el.textContent !== value) el.textContent = value;
        });

        if (nextPrevStats) {
            recState.prevSlotStats = nextPrevStats;
            recState.lastRateTime = now;
        }
    }

    function startRecording(options) {
        options = options || {};
        if (recState.recording) return;
        // 保存队列独立运行，不需要在这里处理旧的保存轮询
        recState.startingNew = true;  // 防止 fetchRecordStatus 轮询覆盖状态
        buildRecordToggles();
        var streams = [];
        Object.keys(recState.selectedStreams).forEach(function(k) {
            if (recState.selectedStreams[k]) streams.push(k);
        });
        if (streams.length === 0 && options.fallbackGripperPos) {
            var fallbackKey = options.fallbackGripperPos + '-gripper';
            recState.selectedStreams[fallbackKey] = true;
            streams.push(fallbackKey);
            buildRecordToggles();
            showToast('未选择录制项，已默认录制' + (options.fallbackGripperPos === 'left' ? '左夹爪' : '右夹爪') + '数据', 'warning');
        }
        if (streams.length === 0) { showToast('请至少选择一项录制内容', 'error'); return; }

        fetch('/api/record', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'start', streams: streams, slotMapping: slotMap, saveMode: recState.saveMode })
        }).then(function(r) { return r.json(); }).then(function(d) {
            recState.startingNew = false;
            if (d.recording) {
                recState.recording = true;
                recState.finalizing = false;
                recState.sessionId = d.sessionId || '';
                recState.startTime = normalizeStartTimeMs(d.startTimeMs) || Date.now();
                applyServerElapsedMs(d.elapsedMs);
                recState.durationText = '00:00:00';
                recState.slotStats = {};
                recState.prevSlotStats = {};
                recState.lastRateTime = null;
                if (d.warnings && d.warnings.length > 0) {
                    d.warnings.forEach(function(w) { showToast(w, 'warning'); });
                }
                updateRecordUI();
            }
        }).catch(function() { recState.startingNew = false; });
    }

    function stopRecording() {
        if (!recState.recording || recState.finalizing) return;
        cacheCurrentDuration();
        recState.recording = false;
        recState.finalizing = true;
        updateRecordUI();

        fetch('/api/record', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'stop' })
        }).then(function(r) { return r.json(); }).then(function(d) {
            recState.sessionId = d.sessionId || recState.sessionId;
            applyServerElapsedMs(d.elapsedMs);
            cacheCurrentDuration();
            // 添加保存卡片，per-session 轮询自动跟踪状态
            addSaveItem(recState.sessionId);
        }).catch(function() {
            recState.finalizing = false;
            updateRecordUI();
            showToast('停止录制请求发送失败', 'error');
        });
    }

    function fetchRecordStatus() {
        if (currentPage !== 'collect') return;
        fetch('/api/record').then(function(r) { return r.json(); }).then(function(d) {
            var wasRecording = recState.recording;
            // 如果正在发起新录制，不让轮询覆盖状态（等 startRecording 的回调来更新）
            if (recState.startingNew) return;
            // 如果已经在录制中，不被后端的 finalizing 状态覆盖（新录制优先）
            if (d.recording === true) {
                recState.recording = true;
                recState.finalizing = false;
            } else {
                recState.recording = false;
                recState.finalizing = d.finalizing === true;
            }
            recState.sessionId = d.sessionId || '';
            if (d.perSlot) recState.slotStats = d.perSlot;
            recState.streamFps = d.streamFps || {};
            recState.captureFps = d.captureFps || {};
            applyServerElapsedMs(d.elapsedMs);
            if (recState.recording) {
                var serverStart = normalizeStartTimeMs(d.startTimeMs);
                if (serverStart) recState.startTime = serverStart;
                else if (!recState.startTime) recState.startTime = Date.now();
            } else {
                if (wasRecording) cacheCurrentDuration();
                if (!recState.finalizing) recState.startTime = null;
            }
            updateRecordUI();
        }).catch(function() {});
    }

    var recPosLabels = { left: '左手', right: '右手', head: '头部' };

    function updateRecordUI() {
        var startBtn = document.getElementById('recordStartBtn');
        var stopBtn = document.getElementById('recordStopBtn');
        var statusPanel = document.getElementById('collectStatus');

        if (recState.finalizing && !recState.recording) {
            // 后台保存中，但不阻止新录制
            document.getElementById('recStatus').textContent = '保存中...';
            document.getElementById('recStatus').style.color = '#f0ad4e';
            startBtn.disabled = false;   // 允许开始新录制
            stopBtn.disabled = true;
            statusPanel.classList.remove('recording');
            stopBtn.classList.remove('pulsing');
        } else if (recState.recording) {
            document.getElementById('recStatus').textContent = '录制中...';
            document.getElementById('recStatus').style.color = 'var(--danger)';
            startBtn.disabled = true;
            stopBtn.disabled = false;
            statusPanel.classList.add('recording');
            stopBtn.classList.add('pulsing');
        } else {
            document.getElementById('recStatus').textContent = '空闲';
            document.getElementById('recStatus').style.color = 'var(--text)';
            startBtn.disabled = false;
            stopBtn.disabled = true;
            statusPanel.classList.remove('recording');
            stopBtn.classList.remove('pulsing');
        }

        document.getElementById('recSessionId').textContent = recState.sessionId ? formatSessionDateLabel(recState.sessionId) : '--';

        if (recState.recording && recState.startTime) {
            recState.durationText = formatDuration(getCurrentRecordingElapsedMs() / 1000);
            document.getElementById('recDuration').textContent = recState.durationText;
        } else {
            document.getElementById('recDuration').textContent = recState.durationText || '00:00:00';
        }

        var now = Date.now();
        var streamLabels = {
            'color': '彩色', 'depth': '深度', 'ir-left': '左红外', 'ir-right': '右红外',
            'pointcloud': '点云', 'gripper': '夹爪'
        };

        updateSelectAllButton();
        updateLiveRates(now, streamLabels);
    }

    var pendingHistoryDeleteSid = '';

    function getCollectSessionPath(sessionId) {
        var base = (currentPaths.collect || '').replace(/[\\\/]+$/, '');
        return base ? (base + '/' + sessionId) : sessionId;
    }

    function showDeleteHistoryConfirm(sessionId) {
        pendingHistoryDeleteSid = sessionId;
        var existing = document.getElementById('deleteHistoryConfirm');
        if (existing) existing.remove();

        var mask = document.createElement('div');
        mask.className = 'delete-confirm-mask';
        mask.id = 'deleteHistoryConfirm';
        mask.innerHTML = '<div class="delete-confirm-dialog">'
            + '<div class="delete-confirm-title">删除这条采集数据？</div>'
            + '<div class="delete-confirm-body">将删除本地会话目录：<br><b>' + escapeHtml(formatSessionId(sessionId)) + '</b><br>删除后无法从页面恢复。</div>'
            + '<label class="delete-confirm-check"><input type="checkbox" id="deleteNoAskCheck"> 后续不再询问</label>'
            + '<div class="delete-confirm-actions">'
            + '<button class="delete-confirm-cancel" type="button" id="deleteCancelBtn">取消</button>'
            + '<button class="delete-confirm-ok" type="button" id="deleteOkBtn">删除</button>'
            + '</div>'
            + '</div>';
        document.body.appendChild(mask);

        document.getElementById('deleteCancelBtn').onclick = function() { mask.remove(); pendingHistoryDeleteSid = ''; };
        document.getElementById('deleteOkBtn').onclick = function() {
            if (document.getElementById('deleteNoAskCheck').checked) {
                localStorage.setItem('deleteHistoryNoAsk', '1');
            }
            var sid = pendingHistoryDeleteSid;
            mask.remove();
            pendingHistoryDeleteSid = '';
            deleteHistorySession(sid);
        };
        mask.addEventListener('click', function(e) {
            if (e.target === mask) {
                mask.remove();
                pendingHistoryDeleteSid = '';
            }
        });
    }

    function requestDeleteHistorySession(sessionId) {
        if (!sessionId) return;
        if (localStorage.getItem('deleteHistoryNoAsk') === '1') {
            deleteHistorySession(sessionId);
            return;
        }
        showDeleteHistoryConfirm(sessionId);
    }

    function deleteHistorySession(sessionId) {
        fetch('/api/data/delete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ sessionId: sessionId, dir: 'collect' })
        }).then(function(r) { return r.json(); }).then(function(d) {
            if (d.deleted) {
                showToast('已删除 ' + formatSessionDateLabel(sessionId), 'success');
                fetchHistory();
            } else {
                showToast('删除失败: ' + (d.error || '未知错误'), 'error');
            }
        }).catch(function() {
            showToast('删除请求发送失败', 'error');
        });
    }

    function fetchHistory() {
        fetch('/api/record/history').then(function(r) { return r.json(); }).then(function(d) {
            var list = document.getElementById('historyList');
            if (!d.sessions || d.sessions.length === 0) {
                list.innerHTML = '<div class="empty-state">暂无录制记录</div>';
                return;
            }
            var html = '';
            var sorted = d.sessions.slice().reverse();
            sorted.forEach(function(sid) {
                html += '<div class="history-item" data-session-id="' + escapeHtml(sid) + '">'
                    + '<span class="history-name">' + escapeHtml(formatSessionId(sid)) + '</span>'
                    + '<button class="history-delete-btn" type="button" title="删除这条数据" data-delete-session="' + escapeHtml(sid) + '">&#128465;</button>'
                    + '</div>';
            });
            list.innerHTML = html;
            list.querySelectorAll('.history-delete-btn').forEach(function(btn) {
                btn.addEventListener('click', function(e) {
                    e.stopPropagation();
                    requestDeleteHistorySession(btn.getAttribute('data-delete-session'));
                });
            });
            requestAnimationFrame(function() {
                list.querySelectorAll('.history-item').forEach(function(el, i) {
                    setTimeout(function() { el.classList.add('visible'); }, i * 50);
                });
            });
        }).catch(function() {});
    }

    document.getElementById('recordStartBtn').addEventListener('click', function(e) { addRipple(this, e); startRecording(); });
    document.getElementById('recordStopBtn').addEventListener('click', stopRecording);

    // ---- 数据转换 ----
    var cvState = { sessions: [], selected: new Set(), loaded: false, converting: false, pollTimer: null, filterYear: 0, filterMonth: 0, filterDay: 0 };
    var cvSessionList = document.getElementById('convertSessionList');

    function parseSessionDate(id) {
        var m = id.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})$/);
        if (!m) return null;
        return new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
    }

    function buildConvertFilterOptions() {
        var years = new Set();
        years.add(new Date().getFullYear());
        cvState.sessions.forEach(function(s) {
            var d = parseSessionDate(s.id);
            if (d) years.add(d.getFullYear());
        });
        var sorted = Array.from(years).sort(function(a, b) { return b - a; });
        var yearSel = document.getElementById('cvFilterYear');
        if (!yearSel) return;
        yearSel.innerHTML = '<option value="0">全部年份</option>' + sorted.map(function(y) {
            return '<option value="' + y + '"' + (y === cvState.filterYear ? ' selected' : '') + '>' + y + '年</option>';
        }).join('');
        var monthSel = document.getElementById('cvFilterMonth');
        monthSel.innerHTML = '<option value="0">全部月份</option>';
        for (var i = 1; i <= 12; i++) {
            monthSel.innerHTML += '<option value="' + i + '"' + (i === cvState.filterMonth ? ' selected' : '') + '>' + i + '月</option>';
        }
        var daySel = document.getElementById('cvFilterDay');
        daySel.innerHTML = '<option value="0">全部</option>';
        var daysInMonth = cvState.filterMonth > 0 ? new Date(cvState.filterYear || new Date().getFullYear(), cvState.filterMonth, 0).getDate() : 31;
        for (var j = 1; j <= daysInMonth; j++) {
            daySel.innerHTML += '<option value="' + j + '"' + (j === cvState.filterDay ? ' selected' : '') + '>' + j + '日</option>';
        }
        document.getElementById('convertFilterBar').style.display = cvState.sessions.length > 0 ? 'flex' : 'none';
    }

    function getFilteredSessions() {
        return cvState.sessions.filter(function(s) {
            var d = parseSessionDate(s.id);
            if (!d) return true;
            if (cvState.filterYear > 0 && d.getFullYear() !== cvState.filterYear) return false;
            if (cvState.filterMonth > 0 && d.getMonth() + 1 !== cvState.filterMonth) return false;
            if (cvState.filterDay > 0 && d.getDate() !== cvState.filterDay) return false;
            return true;
        });
    }

    document.getElementById('cvFilterYear').addEventListener('change', function(e) {
        cvState.filterYear = +e.target.value;
        cvState.filterDay = 0;
        buildConvertFilterOptions();
        updateConvertUI();
    });
    document.getElementById('cvFilterMonth').addEventListener('change', function(e) {
        cvState.filterMonth = +e.target.value;
        cvState.filterDay = 0;
        buildConvertFilterOptions();
        updateConvertUI();
    });
    document.getElementById('cvFilterDay').addEventListener('change', function(e) {
        cvState.filterDay = +e.target.value;
        updateConvertUI();
    });

    // 判断会话是否包含有效视频帧，同时兼容旧版和新版 metadata。
    function sessionHasVideoFrames(m) {
        // 旧版扁平格式：frameCount 位于 metadata 顶层。
        var fc = m.frameCount || {};
        for (var k in fc) { if (fc[k] > 0) return true; }
        // 新版槽位格式：frameCount 位于每个 slot 内部。
        var slots = m.slots;
        if (slots && typeof slots === 'object') {
            for (var slotName in slots) {
                var slotFc = (slots[slotName] || {}).frameCount || {};
                for (var k2 in slotFc) { if (slotFc[k2] > 0) return true; }
            }
        }
        return false;
    }

    function loadSessions() {
        var dir = document.getElementById('convertSourceDir').value.trim();
        var statusEl = document.getElementById('convertPathStatus');
        cvSessionList.innerHTML = '<div class="empty-state">加载中...</div>';
        statusEl.textContent = '';
        statusEl.style.color = 'var(--text-dim)';
        fetch('/api/convert/sessions?dir=' + encodeURIComponent(dir || 'data_capture'))
            .then(function(r) { return r.json(); })
            .then(function(d) {
                var sessions = d.sessions || [];
                cvState.sessions = sessions;
                cvState.selected.clear();
                cvState.loaded = true;
                // 校验会话目录结构是否完整，避免把空目录加入转换列表。
                var validCount = 0;
                sessions.forEach(function(s) {
                    if (sessionHasVideoFrames(s.metadata || {})) validCount++;
                });
                if (sessions.length > 0 && validCount === 0) {
                    statusEl.textContent = '⚠ 目录中没有有效的采集数据（无视频帧）';
                    statusEl.style.color = 'var(--accent-red)';
                } else if (sessions.length > 0) {
                    statusEl.textContent = '✓ 已加载 ' + sessions.length + ' 个会话（' + validCount + ' 个有效）';
                    statusEl.style.color = 'var(--accent-green)';
                } else {
                    statusEl.textContent = '未找到数据，请检查目录路径';
                    statusEl.style.color = 'var(--accent-red)';
                }
                buildConvertFilterOptions();
                updateConvertUI();
                document.getElementById('convertBrowseBtn').textContent = '刷新会话';
                document.getElementById('convertStartBtn').disabled = sessions.length === 0;
                document.getElementById('convertSelectAllBtn').disabled = sessions.length === 0;
                document.getElementById('convertDeselectBtn').disabled = sessions.length === 0;
            })
            .catch(function() {
                cvSessionList.innerHTML = '<div class="empty-state">加载失败，请检查目录路径</div>';
                statusEl.textContent = '✗ 无法连接到服务器或路径无效';
                statusEl.style.color = 'var(--accent-red)';
            });
    }

    function formatFileSize(bytes) {
        if (bytes < 1024) return bytes + ' B';
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
    }

    // 汇总旧版顶层和新版槽位 metadata 中的帧数。
    function aggregateFrameCounts(m) {
        var result = { color: 0, depth: 0, 'ir-left': 0, 'ir-right': 0 };
        var imuTotal = 0, gripperTotal = 0, poseTotal = 0, pcTotal = 0;

        // 旧版扁平格式。
        var fc = m.frameCount || {};
        for (var k in fc) {
            if (k in result) result[k] += fc[k];
        }
        imuTotal += m.imuCount || 0;
        gripperTotal += m.gripperCount || 0;
        var gripperFrames = (m.gripper && m.gripper.frames) ? m.gripper.frames : 0;
        gripperTotal = Math.max(gripperTotal, gripperFrames);
        poseTotal += (m.pose && m.pose.frames) ? m.pose.frames : 0;
        pcTotal += m.pointCloudCount || 0;

        // 新版槽位格式：汇总所有槽位的帧数。
        var slots = m.slots;
        if (slots && typeof slots === 'object') {
            for (var slotName in slots) {
                var slot = slots[slotName] || {};
                var slotFc = slot.frameCount || {};
                for (var sk in slotFc) {
                    if (sk in result) result[sk] += slotFc[sk];
                }
                imuTotal += slot.imuCount || 0;
                var slotGripFrames = (slot.gripper && slot.gripper.frames) ? slot.gripper.frames : 0;
                gripperTotal = Math.max(gripperTotal, slotGripFrames, slot.gripperCount || 0);
                poseTotal += (slot.pose && slot.pose.frames) ? slot.pose.frames : 0;
                pcTotal += slot.pointCloudCount || 0;
            }
        }

        return {
            frameCount: result,
            imuCount: imuTotal,
            gripperCount: gripperTotal,
            poseCount: poseTotal,
            pointCloudCount: pcTotal
        };
    }

    function buildSessionDetailTags(m, s) {
        var agg = aggregateFrameCounts(m);
        var fc = agg.frameCount;
        var parts = [];

        var slots = m.slots;

        // 新格式下显示每个槽位的详细信息。
        if (slots && typeof slots === 'object' && Object.keys(slots).length > 0) {
            var posLabels = { head: '头部', left: '左手', right: '右手' };
            for (var slotName in slots) {
                var slot = slots[slotName] || {};
                var slotFc = slot.frameCount || {};
                var pos = posLabels[slot.position] || slot.position || slotName;
                var items = [];

                if (slotFc.color > 0) items.push('<span style="color:#3b82f6">彩色 ' + slotFc.color + '帧</span>');
                if (slotFc.depth > 0) items.push('<span style="color:#06b6d4">深度 ' + slotFc.depth + '帧</span>');
                if ((slotFc['ir-left'] || 0) > 0) items.push('<span style="color:#64748b">左红外 ' + slotFc['ir-left'] + '帧</span>');
                if ((slotFc['ir-right'] || 0) > 0) items.push('<span style="color:#64748b">右红外 ' + slotFc['ir-right'] + '帧</span>');

                var slotImu = slot.imuCount || 0;
                if (slotImu > 0) items.push('<span style="color:#f59e0b">IMU ' + slotImu + '</span>');

                var slotGrip = (slot.gripper && slot.gripper.frames) ? slot.gripper.frames : (slot.gripperCount || 0);
                if (slotGrip > 0) items.push('<span style="color:#22c55e">夹爪 ' + slotGrip + '行</span>');

                var slotPose = (slot.pose && slot.pose.frames) ? slot.pose.frames : 0;
                if (slotPose > 0) items.push('<span style="color:#ec4899">位姿 ' + slotPose + '</span>');

                var slotPc = slot.pointCloudCount || 0;
                if (slotPc > 0) items.push('<span style="color:#a855f7">点云 ' + slotPc + '</span>');

                if (items.length > 0) {
                    parts.push('<span style="color:#8b5cf6;font-weight:600">' + pos + '</span>: ' + items.join(' · '));
                }
            }
        } else {
            // 旧版扁平格式。
            if (fc.color > 0) parts.push('<span style="color:#3b82f6">彩色 ' + fc.color + '帧</span>');
            if (fc.depth > 0) parts.push('<span style="color:#06b6d4">深度 ' + fc.depth + '帧</span>');
            if (fc['ir-left'] > 0 || fc['ir-right'] > 0) parts.push('<span style="color:#64748b">红外 ' + (fc['ir-left'] || fc['ir-right']) + '帧</span>');
            if (agg.imuCount > 0) parts.push('<span style="color:#f59e0b">IMU ' + agg.imuCount + '</span>');
            if (agg.gripperCount > 0) parts.push('<span style="color:#22c55e">夹爪 ' + agg.gripperCount + '行</span>');
            if (agg.poseCount > 0) parts.push('<span style="color:#ec4899">位姿 ' + agg.poseCount + '</span>');
            if (agg.pointCloudCount > 0) parts.push('<span style="color:#a855f7">点云 ' + agg.pointCloudCount + '</span>');
        }

        // FPS 和分辨率信息固定放在标签末尾，便于快速扫读。
        var fps = m.fps || 30;
        var suffix = '<span style="color:#94a3b8">' + fps + 'fps</span>';
        if (s.size && s.size > 0) suffix += ' · <span style="color:#94a3b8">' + formatFileSize(s.size) + '</span>';
        parts.push(suffix);

        return parts.join('<br>');
    }

    function formatSessionDateLabel(id) {
        var d = parseSessionDate(id);
        if (!d) return id;
        var month = d.getMonth() + 1;
        var day = d.getDate();
        var h = String(d.getHours()).padStart(2, '0');
        var min = String(d.getMinutes()).padStart(2, '0');
        var sec = String(d.getSeconds()).padStart(2, '0');
        return d.getFullYear() + '/' + (month < 10 ? '0' + month : month) + '/' + (day < 10 ? '0' + day : day)
            + ' ' + h + ':' + min + ':' + sec;
    }

    function estimateDuration(m) {
        // 优先使用 metadata 中的精确时长（由全量时间戳计算）
        var maxDuration = 0;
        if (m.slots) {
            var slotKeys = Object.keys(m.slots);
            for (var si = 0; si < slotKeys.length; si++) {
                var slot = m.slots[slotKeys[si]];
                var videos = slot.videos || {};
                var vidKeys = Object.keys(videos);
                for (var vi = 0; vi < vidKeys.length; vi++) {
                    var v = videos[vidKeys[vi]];
                    if (v.durationSec && v.durationSec > maxDuration) {
                        maxDuration = v.durationSec;
                    }
                }
            }
        }
        if (maxDuration > 0) {
            if (maxDuration < 60) return maxDuration.toFixed(1) + '秒';
            if (maxDuration < 3600) return (maxDuration / 60).toFixed(1) + '分钟';
            return (maxDuration / 3600).toFixed(1) + '小时';
        }
        // fallback：旧版 metadata 没有 durationSec 时，用帧数 / 帧率估算
        var agg = aggregateFrameCounts(m);
        var fc = agg.frameCount;
        var fps = m.fps || 30;
        var maxFrames = Math.max(fc.color, fc.depth, fc['ir-left'], fc['ir-right']);
        if (maxFrames > 0 && fps > 0) {
            var seconds = maxFrames / fps;
            if (seconds < 60) return seconds.toFixed(1) + '秒';
            if (seconds < 3600) return (seconds / 60).toFixed(1) + '分钟';
            return (seconds / 3600).toFixed(1) + '小时';
        }
        return '';
    }

    function updateConvertUI() {
        var filtered = getFilteredSessions();
        var countEl = document.getElementById('convertSessionCount');
        if (countEl) countEl.textContent = '（' + filtered.length + ' / ' + cvState.sessions.length + '）';
        var html = '';
        if (filtered.length === 0) {
            html = '<div class="empty-state">' + (cvState.sessions.length === 0 ? '点击「浏览会话」加载可用会话' : '当前筛选无结果') + '</div>';
        }
        filtered.forEach(function(s) {
            var m = s.metadata || {};
            var detailTags = buildSessionDetailTags(m, s);
            var dateLabel = formatSessionDateLabel(s.id);
            var duration = estimateDuration(m);
            var durationTag = duration ? ' <span style="color:#94a3b8;font-size:10px;">~' + duration + '</span>' : '';
            html += '<div class="convert-session-item' + (cvState.selected.has(s.id) ? ' selected' : '') + '" data-id="' + s.id + '">'
                + '<input type="checkbox"' + (cvState.selected.has(s.id) ? ' checked' : '') + '>'
                + '<div class="convert-session-info">'
                + '<div class="convert-session-id">' + dateLabel + durationTag + '</div>'
                + '<div class="convert-session-detail">' + detailTags + '</div>'
                + '</div></div>';
        });
        cvSessionList.innerHTML = html;
        requestAnimationFrame(function() {
            cvSessionList.querySelectorAll('.convert-session-item').forEach(function(el, i) {
                setTimeout(function() { el.classList.add('visible'); }, i * 30);
            });
        });
        // 绑定会话卡片点击事件。
        cvSessionList.querySelectorAll('.convert-session-item').forEach(function(el) {
            el.addEventListener('click', function(e) {
                if (e.target.tagName === 'INPUT') return;
                var id = el.getAttribute('data-id');
                var cb = el.querySelector('input[type=checkbox]');
                if(cvState.selected.has(id)) { cvState.selected.delete(id); cb.checked = false; el.classList.remove('selected'); }
                else { cvState.selected.add(id); cb.checked = true; el.classList.add('selected'); }
                updateConvertBtns();
            });
            el.querySelector('input[type=checkbox]').addEventListener('change', function(e) {
                var id = el.getAttribute('data-id');
                if(e.target.checked) { cvState.selected.add(id); el.classList.add('selected'); }
                else { cvState.selected.delete(id); el.classList.remove('selected'); }
                updateConvertBtns();
            });
        });
        updateConvertBtns();
    }

    function updateConvertBtns() {
        var n = cvState.selected.size;
        document.getElementById('convertStartBtn').disabled = n === 0 || cvState.converting;
        document.getElementById('convertSelectAllBtn').disabled = n === cvState.sessions.length || cvState.converting;
        document.getElementById('convertDeselectBtn').disabled = n === 0 || cvState.converting;
    }

    // 开始数据转换
    function startConvert() {
        if(cvState.selected.size === 0 || cvState.converting) return;
        var sessions = Array.from(cvState.selected);
        var dir = document.getElementById('convertSourceDir').value.trim();
        var outDir = document.getElementById('convertOutputDir').value.trim();
        var format = document.getElementById('convertFormat').value;
        var formatLabels = { lerobot: 'LeRobot v3.0', hdf5: 'HDF5 v1.0', rlds: 'RLDS v0.1' };
        document.getElementById('convertProgressPanel').style.display = '';
        document.getElementById('convertProgressFill').style.width = '0%';
        document.getElementById('convertProgressFill').classList.remove('done');
        document.getElementById('convertProgressFill').classList.add('active');
        document.getElementById('convertProgressText').textContent = '0 / ' + sessions.length;
        document.getElementById('convertProgressStep').textContent = '准备中 · ' + (formatLabels[format] || format) + ' · ' + sessions.length + ' 个会话';
        document.getElementById('convertError').style.display = 'none';
        document.getElementById('convertStartBtn').disabled = true;
        document.getElementById('convertBrowseBtn').disabled = true;
        document.getElementById('convertSelectAllBtn').disabled = true;
        document.getElementById('convertDeselectBtn').disabled = true;
        cvState.converting = true;
        cvConvertStartTime = Date.now();
        cvState.pollTimer = setInterval(pollConvertProgress, 500);
        fetch('/api/convert', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'start', sourceDir: dir, sessions: sessions, task: '', outputDir: outDir, format: format })
        }).catch(function() {
            stopConvertPolling();
            document.getElementById('convertError').style.display = '';
            document.getElementById('convertError').textContent = '无法连接到服务器';
            showToast('转换请求发送失败', 'error');
        });
    }

    var cvConvertStartTime = null;

    function pollConvertProgress() {
        fetch('/api/convert/progress').then(function(r) { return r.json(); }).then(function(d) {
            if(!d.converting && cvState.converting) {
                stopConvertPolling();
                var skipped = (d.skipped || []).length;
                document.getElementById('convertProgressFill').style.width = '100%';
                document.getElementById('convertProgressFill').classList.add('done');
                document.getElementById('convertProgressText').textContent = d.done + ' / ' + d.total;
                var elapsed = cvConvertStartTime ? ((Date.now() - cvConvertStartTime) / 1000).toFixed(1) + '秒' : '';
                if(d.error) {
                    document.getElementById('convertProgressStep').textContent = '失败';
                    document.getElementById('convertError').style.display = '';
                    document.getElementById('convertError').textContent = '错误: ' + d.error;
                    showToast('转换失败: ' + d.error, 'error');
                } else {
                    document.getElementById('convertProgressStep').textContent = '完成' + (elapsed ? ' (' + elapsed + ')' : '');
                    var msg = '转换完成! 成功 ' + (d.done - skipped) + ' 个';
                    if(skipped > 0) msg += '，跳过 ' + skipped + ' 个(无可用视频帧)';
                    showToast(msg, 'success');
                }
                loadConvertResults();
                return;
            }
            var pct = Math.round(d.progress * 100);
            document.getElementById('convertProgressFill').style.width = pct + '%';
            document.getElementById('convertProgressText').textContent = (d.done || 0) + ' / ' + (d.total || 0);
            var stepText = d.step || '处理中...';
            if (d.current) stepText = d.current + ' — ' + stepText;
            document.getElementById('convertProgressStep').textContent = stepText;
            if(d.error) {
                document.getElementById('convertError').style.display = '';
                document.getElementById('convertError').textContent = '错误: ' + d.error;
                stopConvertPolling();
            }
        }).catch(function() {});
    }

    function stopConvertPolling() {
        cvState.converting = false;
        if(cvState.pollTimer) { clearInterval(cvState.pollTimer); cvState.pollTimer = null; }
        document.getElementById('convertStartBtn').disabled = cvState.selected.size === 0;
        document.getElementById('convertBrowseBtn').disabled = false;
        document.getElementById('convertSelectAllBtn').disabled = cvState.sessions.length === 0;
        document.getElementById('convertDeselectBtn').disabled = cvState.selected.size === 0;
        document.getElementById('convertProgressFill').classList.remove('active');
        cvConvertStartTime = null;
    }

    function updateResultHeader(converted, skipped) {
        var panel = document.getElementById('convertResultList');
        if (!panel) return;
        var header = panel.parentElement.querySelector('.status-header');
        if (!header) return;
        var counts = [];
        if (converted > 0) counts.push('<span style="color:#16a34a;font-weight:600">成功 ' + converted + ' 个</span>');
        if (skipped > 0) counts.push('<span style="color:#f59e0b;font-weight:600">跳过 ' + skipped + ' 个</span>');
        if (counts.length > 0) {
            header.innerHTML = '转换结果 <span style="margin-left:10px;font-size:11px;font-weight:400;">' + counts.join('<span style="margin:0 6px;color:var(--text-dim)">|</span>') + '</span>';
        } else {
            header.textContent = '转换结果';
        }
    }

    function loadConvertResults() {
        fetch('/api/convert/progress').then(function(r) { return r.json(); }).then(function(d) {
            var list = document.getElementById('convertResultList');
            var converted = d.converted || [];
            var skipped = d.skipped || [];
            if(converted.length === 0 && skipped.length === 0 && !d.error) {
                list.innerHTML = '<div class="empty-state">暂无转换记录</div>';
                updateResultHeader(0, 0);
                return;
            }
            updateResultHeader(converted.length, skipped.length);
            var formatLabels = { lerobot: 'LeRobot v3.0', hdf5: 'HDF5 v1.0', rlds: 'RLDS v0.1' };
            var curFormat = document.getElementById('convertFormat').value;
            var fmtLabel = formatLabels[curFormat] || curFormat;
            var outputDir = document.getElementById('convertOutputDir').value.trim() || 'data_converted';
            var html = '';
            converted.forEach(function(id) {
                html += '<div class="convert-result-item"><span class="status-ok">&#10003;</span> '
                    + '<span style="font-weight:600">' + formatSessionId(id) + '</span>'
                    + ' <span style="color:var(--primary);font-size:10px;font-weight:600">' + fmtLabel + '</span>'
                    + ' → <span style="color:var(--text-dim);font-size:10px">' + outputDir + '/' + id + '_' + curFormat + '/</span></div>';
            });
            skipped.forEach(function(id) {
                html += '<div class="convert-result-item" style="opacity:0.6"><span style="color:#f59e0b">&#9888;</span> '
                    + '<span style="font-weight:600">' + formatSessionId(id) + '</span>'
                    + ' <span style="color:var(--text-dim);font-size:11px">— 跳过(无可用视频帧)</span></div>';
            });
            if(d.error) {
                html += '<div class="convert-result-item"><span class="status-fail">&#10007;</span> ' + d.error + '</div>';
            }
            list.innerHTML = html;
            requestAnimationFrame(function() {
                list.querySelectorAll('.convert-result-item').forEach(function(el, i) {
                    setTimeout(function() { el.classList.add('visible'); }, i * 40);
                });
            });
        }).catch(function() {});
    }

    document.getElementById('convertBrowseBtn').addEventListener('click', loadSessions);
    document.getElementById('convertStartBtn').addEventListener('click', function(e) { addRipple(this, e); startConvert(); });
    document.getElementById('convertSelectAllBtn').addEventListener('click', function() {
        getFilteredSessions().forEach(function(s) { cvState.selected.add(s.id); });
        updateConvertUI();
    });
    document.getElementById('convertDeselectBtn').addEventListener('click', function() {
        cvState.selected.clear();
        updateConvertUI();
    });

    // 加载转换页面时自动浏览
    if(currentPage === 'convert') loadSessions();

    // ---- Gripper Control Page ----
    var gripPollTimer = null;

    function updateControlGripperPanel(slot, data) {
        var prefix = slot === 'left' ? 'gripLeft' : 'gripRight';
        var pos = data.position !== undefined ? data.position : 0;
        var closePct = Math.round(pos * 100);
        var posEl = document.getElementById(prefix + 'PosBig');
        if (posEl) posEl.textContent = closePct + '%';
        var rawEl = document.getElementById(prefix + 'RawBig');
        if (rawEl) rawEl.textContent = pos.toFixed(4);
        var barEl = document.getElementById(prefix + 'BarBig');
        if (barEl) {
            barEl.style.width = closePct + '%';
            barEl.style.background = closePct > 80 ? 'linear-gradient(90deg,#ef4444,#f97316)' :
                                     closePct > 50 ? 'linear-gradient(90deg,#f59e0b,#eab308)' :
                                     'linear-gradient(90deg,#3b82f6,#8b5cf6)';
        }
        var btn1El = document.getElementById(prefix + 'Btn1Big');
        var btn2El = document.getElementById(prefix + 'Btn2Big');
        if (btn1El) btn1El.style.background = (data.button1 === 1 || data.button1 === 2) ? '#ef4444' : '#22c55e';
        if (btn2El) btn2El.style.background = (data.button2 === 1 || data.button2 === 2) ? '#ef4444' : '#22c55e';
        var connEl = document.getElementById(prefix + 'CtrlConn');
        if (connEl) {
            connEl.textContent = data.connected ? '已连接' : '未连接';
            connEl.style.color = data.connected ? 'var(--accent-green)' : 'var(--text-dim)';
        }
    }

    // 更新侧边栏夹爪连接状态（用 gripperSlotMap 映射显示位置→后端槽位）
    function updateGripConnStatus() {
        ['left', 'right'].forEach(function(displayPos) {
            var backendSlot = gripperSlotMap[displayPos] || displayPos;
            var gs = (deviceInfo.gripperSlots || {})[backendSlot] || {};
            var connEl = document.getElementById(displayPos === 'left' ? 'gripConn1' : 'gripConn2');
            if (connEl) {
                connEl.textContent = gs.connected ? '已连接 (' + (gs.port || '') + ')' : '未连接';
                connEl.style.color = gs.connected ? 'var(--accent-green)' : 'var(--text-dim)';
            }
        });
    }

    function fetchGripperStatus() {
        updateGripConnStatus();
        ['left', 'right'].forEach(function(displayPos) {
            var backendSlot = gripperSlotMap[displayPos] || displayPos;
            fetch(API_ALT + '/api/gripper/' + backendSlot).then(function(r){return r.json();}).then(function(data) {
                if (!data) return;
                updateControlGripperPanel(displayPos, data);
            }).catch(function(){});
        });
    }

    // LED 发送（指定后端槽位）
    function sendGripperLed(backendSlot) {
        var r = document.getElementById('ledR').value || 0;
        var g = document.getElementById('ledG').value || 0;
        var b = document.getElementById('ledB').value || 0;
        var brightness = document.getElementById('ledBrightness');
        var bv = brightness ? Math.round(brightness.value * 255 / 100) : 128;
        fetch('/api/gripper/' + backendSlot + '/control', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'led', r: +r, g: +g, b: +b, brightness: +bv })
        }).then(function(r){return r.json();}).then(function(d) {
            if (d.success) {
                showToast((backendSlot === 'left' ? '左' : '右') + '夹爪 LED 设置成功', 'success');
            } else {
                showToast('LED 设置失败: ' + (d.error || '未知错误'), 'error');
            }
        }).catch(function(){ showToast('通信失败', 'error'); });
    }

    // 左夹爪 LED 按钮
    var gripLedLeftBtn = document.getElementById('gripLedLeftBtn');
    if (gripLedLeftBtn) gripLedLeftBtn.addEventListener('click', function() {
        sendGripperLed(gripperSlotMap.left || 'left');
    });
    // 右夹爪 LED 按钮
    var gripLedRightBtn = document.getElementById('gripLedRightBtn');
    if (gripLedRightBtn) gripLedRightBtn.addEventListener('click', function() {
        sendGripperLed(gripperSlotMap.right || 'right');
    });

    // 亮度滑块
    var ledBrightnessSlider = document.getElementById('ledBrightness');
    var ledBrightnessVal = document.getElementById('ledBrightnessVal');
    if (ledBrightnessSlider && ledBrightnessVal) {
        ledBrightnessSlider.addEventListener('input', function() {
            ledBrightnessVal.textContent = this.value + '%';
        });
    }

    // 控制页自动轮询夹爪状态。
    var origSwitchPage = switchPage;
    switchPage = function(pageName) {
        origSwitchPage(pageName);
        if (pageName === 'control') {
            fetchGripperStatus();
            gripPollTimer = setInterval(fetchGripperStatus, 200);
        } else {
            if (gripPollTimer) { clearInterval(gripPollTimer); gripPollTimer = null; }
        }
        if (pageName === 'electric') {
            fetchElectricGripperStatus();
            egPollTimer = setInterval(fetchElectricGripperStatus, 100);
            if (!egAutoQueried) {
                egAutoQueried = true;
            }
        } else {
            if (egPollTimer) { clearInterval(egPollTimer); egPollTimer = null; }
        }
        if (pageName === 'rps') {
            initRpsPage();
        } else {
            stopRpsCamera();
        }
    };

    // ---- 电动夹爪控制页 ----
    var egPollTimer = null;
    var egCurrentSlot = 'left';
    var egAutoQueried = false;
    var egKnownMotorId = null;

    function getElectricGripperSlot() {
        var gripperSlots = deviceInfo.gripperSlots || {};
        for (var pos in gripperSlots) {
            if (gripperSlots[pos].type === 'electric' && gripperSlots[pos].connected) return pos;
        }
        return null;
    }

    function fetchElectricGripperStatus() {
        var slot = getElectricGripperSlot();
        if (!slot) {
            var el = document.getElementById('egConnStatus');
            if (el) { el.textContent = '未检测到电动夹爪'; }
            return;
        }
        egCurrentSlot = slot;
        fetch(API_ALT + '/api/electric-gripper/' + slot).then(function(r){return r.json();}).then(function(data) {
            if (!data) return;
            var connEl = document.getElementById('egConnStatus');
            if (connEl) connEl.textContent = data.connected ? 'CAN 已连接' : '未连接';
            var canEl = document.getElementById('egCanStatus');
            if (canEl) canEl.textContent = data.connected ? 'GCAN USBCAN 已连接' : '未连接';
            var connDot = document.getElementById('egConnDot');
            if (connDot) connDot.className = 'eg-conn-dot' + (data.connected ? ' online' : '');
            var idEl = document.getElementById('egMotorIdVal');
            var idDot = document.getElementById('egIdDot');
            if (egKnownMotorId) {
                if (idEl) idEl.textContent = '0x' + egKnownMotorId.toString(16).toUpperCase() + ' (' + egKnownMotorId + ')';
                if (idDot) idDot.className = 'eg-conn-dot eg-conn-dot-id found';
            } else if (data.connected) {
                egKnownMotorId = 0x15;
                if (idEl) idEl.textContent = '0x015 (21)';
                if (idDot) idDot.className = 'eg-conn-dot eg-conn-dot-id found';
            } else {
                if (idEl) idEl.textContent = '未连接';
                if (idDot) idDot.className = 'eg-conn-dot eg-conn-dot-id';
            }
            if (!data.has) return;
            var posEl = document.getElementById('egPositionVal');
            if (posEl) posEl.textContent = data.positionDeg.toFixed(2) + '°';
            var barPct = Math.max(0, Math.min(100, data.positionDeg / EG_MAX_POSITION_DEG * 100));
            var barEl = document.getElementById('egPositionBar');
            if (barEl) {
                barEl.style.width = barPct + '%';
                barEl.style.background = barPct > 80 ? 'linear-gradient(90deg,#ef4444,#f97316)' : barPct > 50 ? 'linear-gradient(90deg,#f59e0b,#eab308)' : 'linear-gradient(90deg,#3b82f6,#8b5cf6)';
            }
            var actualEl = document.getElementById('egActualPosLabel');
            if (actualEl) actualEl.textContent = '实际: ' + data.positionDeg.toFixed(1) + '°';
            var velEl = document.getElementById('egVelocityVal');
            if (velEl) velEl.textContent = data.velocity.toFixed(1);
            var curEl = document.getElementById('egCurrentVal');
            if (curEl) curEl.textContent = data.current.toFixed(2);
            var mtEl = document.getElementById('egMotorTempVal');
            if (mtEl) { mtEl.textContent = data.motorTemp.toFixed(1); mtEl.style.color = data.motorTemp > 80 ? '#ef4444' : data.motorTemp > 60 ? '#f59e0b' : '#fff'; }
            var mosEl = document.getElementById('egMosTempVal');
            if (mosEl) { mosEl.textContent = data.mosTemp.toFixed(1); mosEl.style.color = data.mosTemp > 80 ? '#ef4444' : data.mosTemp > 60 ? '#f59e0b' : '#fff'; }
            var errEl = document.getElementById('egErrorCodeVal');
            if (errEl) { errEl.textContent = data.errorCode === 0 ? '正常' : '0x' + data.errorCode.toString(16); errEl.style.color = data.errorCode === 0 ? '#4ade80' : '#f87171'; }
            var rawEl = document.getElementById('egRawFrameVal');
            if (rawEl) rawEl.textContent = data.rawFrame || '--';
        }).catch(function(){});
    }

    function clampCur(v) { return Math.max(0.1, Math.min(4.0, v)); }
    function clampSpd(v) { return Math.max(1, Math.min(3276, v)); }

    function sendEgCommand(action, params, silent) {
        var slot = egCurrentSlot;
        if (!slot) { showToast('未检测到电动夹爪', 'error'); return; }
        var body = { action: action };
        if (params) { for (var k in params) body[k] = params[k]; }
        fetch('/api/electric-gripper/' + slot + '/control', {
            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
        }).then(function(r){return r.json();}).then(function(d) {
            if (!silent) {
                if (d.success) showToast(egActionLabel(action) + ' - 已发送', 'success');
                else if (d.error) showToast(egActionLabel(action) + ' 失败: ' + d.error, 'error');
            }
        }).catch(function(){ if (!silent) showToast('通信失败', 'error'); });
    }

    function egActionLabel(a) {
        var m = { enable:'使能电机', disable:'失能电机', clear_error:'清除错误', halt:'急停', set_zero:'设置零点', find_zero:'回零', query_motor_id:'查询电机ID', query_position:'查询位置', query_speed:'查询速度', query_current:'查询电流', set_position:'位置控制', set_speed:'速度控制', set_current:'电流控制', set_mit:'MIT控制', set_acceleration:'设置加速度' };
        return m[a] || a;
    }

    // 位置滑条节流：拖动时立即发 CAN，但限制在约 20Hz，避免总线过载。
    var egPosSlider = document.getElementById('egPositionSlider');
    var egPosLabel = document.getElementById('egTargetPosLabel');
    var egSliderThrottlePending = false;
    var egSliderLastSent = 0;
    var EG_SLIDER_MIN_INTERVAL = 50; // ms, ~20 sends/sec
    if (egPosSlider) {
        egPosSlider.addEventListener('input', function() {
            var sliderVal = parseFloat(this.value);
            if (egPosLabel) egPosLabel.textContent = sliderVal.toFixed(0) + '°';
            // 同步内联面板滑块
            syncControlPageToInline(sliderVal);
            var barPct = Math.max(0, Math.min(100, sliderVal / EG_MAX_POSITION_DEG * 100));
            var barEl = document.getElementById('egPositionBar');
            if (barEl) barEl.style.width = barPct + '%';
            var now = Date.now();
            if (now - egSliderLastSent >= EG_SLIDER_MIN_INTERVAL) {
                egSliderLastSent = now;
                var spd = clampSpd(parseFloat(document.getElementById('egSpeedInput').value) || 10);
                var cur = clampCur(parseFloat(document.getElementById('egCurrentInput').value) || 1.0);
                sendEgCommand('set_position', { position: sliderVal, speed: spd, current_limit: cur }, true);
                egSliderThrottlePending = false;
            } else if (!egSliderThrottlePending) {
                egSliderThrottlePending = true;
                var capturedVal = sliderVal;
                setTimeout(function() {
                    var spd = clampSpd(parseFloat(document.getElementById('egSpeedInput').value) || 10);
                    var cur = clampCur(parseFloat(document.getElementById('egCurrentInput').value) || 1.0);
                    sendEgCommand('set_position', { position: capturedVal, speed: spd, current_limit: cur }, true);
                    egSliderThrottlePending = false;
                    egSliderLastSent = Date.now();
                }, EG_SLIDER_MIN_INTERVAL - (now - egSliderLastSent));
            }
        });
    }

    // 位置移动按钮。
    var egMoveBtn = document.getElementById('egMoveBtn');
    if (egMoveBtn) egMoveBtn.addEventListener('click', function() {
        var pos = parseFloat(document.getElementById('egPositionSlider').value);
        var spd = clampSpd(parseFloat(document.getElementById('egSpeedInput').value) || 10);
        var cur = clampCur(parseFloat(document.getElementById('egCurrentInput').value) || 1.0);
        if (cur > 4.0) { showToast('电流限制最大 4A，已自动限制', 'error'); cur = 4.0; }
        sendEgCommand('set_position', { position: pos, speed: spd, current_limit: cur });
    });

    // 电机使能、失能、清错和急停控制。
    var egEnableBtn = document.getElementById('egEnableBtn');
    if (egEnableBtn) egEnableBtn.addEventListener('click', function() { sendEgCommand('enable'); });
    var egDisableBtn = document.getElementById('egDisableBtn');
    if (egDisableBtn) egDisableBtn.addEventListener('click', function() { sendEgCommand('disable'); });
    var egClearErrBtn = document.getElementById('egClearErrBtn');
    if (egClearErrBtn) egClearErrBtn.addEventListener('click', function() { sendEgCommand('clear_error'); });
    var egHaltBtn = document.getElementById('egHaltBtn');
    if (egHaltBtn) egHaltBtn.addEventListener('click', function() { sendEgCommand('halt'); });

    // 电机配置类命令。
    var egSetZeroBtn = document.getElementById('egSetZeroBtn');
    if (egSetZeroBtn) egSetZeroBtn.addEventListener('click', function() { sendEgCommand('set_zero'); });
    var egFindZeroBtn = document.getElementById('egFindZeroBtn');
    if (egFindZeroBtn) egFindZeroBtn.addEventListener('click', function() { sendEgCommand('find_zero'); });
    var egQueryIdBtn = document.getElementById('egQueryIdBtn');
    if (egQueryIdBtn) egQueryIdBtn.addEventListener('click', function() {
        sendEgCommand('query_motor_id');
        setTimeout(function() {
            egKnownMotorId = 0x15;
            var idEl = document.getElementById('egMotorIdVal');
            if (idEl && idEl.textContent.indexOf('0x') === -1) { idEl.textContent = '0x015 (21)'; }
            var idDot = document.getElementById('egIdDot');
            if (idDot) idDot.className = 'eg-conn-dot eg-conn-dot-id found';
        }, 800);
    });

    // 查找最小闭合极限。


    // 预设位置按钮。
    var presetBtns = document.querySelectorAll('.eg-preset-btn');
    for (var i = 0; i < presetBtns.length; i++) {
        presetBtns[i].addEventListener('click', function() {
            var pos = parseFloat(this.getAttribute('data-pos'));
            var slider = document.getElementById('egPositionSlider');
            var label = document.getElementById('egTargetPosLabel');
            if (slider) slider.value = pos;
            if (label) label.textContent = pos + '°';
            var barPct = Math.max(0, Math.min(100, pos / EG_MAX_POSITION_DEG * 100));
            var barEl = document.getElementById('egPositionBar');
            if (barEl) barEl.style.width = barPct + '%';
            var spd = clampSpd(parseFloat(document.getElementById('egSpeedInput').value) || 10);
            var cur = clampCur(parseFloat(document.getElementById('egCurrentInput').value) || 1.0);
            sendEgCommand('set_position', { position: pos, speed: spd, current_limit: cur });
        });
    }

    // 手动查询位置、速度和电流。
    var egQueryPosBtn = document.getElementById('egQueryPosBtn');
    if (egQueryPosBtn) egQueryPosBtn.addEventListener('click', function() { sendEgCommand('query_position'); });
    var egQuerySpdBtn = document.getElementById('egQuerySpdBtn');
    if (egQuerySpdBtn) egQuerySpdBtn.addEventListener('click', function() { sendEgCommand('query_speed'); });
    var egQueryCurBtn = document.getElementById('egQueryCurBtn');
    if (egQueryCurBtn) egQueryCurBtn.addEventListener('click', function() { sendEgCommand('query_current'); });

    // 加速度配置。
    var egSetAccelBtn = document.getElementById('egSetAccelBtn');
    if (egSetAccelBtn) egSetAccelBtn.addEventListener('click', function() {
        var accel = parseFloat(document.getElementById('egAccelInput').value) || 10;
        sendEgCommand('set_acceleration', { acceleration: accel });
    });

    // MIT
    var egMitKpSlider = document.getElementById('egMitKpSlider');
    var egMitKdSlider = document.getElementById('egMitKdSlider');
    if (egMitKpSlider) { egMitKpSlider.addEventListener('input', function() { var el = document.getElementById('egMitKpVal'); if (el) el.textContent = this.value; }); }
    if (egMitKdSlider) { egMitKdSlider.addEventListener('input', function() { var el = document.getElementById('egMitKdVal'); if (el) el.textContent = this.value; }); }
    var egMitExecBtn = document.getElementById('egMitExecBtn');
    if (egMitExecBtn) egMitExecBtn.addEventListener('click', function() {
        var kp = parseFloat(document.getElementById('egMitKpSlider').value) || 0;
        var kd = parseFloat(document.getElementById('egMitKdSlider').value) || 0;
        var pos = parseFloat(document.getElementById('egMitPosInput').value) || 0;
        var spd = parseFloat(document.getElementById('egMitSpeedInput').value) || 0;
        var trq = parseFloat(document.getElementById('egMitTorqueInput').value) || 0;
        sendEgCommand('set_mit', { kp: kp, kd: kd, position: pos, speed: spd, torque: trq });
    });

    // 速度控制滑条和快捷按钮。
    var egSpeedSlider = document.getElementById('egTargetSpeedInput');
    function egUpdateSpeedUI(val) {
        var v = parseInt(val) || 0;
        var barEl = document.getElementById('egSpeedBar');
        var valEl = document.getElementById('egSpeedValLabel');
        if (valEl) valEl.textContent = v;
        if (barEl) {
            var pct = Math.abs(v) / 500 * 50;
            if (v >= 0) {
                barEl.style.left = '50%';
                barEl.style.width = pct + '%';
                barEl.className = 'eg-speed-bar';
            } else {
                barEl.style.left = (50 - pct) + '%';
                barEl.style.width = pct + '%';
                barEl.className = 'eg-speed-bar neg';
            }
        }
    }
    if (egSpeedSlider) {
        egUpdateSpeedUI(egSpeedSlider.value);
        egSpeedSlider.addEventListener('input', function() { egUpdateSpeedUI(this.value); });
    }
    var egSpeedAdjBtns = document.querySelectorAll('.eg-btn-speed-adj');
    for (var j = 0; j < egSpeedAdjBtns.length; j++) {
        egSpeedAdjBtns[j].addEventListener('click', function() {
            var v = parseInt(this.getAttribute('data-delta'));
            if (egSpeedSlider) { egSpeedSlider.value = v; egUpdateSpeedUI(v); }
            var cur = clampCur(parseFloat(document.getElementById('egSpeedCurrentInput').value) || 1.0);
            sendEgCommand('set_speed', { speed: v, current_limit: cur });
        });
    }
    var egSpeedCtrlBtn = document.getElementById('egSpeedCtrlBtn');
    if (egSpeedCtrlBtn) egSpeedCtrlBtn.addEventListener('click', function() {
        var spd = parseFloat(document.getElementById('egTargetSpeedInput').value) || 0;
        var cur = clampCur(parseFloat(document.getElementById('egSpeedCurrentInput').value) || 1.0);
        sendEgCommand('set_speed', { speed: spd, current_limit: cur });
    });
    var egSpeedStopBtn = document.getElementById('egSpeedStopBtn');
    if (egSpeedStopBtn) egSpeedStopBtn.addEventListener('click', function() {
        if (egSpeedSlider) { egSpeedSlider.value = 0; egUpdateSpeedUI(0); }
        sendEgCommand('set_speed', { speed: 0, current_limit: 1.0 });
    });

    // 电流控制。
    var egCurrentCtrlBtn = document.getElementById('egCurrentCtrlBtn');
    if (egCurrentCtrlBtn) egCurrentCtrlBtn.addEventListener('click', function() {
        var cur = parseFloat(document.getElementById('egTargetCurrentInput').value) || 0;
        if (Math.abs(cur) > 4) { showToast('电流限制最大 ±4A', 'error'); return; }
        sendEgCommand('set_current', { current: cur });
    });

    // ---- 剪刀石头布：IMX335 手势识别 + 电动夹爪出拳 ----
    var rpsState = {
        initialized: false,
        stream: null,
        hands: null,
        running: false,
        processing: false,
        lastGesture: 'unknown',
        stableGesture: 'unknown',
        stableCount: 0,
        lastCommandGesture: '',
        scissorsTimer: null,
        scissorsNext: EG_RPS_SCISSORS_A_DEG,
        modelReady: false
    };

    var RPS_LABELS = {
        rock: '拳头',
        paper: '布',
        scissors: '剪刀',
        unknown: '未识别'
    };
    var RPS_GRIPPER_LABELS = {
        rock: '布',
        paper: '剪刀',
        scissors: '拳头',
        unknown: '--'
    };

    function initRpsPage() {
        if (!rpsState.initialized) {
            rpsState.initialized = true;
            bindRpsEvents();
            initRpsHandsModel();
            refreshRpsCameras();
        }
        updateRpsGripperSlot();
    }

    function bindRpsEvents() {
        var startBtn = document.getElementById('rpsStartCameraBtn');
        var stopBtn = document.getElementById('rpsStopCameraBtn');
        var enableBtn = document.getElementById('rpsEnableBtn');
        var stopMotionBtn = document.getElementById('rpsStopMotionBtn');
        var cameraSelect = document.getElementById('rpsCameraSelect');
        if (startBtn) startBtn.addEventListener('click', startRpsCamera);
        if (stopBtn) stopBtn.addEventListener('click', stopRpsCamera);
        if (enableBtn) enableBtn.addEventListener('click', function() {
            updateRpsGripperSlot();
            sendEgCommand('enable');
        });
        if (stopMotionBtn) stopMotionBtn.addEventListener('click', function() {
            stopRpsScissorsMotion();
            sendRpsGripperAction('stop_motion', {}, false);
        });
        if (cameraSelect) cameraSelect.addEventListener('change', function() {
            if (rpsState.running) startRpsCamera();
        });
        if (navigator.mediaDevices && navigator.mediaDevices.addEventListener) {
            navigator.mediaDevices.addEventListener('devicechange', refreshRpsCameras);
        }
    }

    function initRpsHandsModel() {
        var statusEl = document.getElementById('rpsCameraStatus');
        if (!window.Hands) {
            if (statusEl) statusEl.textContent = '手势模型未加载，请确认网络可访问 jsdelivr';
            return;
        }
        rpsState.hands = new Hands({
            locateFile: function(file) {
                return 'https://cdn.jsdelivr.net/npm/@mediapipe/hands/' + file;
            }
        });
        rpsState.hands.setOptions({
            maxNumHands: 1,
            modelComplexity: 1,
            minDetectionConfidence: 0.65,
            minTrackingConfidence: 0.65
        });
        rpsState.hands.onResults(onRpsHandsResults);
        rpsState.modelReady = true;
        if (statusEl) statusEl.textContent = '模型已就绪';
    }

    function refreshRpsCameras() {
        var select = document.getElementById('rpsCameraSelect');
        if (!select || !navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices) return;
        navigator.mediaDevices.enumerateDevices().then(function(devices) {
            var cameras = devices.filter(function(d) { return d.kind === 'videoinput'; });
            var currentValue = select.value;
            select.innerHTML = '';
            cameras.forEach(function(cam, index) {
                var opt = document.createElement('option');
                opt.value = cam.deviceId;
                opt.textContent = cam.label || ('摄像头 ' + (index + 1));
                select.appendChild(opt);
            });
            if (cameras.length === 0) {
                var empty = document.createElement('option');
                empty.value = '';
                empty.textContent = '未发现摄像头';
                select.appendChild(empty);
                return;
            }
            var imx = cameras.filter(function(cam) {
                return (cam.label || '').toLowerCase().indexOf('imx335') >= 0;
            })[0];
            select.value = currentValue || (imx ? imx.deviceId : cameras[0].deviceId);
        }).catch(function() {});
    }

    function startRpsCamera() {
        if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
            showToast('当前浏览器不支持摄像头访问', 'error');
            return;
        }
        if (!rpsState.modelReady) {
            initRpsHandsModel();
            if (!rpsState.modelReady) {
                showToast('手势模型未加载，无法识别', 'error');
                return;
            }
        }
        stopRpsCamera();
        var select = document.getElementById('rpsCameraSelect');
        var deviceId = select ? select.value : '';
        var constraints = {
            video: {
                width: { ideal: 1280 },
                height: { ideal: 720 },
                frameRate: { ideal: 30 }
            },
            audio: false
        };
        if (deviceId) constraints.video.deviceId = { exact: deviceId };
        navigator.mediaDevices.getUserMedia(constraints).then(function(stream) {
            rpsState.stream = stream;
            rpsState.running = true;
            var video = document.getElementById('rpsVideo');
            if (video) {
                video.srcObject = stream;
                video.onloadedmetadata = function() {
                    video.play();
                    resizeRpsCanvas();
                    refreshRpsCameras();
                    rpsProcessFrame();
                };
            }
            setRpsStatus('相机运行中');
        }).catch(function(err) {
            setRpsStatus('相机打开失败');
            showToast('相机打开失败: ' + (err && err.message ? err.message : '未知错误'), 'error');
        });
    }

    function stopRpsCamera() {
        rpsState.running = false;
        rpsState.processing = false;
        stopRpsScissorsMotion();
        if (rpsState.stream) {
            rpsState.stream.getTracks().forEach(function(track) { track.stop(); });
            rpsState.stream = null;
        }
        var video = document.getElementById('rpsVideo');
        if (video) video.srcObject = null;
        clearRpsCanvas();
        updateRpsGestureUI('unknown', 0, true);
        setRpsStatus('相机已关闭');
    }

    function resizeRpsCanvas() {
        var canvas = document.getElementById('rpsCanvas');
        if (!canvas) return;
        var rect = canvas.getBoundingClientRect();
        var dpr = window.devicePixelRatio || 1;
        var w = Math.max(1, Math.floor(rect.width * dpr));
        var h = Math.max(1, Math.floor(rect.height * dpr));
        if (canvas.width !== w || canvas.height !== h) {
            canvas.width = w;
            canvas.height = h;
        }
    }

    function clearRpsCanvas() {
        var canvas = document.getElementById('rpsCanvas');
        if (!canvas) return;
        var ctx = canvas.getContext('2d');
        if (ctx) ctx.clearRect(0, 0, canvas.width, canvas.height);
    }

    function rpsProcessFrame() {
        if (!rpsState.running) return;
        var video = document.getElementById('rpsVideo');
        if (!video || !rpsState.hands || video.readyState < 2) {
            requestAnimationFrame(rpsProcessFrame);
            return;
        }
        if (!rpsState.processing) {
            rpsState.processing = true;
            rpsState.hands.send({ image: video }).catch(function() {}).then(function() {
                rpsState.processing = false;
            });
        }
        requestAnimationFrame(rpsProcessFrame);
    }

    function onRpsHandsResults(results) {
        resizeRpsCanvas();
        clearRpsCanvas();
        var confidence = 0;
        var gesture = 'unknown';
        var hands = results && results.multiHandLandmarks ? results.multiHandLandmarks : [];
        if (hands.length > 0) {
            confidence = results.multiHandedness && results.multiHandedness[0] ? (results.multiHandedness[0].score || 0.8) : 0.8;
            drawRpsLandmarks(hands[0]);
            gesture = classifyRpsGesture(hands[0]);
        }
        updateRpsGestureUI(gesture, confidence, false);
        maybeApplyRpsGesture(gesture);
    }

    function drawRpsLandmarks(landmarks) {
        var canvas = document.getElementById('rpsCanvas');
        if (!canvas || !landmarks) return;
        var ctx = canvas.getContext('2d');
        var w = canvas.width;
        var h = canvas.height;
        var links = [
            [0,1],[1,2],[2,3],[3,4],
            [0,5],[5,6],[6,7],[7,8],
            [5,9],[9,10],[10,11],[11,12],
            [9,13],[13,14],[14,15],[15,16],
            [13,17],[17,18],[18,19],[19,20],[0,17]
        ];
        ctx.save();
        ctx.lineCap = 'round';
        ctx.lineJoin = 'round';
        ctx.lineWidth = Math.max(2, w * 0.004);
        ctx.strokeStyle = 'rgba(59,130,246,0.95)';
        links.forEach(function(pair) {
            var a = landmarks[pair[0]];
            var b = landmarks[pair[1]];
            ctx.beginPath();
            ctx.moveTo((1 - a.x) * w, a.y * h);
            ctx.lineTo((1 - b.x) * w, b.y * h);
            ctx.stroke();
        });
        ctx.fillStyle = '#22c55e';
        landmarks.forEach(function(p) {
            ctx.beginPath();
            ctx.arc((1 - p.x) * w, p.y * h, Math.max(4, w * 0.006), 0, Math.PI * 2);
            ctx.fill();
        });
        ctx.restore();
    }

    function classifyRpsGesture(lm) {
        function extended(tip, pip) {
            return lm[tip].y < lm[pip].y - 0.025;
        }
        var index = extended(8, 6);
        var middle = extended(12, 10);
        var ring = extended(16, 14);
        var pinky = extended(20, 18);
        var extendedCount = [index, middle, ring, pinky].filter(Boolean).length;
        if (index && middle && !ring && !pinky) return 'scissors';
        if (extendedCount >= 3 && index && middle) return 'paper';
        if (extendedCount <= 1 && !index && !middle) return 'rock';
        return 'unknown';
    }

    function updateRpsGestureUI(gesture, confidence, resetStable) {
        var label = RPS_LABELS[gesture] || RPS_LABELS.unknown;
        var pill = document.getElementById('rpsGesturePill');
        var conf = document.getElementById('rpsConfidence');
        var user = document.getElementById('rpsUserGesture');
        if (pill) pill.textContent = label;
        if (conf) conf.textContent = confidence ? Math.round(confidence * 100) + '%' : '--';
        if (user) user.textContent = label;
        if (resetStable) {
            rpsState.lastGesture = 'unknown';
            rpsState.stableGesture = 'unknown';
            rpsState.stableCount = 0;
            rpsState.lastCommandGesture = '';
        }
    }

    function maybeApplyRpsGesture(gesture) {
        if (gesture === rpsState.lastGesture) {
            rpsState.stableCount++;
        } else {
            rpsState.lastGesture = gesture;
            rpsState.stableCount = 1;
        }
        if (gesture === 'unknown') {
            if (rpsState.stableCount > 8) {
                stopRpsScissorsMotion();
                setRpsRound('--', '--', '等待稳定手势');
                rpsState.lastCommandGesture = '';
            }
            return;
        }
        if (rpsState.stableCount < 4 || gesture === rpsState.lastCommandGesture) return;
        rpsState.stableGesture = gesture;
        rpsState.lastCommandGesture = gesture;
        applyRpsGripperMove(gesture);
    }

    function applyRpsGripperMove(gesture) {
        var autoPlay = document.getElementById('rpsAutoPlayToggle');
        if (autoPlay && !autoPlay.checked) {
            setRpsRound(RPS_GRIPPER_LABELS[gesture], '--', '已识别，自动出拳关闭');
            return;
        }
        updateRpsGripperSlot();
        if (gesture === 'scissors') {
            stopRpsScissorsMotion();
            setRpsRound('拳头', EG_MAX_POSITION_DEG + '°', '夹爪出拳：拳头');
            sendRpsPosition(EG_MAX_POSITION_DEG, false);
        } else if (gesture === 'rock') {
            stopRpsScissorsMotion();
            setRpsRound('布', '0°', '夹爪出拳：布');
            sendRpsPosition(0, false);
        } else if (gesture === 'paper') {
            setRpsRound('剪刀', EG_RPS_SCISSORS_A_DEG + '° / ' + EG_RPS_SCISSORS_B_DEG + '°', '夹爪出拳：剪刀');
            startRpsScissorsMotion();
        }
    }

    function sendRpsPosition(position, silent) {
        sendRpsGripperAction('set_position', {
            position: position,
            speed: 3000,
            current_limit: 4.0
        }, silent);
    }

    function sendRpsGripperAction(action, params, silent) {
        var slot = getElectricGripperSlot();
        if (!slot) {
            if (!silent) showToast('未检测到电动夹爪', 'error');
            setRpsRound('--', '--', '未检测到电动夹爪');
            return;
        }
        egCurrentSlot = slot;
        var body = { action: action };
        params = params || {};
        for (var k in params) body[k] = params[k];
        fetch('/api/electric-gripper/' + slot + '/control', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        }).then(function(r) { return r.json(); }).then(function(d) {
            if (!silent && !d.success) showToast('夹爪指令失败: ' + (d.error || '未知错误'), 'error');
        }).catch(function() {
            if (!silent) showToast('夹爪通信失败', 'error');
        });
    }

    function startRpsScissorsMotion() {
        if (rpsState.scissorsTimer) return;
        rpsState.scissorsNext = EG_RPS_SCISSORS_A_DEG;
        sendRpsPosition(rpsState.scissorsNext, true);
        rpsState.scissorsTimer = setInterval(function() {
            rpsState.scissorsNext = rpsState.scissorsNext === EG_RPS_SCISSORS_A_DEG ? EG_RPS_SCISSORS_B_DEG : EG_RPS_SCISSORS_A_DEG;
            sendRpsPosition(rpsState.scissorsNext, true);
            var target = document.getElementById('rpsTargetPosition');
            if (target) target.textContent = rpsState.scissorsNext + '°';
        }, 650);
    }

    function stopRpsScissorsMotion() {
        if (rpsState.scissorsTimer) {
            clearInterval(rpsState.scissorsTimer);
            rpsState.scissorsTimer = null;
        }
    }

    function setRpsRound(gripperGesture, targetPosition, status) {
        var g = document.getElementById('rpsGripperGesture');
        var t = document.getElementById('rpsTargetPosition');
        var s = document.getElementById('rpsRoundStatus');
        if (g) g.textContent = gripperGesture || '--';
        if (t) t.textContent = targetPosition || '--';
        if (s) s.textContent = status || '待机';
    }

    function setRpsStatus(text) {
        var el = document.getElementById('rpsCameraStatus');
        if (el) el.textContent = text;
    }

    function updateRpsGripperSlot() {
        var slot = getElectricGripperSlot();
        var el = document.getElementById('rpsGripperSlot');
        if (el) el.textContent = slot ? (slot === 'left' ? '左夹爪' : '右夹爪') : '未检测';
        if (slot) egCurrentSlot = slot;
    }

    // ---- 按钮涟漪效果 ----
    function addRipple(btn, e) {
        var rect = btn.getBoundingClientRect();
        var ripple = document.createElement('span');
        ripple.className = 'ripple';
        var size = Math.max(rect.width, rect.height);
        ripple.style.width = ripple.style.height = size + 'px';
        ripple.style.left = (e.clientX - rect.left - size / 2) + 'px';
        ripple.style.top = (e.clientY - rect.top - size / 2) + 'px';
        btn.appendChild(ripple);
        setTimeout(function() { ripple.remove(); }, 600);
    }

    // ---- 全局事件绑定 ----
    window.addEventListener('beforeunload', function() { stopAllStreamImgs(); });

    // 交换左右摄像头按钮
    document.getElementById('swapCamerasBtn').addEventListener('click', function() {
        // 切换交换状态
        handsSwapped = !handsSwapped;
        // 关闭所有活跃流
        Object.keys(state).forEach(function(k) {
            if (state[k]) {
                var bk = streamBackendKeys[k] || toBackendKey(k);
                delete streamBackendKeys[k];
                fetch('/api/control?stream=' + bk + '&action=off').catch(function(){});
                state[k] = false;
            }
        });
        // 停止视频
        stopAllStreamImgs();
        videoGrid.className = 'video-grid guide-mode';
        videoGrid.innerHTML = emptyGuideHtml();
        var _gs3 = document.getElementById('gripperSection');
        if (_gs3) _gs3.style.display = 'none';
        // 重建映射和开关
        rebuildSlotMap();
        lastToggleHash = '';
        buildCameraToggles();
        updateActiveCount();
        showToast(handsSwapped ? '已交换左右手摄像头' : '已恢复原始左右手', 'success');
    });

    // 一键全开/全关按钮
    var toggleAllBtn = document.getElementById('toggleAllBtn');
    if (toggleAllBtn) {
        toggleAllBtn.addEventListener('click', function() {
            var keys = getAvailableStreamKeys();
            if (keys.length === 0) {
                updateActiveCount();
                showToast('当前没有可开启的设备', 'warning');
                return;
            }
            var allCurrentlyOn = true;
            keys.forEach(function(key) { if (!state[key]) allCurrentlyOn = false; });
            var turnOn = !allCurrentlyOn;
            keys.forEach(function(key) {
                state[key] = turnOn;
                updateSwitchUI(key);
                var backendKey = turnOn ? toBackendKey(key) : (streamBackendKeys[key] || toBackendKey(key));
                if (turnOn) streamBackendKeys[key] = backendKey;
                else delete streamBackendKeys[key];
                fetch('/api/control?stream=' + backendKey + '&action=' + (turnOn ? 'on' : 'off'))
                    .then(function() { serverOnline = true; statusDot.classList.add('online'); statusText.textContent = '已连接'; })
                    .catch(function() {});
            });
            updateActiveCount();
            updateVideoCards();
            // 点云安全网：2秒后检查点云 viewer 是否收到数据，没有则重新发送控制命令并重建 viewer
            setTimeout(function() {
                var pcKeys = Object.keys(pcViewers);
                pcKeys.forEach(function(k) {
                    var v = pcViewers[k];
                    if (v && v.count === 0) {
                        var bk2 = toBackendKey(k);
                        var sl = bk2.replace('-pointcloud', '');
                        streamBackendKeys[k] = bk2;
                        fetch('/api/control?stream=' + bk2 + '&action=on').catch(function(){});
                        stopPointCloudViewer(k);
                        var c2 = document.getElementById('pc_' + k);
                        if (c2) startPointCloudViewer(k, c2, sl);
                    }
                });
            }, 2000);
        });
    }

    var resizeTimer = null;
    window.addEventListener('resize', function() {
        clearTimeout(resizeTimer);
        resizeTimer = setTimeout(function() {
            // 当前页面没有 IMU 画布时无需调整尺寸。
        }, 100);
    });

    // ===== 路径配置 & 目录浏览器 =====
    var currentPaths = { collect: '', converted: '' };

    function loadPaths() {
        fetch('/api/paths').then(function(r) { return r.json(); }).then(function(d) {
            currentPaths = d;
            var el = document.getElementById('collectPathDisplay');
            if (el) el.textContent = d.collect || '--';
            if (currentPage === 'collect') fetchHistory();
        }).catch(function() {});
    }
    loadPaths();

    function savePath(type, value) {
        var body = {};
        body[type] = value;
        currentPaths[type] = value;
        fetch('/api/paths', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        }).then(function(r) { return r.json(); }).then(function(d) {
            currentPaths = d;
            var el = document.getElementById('collectPathDisplay');
            if (el) el.textContent = d.collect || '--';
            if (type === 'collect') fetchHistory();
        }).catch(function() {});
    }

    // 目录浏览器弹窗
    var dirModalState = { currentPath: '/', callback: null };

    function openDirBrowser(callback, startPath) {
        dirModalState.callback = callback;
        dirModalState.currentPath = startPath || '/';
        document.getElementById('dirModalOverlay').style.display = 'flex';
        loadDirList(dirModalState.currentPath);
    }

    function closeDirBrowser() {
        document.getElementById('dirModalOverlay').style.display = 'none';
        dirModalState.callback = null;
    }

    function loadDirList(path) {
        dirModalState.currentPath = path;
        document.getElementById('dirModalPath').textContent = path;
        var listEl = document.getElementById('dirModalList');
        listEl.innerHTML = '<div style="padding:16px;color:var(--text-dim)">加载中...</div>';
        fetch('/api/browse-dir?path=' + encodeURIComponent(path)).then(function(r) { return r.json(); }).then(function(d) {
            var html = '';
            // 上级目录入口。
            if (d.parent && d.parent !== d.path) {
                html += '<div class="dir-item dir-parent" data-path="' + d.parent + '">&#128281; ..</div>';
            }
            // 子目录列表：直接使用后端返回的绝对路径。
            var base = d.path || '';
            if (base.length > 0 && base.charAt(base.length - 1) !== '\\') base += '\\';
            (d.dirs || []).forEach(function(name) {
                html += '<div class="dir-item" data-path="' + base + name + '">&#128193; ' + name + '</div>';
            });
            if (!html) html = '<div style="padding:16px;color:var(--text-dim)">空目录</div>';
            listEl.innerHTML = html;
            listEl.querySelectorAll('.dir-item').forEach(function(el) {
                el.addEventListener('click', function() {
                    loadDirList(el.getAttribute('data-path'));
                });
            });
        }).catch(function() {
            listEl.innerHTML = '<div style="padding:16px;color:var(--danger)">无法读取目录</div>';
        });
    }

    document.getElementById('dirModalClose').addEventListener('click', closeDirBrowser);
    document.getElementById('dirModalCancel').addEventListener('click', closeDirBrowser);
    document.getElementById('dirModalConfirm').addEventListener('click', function() {
        if (dirModalState.callback) dirModalState.callback(dirModalState.currentPath);
        closeDirBrowser();
    });
    document.getElementById('dirModalOverlay').addEventListener('click', function(e) {
        if (e.target === this) closeDirBrowser();
    });

    // 数据采集页面路径选择按钮
    document.getElementById('collectPathBtn').addEventListener('click', function() {
        openDirBrowser(function(path) {
            savePath('collect', path);
        }, currentPaths.collect || '/');
    });

    // 数据转换页面路径选择按钮 - 从项目实际路径开始浏览
    document.querySelectorAll('.convert-dir-btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            var targetId = btn.getAttribute('data-target');
            var input = document.getElementById(targetId);
            // 先从后端获取绝对路径，再以该路径作为目录浏览起点。
            fetch('/api/paths').then(function(r) { return r.json(); }).then(function(paths) {
                var startPath;
                if (targetId === 'convertSourceDir') {
                    startPath = paths.collect || 'C:\\';
                } else if (targetId === 'convertOutputDir') {
                    startPath = paths.converted || 'C:\\';
                } else {
                    startPath = (input && input.value) ? input.value : 'C:\\';
                }
                openDirBrowser(function(path) {
                    if (input) input.value = path;
                }, startPath);
            }).catch(function() {
                openDirBrowser(function(path) {
                    if (input) input.value = path;
                }, 'C:\\');
            });
        });
    });

    // ===== 点云 Canvas 2D 渲染器 =====
    var pcViewers = {};  // key -> viewer state

    var _pcRotCos = Math.cos(130 * Math.PI / 180), _pcRotSin = Math.sin(130 * Math.PI / 180);

    function pcHeatmapColor(t) {
        t = Math.max(0, Math.min(1, t));
        if(t<0.2) return [0, 0, Math.round(128+t*5*127)];
        if(t<0.4){var s=(t-0.2)*5; return [0, Math.round(s*255), 255];}
        if(t<0.6){var s=(t-0.4)*5; return [0, 255, Math.round((1-s)*255)];}
        if(t<0.8){var s=(t-0.6)*5; return [Math.round(s*255), 255, 0];}
        var s=(t-0.8)*5; return [255, Math.round((1-s)*255), 0];
    }

    // 红外模拟：单色绿白色调
    function pcIRColor(t) {
        t = Math.max(0, Math.min(1, t));
        var v = Math.round(40 + t * 215);
        return [Math.round(v * 0.3), v, Math.round(v * 0.25)];
    }

    // 写入一个 2x2 像素块（带深度检测）
    function pcPutPixel(px, db, di, w, totalPx, rz2, r, g, b) {
        if (di < 0 || di + w + 1 >= totalPx) return;
        if (rz2 >= db[di]) return;
        db[di] = rz2; var pi = di * 4;
        px[pi] = r; px[pi+1] = g; px[pi+2] = b; px[pi+3] = 255;
        if (rz2 < db[di+1]) { db[di+1] = rz2; pi = (di+1)*4; px[pi]=r; px[pi+1]=g; px[pi+2]=b; px[pi+3]=255; }
        var di2 = di + w;
        if (rz2 < db[di2]) { db[di2] = rz2; pi = di2*4; px[pi]=r; px[pi+1]=g; px[pi+2]=b; px[pi+3]=255; }
        if (rz2 < db[di2+1]) { db[di2+1] = rz2; pi = (di2+1)*4; px[pi]=r; px[pi+1]=g; px[pi+2]=b; px[pi+3]=255; }
    }

    function pcProj3D(x, y, z, cY, sY, cX, sX, ox, oy, sc) {
        var rx2 = x*_pcRotCos + z*_pcRotSin, rz3 = -x*_pcRotSin + z*_pcRotCos;
        x = rx2; z = rz3;
        var rx = x*cY + z*sY, rz = -x*sY + z*cY;
        var ry = y*cX - rz*sX;
        return [ox + rx*sc, oy - ry*sc];
    }

    function startPointCloudViewer(key, canvas, slot) {
        if (pcViewers[key]) { console.log('[PC] already exists: ' + key); return; }
        console.log('[PC] starting viewer: key=' + key + ' slot=' + slot);
        var ctx = canvas.getContext('2d');
        if (!ctx) return;

        var state = {
            pts: [], count: 0,
            yaw: 0.6, pitch: 0.4, zoom: 1, panX: 0, panY: 0,
            drag: false, pan: false, lx: 0, ly: 0,
            _img: null, _dep: null, _w: 0, _h: 0,
            fps: 0, _fc: 0, _ft: performance.now(),
            _fitted: false, _dataRange: 1, _baseScale: 1,
            mode: 'depth',
            _minZ: 0, _maxZ: 1, _minY: 0, _maxY: 1,
            running: true, timer: null, canvas: canvas, ctx: ctx,
            busy: false
        };

        // 鼠标交互
        canvas.addEventListener('mousedown', function(e) {
            if (e.button === 0) { state.drag = true; }
            else if (e.button === 2) { state.pan = true; }
            state.lx = e.clientX; state.ly = e.clientY;
        });
        canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });
        window.addEventListener('mouseup', function() { state.drag = false; state.pan = false; });
        window.addEventListener('mousemove', function(e) {
            if (state.drag) {
                state.yaw += (e.clientX - state.lx) * 0.005;
                state.pitch += (e.clientY - state.ly) * 0.005;
                state.pitch = Math.max(-1.4, Math.min(1.4, state.pitch));
                state.lx = e.clientX; state.ly = e.clientY;
            }
            if (state.pan) {
                state.panX += e.clientX - state.lx;
                state.panY += e.clientY - state.ly;
                state.lx = e.clientX; state.ly = e.clientY;
            }
        });
        canvas.addEventListener('wheel', function(e) {
            e.preventDefault();
            e.stopPropagation();
            state.zoom *= e.deltaY < 0 ? 1.12 : 0.89;
            state.zoom = Math.max(0.05, Math.min(30, state.zoom));
        }, {passive: false});

        pcViewers[key] = state;

        // 渲染循环
        function drawPC() {
            if (!state.running) return;
            var dpr = window.devicePixelRatio || 1;
            var rect = canvas.getBoundingClientRect();
            var w = Math.floor(rect.width * dpr);
            var h = Math.floor(rect.height * dpr);
            if (w <= 0 || h <= 0) { requestAnimationFrame(drawPC); return; }
            if (canvas.width !== w || canvas.height !== h) {
                canvas.width = w;
                canvas.height = h;
            }

            if (state._w !== w || state._h !== h) {
                state._img = ctx.createImageData(w, h);
                state._dep = new Float32Array(w * h);
                state._w = w; state._h = h;
            }

            var px = state._img.data, db = state._dep;
            var len = w * h;
            var p32 = new Uint32Array(px.buffer);
            p32.fill(0xFF181E26);
            db.fill(1e10);

            if (!state._fitted && state.pts.length > 6) {
                var maxC = 0;
                for (var fi = 0; fi < state.pts.length; fi += 6) {
                    var mx = Math.max(Math.abs(state.pts[fi]), Math.abs(state.pts[fi+1]), Math.abs(state.pts[fi+2]));
                    if (mx > maxC) maxC = mx;
                }
                if (maxC > 0.01) {
                    state._dataRange = maxC;
                    state._baseScale = (Math.min(w, h) * 0.45) / maxC;
                    state._fitted = true;
                }
            }

            var mode = state.mode;
            if (state.pts.length > 0) {
                var cY = Math.cos(state.yaw), sY = Math.sin(state.yaw);
                var cX = Math.cos(state.pitch), sX = Math.sin(state.pitch);
                var sc = state._baseScale * state.zoom;
                var ox = w/2 + state.panX, oy = h*0.5 + state.panY;
                var pts = state.pts, n = pts.length;
                var fadeRange = state._dataRange * 2;
                var zRange = state._maxZ - state._minZ || 1;
                var yRange = state._maxY - state._minY || 1;

                for (var i = 0; i < n; i += 6) {
                    var x=pts[i], y=-pts[i+1], z=-pts[i+2];
                    var cr=pts[i+3], cg=pts[i+4], cb=pts[i+5];

                    if (mode === 'depth') {
                        var rawZ = -z;
                        var zt = (rawZ - state._minZ) / zRange;
                        var hc = pcHeatmapColor(zt); cr=hc[0]; cg=hc[1]; cb=hc[2];
                    } else if (mode === 'rgb') {
                        // 后端已做 BGR→RGB 转换，直接使用原始颜色
                    } else if (mode === 'ir') {
                        var rawZ2 = -z;
                        var zt2 = (rawZ2 - state._minZ) / zRange;
                        var hc2 = pcIRColor(zt2); cr=hc2[0]; cg=hc2[1]; cb=hc2[2];
                    } else if (mode === 'height') {
                        var rawY = -y;
                        var yt = (rawY - state._minY) / yRange;
                        var hc3 = pcHeatmapColor(yt); cr=hc3[0]; cg=hc3[1]; cb=hc3[2];
                    }

                    var rx=x*cY+z*sY, rz=-x*sY+z*cY;
                    var ry=y*cX-rz*sX, rz2=y*sX+rz*cX;

                    var sx=(ox+rx*sc)|0, sy=(oy-ry*sc)|0;
                    if(sx<1||sx>=w-2||sy<1||sy>=h-2) continue;

                    var fade=Math.max(0.15, 1-Math.abs(rz2)/fadeRange);
                    var fr=(cr*fade)|0, fg=(cg*fade)|0, fb=(cb*fade)|0;
                    fr = fr > 255 ? 255 : fr; fg = fg > 255 ? 255 : fg; fb = fb > 255 ? 255 : fb;

                    var di = sy * w + sx;
                    pcPutPixel(px, db, di, w, len, rz2, fr, fg, fb);
                }
            }

            ctx.putImageData(state._img, 0, 0);

            // 网格和坐标轴
            var cY2=Math.cos(state.yaw), sY2=Math.sin(state.yaw);
            var cX2=Math.cos(state.pitch), sX2=Math.sin(state.pitch);
            var sc2 = state._baseScale * state.zoom;
            var ox2=w/2+state.panX, oy2=h*0.5+state.panY;
            var dr = state._dataRange;
            var dpr = window.devicePixelRatio || 1;
            var gs = dr > 10 ? Math.pow(10, Math.floor(Math.log10(dr))) : 1;
            if (dr / gs > 5) gs *= 5;
            if (dr / gs < 2) gs /= 2;
            var gr = Math.ceil(dr * 1.3 / gs) * gs;

            ctx.strokeStyle='rgba(255,255,255,0.07)'; ctx.lineWidth=1;
            var a=pcProj3D(-gr,0,0,cY2,sY2,cX2,sX2,ox2,oy2,sc2), b=pcProj3D(gr,0,0,cY2,sY2,cX2,sX2,ox2,oy2,sc2);
            ctx.beginPath();ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1]);ctx.stroke();
            a=pcProj3D(0,0,-gr,cY2,sY2,cX2,sX2,ox2,oy2,sc2); b=pcProj3D(0,0,gr,cY2,sY2,cX2,sX2,ox2,oy2,sc2);
            ctx.beginPath();ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1]);ctx.stroke();

            ctx.strokeStyle='rgba(255,255,255,0.03)';
            for(var gi=-gr;gi<=gr;gi+=gs){
                if(Math.abs(gi)<0.001) continue;
                a=pcProj3D(-gr,0,gi,cY2,sY2,cX2,sX2,ox2,oy2,sc2);
                b=pcProj3D(gr,0,gi,cY2,sY2,cX2,sX2,ox2,oy2,sc2);
                ctx.beginPath();ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1]);ctx.stroke();
                a=pcProj3D(gi,0,-gr,cY2,sY2,cX2,sX2,ox2,oy2,sc2);
                b=pcProj3D(gi,0,gr,cY2,sY2,cX2,sX2,ox2,oy2,sc2);
                ctx.beginPath();ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1]);ctx.stroke();
            }

            ctx.fillStyle='rgba(255,255,255,0.18)'; ctx.font=(9*dpr)+'px monospace'; ctx.textAlign='center';
            for(var gi=-gr;gi<=gr;gi+=gs*2){
                if(Math.abs(gi)<0.001) continue;
                var lp=pcProj3D(gi,0,0,cY2,sY2,cX2,sX2,ox2,oy2,sc2);
                if(lp[0]>30*dpr && lp[0]<w-30*dpr && lp[1]>30*dpr && lp[1]<h-30*dpr)
                    ctx.fillText(gi>=10?gi.toFixed(0):gi.toFixed(1),lp[0],lp[1]+14*dpr);
            }

            var al=dr*0.15;
            var axOff=dr*0.12;
            var axData=[[al,0,0,'#ef4444','X'],[0,al,0,'#22c55e','Y'],[0,0,al,'#3b82f6','Z']];
            var o=pcProj3D(-axOff,-axOff,-axOff,cY2,sY2,cX2,sX2,ox2,oy2,sc2);
            ctx.lineWidth=2*dpr;
            for(var ai=0;ai<3;ai++){
                var e=pcProj3D(axData[ai][0]-axOff,axData[ai][1]-axOff,axData[ai][2]-axOff,cY2,sY2,cX2,sX2,ox2,oy2,sc2);
                ctx.strokeStyle=axData[ai][3];ctx.beginPath();ctx.moveTo(o[0],o[1]);ctx.lineTo(e[0],e[1]);ctx.stroke();
                ctx.fillStyle=axData[ai][3];ctx.font='bold '+(12*dpr)+'px sans-serif';
                ctx.textAlign='left';ctx.textBaseline='middle';
                ctx.fillText(axData[ai][4],e[0]+6*dpr,e[1]);
            }
            ctx.textBaseline='alphabetic';

            state._fc++;
            var now=performance.now();
            if(now-state._ft>1000){state.fps=state._fc;state._fc=0;state._ft=now;}
            ctx.fillStyle='rgba(255,255,255,0.5)';ctx.font=(11*dpr)+'px monospace';ctx.textAlign='left';
            var cnt=state.count>0?state.count.toLocaleString():'0';
            ctx.fillText('点数: '+cnt+'  |  FPS: '+state.fps+'  |  缩放: '+state.zoom.toFixed(1)+'x  |  模式: '+state.mode,10*dpr,20*dpr);

            ctx.fillStyle='rgba(255,255,255,0.18)';ctx.font=(10*dpr)+'px sans-serif';ctx.textAlign='right';
            ctx.fillText('左键:旋转 右键:平移 滚轮:缩放',w-10*dpr,h-10*dpr);

            requestAnimationFrame(drawPC);
        }
        requestAnimationFrame(drawPC);

        // 轮询点云 JSON 数据
        state.timer = setInterval(function() {
            if (!state.running || state.busy) return;
            state.busy = true;
            var pcUrl = API_ALT + '/api/pointcloud/' + slot;
            console.log('[PC poll] fetching ' + pcUrl);
            fetch(pcUrl).then(function(r) {
                if (!r.ok) return null;
                return r.json();
            }).then(function(data) {
                console.log('[PC poll] response has=' + (data ? data.has : 'null') + ' pLen=' + (data && data.p ? data.p.length : 0));
                if (data && data.has && data.p && data.p.length > 0) {
                    var raw = data.p;
                    var numPts = raw.length / 6;
                    var sumX=0, sumY=0, sumZ=0, vc=0;
                    for (var pi = 0; pi < numPts; pi++) {
                        var idx = pi * 6;
                        var vx = raw[idx], vy = raw[idx+1], vz = raw[idx+2];
                        if (vx===0 && vy===0 && vz===0) continue;
                        if (!isFinite(vx)||!isFinite(vy)||!isFinite(vz)) continue;
                        sumX+=vx; sumY+=vy; sumZ+=vz; vc++;
                    }
                    if (vc < 10) return;
                    var avgX=sumX/vc||0, avgY=sumY/vc||0, avgZ=sumZ/vc||0;
                    var sX=0,sY=0,sZ=0; vc=0;
                    for (var pi2 = 0; pi2 < numPts; pi2++) {
                        var idx2 = pi2 * 6;
                        var vx2=raw[idx2],vy2=raw[idx2+1],vz2=raw[idx2+2];
                        if (vx2===0&&vy2===0&&vz2===0) continue;
                        if (!isFinite(vx2)||!isFinite(vy2)||!isFinite(vz2)) continue;
                        sX+=(vx2-avgX)*(vx2-avgX); sY+=(vy2-avgY)*(vy2-avgY); sZ+=(vz2-avgZ)*(vz2-avgZ); vc++;
                    }
                    var stX=Math.sqrt(sX/vc)||1, stY=Math.sqrt(sY/vc)||1, stZ=Math.sqrt(sZ/vc)||1;
                    var th = 3.0;
                    var filtered = [];
                    for (var fi = 0; fi < numPts; fi++) {
                        var fi6 = fi*6;
                        var fx=raw[fi6],fy=raw[fi6+1],fz=raw[fi6+2];
                        if (fx===0&&fy===0&&fz===0) continue;
                        if (!isFinite(fx)||!isFinite(fy)||!isFinite(fz)) continue;
                        if (Math.abs(fx-avgX)>th*stX||Math.abs(fy-avgY)>th*stY||Math.abs(fz-avgZ)>th*stZ) continue;
                        filtered.push(fx,fy,fz,raw[fi6+3],raw[fi6+4],raw[fi6+5]);
                    }
                    state.pts = filtered;
                    state.count = filtered.length / 6;
                    var minZ=1e10, maxZ=-1e10, minY=1e10, maxY=-1e10;
                    for (var i = 0; i < filtered.length; i += 6) {
                        if(filtered[i+2]<minZ) minZ=filtered[i+2];
                        if(filtered[i+2]>maxZ) maxZ=filtered[i+2];
                        if(filtered[i+1]<minY) minY=filtered[i+1];
                        if(filtered[i+1]>maxY) maxY=filtered[i+1];
                    }
                    state._minZ=minZ; state._maxZ=maxZ; state._minY=minY; state._maxY=maxY;

                    var ph = canvas.parentElement.querySelector('.placeholder');
                    if (ph) ph.style.display = 'none';

                    if (streamFps[key]) streamFps[key].count++;
                }
            }).catch(function(err){ console.warn('[PC poll] error:', err); }).then(function(){ state.busy = false; });
        }, 500);
    }

    function stopPointCloudViewer(key) {
        var v = pcViewers[key];
        if (!v) return;
        v.running = false;
        if (v.timer) clearInterval(v.timer);
        delete pcViewers[key];
    }

    // ===== LED 炫彩色带 =====
    (function() {
        var strip = document.getElementById('ledColorStrip');
        if (!strip) return;
        var ctx = strip.getContext('2d');
        var w, h;

        function drawStrip() {
            var rect = strip.getBoundingClientRect();
            var dpr = window.devicePixelRatio || 1;
            w = Math.floor(rect.width * dpr);
            h = Math.floor(rect.height * dpr);
            if (strip.width !== w || strip.height !== h) {
                strip.width = w;
                strip.height = h;
            }
            // 绘制彩虹渐变色带。
            var grad = ctx.createLinearGradient(0, 0, w, 0);
            grad.addColorStop(0,    '#ff0000');
            grad.addColorStop(0.16, '#ffff00');
            grad.addColorStop(0.33, '#00ff00');
            grad.addColorStop(0.50, '#00ffff');
            grad.addColorStop(0.66, '#0000ff');
            grad.addColorStop(0.83, '#ff00ff');
            grad.addColorStop(1,    '#ff0000');
            ctx.fillStyle = grad;
            ctx.fillRect(0, 0, w, h);
        }
        drawStrip();
        window.addEventListener('resize', drawStrip);
        window._drawLedStrip = drawStrip;

        // 预设色块点击
        document.querySelectorAll('.led-preset').forEach(function(el) {
            el.addEventListener('click', function() {
                var r = parseInt(el.getAttribute('data-r'));
                var g = parseInt(el.getAttribute('data-g'));
                var b = parseInt(el.getAttribute('data-b'));
                document.getElementById('ledR').value = r;
                document.getElementById('ledG').value = g;
                document.getElementById('ledB').value = b;
                var hex = '#' + ((1<<24)+(r<<16)+(g<<8)+b).toString(16).slice(1).toUpperCase();
                document.getElementById('ledColorPreview').style.background = hex;
                document.getElementById('ledColorHex').textContent = hex;
                document.querySelectorAll('.led-preset').forEach(function(p) { p.classList.remove('active'); });
                el.classList.add('active');
            });
        });

        function hsvToRgb(h, s, v) {
            var r, g, b;
            var i = Math.floor(h * 6);
            var f = h * 6 - i;
            var p = v * (1 - s);
            var q = v * (1 - f * s);
            var t = v * (1 - (1 - f) * s);
            switch (i % 6) {
                case 0: r=v;g=t;b=p;break;
                case 1: r=q;g=v;b=p;break;
                case 2: r=p;g=v;b=t;break;
                case 3: r=p;g=q;b=v;break;
                case 4: r=t;g=p;b=v;break;
                case 5: r=v;g=p;b=q;break;
            }
            return [Math.round(r*255), Math.round(g*255), Math.round(b*255)];
        }

        function pickColor(e) {
            var rect = strip.getBoundingClientRect();
            var x = Math.max(0, Math.min(rect.width - 1, e.clientX - rect.left));
            var ratio = x / rect.width;
            var rgb = hsvToRgb(ratio, 1, 1);
            var r = rgb[0], g = rgb[1], b = rgb[2];
            document.getElementById('ledR').value = r;
            document.getElementById('ledG').value = g;
            document.getElementById('ledB').value = b;
            var hex = '#' + ((1<<24)+(r<<16)+(g<<8)+b).toString(16).slice(1).toUpperCase();
            document.getElementById('ledColorHex').textContent = hex;
            document.getElementById('ledColorPreview').style.background = hex;
        }

        var dragging = false;
        strip.addEventListener('mousedown', function(e) { dragging = true; pickColor(e); });
        window.addEventListener('mousemove', function(e) { if (dragging) pickColor(e); });
        window.addEventListener('mouseup', function() { dragging = false; });
        strip.addEventListener('touchstart', function(e) { e.preventDefault(); dragging = true; pickColor(e.touches[0]); }, {passive:false});
        strip.addEventListener('touchmove', function(e) { e.preventDefault(); if (dragging) pickColor(e.touches[0]); }, {passive:false});
        strip.addEventListener('touchend', function() { dragging = false; });
    })();

    // 重写 stopStreamImg 以同时停止点云 viewer
    var origStopStreamImg = stopStreamImg;
    stopStreamImg = function(key) {
        if (key.indexOf('pointcloud') >= 0) {
            stopPointCloudViewer(key);
        }
        origStopStreamImg(key);
    };
})();
