'use strict'
const https = require('https');
const fs = require('fs');
const ws = require('ws');

const options = {
    key: fs.readFileSync('../../http-server/server.key'),
    cert: fs.readFileSync('../../http-server/server.crt')
};

// const index = fs.readFileSync('./index.html');

let server = https.createServer(options, (req, res) => {
    res.writeHead(200);
    // res.end(index);
});
server.addListener('upgrade', (req, res, head) => console.log('UPGRADE:', req.url));
server.on('error', (err) => console.error(err));
server.listen(3000, () => console.log('Https running on port 8000'));

const wss = new ws.Server({ server, path: '/' });
wss.on('connection', function connection(ws) {
    ws.on('message', (data) => ws.send('Receive: ' + data));
});