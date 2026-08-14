const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const { execFile } = require('child_process');
const path = require('path');
const fs = require('fs');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

const PORT = process.env.PORT || 3000;

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// C++ Binary Path resolution (supports Windows .exe and Unix binary)
const ENGINE_BIN = (() => {
    const exePath = path.join(__dirname, '..', 'bin', 'trading_engine.exe');
    const binPath = path.join(__dirname, '..', 'bin', 'trading_engine');
    if (process.platform === 'win32' && fs.existsSync(exePath)) return exePath;
    if (fs.existsSync(binPath)) return binPath;
    if (fs.existsSync(exePath)) return exePath;
    return binPath;
})();
const ENGINE_TIMEOUT_MS = 120000;
const ENGINE_MAX_BUFFER = 16 * 1024 * 1024;
const ALLOWED_STRATEGIES = new Set(['vwap', 'delta_neutral']);

function runEngine(args, res, onSuccess) {
    execFile(
        ENGINE_BIN,
        args,
        { timeout: ENGINE_TIMEOUT_MS, maxBuffer: ENGINE_MAX_BUFFER, windowsHide: true },
        (error, stdout, stderr) => {
            if (error) {
                return res.status(500).json({ error: error.message, stderr });
            }
            onSuccess(stdout, stderr);
        }
    );
}

// API: Run C++ Backtest
app.get('/api/backtest', (req, res) => {
    const strategy = req.query.strategy || 'vwap';
    const capital = Number(req.query.capital ?? 1000000);
    const slippage = Number(req.query.slippage ?? 0.5);

    if (!ALLOWED_STRATEGIES.has(strategy)) {
        return res.status(400).json({ error: 'Invalid strategy' });
    }
    if (!Number.isFinite(capital) || capital <= 0) {
        return res.status(400).json({ error: 'Invalid capital' });
    }
    if (!Number.isFinite(slippage) || slippage < 0) {
        return res.status(400).json({ error: 'Invalid slippage' });
    }

    runEngine(['--run-backtest', strategy, String(capital), String(slippage)], res, (stdout) => {
        try {
            const data = JSON.parse(stdout);
            res.json(data);
        } catch (e) {
            res.status(500).json({ error: "Failed to parse backtest JSON output", raw: stdout });
        }
    });
});

// API: Run C++ Benchmark (Mutex vs Lock-Free Queue)
app.get('/api/benchmark', (req, res) => {
    runEngine(['--run-benchmark'], res, (stdout) => {
        // Parse benchmark output
        const lines = stdout.split('\n');
        let mutexResult = {};
        let lockFreeResult = {};

        lines.forEach(line => {
            if (line.includes('[Mutex Queue]')) {
                const matchTime = line.match(/Time:\s*([\d.]+)\s*ms/);
                const matchMops = line.match(/Throughput:\s*([\d.]+)\s*MOps/);
                const matchLat = line.match(/Avg Latency:\s*([\d.]+)\s*ns/);
                if (matchTime && matchMops && matchLat) {
                    mutexResult = { time_ms: parseFloat(matchTime[1]), mops: parseFloat(matchMops[1]), latency_ns: parseFloat(matchLat[1]) };
                }
            } else if (line.includes('[Lock-Free SPSC]')) {
                const matchTime = line.match(/Time:\s*([\d.]+)\s*ms/);
                const matchMops = line.match(/Throughput:\s*([\d.]+)\s*MOps/);
                const matchLat = line.match(/Avg Latency:\s*([\d.]+)\s*ns/);
                if (matchTime && matchMops && matchLat) {
                    lockFreeResult = { time_ms: parseFloat(matchTime[1]), mops: parseFloat(matchMops[1]), latency_ns: parseFloat(matchLat[1]) };
                }
            }
        });

        res.json({
            raw: stdout,
            mutex: mutexResult,
            lockFree: lockFreeResult,
            speedup: mutexResult.time_ms && lockFreeResult.time_ms
                ? (mutexResult.time_ms / lockFreeResult.time_ms).toFixed(2)
                : null
        });
    });
});

// API: Run C++ Unit Tests
app.get('/api/tests', (req, res) => {
    runEngine(['--run-tests'], res, (stdout) => {
        res.json({ success: true, output: stdout });
    });
});

// API: Run C++ simulation telemetry run
app.get('/api/live-run', (req, res) => {
    runEngine(['--run-live'], res, (stdout) => {
        res.json({ output: stdout });
    });
});

// WebSocket synthetic stream for the interactive UI
let currentLtp = 24500.0;
let killsSwitch = false;

wss.on('connection', (ws) => {
    console.log('[WebSocket] Client connected');

    ws.on('message', (message) => {
        try {
            const data = JSON.parse(message);
            if (data.action === 'toggle_kill_switch') {
                killsSwitch = data.value;
                ws.send(JSON.stringify({ type: 'risk_update', killSwitch: killsSwitch }));
            }
        } catch (e) {}
    });

    const interval = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) {
            // Generate tick step
            const delta = (Math.random() - 0.49) * 2.5;
            currentLtp += delta;
            if (currentLtp < 1000) currentLtp = 1000;

            const spread = 0.50;
            const bestBid = (currentLtp - spread/2).toFixed(2);
            const bestAsk = (currentLtp + spread/2).toFixed(2);

            const depth = {
                bids: [
                    { price: (currentLtp - 0.25).toFixed(2), qty: Math.floor(Math.random() * 200) + 25, orders: Math.floor(Math.random()*5)+1 },
                    { price: (currentLtp - 0.50).toFixed(2), qty: Math.floor(Math.random() * 300) + 50, orders: Math.floor(Math.random()*8)+1 },
                    { price: (currentLtp - 0.75).toFixed(2), qty: Math.floor(Math.random() * 400) + 75, orders: Math.floor(Math.random()*12)+1 },
                    { price: (currentLtp - 1.00).toFixed(2), qty: Math.floor(Math.random() * 500) + 100, orders: Math.floor(Math.random()*15)+1 },
                    { price: (currentLtp - 1.25).toFixed(2), qty: Math.floor(Math.random() * 600) + 150, orders: Math.floor(Math.random()*20)+1 }
                ],
                asks: [
                    { price: (currentLtp + 0.25).toFixed(2), qty: Math.floor(Math.random() * 200) + 25, orders: Math.floor(Math.random()*5)+1 },
                    { price: (currentLtp + 0.50).toFixed(2), qty: Math.floor(Math.random() * 300) + 50, orders: Math.floor(Math.random()*8)+1 },
                    { price: (currentLtp + 0.75).toFixed(2), qty: Math.floor(Math.random() * 400) + 75, orders: Math.floor(Math.random()*12)+1 },
                    { price: (currentLtp + 1.00).toFixed(2), qty: Math.floor(Math.random() * 500) + 100, orders: Math.floor(Math.random()*15)+1 },
                    { price: (currentLtp + 1.25).toFixed(2), qty: Math.floor(Math.random() * 600) + 150, orders: Math.floor(Math.random()*20)+1 }
                ]
            };

            // Order Book Imbalance Calculation
            const bidQtySum = depth.bids.reduce((a, b) => a + b.qty, 0);
            const askQtySum = depth.asks.reduce((a, b) => a + b.qty, 0);
            const imbalance = ((bidQtySum - askQtySum) / (bidQtySum + askQtySum)).toFixed(3);

            // Synthetic risk check latency sample for dashboard animation.
            const riskLatencyNs = Math.floor(Math.random() * 80) + 110;

            ws.send(JSON.stringify({
                type: 'tick',
                symbol: 'NIFTY26AUGFUT',
                ltp: currentLtp.toFixed(2),
                bestBid,
                bestAsk,
                depth,
                imbalance,
                riskLatencyNs,
                simulated: true,
                killSwitch: killsSwitch,
                timestamp: new Date().toISOString()
            }));
        }
    }, 150);

    ws.on('close', () => clearInterval(interval));
});

server.listen(PORT, '0.0.0.0', () => {
    console.log(`[Dashboard Server] Listening on http://0.0.0.0:${PORT}`);
});
