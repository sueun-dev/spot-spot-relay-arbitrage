import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';

import { WebSocketServer } from 'ws';

test('monitor handles reconnects and keeps stale/positive accounting correct', async () => {
  const tempDir = await mkdtemp(path.join(os.tmpdir(), 'relay-monitor-test-'));
  const jsonPath = path.join(tempDir, 'snapshot.json');

  const httpServer = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');
    res.setHeader('content-type', 'application/json');

    if (url.pathname === '/bithumb/orderbook') {
      res.end(JSON.stringify({
        status: '0000',
        data: {
          payment_currency: 'KRW',
          timestamp: String(Date.now()),
          USDT: {
            bids: [{ price: '1300', quantity: '1000' }],
            asks: [{ price: '1301', quantity: '1000' }],
          },
          AAA: {
            bids: [{ price: '999', quantity: '10' }],
            asks: [{ price: '1000', quantity: '10' }],
          },
          BBB: {
            bids: [{ price: '999', quantity: '10' }],
            asks: [{ price: '1000', quantity: '10' }],
          },
        },
      }));
      return;
    }

    if (url.pathname === '/bybit/instruments') {
      res.end(JSON.stringify({
        retCode: 0,
        retMsg: 'OK',
        result: {
          list: [
            { baseCoin: 'AAA', quoteCoin: 'USDT', status: 'Trading', marginTrading: 'both' },
            { baseCoin: 'BBB', quoteCoin: 'USDT', status: 'Trading', marginTrading: 'both' },
          ],
          nextPageCursor: '',
        },
      }));
      return;
    }

    res.statusCode = 404;
    res.end(JSON.stringify({ error: 'not found' }));
  });
  httpServer.listen(0, '127.0.0.1');
  await once(httpServer, 'listening');
  const httpPort = httpServer.address().port;

  const bithumbWss = new WebSocketServer({ port: 0 });
  await once(bithumbWss, 'listening');
  const bithumbPort = bithumbWss.address().port;
  let bithumbConnections = 0;

  bithumbWss.on('connection', (ws) => {
    bithumbConnections += 1;
    ws.once('message', () => {
      ws.send(JSON.stringify({
        type: 'orderbookdepth',
        content: {
          list: [
            { symbol: 'USDT_KRW', orderType: 'bid', price: '1300', quantity: '1000' },
            { symbol: 'AAA_KRW', orderType: 'ask', price: '1000', quantity: '10' },
            { symbol: 'AAA_KRW', orderType: 'bid', price: '999', quantity: '10' },
            { symbol: 'BBB_KRW', orderType: 'ask', price: '1000', quantity: '10' },
            { symbol: 'BBB_KRW', orderType: 'bid', price: '999', quantity: '10' },
          ],
        },
      }));

      if (bithumbConnections === 1) {
        setTimeout(() => ws.close(), 120);
      }
    });
  });

  const bybitWss = new WebSocketServer({ port: 0 });
  await once(bybitWss, 'listening');
  const bybitPort = bybitWss.address().port;
  let bybitConnections = 0;

  bybitWss.on('connection', (ws) => {
    bybitConnections += 1;
    ws.once('message', () => {
      ws.send(JSON.stringify({
        topic: 'orderbook.1.AAAUSDT',
        data: { s: 'AAAUSDT', b: [['0.81', '10']], a: [['0.82', '10']] },
      }));
      ws.send(JSON.stringify({
        topic: 'orderbook.1.BBBUSDT',
        data: { s: 'BBBUSDT', b: [['0.771', '10']], a: [['0.772', '10']] },
      }));

      if (bybitConnections === 1) {
        setTimeout(() => ws.close(), 120);
      }
    });
  });

  const child = spawn(process.execPath, [
    'scripts/spot-relay-live.mjs',
    '--duration-sec', '4',
    '--refresh-ms', '100',
    '--stale-ms', '5000',
    '--json-out', jsonPath,
    '--bithumb-orderbook-url', `http://127.0.0.1:${httpPort}/bithumb/orderbook`,
    '--bybit-instruments-url', `http://127.0.0.1:${httpPort}/bybit/instruments`,
    '--bithumb-ws-url', `ws://127.0.0.1:${bithumbPort}`,
    '--bybit-ws-url', `ws://127.0.0.1:${bybitPort}`,
  ], {
    cwd: process.cwd(),
    env: process.env,
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  let stdout = '';
  let stderr = '';
  child.stdout.on('data', (chunk) => { stdout += chunk.toString(); });
  child.stderr.on('data', (chunk) => { stderr += chunk.toString(); });

  const [exitCode] = await once(child, 'exit');

  const raw = await readFile(jsonPath, 'utf8');
  const snapshot = JSON.parse(raw);

  assert.equal(exitCode, 0, stderr || stdout);
  assert.ok(bithumbConnections >= 2, `expected bithumb reconnect, got ${bithumbConnections}`);
  assert.ok(bybitConnections >= 2, `expected bybit reconnect, got ${bybitConnections}`);
  assert.equal(snapshot.summary.tracked, 2);
  assert.equal(snapshot.summary.bithumbSeen, 2);
  assert.equal(snapshot.summary.bybitSeen, 2);
  assert.equal(snapshot.summary.bothSeen, 2);
  assert.equal(snapshot.summary.positiveShown, 1);
  assert.equal(snapshot.summary.filteredNegative, 1);
  assert.equal(snapshot.rows.length, 2);
  assert.deepEqual(snapshot.rows.map((row) => row.base), ['AAA', 'BBB']);
  assert.ok(snapshot.rows[0].netEdgePct > 0);
  assert.ok(snapshot.rows[1].netEdgePct < 0);
  assert.equal(snapshot.summary.exchangeRateLabel, '원화/USDT');

  await Promise.all([
    new Promise((resolve) => httpServer.close(resolve)),
    new Promise((resolve) => bithumbWss.close(resolve)),
    new Promise((resolve) => bybitWss.close(resolve)),
  ]);
  await rm(tempDir, { recursive: true, force: true });
});
