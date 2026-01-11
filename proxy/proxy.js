#!/usr/bin/env node

const http = require('http');
const https = require('https');

const PORT = 8080;
const ANTHROPIC_HOST = 'api.anthropic.com';
const ANTHROPIC_PATH = '/v1/messages';

const server = http.createServer((req, res) => {
  const timestamp = new Date().toISOString();
  
  console.log('\n' + '='.repeat(60));
  console.log(`[${timestamp}] INCOMING REQUEST`);
  console.log('='.repeat(60));
  console.log(`Method: ${req.method}`);
  console.log(`URL: ${req.url}`);
  console.log(`HTTP Version: ${req.httpVersion}`);
  console.log(`Remote: ${req.socket.remoteAddress}:${req.socket.remotePort}`);
  console.log('\n--- Headers ---');
  for (const [key, value] of Object.entries(req.headers)) {
    if (key.toLowerCase() === 'x-api-key') {
      console.log(`${key}: ${value.substring(0, 20)}...MASKED`);
    } else {
      console.log(`${key}: ${value}`);
    }
  }

  if (req.method === 'OPTIONS') {
    res.writeHead(200);
    res.end();
    return;
  }

  let body = '';
  let bodyBytes = 0;
  
  req.on('data', chunk => {
    body += chunk;
    bodyBytes += chunk.length;
    console.log(`[${new Date().toISOString()}] Received chunk: ${chunk.length} bytes (total: ${bodyBytes})`);
  });

  req.on('end', () => {
    console.log('\n--- Request Body ---');
    console.log(`Total body length: ${body.length} bytes`);
    
    try {
      const parsed = JSON.parse(body);
      console.log('Parsed JSON:');
      console.log(JSON.stringify(parsed, null, 2));
    } catch (e) {
      console.log('Raw body (first 500 chars):');
      console.log(body.substring(0, 500));
    }

    console.log('\n--- Forwarding to Anthropic ---');
    
    const options = {
      hostname: ANTHROPIC_HOST,
      port: 443,
      path: ANTHROPIC_PATH,
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(body),
        'x-api-key': req.headers['x-api-key'] || '',
        'anthropic-version': req.headers['anthropic-version'] || '2023-06-01',
        'User-Agent': 'nintencode-3ds-proxy/1.0'
      }
    };

    console.log(`Target: https://${options.hostname}${options.path}`);

    const proxyReq = https.request(options, (proxyRes) => {
      console.log(`\n--- Anthropic Response ---`);
      console.log(`Status: ${proxyRes.statusCode} ${proxyRes.statusMessage}`);
      console.log('Headers:', JSON.stringify(proxyRes.headers, null, 2));
      
      let responseBody = '';
      proxyRes.on('data', chunk => {
        responseBody += chunk;
      });
      
      proxyRes.on('end', () => {
        console.log(`Response body length: ${responseBody.length}`);
        try {
          const parsed = JSON.parse(responseBody);
          console.log('Response JSON:');
          console.log(JSON.stringify(parsed, null, 2).substring(0, 1000));
        } catch (e) {
          console.log('Raw response (first 500 chars):');
          console.log(responseBody.substring(0, 500));
        }
        console.log('='.repeat(60) + '\n');
      });
      
      res.writeHead(proxyRes.statusCode, proxyRes.headers);
      proxyRes.pipe(res);
    });

    proxyReq.on('error', (err) => {
      console.error(`Proxy error: ${err.message}`);
      res.writeHead(502, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'Proxy error', message: err.message }));
    });

    proxyReq.write(body);
    proxyReq.end();
  });

  req.on('error', (err) => {
    console.error(`Request error: ${err.message}`);
  });
});

server.on('connection', (socket) => {
  console.log(`[${new Date().toISOString()}] New TCP connection from ${socket.remoteAddress}:${socket.remotePort}`);
});

server.listen(PORT, '0.0.0.0', () => {
  console.log('');
  console.log('='.repeat(60));
  console.log('  NINTENCODE 3DS - DEBUG PROXY');
  console.log('='.repeat(60));
  console.log(`Listening on http://0.0.0.0:${PORT}`);
  console.log(`Forwarding to https://${ANTHROPIC_HOST}${ANTHROPIC_PATH}`);
  console.log('');
  console.log('Waiting for connections from 3DS...');
  console.log('='.repeat(60));
});
