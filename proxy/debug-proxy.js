#!/usr/bin/env node

const http = require('http');
const https = require('https');
const fs = require('fs');

const PORT = 8080;
const LOG_FILE = '/tmp/nintencode-debug.log';

function log(msg) {
  const timestamp = new Date().toISOString();
  const line = `[${timestamp}] ${msg}\n`;
  console.log(line.trim());
  fs.appendFileSync(LOG_FILE, line);
}

const server = http.createServer((req, res) => {
  log(`=== INCOMING REQUEST ===`);
  log(`Method: ${req.method}`);
  log(`URL: ${req.url}`);
  log(`Headers: ${JSON.stringify(req.headers, null, 2)}`);

  let body = '';
  req.on('data', chunk => {
    body += chunk;
  });

  req.on('end', () => {
    log(`Body length: ${body.length}`);
    log(`Body (first 2000 chars):\n${body.substring(0, 2000)}`);
    
    try {
      JSON.parse(body);
      log(`JSON: VALID`);
    } catch (e) {
      log(`JSON: INVALID - ${e.message}`);
      const match = e.message.match(/position (\d+)/);
      if (match) {
        const pos = parseInt(match[1]);
        log(`Error near: ...${body.substring(Math.max(0, pos-50), pos)}<<<HERE>>>${body.substring(pos, pos+50)}...`);
      }
    }

    const options = {
      hostname: 'api.anthropic.com',
      port: 443,
      path: '/v1/messages',
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(body),
        'x-api-key': req.headers['x-api-key'] || '',
        'anthropic-version': req.headers['anthropic-version'] || '2023-06-01',
      }
    };

    const proxyReq = https.request(options, (proxyRes) => {
      log(`=== ANTHROPIC RESPONSE ===`);
      log(`Status: ${proxyRes.statusCode}`);
      
      let responseBody = '';
      proxyRes.on('data', chunk => responseBody += chunk);
      proxyRes.on('end', () => {
        log(`Response (first 1000 chars): ${responseBody.substring(0, 1000)}`);
        res.writeHead(proxyRes.statusCode, proxyRes.headers);
        res.end(responseBody);
      });
    });

    proxyReq.on('error', (err) => {
      log(`Proxy error: ${err.message}`);
      res.writeHead(502);
      res.end(JSON.stringify({ error: err.message }));
    });

    proxyReq.write(body);
    proxyReq.end();
  });
});

fs.writeFileSync(LOG_FILE, '');

server.listen(PORT, '0.0.0.0', () => {
  console.log(`Debug proxy running on http://0.0.0.0:${PORT}`);
  console.log(`Logs written to ${LOG_FILE}`);
  console.log(`\nUpdate 3DS app to use this proxy IP instead of direct API`);
});
