document.addEventListener('DOMContentLoaded', () => {
    // Navigation Tabs Logic
    const tabs = document.querySelectorAll('.nav-tab');
    const tabContents = document.querySelectorAll('.tab-content');

    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            const target = tab.dataset.tab;
            tabs.forEach(t => t.classList.remove('active', 'border-brand-500', 'text-brand-500'));
            tabs.forEach(t => t.classList.add('border-transparent', 'text-gray-400'));
            
            tab.classList.add('active', 'border-brand-500', 'text-brand-500');
            tab.classList.remove('border-transparent', 'text-gray-400');

            tabContents.forEach(content => {
                if (content.id === target) {
                    content.classList.remove('hidden');
                } else {
                    content.classList.add('hidden');
                }
            });
        });
    });

    // WebSocket synthetic telemetry connection
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}`;
    let ws;

    function connectWs() {
        ws = new WebSocket(wsUrl);

        ws.onmessage = (event) => {
            const data = JSON.parse(event.data);
            if (data.type === 'tick') {
                updateLiveTelemetry(data);
            }
        };

        ws.onclose = () => {
            setTimeout(connectWs, 2000);
        };
    }

    connectWs();

    // DOM elements for synthetic telemetry
    const liveLtp = document.getElementById('live-ltp');
    const liveSpread = document.getElementById('live-spread');
    const liveImbalance = document.getElementById('live-imbalance');
    const liveMicroprice = document.getElementById('live-microprice');
    const liveRiskLatency = document.getElementById('live-risk-latency');
    const bidsContainer = document.getElementById('bids-container');
    const asksContainer = document.getElementById('asks-container');
    const liveTerminalLog = document.getElementById('live-terminal-log');

    function updateLiveTelemetry(data) {
        if (liveLtp) liveLtp.textContent = `₹ ${data.ltp}`;
        if (liveSpread) liveSpread.textContent = `₹ ${(data.bestAsk - data.bestBid).toFixed(2)}`;
        if (liveImbalance) {
            liveImbalance.textContent = `${data.imbalance > 0 ? '+' : ''}${data.imbalance}`;
            liveImbalance.className = `mt-2 text-2xl font-extrabold font-mono ${data.imbalance >= 0 ? 'text-emerald-400' : 'text-red-400'}`;
        }
        if (liveMicroprice) {
            const mp = (parseFloat(data.bestBid) + parseFloat(data.bestAsk)) / 2;
            liveMicroprice.textContent = `₹ ${mp.toFixed(2)}`;
        }
        if (liveRiskLatency) {
            liveRiskLatency.textContent = `${data.riskLatencyNs} ns`;
        }

        // Render Bids
        if (bidsContainer && data.depth) {
            bidsContainer.innerHTML = data.depth.bids.map(b => `
                <div class="grid grid-cols-3 py-0.5 border-b border-gray-800/50">
                    <span class="text-gray-400">${b.orders}</span>
                    <span class="text-right text-gray-300">${b.qty}</span>
                    <span class="text-right font-bold text-emerald-400">₹ ${b.price}</span>
                </div>
            `).join('');
        }

        // Render Asks
        if (asksContainer && data.depth) {
            asksContainer.innerHTML = data.depth.asks.map(a => `
                <div class="grid grid-cols-3 py-0.5 border-b border-gray-800/50">
                    <span class="font-bold text-red-400">₹ ${a.price}</span>
                    <span class="text-right text-gray-300">${a.qty}</span>
                    <span class="text-right text-gray-400">${a.orders}</span>
                </div>
            `).join('');
        }
    }

    // Toggle Emergency Kill Switch
    const killSwitchBtn = document.getElementById('toggle-kill-switch');
    let isKilled = false;

    if (killSwitchBtn) {
        killSwitchBtn.addEventListener('click', () => {
            isKilled = !isKilled;
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({ action: 'toggle_kill_switch', value: isKilled }));
            }
            const statusEl = document.getElementById('kill-switch-status');
            if (isKilled) {
                statusEl.textContent = 'BLOCKED (KILL SWITCH)';
                statusEl.className = 'px-2.5 py-1 text-xs font-bold rounded-md bg-red-500/20 text-red-400 border border-red-500/30';
                killSwitchBtn.textContent = 'Reset Risk';
                killSwitchBtn.className = 'px-3 py-1 bg-emerald-600 hover:bg-emerald-700 text-white rounded text-xs font-semibold transition';
                addLog('[RISK] EMERGENCY KILL SWITCH ACTIVATED! All new orders blocked.', 'text-red-400');
            } else {
                statusEl.textContent = 'ACTIVE';
                statusEl.className = 'px-2.5 py-1 text-xs font-bold rounded-md bg-emerald-500/20 text-emerald-400 border border-emerald-500/30';
                killSwitchBtn.textContent = 'Emergency Kill';
                killSwitchBtn.className = 'px-3 py-1 bg-red-600 hover:bg-red-700 text-white rounded text-xs font-semibold transition';
                addLog('[RISK] Risk system re-armed and active.', 'text-emerald-400');
            }
        });
    }

    function addLog(msg, colorClass = 'text-gray-300') {
        if (!liveTerminalLog) return;
        const div = document.createElement('div');
        div.className = colorClass;
        div.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
        liveTerminalLog.appendChild(div);
        liveTerminalLog.scrollTop = liveTerminalLog.scrollHeight;
    }

    // Trigger C++ Multi-Threaded Live Run
    const liveRunBtn = document.getElementById('trigger-live-c-run');
    if (liveRunBtn) {
        liveRunBtn.addEventListener('click', async () => {
            addLog('[SYSTEM] Executing C++ Multi-threaded Engine binary...', 'text-blue-400');
            try {
                const res = await fetch('/api/live-run');
                const data = await res.json();
                addLog('[SYSTEM] C++ Binary Execution Completed:', 'text-emerald-400');
                data.output.split('\n').forEach(line => {
                    if (line.trim()) addLog(line, 'text-gray-300');
                });
            } catch (e) {
                addLog('[ERROR] Failed to run C++ binary', 'text-red-400');
            }
        });
    }

    // Run C++ Unit Tests Button
    const runTestsBtn = document.getElementById('run-unit-tests-btn');
    if (runTestsBtn) {
        runTestsBtn.addEventListener('click', async () => {
            try {
                const res = await fetch('/api/tests');
                const data = await res.json();
                if (data.success) {
                    alert('C++ Internal Unit Tests Passed Successfully!\n\n' + data.output);
                } else {
                    alert('Unit Tests Failed:\n' + data.error);
                }
            } catch (e) {
                alert('Error running unit tests: ' + e.message);
            }
        });
    }

    // BACKTEST CHART & RUNNER
    let equityChart;
    const runBtBtn = document.getElementById('run-backtest-btn');

    if (runBtBtn) {
        runBtBtn.addEventListener('click', async () => {
            const strategy = document.getElementById('bt-strategy').value;
            const capital = document.getElementById('bt-capital').value;
            const slippage = document.getElementById('bt-slippage').value;

            runBtBtn.disabled = true;
            runBtBtn.innerHTML = `<span>Running C++ Engine...</span>`;

            try {
                const res = await fetch(`/api/backtest?strategy=${strategy}&capital=${capital}&slippage=${slippage}`);
                const data = await res.json();

                // Update Metrics
                document.getElementById('res-net-pnl').textContent = `₹ ${data.net_pnl.toFixed(2)}`;
                document.getElementById('res-net-pnl').className = `text-xl font-extrabold font-mono mt-1 ${data.net_pnl >= 0 ? 'text-emerald-400' : 'text-red-400'}`;
                document.getElementById('res-sharpe').textContent = data.sharpe_ratio.toFixed(2);
                document.getElementById('res-drawdown').textContent = `${data.max_drawdown_pct.toFixed(2)}%`;
                document.getElementById('res-win-rate').textContent = `${data.win_rate_pct.toFixed(1)}%`;

                // Update Tax Breakdown
                if (data.taxes) {
                    document.getElementById('res-total-taxes').textContent = `Total Charges: ₹ ${data.taxes.total_charges.toFixed(2)}`;
                    document.getElementById('tax-brokerage').textContent = `₹ ${data.taxes.brokerage.toFixed(2)}`;
                    document.getElementById('tax-stt').textContent = `₹ ${data.taxes.stt.toFixed(2)}`;
                    document.getElementById('tax-exchange').textContent = `₹ ${data.taxes.exchange_fee.toFixed(2)}`;
                    document.getElementById('tax-sebi').textContent = `₹ ${data.taxes.sebi_fee.toFixed(2)}`;
                    document.getElementById('tax-stamp').textContent = `₹ ${data.taxes.stamp_duty.toFixed(2)}`;
                    document.getElementById('tax-gst').textContent = `₹ ${data.taxes.gst.toFixed(2)}`;
                }

                // Render Equity Curve Chart
                if (data.equity_curve) {
                    renderEquityChart(data.equity_curve);
                }

            } catch (e) {
                alert('Backtest failed: ' + e.message);
            } finally {
                runBtBtn.disabled = false;
                runBtBtn.innerHTML = `<i data-lucide="play-circle" class="w-4 h-4"></i><span>Run C++ Backtest</span>`;
                lucide.createIcons();
            }
        });
    }

    function renderEquityChart(curveData) {
        const ctx = document.getElementById('equity-chart').getContext('2d');
        if (equityChart) equityChart.destroy();

        // Downsample if huge
        const step = Math.max(1, Math.floor(curveData.length / 300));
        const sampled = curveData.filter((_, idx) => idx % step === 0);
        const labels = sampled.map((_, idx) => `T+${idx * step}`);

        equityChart = new Chart(ctx, {
            type: 'line',
            data: {
                labels,
                datasets: [{
                    label: 'Account Equity (₹)',
                    data: sampled,
                    borderColor: '#22c55e',
                    borderWidth: 2,
                    fill: true,
                    backgroundColor: 'rgba(34, 197, 94, 0.05)',
                    tension: 0.1,
                    pointRadius: 0
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                scales: {
                    x: { display: false },
                    y: { grid: { color: '#1f2937' }, ticks: { color: '#9ca3af', font: { family: 'monospace' } } }
                },
                plugins: { legend: { display: false } }
            }
        });
    }

    // BENCHMARK CHARTS & RUNNER
    let mopsChart, latChart;
    const runBenchBtn = document.getElementById('run-benchmark-btn');

    if (runBenchBtn) {
        runBenchBtn.addEventListener('click', async () => {
            runBenchBtn.disabled = true;
            runBenchBtn.innerHTML = `<span>Benchmarking 5M items...</span>`;

            try {
                const res = await fetch('/api/benchmark');
                const data = await res.json();

                document.getElementById('benchmark-raw-out').textContent = data.raw;

                if (data.mutex && data.lockFree) {
                    renderBenchmarkCharts(data.mutex, data.lockFree);
                }
            } catch (e) {
                alert('Benchmark failed: ' + e.message);
            } finally {
                runBenchBtn.disabled = false;
                runBenchBtn.innerHTML = `<i data-lucide="zap" class="w-4 h-4"></i><span>Execute 5M Message Benchmark</span>`;
                lucide.createIcons();
            }
        });
    }

    function renderBenchmarkCharts(mutex, lockFree) {
        // Throughput Chart
        const mopsCtx = document.getElementById('benchmark-mops-chart').getContext('2d');
        if (mopsChart) mopsChart.destroy();

        mopsChart = new Chart(mopsCtx, {
            type: 'bar',
            data: {
                labels: ['std::mutex Queue', 'Lock-Free SPSC Queue'],
                datasets: [{
                    data: [mutex.mops, lockFree.mops],
                    backgroundColor: ['#ef4444', '#22c55e'],
                    borderRadius: 6
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                plugins: { legend: { display: false } },
                scales: { y: { grid: { color: '#1f2937' }, ticks: { color: '#9ca3af' } }, x: { ticks: { color: '#9ca3af' } } }
            }
        });

        // Latency Chart
        const latCtx = document.getElementById('benchmark-latency-chart').getContext('2d');
        if (latChart) latChart.destroy();

        latChart = new Chart(latCtx, {
            type: 'bar',
            data: {
                labels: ['std::mutex Queue', 'Lock-Free SPSC Queue'],
                datasets: [{
                    data: [mutex.latency_ns, lockFree.latency_ns],
                    backgroundColor: ['#ef4444', '#3b82f6'],
                    borderRadius: 6
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                plugins: { legend: { display: false } },
                scales: { y: { grid: { color: '#1f2937' }, ticks: { color: '#9ca3af' } }, x: { ticks: { color: '#9ca3af' } } }
            }
        });
    }
});
